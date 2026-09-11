// Developer-console core logic (issue #179): pure formatting, bounded ring
// histories, sustained-condition health monitors, chunk debug snapshot
// extraction and the bounded chunk event trace — all without ImGui or a
// window. The telemetry live-read semantics are covered in
// test_workload_telemetry.cpp.
#include <Engine/DebugUI/DebugUiCore.hpp>
#include <Engine/DebugUI/DebugUiEngine.hpp>
#include <Chunk/ChunkManager.hpp>
#include <Chunk/ChunkPool.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Camera/Camera.hpp>
#include <Engine/Profiler.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

static int g_fails = 0;
#define CHECK(cond, msg)                                                       \
	do                                                                         \
	{                                                                          \
		if (!(cond))                                                           \
		{                                                                      \
			std::cerr << "FAIL: " << msg << " (" << __LINE__ << ")\n";         \
			++g_fails;                                                         \
		}                                                                      \
	} while (0)

// Friend probe (same pattern as test_chunk_lifecycle): test-only access to
// the main-thread trace writer and chunk registration without streaming.
struct ChunkManagerProbe
{
	static void registerChunk(ChunkManager &m, const glm::ivec3 &pos, Chunk *c)
	{
		m.m_chunks[pos] = c;
		m.m_activeChunks.push_back(c);
		c->setActiveIndex(m.m_activeChunks.size() - 1);
	}
	static void recordEvent(ChunkManager &m, Chunk *c, const char *kind, uint32_t sections = 0)
	{
		m.recordChunkEvent(c, kind, sections);
	}
};

namespace
{
void testFormatBytes()
{
	std::cout << "[formatBytes] ";
	CHECK(debugui::formatBytes(0) == "0 B", "zero bytes");
	CHECK(debugui::formatBytes(512) == "512 B", "sub-KiB");
	CHECK(debugui::formatBytes(1536) == "1.5 KiB", "KiB scaling");
	CHECK(debugui::formatBytes(1024ull * 1024) == "1.0 MiB", "MiB scaling");
	CHECK(debugui::formatBytes(3ull * 1024 * 1024 * 1024) == "3.00 GiB", "GiB scaling");
	CHECK(debugui::formatBytes(1536ull * 1024 * 1024 * 1024) == "1536.0 GiB",
		  "above 100 GiB still GiB (no TiB unit defined)");
	std::cout << "PASS\n";
}

void testFormatCountAndMs()
{
	std::cout << "[formatCount/formatMs] ";
	CHECK(debugui::formatCount(9999) == "9999", "counts below 10k stay exact");
	CHECK(debugui::formatCount(10000) == "10.0k", "k scaling");
	CHECK(debugui::formatCount(2500000) == "2.5M", "M scaling");
	CHECK(debugui::formatMs(0.f) == "0", "zero ms");
	CHECK(debugui::formatMs(0.031f) == "0.031", "sub-ms precision");
	CHECK(debugui::formatMs(1.42f) == "1.42", "ms precision");
	CHECK(debugui::formatMs(150.f) == "150", "large ms drops precision");
	std::cout << "PASS\n";
}

void testMetricHistory()
{
	std::cout << "[MetricHistory] ";
	debugui::MetricHistory h;
	CHECK(h.count() == 0 && h.latest() == 0.f, "empty history");

	for (float v = 1.f; v <= 5.f; v += 1.f)
		h.push(v);
	CHECK(h.count() == 5, "count tracks pushes");
	CHECK(h.latest() == 5.f, "latest is last pushed");
	CHECK(h.back(0) == 5.f && h.back(4) == 1.f, "back(0)=latest, back(n-1)=oldest");

	// Ring wraparound: push 300 samples into a 256-slot ring.
	debugui::MetricHistory ring;
	for (int i = 1; i <= 300; ++i)
		ring.push(float(i));
	CHECK(ring.count() == debugui::kMetricHistorySize, "count saturates at ring size");
	CHECK(ring.latest() == 300.f, "latest survives wraparound");
	CHECK(ring.back(255) == 45.f, "oldest retained sample is 45");

	// The chronological read used by the plots must be oldest-first.
	std::array<float, debugui::kMetricHistorySize> ordered{};
	ring.copyOrdered(ordered.data());
	CHECK(ordered[0] == 45.f && ordered[255] == 300.f, "ordered copy is chronological");
	std::cout << "PASS\n";
}

void testHealthMonitor()
{
	std::cout << "[HealthMonitor] ";
	// Sustained-condition: raises after 2 s, clears only after 3 s quiet.
	debugui::HealthMonitor m(2.f, 3.f);
	CHECK(!m.update(true, 1.f), "below enter window stays quiet");
	CHECK(m.update(true, 1.f), "raises once condition held enter seconds");
	CHECK(m.update(false, 1.f), "stays raised during clear hysteresis");
	CHECK(m.update(false, 1.5f), "stays raised until clear window elapses");
	CHECK(!m.update(false, 0.8f), "clears after quiet for exit seconds");

	// Re-raising requires the full enter window again.
	CHECK(!m.update(true, 1.f), "re-enter needs sustained condition again");
	CHECK(m.update(true, 1.f), "re-enters after sustained condition");

	// Recent-event shape: enter immediately, clear after quiet window.
	debugui::HealthMonitor event(0.f, 2.f);
	CHECK(event.update(true, 0.f), "zero enter raises immediately");
	CHECK(!event.update(false, 2.f), "clears after exit seconds quiet");

	debugui::HealthMonitor r(1.f, 1.f);
	r.update(true, 2.f);
	r.reset();
	CHECK(!r.active(), "reset clears the raised state");
	std::cout << "PASS\n";
}

void testChunkSnapshotAndTrace()
{
	std::cout << "[ChunkDebugSnapshot/trace] ";
	TerrainGenerator generator(42);
	ChunkPool pool(8);
	ChunkManager manager(&generator, nullptr, &pool);

	const glm::ivec3 coord{10, 0, 10};
	Chunk *chunk = pool.acquire(glm::vec3(float(coord.x * CHUNK_SIZE), 0.f,
										  float(coord.z * CHUNK_SIZE)));
	CHECK(chunk != nullptr, "pool acquire provides a chunk");
	if (!chunk)
	{
		std::cout << "SKIP (pool exhausted)\n";
		return;
	}
	ChunkManagerProbe::registerChunk(manager, coord, chunk);

	// Unknown coordinates report an unloaded snapshot with distance.
	const glm::vec3 camPos(float(coord.x * CHUNK_SIZE + 8), 90.f,
						   float(coord.z * CHUNK_SIZE + 8));
	debugui::ChunkDebugSnapshot missing = debugui::makeChunkDebugSnapshot(manager, {99, 0, -3}, camPos);
	CHECK(!missing.loaded && !missing.valid, "unknown coord is not loaded");

	// Shape the chunk into a MESHED, upload-pending state.
	chunk->setState(ChunkState::MESHED);
	chunk->setInTransit(true);
	chunk->markSectionsDirty(0x5);
	chunk->setLocalLightCacheWanted(true);
	debugui::ChunkDebugSnapshot snap = debugui::makeChunkDebugSnapshot(manager, coord, camPos);
	CHECK(snap.loaded && snap.valid, "registered chunk snapshots");
	CHECK(snap.state == ChunkState::MESHED, "lifecycle state captured");
	CHECK(snap.inTransit, "in-transit flag captured");
	CHECK(snap.dirtySections == 0x5, "dirty section mask captured");
	CHECK(snap.lightCacheWanted && !snap.lightCachePresent, "light-cache intent captured");
	CHECK(snap.distance < 1.f, "camera at chunk center => ~zero distance");
	CHECK(snap.liveGpuSections == 0, "no GPU sections without upload");

	// Trace: disabled by default; enabling records into the bounded ring.
	CHECK(!manager.chunkEventTraceEnabled(), "trace disabled by default");
	CHECK(manager.chunkDebugEvents().empty(), "no events recorded while disabled");
	manager.setChunkEventTraceEnabled(true);
	ChunkManagerProbe::recordEvent(manager, chunk, "load");
	ChunkManagerProbe::recordEvent(manager, chunk, "genDone");
	ChunkManagerProbe::recordEvent(manager, chunk, "meshDone", 0x5);
	std::vector<ChunkDebugEvent> events = manager.chunkDebugEvents();
	CHECK(events.size() == 3, "events recorded while enabled");
	CHECK(events[0].kind && std::string(events[0].kind) == "load" &&
			  events[2].kind && std::string(events[2].kind) == "meshDone",
		  "events are chronological");
	CHECK(events[0].chunk.x == coord.x && events[0].chunk.z == coord.z,
		  "event carries the chunk coordinate");
	CHECK(events[2].sections == 0x5, "event carries section context");

	// Bounded: overflow evicts the oldest instead of growing.
	for (int i = 0; i < int(ChunkManager::kChunkEventRingSize) + 10; ++i)
		ChunkManagerProbe::recordEvent(manager, chunk, "meshQueued", unsigned(i));
	events = manager.chunkDebugEvents();
	CHECK(events.size() == ChunkManager::kChunkEventRingSize, "ring stays bounded");
	CHECK(std::string(events.front().kind) == "meshQueued" && events.front().sections == 10,
		  "oldest events evicted in order");
	CHECK(std::string(events.back().kind) == "meshQueued" &&
			  events.back().sections == unsigned(ChunkManager::kChunkEventRingSize + 10 - 1),
		  "newest event retained");
	std::cout << "PASS\n";
}

void testUpdateDebugUiState()
{
	std::cout << "[updateDebugUiState] ";
	TerrainGenerator generator(42);
	ChunkPool pool(8);
	ChunkManager manager(&generator, nullptr, &pool);
	Camera camera;
	camera.setPosition(glm::vec3(160.f, 90.f, 200.f));

	GameUIFrame f{};
	f.camera = &camera;
	f.chunks = &manager;
	f.pool = &pool;

	// CPU frame time must come from the profiler, not the paced simulation
	// delta (issue #179 review): feed a deliberately divergent dt and check
	// the snapshot keeps the two apart.
	Profiler &prof = GetProfiler();
	f.frameMs = 12345.6f;

	debugui::UiState state;
	state.panels.streaming = true; // consumer panel open

	const double t0 = 100.0;
	debugui::updateDebugUiState(state, f, t0);
	CHECK(state.frame.cpuFrameMs == prof.lastFrameMs(),
		  "snapshot CPU frame time sourced from the profiler");
	CHECK(state.frame.cpuFrameMs != 12345.6f || prof.lastFrameMs() == 12345.6f,
		  "paced simulation dt is not reported as CPU frame time");
	CHECK(state.frame.simulationDtMs == 12345.6f, "simulation dt kept in its own field");
	CHECK(state.streaming.loadedChunks == 0, "streaming snapshot sampled (no chunks yet)");
	CHECK(state.activeChunks.count() == 1, "history pushed on first sample");
	CHECK(std::abs(state.memory.sampledAt - t0) < 1e-9, "sample timestamp recorded");

	// Heavy sampling is throttled to 10 Hz: an immediate second call must
	// not re-sample.
	debugui::updateDebugUiState(state, f, t0 + 0.01);
	CHECK(state.activeChunks.count() == 1, "throttled call does not re-sample");

	// With every consumer closed the snapshot is not refreshed at all.
	state.panels.streaming = false;
	debugui::updateDebugUiState(state, f, t0 + 10.0);
	CHECK(state.activeChunks.count() == 1, "closed panels stop sampling");

	// The opt-in chunk trace flag is propagated to a fresh ChunkManager by
	// the sampler itself — even with the inspector panel closed — so it
	// survives world reloads (issue #179 review).
	ChunkManager reloaded(&generator, nullptr, &pool);
	state.eventTraceEnabled = true;
	f.chunks = &reloaded;
	debugui::updateDebugUiState(state, f, t0 + 10.01);
	CHECK(reloaded.chunkEventTraceEnabled(), "trace flag re-applied to a recreated manager with F9 closed");

	// reloadWorld() (issue #179 review round 2): the dying manager's flag is
	// captured and re-applied to the replacement BEFORE generateInitialArea()
	// — bootstrap lifecycle events must land before any updateDebugUiState
	// call, not just once the UI has re-synced the flag.
	const bool carried = reloaded.chunkEventTraceEnabled();
	CHECK(carried, "dying manager's trace flag captured for the reload");
	ChunkManager rebootstrap(&generator, nullptr, &pool);
	rebootstrap.setChunkEventTraceEnabled(carried);
	Chunk *bootstrapped = pool.acquire(glm::vec3(0.f, 0.f, 0.f));
	CHECK(bootstrapped != nullptr, "pool provides a chunk for the reload bootstrap");
	if (bootstrapped)
	{
		ChunkManagerProbe::registerChunk(rebootstrap, {0, 0, 0}, bootstrapped);
		ChunkManagerProbe::recordEvent(rebootstrap, bootstrapped, "load");
		CHECK(!rebootstrap.chunkDebugEvents().empty(),
			  "bootstrap events recorded on the reloaded manager before any UI sample");
	}

	std::cout << "PASS\n";
}
void testScopeStatsLifecycle()
{
	std::cout << "[ScopeStats lifecycle] ";
	GameUIFrame f{};
	debugui::UiState state;
	state.panels.performance = true;
	Profiler &prof = GetProfiler();

	// Deterministic scope durations via a busy wait (real elapsed time).
	auto busyMs = [](float ms) {
		const auto start = std::chrono::steady_clock::now();
		while (std::chrono::duration<float, std::milli>(
				   std::chrono::steady_clock::now() - start)
				   .count() < ms)
		{
		}
	};
	// One captured frame: TestOuter (busy ~outerMs) containing TestInner.
	auto captureFrame = [&](float outerMs) {
		prof.setEnabled(true);
		prof.beginFrame();
		prof.push("TestOuter");
		busyMs(outerMs);
		prof.push("TestInner");
		prof.pop();
		prof.pop();
		prof.endFrame();
	};

	// 10 captured frames, each followed by a sampler call exactly as the
	// engine loop does (once per rendered frame), all inside the first
	// 10 Hz window: frame-resolution columns must fold ALL frames, while
	// the history keeps one sample per UI tick (issue #179 review round 2).
	double t = 400.0;
	for (int i = 0; i < 10; ++i)
	{
		captureFrame(1.f);
		debugui::updateDebugUiState(state, f, t);
		t += 0.001;
	}
	CHECK(state.scopeStatCount == 2, "both scopes enter the table");
	const debugui::ScopeStats *outer = nullptr;
	const debugui::ScopeStats *inner = nullptr;
	for (size_t i = 0; i < state.scopeStatCount; ++i)
	{
		if (std::string(state.scopeStats[i].name) == "TestOuter")
			outer = &state.scopeStats[i];
		if (std::string(state.scopeStats[i].name) == "TestInner")
			inner = &state.scopeStats[i];
	}
	CHECK(outer && inner, "both test scopes found");
	CHECK(outer->frames == 10, "frame-resolution stats fold every frame");
	CHECK(outer->history.count() == 1, "history stays at UI (10 Hz) resolution");
	CHECK(inner->depth == 1 && outer->depth == 0, "scope depth tracked");

	// Spike detection across frames: 1 ms, 15 ms, 1 ms between two history
	// samples must surface in peak even though the spike frame was not the
	// last aggregated one.
	captureFrame(1.f);
	debugui::updateDebugUiState(state, f, t);
	t += 0.001;
	captureFrame(15.f);
	debugui::updateDebugUiState(state, f, t);
	t += 0.001;
	captureFrame(1.f);
	debugui::updateDebugUiState(state, f, t);
	t += 0.001;
	CHECK(outer->frames == 13, "frames count every captured frame");
	CHECK(outer->peakMs >= 15.f, "mid-window spike captured by peak");

	// Sampling beyond the 10 Hz window finally pushes one history sample.
	// The engine calls the sampler once per captured frame (1:1), so this
	// tick also aggregates one new frame.
	captureFrame(1.f);
	debugui::updateDebugUiState(state, f, t + 0.2);
	t += 0.2;
	CHECK(outer->frames == 14, "frames advance with captured frames");
	CHECK(outer->history.count() == 2, "history sampled once per UI tick");

	// Throttled tick: histories pause, frame columns keep advancing.
	captureFrame(1.f);
	debugui::updateDebugUiState(state, f, t + 0.01);
	CHECK(outer->frames == 15, "frame stats update even when history is throttled");
	CHECK(outer->history.count() == 2, "history unchanged inside the 10 Hz window");

	// Capture off: the frozen last frame must not be re-folded forever, and
	// no stale history sample is pushed either.
	prof.setEnabled(false);
	debugui::updateDebugUiState(state, f, t + 0.4);
	CHECK(outer->frames == 15, "capture off: stale frame not re-aggregated");
	CHECK(outer->history.count() == 2, "capture off: no stale history sample");

	// Capture-epoch change (Clear history / world reload) resets the table.
	state.selectedScopeGraph = 3;
	prof.setEnabled(true);
	prof.clearHistory();
	debugui::updateDebugUiState(state, f, t + 0.6);
	CHECK(state.scopeStatCount == 0, "epoch change resets the scope table");
	CHECK(state.selectedScopeGraph == -1, "plot selection cleared on epoch change");
	std::cout << "PASS\n";
}
} // namespace

int main()
{
	try
	{
		testFormatBytes();
		testFormatCountAndMs();
		testMetricHistory();
		testHealthMonitor();
		testChunkSnapshotAndTrace();
		testUpdateDebugUiState();
		testScopeStatsLifecycle();
	}
	catch (const std::exception &e)
	{
		std::cerr << "EXCEPTION: " << e.what() << '\n';
		return 1;
	}
	if (g_fails)
	{
		std::cerr << g_fails << " failure(s)\n";
		return 1;
	}
	std::cout << "PASS: debug ui core (formatting, histories, health, chunk snapshot, trace)\n";
	return 0;
}
