// bench_gameplay_items — repeatable dropped-item simulation benchmark
// (stabilization PR4, Phase 5). CPU-only: links ONLY ft_vox_core, no GPU,
// window or camera. Deliberately NOT registered with ctest (manual run):
//
//   bench_gameplay_items [--reps 5] [--seed 42] [--ticks 600] [--warmup 240]
//                        [--workers 0] [--json out.json] [--csv out.csv]
//                        [--scenario baseline|light|medium|max|pickup|streaming|save]
//
// Scenarios (fixed seed, fixed host dt = 1/60, fixed streaming budgets):
//   baseline   0 items     — fixed gameplay overhead (mobs disabled, flight)
//   light      100 items   — normal gameplay population
//   medium     512 items   — stress
//   max        1024 items  — population-cap behavior
//   pickup     1024 items  — inventory merge + removal burst
//   streaming  100->1024   — observer crossing chunk boundaries while spawning
//   save       1024 items  — savePlayerState / close / reopen + serialized bytes
//
// Methodology: Release build; warm-up before measuring; >= 5 repetitions,
// each a FRESH simulation; medians + min/max variability reported. The same
// source builds against the baseline commit (only public facade APIs that
// exist at 345dfa12 are used), so baseline-vs-current comparisons use the
// identical methodology on the same hardware/compiler/worker count.
//
// Measured independently:
//   * item simulation CPU time (advanceItems wall time),
//   * full sim frame time (advancePlayer+advanceMobs+advanceItems+streaming
//     +pumpWorld+snapshot+event drain — the host's per-frame CPU cost),
//   * snapshot generation time (itemSnapshots into a reused buffer),
//   * collision-query cost (physics::QueryStats cells/iterations counters),
//   * allocations per tick (global new/delete counter delta over the window),
//   * peak resident memory (platform RSS high-water),
//   * save duration + serialized bytes + reopen duration (save scenario).
// GPU rendering time is OUT OF SCOPE for this CPU-only bench (Phase 6 covers
// the graphical validation); frame percentiles here are CPU sim time, not
// presented-frame time. Pickup processing is included in the item simulation
// time; the pickup scenario isolates the burst window and reports the
// Removed/PickedUp event counts so the merge cost is attributable.

#include <Sim/GameSimulation.hpp>
#include <Core/ThreadPool.hpp>
#include <Items/ItemRegistry.hpp>
#include <Items/ItemTypes.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <sys/resource.h>
#endif

namespace
{
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

// --- Allocation counter (global new/delete; process-wide) ------------------
// Counts every allocation through the replaced global operators during the
// measured window. The counter itself never allocates. Allocations on worker
// threads are included (documented; --workers 0 keeps everything on the main
// thread for the cleanest attribution).
std::atomic<uint64_t> g_allocCount{0};
std::atomic<bool> g_countAllocs{false};

void *countingNew(std::size_t n)
{
    if (g_countAllocs.load(std::memory_order_relaxed))
        g_allocCount.fetch_add(1, std::memory_order_relaxed);
    if (void *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void countingDelete(void *p) noexcept { std::free(p); }

// --- Null streambuf: silence the grant audit log without allocating --------
struct NullBuf : std::streambuf
{
    int overflow(int c) override { return traits_type::not_eof(c); }
    std::streamsize xsputn(const char *, std::streamsize n) override { return n; }
};

uint64_t peakRssKB()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<uint64_t>(pmc.PeakWorkingSetSize / 1024);
    return 0;
#else
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0)
        return static_cast<uint64_t>(ru.ru_maxrss); // KiB on Linux
    return 0;
#endif
}

double msSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Percentiles
{
    double median = 0, p50 = 0, p95 = 0, p99 = 0, min = 0, max = 0;
};

Percentiles summarize(std::vector<double> v)
{
    Percentiles p;
    if (v.empty())
        return p;
    std::sort(v.begin(), v.end());
    auto pct = [&](double q) {
        const size_t idx = static_cast<size_t>(q * static_cast<double>(v.size() - 1));
        return v[std::min(idx, v.size() - 1)];
    };
    p.median = pct(0.5);
    p.p50 = p.median;
    p.p95 = pct(0.95);
    p.p99 = pct(0.99);
    p.min = v.front();
    p.max = v.back();
    return p;
}

// --- Scenario definitions ---------------------------------------------------
enum class Mode { Static, Pickup, Streaming, Save };
struct Scenario
{
    const char *name;
    int items;
    Mode mode;
};
const Scenario kScenarios[] = {
    {"baseline", 0, Mode::Static},
    {"light", 100, Mode::Static},
    {"medium", 512, Mode::Static},
    {"max", 1024, Mode::Static},
    {"pickup", 1024, Mode::Pickup},
    {"streaming", 1024, Mode::Streaming},
    {"save", 1024, Mode::Save},
};

struct Options
{
    int reps = 5;
    int seed = 42;
    int ticks = 600;
    int warmup = 240;
    int workers = 0;
    std::string jsonPath;
    std::string csvPath;
    std::string only; // empty = all scenarios
};

// One repetition's raw measurements.
struct RepResult
{
    std::vector<double> frameMs, itemMs, snapMs;
    uint64_t queryCells = 0, queryIters = 0;
    uint64_t allocs = 0;
    uint64_t spawned = 0, removed = 0, pickedUp = 0;
    size_t finalPopulation = 0;
    double saveMs = 0, closeMs = 0, reopenMs = 0;
    uint64_t savedBytes = 0;
    bool saveVerified = false;
};

// Deterministic drop direction: golden-angle spread (static/streaming) or a
// near-vertical tight cluster (pickup burst lands within the pickup radius).
glm::dvec3 dropDir(int i, bool tight)
{
    const double a = static_cast<double>(i) * 0.6180339887498949 * 6.283185307179586;
    const double h = tight ? 0.05 : 0.6;
    return {h * std::cos(a), 1.0, h * std::sin(a)};
}

// Spawn `count` items through the PUBLIC facade commands only (grant + drop):
// no test-only seams, identical on the baseline commit. The player is flying
// (host-driven), so the body never moves on its own.
void spawnItems(sim::GameSimulation &sim, int count, bool tight)
{
    for (int i = 0; i < count; ++i)
    {
        if (sim.inventory().stack(0).isEmpty())
            sim.grantItems(items::ItemId::Stone, 64);
        sim.dropFromSlot(0, 1, dropDir(i, tight));
    }
}

void streamStep(sim::GameSimulation &sim, const glm::dvec3 &observer, int workers)
{
    WorldStreamSettings settings;
    settings.maxRenderDistance = 192;
    settings.minRenderDistance = 96;
    sim.world()->updateStreaming(glm::vec3(observer), {1.f, 0.f}, settings);
    sim.world()->processChunkLoading(4);
    if (workers == 0)
        sim.world()->generatePendingVoxelsSync(glm::vec3(observer), 4);
    else
        sim.world()->generatePendingVoxels(glm::vec3(observer), 192.f * 192.f, 4);
}

uint64_t dirBytes(const fs::path &root)
{
    uint64_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
        if (!ec && it->is_regular_file(ec))
            total += static_cast<uint64_t>(it->file_size(ec));
    return total;
}

RepResult runRep(const Scenario &sc, const Options &opt, const fs::path &tempRoot)
{
    RepResult r;
    // workers == 0 -> fully synchronous generation (nullptr pool, the
    // headless fixture contract); ThreadPool itself requires >= 1 worker.
    std::unique_ptr<ThreadPool> pool;
    if (opt.workers > 0)
        pool = std::make_unique<ThreadPool>(static_cast<unsigned>(opt.workers));
    sim::GameSimulation sim(pool.get());
    sim::GameSimulation::Config cfg;
    cfg.seed = opt.seed;
    if (sc.mode == Mode::Save)
    {
        cfg.worldName = "bench";
        cfg.savesRoot = tempRoot;
    }
    const glm::dvec3 center(8.0, 100.0, 8.0);
    const glm::dvec3 boot = sim.initialize(cfg, center);
    sim.bootstrapTerrain(boot);
    sim.setMobsEnabled(false); // isolate the item cost (documented)
    sim.setPlayerFlight(true, {8.5, 100.0, 8.5});

    const bool tight = sc.mode == Mode::Pickup;
    const int initialItems = sc.mode == Mode::Streaming ? 100 : sc.items;
    spawnItems(sim, initialItems, tight);
    if (sc.mode == Mode::Pickup)
    {
        // Park the feet FAR above the spawn point immediately: a tight
        // cluster falls back through the spawn eye-height right when the
        // 40-tick pickup delay expires, so leaving the feet here would
        // consume the whole population during warm-up. The burst is armed
        // onto the LANDED cluster at the end of warm-up instead.
        sim.setPlayerFlight(true, {8.5, 200.0, 8.5});
    }

    const double dt = 1.0 / 60.0;
    const sim::PlayerCommand cmd{};
    std::vector<sim::ItemEntitySnapshot> snaps;
    std::vector<items::ItemEntityEvent> events;
    snaps.reserve(items::ItemEntitySettings::capacity);
    events.reserve(4096);

    glm::dvec3 observer = sim.playerFeet();
    bool pickupArmed = false;

    // The measured loop body is shared by warm-up and measurement (identical
    // work; only the sampling differs).
    const auto frame = [&](bool measure, int tick) {
        const auto t0 = Clock::now();
        sim.advancePlayer(dt, cmd, false);
        sim.advanceMobs(dt, observer, 192.0, false);
        const auto ti0 = Clock::now();
        sim.advanceItems(dt, false);
        const auto ti1 = Clock::now();
        if (sc.mode == Mode::Streaming)
        {
            // 8 blocks/s along +X: a chunk boundary every 2 s.
            observer = sim.playerFeet() + glm::dvec3(8.0 * dt * tick, 0, 0);
            sim.setPlayerPosition(observer);
            streamStep(sim, observer, opt.workers);
            if (tick % 120 == 0 && sim.itemEntities().size() < 1024)
                spawnItems(sim, 100, false);
        }
        sim.pumpWorld();
        const auto ts0 = Clock::now();
        sim.itemSnapshots(snaps);
        const auto ts1 = Clock::now();
        sim.takeItemEvents(events);
        for (const auto &e : events)
        {
            if (e.kind == items::ItemEventKind::Spawned)
                ++r.spawned;
            else
            {
                ++r.removed;
                if (e.reason == items::ItemRemoveReason::PickedUp)
                    ++r.pickedUp;
            }
        }
        events.clear();
        const auto t1 = Clock::now();
        if (measure)
        {
            r.frameMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            r.itemMs.push_back(std::chrono::duration<double, std::milli>(ti1 - ti0).count());
            r.snapMs.push_back(std::chrono::duration<double, std::milli>(ts1 - ts0).count());
            const auto &q = sim.itemEntities().queryStats();
            r.queryCells += q.cells;
            r.queryIters += q.iterations;
        }
        // Pickup burst: once the warm-up has settled the cluster, park the
        // feet on the first item so the whole population is in radius.
        if (sc.mode == Mode::Pickup && !pickupArmed && !measure && tick == opt.warmup - 1 &&
            !snaps.empty())
        {
            sim.setPlayerFlight(true, snaps[0].current);
            pickupArmed = true;
        }
    };

    for (int t = 0; t < opt.warmup; ++t)
        frame(false, t);

    // Reserve the sample storage BEFORE the measured window: the benchmark's
    // own vector growth must not pollute the allocations-per-tick metric.
    r.frameMs.reserve(static_cast<size_t>(opt.ticks));
    r.itemMs.reserve(static_cast<size_t>(opt.ticks));
    r.snapMs.reserve(static_cast<size_t>(opt.ticks));
    g_allocCount.store(0, std::memory_order_relaxed);
    g_countAllocs.store(true, std::memory_order_relaxed);
    for (int t = 0; t < opt.ticks; ++t)
        frame(true, opt.warmup + t);
    g_countAllocs.store(false, std::memory_order_relaxed);
    r.allocs = g_allocCount.load(std::memory_order_relaxed);
    r.finalPopulation = sim.itemEntities().size();

    if (sc.mode == Mode::Save)
    {
        const auto s0 = Clock::now();
        const bool saved = sim.savePlayerState(0.f, 0.f);
        r.saveMs = msSince(s0);
        const auto c0 = Clock::now();
        sim.shutdown();
        r.closeMs = msSince(c0);
        r.savedBytes = dirBytes(tempRoot / "bench");
        const auto r0 = Clock::now();
        sim::GameSimulation re(pool.get());
        const glm::dvec3 reBoot = re.initialize(cfg, center);
        re.bootstrapTerrain(reBoot);
        re.finalizePlayerSpawn(reBoot);
        r.reopenMs = msSince(r0);
        r.saveVerified = saved && re.itemEntities().size() == r.finalPopulation;
        re.shutdown();
    }
    else
    {
        sim.shutdown();
    }
    return r;
}

// --- Reporting ---------------------------------------------------------------
struct ScenarioReport
{
    std::string name;
    int items = 0;
    int reps = 0;
    Percentiles frame, item, snap;
    double queryCellsPerTick = 0, queryItersPerTick = 0, allocsPerTick = 0;
    double frameMedianMin = 0, frameMedianMax = 0; // variability across reps
    uint64_t peakRssKB = 0;
    uint64_t spawned = 0, removed = 0, pickedUp = 0;
    size_t finalPopulation = 0;
    double saveMs = 0, closeMs = 0, reopenMs = 0;
    uint64_t savedBytes = 0;
    bool saveVerified = false;
};

std::string pctJson(const Percentiles &p)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"median\":%.6f,\"p50\":%.6f,\"p95\":%.6f,\"p99\":%.6f,\"min\":%.6f,\"max\":%.6f}",
                  p.median, p.p50, p.p95, p.p99, p.min, p.max);
    return buf;
}

void writeJson(const std::string &path, const Options &opt, const std::vector<ScenarioReport> &reports)
{
    std::ofstream out(path);
    out << "{\n  \"benchmark\": \"gameplay-items\",\n  \"config\": {"
        << "\"seed\":" << opt.seed << ",\"ticks\":" << opt.ticks << ",\"warmup\":" << opt.warmup
        << ",\"reps\":" << opt.reps << ",\"workers\":" << opt.workers << ",\"dt\":0.0166667},"
        << "\n  \"scenarios\": [\n";
    for (size_t i = 0; i < reports.size(); ++i)
    {
        const auto &r = reports[i];
        out << "    {\"name\":\"" << r.name << "\",\"items\":" << r.items << ",\"reps\":" << r.reps
            << ",\n     \"frameMs\":" << pctJson(r.frame)
            << ",\n     \"itemMs\":" << pctJson(r.item)
            << ",\n     \"snapshotMs\":" << pctJson(r.snap)
            << ",\n     \"frameMedianAcrossReps\":{\"min\":" << r.frameMedianMin
            << ",\"max\":" << r.frameMedianMax << "}"
            << ",\n     \"queryCellsPerTick\":" << r.queryCellsPerTick
            << ",\"queryIterationsPerTick\":" << r.queryItersPerTick
            << ",\"allocsPerTick\":" << r.allocsPerTick
            << ",\"peakRssKB\":" << r.peakRssKB
            << ",\n     \"events\":{\"spawned\":" << r.spawned << ",\"removed\":" << r.removed
            << ",\"pickedUp\":" << r.pickedUp << "},\"finalPopulation\":" << r.finalPopulation;
        if (r.saveMs > 0 || r.savedBytes > 0)
            out << ",\n     \"save\":{\"saveMs\":" << r.saveMs << ",\"closeMs\":" << r.closeMs
                << ",\"reopenMs\":" << r.reopenMs << ",\"serializedBytes\":" << r.savedBytes
                << ",\"verified\":" << (r.saveVerified ? "true" : "false") << "}";
        out << "\n    }" << (i + 1 < reports.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
}

void writeCsv(const std::string &path, const std::vector<ScenarioReport> &reports)
{
    std::ofstream out(path);
    out << "scenario,items,reps,frame_median_ms,frame_p95_ms,frame_p99_ms,item_median_ms,"
           "item_p95_ms,snapshot_median_ms,query_cells_per_tick,query_iters_per_tick,"
           "allocs_per_tick,peak_rss_kb,spawned,removed,picked_up,final_population,"
           "save_ms,close_ms,reopen_ms,serialized_bytes\n";
    for (const auto &r : reports)
        out << r.name << ',' << r.items << ',' << r.reps << ',' << r.frame.median << ','
            << r.frame.p95 << ',' << r.frame.p99 << ',' << r.item.median << ',' << r.item.p95
            << ',' << r.snap.median << ',' << r.queryCellsPerTick << ',' << r.queryItersPerTick
            << ',' << r.allocsPerTick << ',' << r.peakRssKB << ',' << r.spawned << ','
            << r.removed << ',' << r.pickedUp << ',' << r.finalPopulation << ',' << r.saveMs
            << ',' << r.closeMs << ',' << r.reopenMs << ',' << r.savedBytes << '\n';
}

} // namespace

// Global allocation counters (see the note above the counting helpers).
void *operator new(std::size_t n) { return countingNew(n); }
void operator delete(void *p) noexcept { countingDelete(p); }
void *operator new[](std::size_t n) { return countingNew(n); }
void operator delete[](void *p) noexcept { countingDelete(p); }
void operator delete(void *p, std::size_t) noexcept { countingDelete(p); }
void operator delete[](void *p, std::size_t) noexcept { countingDelete(p); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept { return countingNew(n); }
void operator delete(void *p, const std::nothrow_t &) noexcept { countingDelete(p); }

int main(int argc, char **argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&](const char *name) -> const char * {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--reps") opt.reps = std::atoi(next("--reps"));
        else if (a == "--seed") opt.seed = std::atoi(next("--seed"));
        else if (a == "--ticks") opt.ticks = std::atoi(next("--ticks"));
        else if (a == "--warmup") opt.warmup = std::atoi(next("--warmup"));
        else if (a == "--workers") opt.workers = std::atoi(next("--workers"));
        else if (a == "--json") opt.jsonPath = next("--json");
        else if (a == "--csv") opt.csvPath = next("--csv");
        else if (a == "--scenario") opt.only = next("--scenario");
        else
        {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }
    if (opt.reps < 1 || opt.ticks < 1 || opt.warmup < 0)
    {
        std::fprintf(stderr, "invalid reps/ticks/warmup\n");
        return 2;
    }

    // Silence the grant audit log for the whole run (no allocations).
    NullBuf nullBuf;
    std::streambuf *oldCout = std::cout.rdbuf(&nullBuf);

    const fs::path tempRoot = fs::temp_directory_path() /
        ("ft-vox-bench-items-" + std::to_string(Clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(tempRoot, ec);

    std::vector<ScenarioReport> reports;
    for (const Scenario &sc : kScenarios)
    {
        if (!opt.only.empty() && opt.only != sc.name)
            continue;
        std::vector<RepResult> reps;
        std::vector<double> repFrameMedians;
        for (int rep = 0; rep < opt.reps; ++rep)
        {
            // Each repetition is a fresh simulation; the save scenario gets a
            // clean per-rep directory so serialized bytes are exact.
            const fs::path repRoot = tempRoot / ("rep" + std::to_string(rep));
            fs::create_directories(repRoot, ec);
            RepResult r = runRep(sc, opt, repRoot);
            repFrameMedians.push_back(summarize(r.frameMs).median);
            reps.push_back(std::move(r));
            fs::remove_all(repRoot, ec);
        }
        ScenarioReport rep;
        rep.name = sc.name;
        rep.items = sc.items;
        rep.reps = opt.reps;
        // Pool the per-frame samples of every repetition for the percentiles;
        // variability is the spread of the per-rep medians.
        std::vector<double> frames, items, snaps;
        for (const auto &r : reps)
        {
            frames.insert(frames.end(), r.frameMs.begin(), r.frameMs.end());
            items.insert(items.end(), r.itemMs.begin(), r.itemMs.end());
            snaps.insert(snaps.end(), r.snapMs.begin(), r.snapMs.end());
            rep.queryCellsPerTick += static_cast<double>(r.queryCells) / opt.ticks;
            rep.queryItersPerTick += static_cast<double>(r.queryIters) / opt.ticks;
            rep.allocsPerTick += static_cast<double>(r.allocs) / opt.ticks;
            rep.spawned += r.spawned;
            rep.removed += r.removed;
            rep.pickedUp += r.pickedUp;
            rep.finalPopulation = r.finalPopulation;
            rep.saveMs += r.saveMs;
            rep.closeMs += r.closeMs;
            rep.reopenMs += r.reopenMs;
            rep.savedBytes += r.savedBytes;
            rep.saveVerified = rep.saveVerified || r.saveVerified;
        }
        rep.queryCellsPerTick /= opt.reps;
        rep.queryItersPerTick /= opt.reps;
        rep.allocsPerTick /= opt.reps;
        rep.saveMs /= opt.reps;
        rep.closeMs /= opt.reps;
        rep.reopenMs /= opt.reps;
        rep.savedBytes /= static_cast<uint64_t>(opt.reps);
        rep.frame = summarize(std::move(frames));
        rep.item = summarize(std::move(items));
        rep.snap = summarize(std::move(snaps));
        rep.frameMedianMin = *std::min_element(repFrameMedians.begin(), repFrameMedians.end());
        rep.frameMedianMax = *std::max_element(repFrameMedians.begin(), repFrameMedians.end());
        rep.peakRssKB = peakRssKB();
        reports.push_back(std::move(rep));
    }

    fs::remove_all(tempRoot, ec);
    std::cout.rdbuf(oldCout);

    // Console table (human-readable summary).
    std::printf("%-10s %6s %10s %10s %10s %10s %10s %9s %8s\n",
                "scenario", "items", "frame_p50", "frame_p95", "frame_p99", "item_p50",
                "snap_p50", "allocs/t", "pop");
    for (const auto &r : reports)
        std::printf("%-10s %6d %10.4f %10.4f %10.4f %10.4f %10.4f %9.2f %8zu\n",
                    r.name.c_str(), r.items, r.frame.p50, r.frame.p95, r.frame.p99,
                    r.item.p50, r.snap.p50, r.allocsPerTick, r.finalPopulation);
    for (const auto &r : reports)
        if (r.savedBytes > 0)
            std::printf("save: saveMs=%.2f closeMs=%.2f reopenMs=%.2f bytes=%llu verified=%s\n",
                        r.saveMs, r.closeMs, r.reopenMs,
                        static_cast<unsigned long long>(r.savedBytes),
                        r.saveVerified ? "yes" : "NO");
    // The pickup burst lives in the first frames of the window (the median
    // hides it): report the peak item-step time and the merged entity count.
    for (const auto &r : reports)
        if (r.name == std::string("pickup"))
            std::printf("pickup burst: pickedUp=%llu itemMaxMs=%.4f finalPop=%zu\n",
                        static_cast<unsigned long long>(r.pickedUp), r.item.max,
                        r.finalPopulation);

    if (!opt.jsonPath.empty())
        writeJson(opt.jsonPath, opt, reports);
    if (!opt.csvPath.empty())
        writeCsv(opt.csvPath, reports);

    // Sanity gates (not FPS thresholds): population cap respected, no
    // unbounded growth, save round-trip verified.
    int failures = 0;
    for (const auto &r : reports)
    {
        if (r.finalPopulation > items::ItemEntitySettings::capacity)
        {
            std::fprintf(stderr, "FAIL: %s population %zu exceeds cap\n", r.name.c_str(),
                         r.finalPopulation);
            ++failures;
        }
        if (r.name == std::string("save") && !r.saveVerified)
        {
            std::fprintf(stderr, "FAIL: save/reload population mismatch\n");
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
