#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

// Independent of the CPU/GPU profilers. Gauges describe ownership; events
// describe a capture interval. No worker-owned vector is read by the sampler.
namespace telemetry {
enum Gauge : size_t {
    VoxelBytes, ShellBytes, ShellCapacity, OpaqueVertexBytes, OpaqueVertexCapacity,
    OpaqueIndexBytes, OpaqueIndexCapacity, WaterVertexBytes, WaterVertexCapacity,
    WaterIndexBytes, WaterIndexCapacity, ColumnBytes, OccupancyBytes,
    GpuOpaqueVertex, GpuOpaqueIndex, GpuWaterVertex, GpuWaterIndex,
    RetiredBytes, RetiredBuffers, PoolCapacity, PoolAcquired, PoolFree,
    ActiveChunks, DeferredChunks, StagingUsed, CpuMeshCapacity, GpuLiveBytes,
    VoxelPoolCapacity, VoxelPoolActive, VoxelPoolFree, VoxelPoolCapacityBytes,
    BorderPoolCapacity, BorderPoolActive, BorderPoolFree, BorderPoolCapacityBytes,
    MeshPoolCapacity, MeshPoolActive, MeshPoolFree, MeshPoolCapacityBytes,
    LightPoolCapacity, LightPoolActive, LightPoolFree, LightPoolCapacityBytes,
    ArenaPages, ArenaFreeBytes, ArenaHighWater, GaugeCount
};
inline constexpr const char* gaugeNames[] = {
    "voxel.bytes", "shell.sizeBytes", "shell.capacityBytes",
    "cpu.opaque.vertex.sizeBytes", "cpu.opaque.vertex.capacityBytes",
    "cpu.opaque.index.sizeBytes", "cpu.opaque.index.capacityBytes",
    "cpu.water.vertex.sizeBytes", "cpu.water.vertex.capacityBytes",
    "cpu.water.index.sizeBytes", "cpu.water.index.capacityBytes",
    "column.bytes", "occupancy.bytes", "gpu.opaque.vertex.bytes",
    "gpu.opaque.index.bytes", "gpu.water.vertex.bytes", "gpu.water.index.bytes",
    "gpu.retired.bytes", "gpu.retired.buffers", "pool.capacity", "pool.acquired",
    "pool.free", "chunks.active", "chunks.deferred", "staging.slice.bytes",
    "cpu.mesh.capacityBytes", "gpu.live.bytes",
    "voxel.pool.capacity", "voxel.pool.active", "voxel.pool.free",
    "voxel.pool.capacityBytes", "border.pool.capacity", "border.pool.active",
    "border.pool.free", "border.pool.capacityBytes",
    "mesh.pool.capacity", "mesh.pool.active", "mesh.pool.free",
    "mesh.pool.capacityBytes",
    "light.pool.capacity", "light.pool.active", "light.pool.free",
    "light.pool.capacityBytes",
    "arena.pages", "arena.freeBytes",
    "arena.highWaterBytes"
};
enum Event : size_t {
    AllocCreated, AllocDestroyed, PoolRejected, UploadChunks, UploadVertexBytes,
    UploadIndexBytes, UploadDeferred, StagingFailures, OpaqueDraws, WaterDraws,
    Shadow0, Shadow1, Shadow2, VoxelPoolGrow, BorderPoolGrow, MeshPoolGrow,
    ArenaBinds, ArenaGrow, EventCount
};
inline constexpr const char* eventNames[] = {
    "mesh.allocations.created", "mesh.allocations.destroyed", "pool.rejected",
    "upload.chunks", "upload.vertexBytes", "upload.indexBytes", "upload.deferred",
    "staging.failures", "draws.opaque", "draws.water", "draws.shadow.0",
    "draws.shadow.1", "draws.shadow.2", "voxel.pool.growEvents",
    "border.pool.growEvents", "mesh.pool.growEvents", "arena.binds",
    "arena.growEvents"
};
enum Stage : size_t { Skylight, Blocklight, Occupancy, FacesGreedyAO, Lod, HaloFill, StageCount };
inline constexpr const char* stageNames[] = {"skylight", "blocklight", "occupancy", "facesGreedyAO", "LOD", "haloFill"};
// Sample families (issue #173 review): a real full/LOD mesh build reports
// under "mesh.*"; an entity light-cache-only build (no geometry) reports
// under "lightCache.*". Both share the stage-chain timing mechanics, but a
// family never publishes into the other's gauges or per-build totals.
enum Family : size_t { MeshFamily, LightCacheFamily, FamilyCount };
inline constexpr const char* familyNames[] = {"mesh", "lightCache"};

// Capture-local gauges describe transient state sampled during a capture
// window (staging slice usage; end-of-frame chunk-manager samples; the
// VoxelPool active/free split). Unlike ownership gauges they are NOT
// carried across a beginCapture() boundary: a warmup high-water mark must
// not become a measurement peak. Gauges default to persistent ownership;
// classify one here only if its value is meaningless outside the currently
// running capture. Note VoxelPoolCapacity/VoxelPoolCapacityBytes stay
// persistent: retained backing memory really exists at the boundary.
inline constexpr bool isCaptureLocalGauge(Gauge g) {
    switch (g) {
    case StagingUsed:
    case ActiveChunks:
    case DeferredChunks:
    case VoxelPoolActive:
    case VoxelPoolFree:
    case BorderPoolActive:
    case BorderPoolFree:
    case MeshPoolActive:
    case MeshPoolFree:
    case LightPoolActive:
    case LightPoolFree:
        return true;
    default:
        return false;
    }
}
struct Snapshot {
    bool enabled{false};
    std::array<uint64_t, GaugeCount> current{}, peak{};
    std::array<uint64_t, EventCount> events{};
    std::array<std::array<uint64_t, StageCount>, FamilyCount> stageNs{}, stageCalls{};
    // Per-call durations (ms) retained alongside the stage totals: one entry
    // per completed stage segment (MeshSample::next / StageSample scope), so
    // the benchmark can report avg + p95 per stage. Same capture lifetime as
    // the totals: cleared by beginCapture(), merged under the registry mutex.
    std::array<std::array<std::vector<float>, StageCount>, FamilyCount> stageSamplesMs{};
    // One sample per completed MeshSample (a finished full or LOD mesh build
    // for MeshFamily, an entity light-cache-only build for LightCacheFamily):
    // the sum of that sample's stage-chain segments in ms.
    std::array<std::vector<float>, FamilyCount> totalSamplesMs{};
    uint64_t maskCells{}, aoVertices{}, opaqueVertices{}, opaqueIndices{}, waterVertices{}, waterIndices{};
};
class Registry {
public:
    const bool enabled = [] { const char* e = std::getenv("FT_VOX_TELEMETRY"); return !e || std::strcmp(e, "0") != 0; }();
    void replace(Gauge g, uint64_t before, uint64_t after) {
        if (!enabled) return;
        std::lock_guard lock(mutex);
        data.current[g] = data.current[g] - before + after;
        data.peak[g] = std::max(data.peak[g], data.current[g]);
        if (g >= GpuOpaqueVertex && g <= GpuWaterIndex) {
            data.current[GpuLiveBytes] = data.current[GpuOpaqueVertex] + data.current[GpuOpaqueIndex]
                + data.current[GpuWaterVertex] + data.current[GpuWaterIndex];
            data.peak[GpuLiveBytes] = std::max(data.peak[GpuLiveBytes], data.current[GpuLiveBytes]);
        }
    }
    void set(Gauge g, uint64_t value) {
        if (!enabled) return;
        std::lock_guard lock(mutex);
        data.current[g] = value;
        data.peak[g] = std::max(data.peak[g], value);
    }
    template<size_t N> void replaceCpu(const std::array<uint64_t, N>& before, const std::array<uint64_t, N>& after) {
        if (!enabled) return;
        std::lock_guard lock(mutex);
        for (size_t i=0; i<N; ++i) {
            data.current[i] = data.current[i] - before[i] + after[i];
            data.peak[i] = std::max(data.peak[i], data.current[i]);
        }
        // CpuMeshCapacity is NOT derived here anymore (issue #104): mesh
        // build buffers no longer live on Chunks, so the engine publishes
        // it (with the cpu.opaque/water.* gauges) from MeshResultPoolStats.
    }
    void add(Event e, uint64_t n = 1) {
        if (enabled) events[e].fetch_add(n, std::memory_order_relaxed);
    }
    // Only the main thread resets events. Worker batches carry an epoch.
    void beginCapture() {
        std::lock_guard lock(mutex);
        ++epoch;
        const auto previousCurrent = data.current;
        data = {};
        // Ownership gauges describe state that still exists at the capture
        // boundary: carry the value over and rebase the peak to it, so the
        // world built during warmup is not under-reported. Capture-local
        // gauges start at zero so warmup high-water marks (staging slice
        // usage, chunk-manager frame samples) cannot leak into measurement.
        for (size_t i = 0; i < GaugeCount; ++i) {
            if (!isCaptureLocalGauge(static_cast<Gauge>(i)))
                data.current[i] = data.peak[i] = previousCurrent[i];
        }
        for (auto& e : events) e.store(0, std::memory_order_relaxed);
    }
    uint64_t captureEpoch() {
        if (!enabled) return 0;
        std::lock_guard lock(mutex);
        return epoch;
    }
    void worker(uint64_t tag, const Snapshot& s) {
        if (!enabled) return;
        std::lock_guard lock(mutex);
        if (tag != epoch) return;
        for (size_t f=0; f<FamilyCount; ++f) {
            for (size_t i=0; i<StageCount; ++i) {
                data.stageNs[f][i] += s.stageNs[f][i]; data.stageCalls[f][i] += s.stageCalls[f][i];
                data.stageSamplesMs[f][i].insert(data.stageSamplesMs[f][i].end(),
                                                  s.stageSamplesMs[f][i].begin(), s.stageSamplesMs[f][i].end());
            }
            data.totalSamplesMs[f].insert(data.totalSamplesMs[f].end(),
                                          s.totalSamplesMs[f].begin(), s.totalSamplesMs[f].end());
        }
        data.maskCells += s.maskCells; data.aoVertices += s.aoVertices;
        data.opaqueVertices += s.opaqueVertices; data.opaqueIndices += s.opaqueIndices;
        data.waterVertices += s.waterVertices; data.waterIndices += s.waterIndices;
    }
    // Terminal read for a measurement window (benchmark finalize): totals and
    // gauges are copied, per-call samples are moved out so the registry does
    // not keep multi-megabyte duration buffers after the report is built.
    Snapshot snapshot() {
        std::lock_guard lock(mutex);
        Snapshot s = std::move(data);
        s.enabled = enabled;
        for (size_t i=0; i<EventCount; ++i) s.events[i] = events[i].load(std::memory_order_relaxed);
        return s;
    }
private:
    std::mutex mutex;
    Snapshot data{};
    std::array<std::atomic<uint64_t>, EventCount> events{};
    uint64_t epoch{1};
};
inline Registry& registry() { static Registry r; return r; }

// A few clock reads per build, one mutex acquisition on completion. Face/AO
// counts are local; no clock or atomic operation in the inner voxel loops.
// Each completed stage segment pushes one ms duration into
// data.stageSamplesMs[family]; the destructor pushes one chain-sum ms sample
// into data.totalSamplesMs[family], giving one per-build total per completed
// sample. The family selects the gauge namespace: mesh stages stay exclusive
// to real mesh builds (issue #173 review).
class MeshSample {
    using Clock = std::chrono::steady_clock;
    uint64_t tag = registry().captureEpoch();
    Stage stage;
    Family family;
    Clock::time_point start{};
public:
    Snapshot data{};
    explicit MeshSample(Stage s, Family f = MeshFamily) : stage(s), family(f) { if (tag) start = Clock::now(); }
    void next(Stage s) {
        if (!tag) return;
        auto now = Clock::now();
        const uint64_t ns = uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now-start).count());
        data.stageNs[family][stage] += ns;
        data.stageSamplesMs[family][stage].push_back(float(double(ns) / 1e6));
        ++data.stageCalls[family][stage]; start = now; stage = s;
    }
    ~MeshSample() {
        if (!tag) return;
        next(stage);
        uint64_t chainNs = 0;
        for (size_t i=0; i<StageCount; ++i) chainNs += data.stageNs[family][i];
        data.totalSamplesMs[family].push_back(float(double(chainNs) / 1e6));
        registry().worker(tag, data);
    }
};

// Single-stage scope for timed work that sits outside MeshSample's
// sequential phase chain (e.g. the occupancy prepass in Chunk::buildMesh,
// which runs before the Skylight/Blocklight/FacesGreedyAO phases and may
// end in an early return). The family selects the gauge namespace, matching
// the build the scope belongs to. One clock-read pair and one worker() merge
// per scope; zero cost when telemetry is disabled.
class StageSample {
    using Clock = std::chrono::steady_clock;
    uint64_t tag = registry().captureEpoch();
    Stage stage;
    Family family;
    Clock::time_point start{};
public:
    explicit StageSample(Stage s, Family f = MeshFamily) : stage(s), family(f) { if (tag) start = Clock::now(); }
    ~StageSample() {
        if (!tag) return;
        Snapshot s{};
        const uint64_t ns = uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        s.stageNs[family][stage] = ns;
        s.stageSamplesMs[family][stage].push_back(float(double(ns) / 1e6));
        s.stageCalls[family][stage] = 1;
        registry().worker(tag, s);
    }
};
} // namespace telemetry
