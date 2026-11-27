// Developer-console core logic (issue #179): pure formatting, bounded ring
// histories, sustained-condition health monitors, chunk debug snapshot
// extraction and the bounded chunk event trace — all without ImGui or a
// window. The telemetry live-read semantics are covered in
// test_workload_telemetry.cpp.
#include <Engine/DebugUI/DebugUiCore.hpp>
#include <Engine/DebugUI/DebugUiEngine.hpp>
#include <Renderer/RenderWorldBridge.hpp>
#include <Chunk/ChunkPool.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Camera/Camera.hpp>
#include <Engine/Profiler.hpp>
#include <Vulkan/VkGpuProfiler.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "TestMain.hpp"

// Friend probe (same pattern as test_chunk_lifecycle): test-only access to
// the main-thread trace writer and chunk registration without streaming.
struct RenderBridgeProbe
{
	static void registerChunk(RenderWorldBridge &m, const glm::ivec3 &pos, Chunk *c)
	{
		m.m_chunks[pos] = c;
		m.m_activeChunks.push_back(c);
		c->setActiveIndex(m.m_activeChunks.size() - 1);
	}
	static void recordEvent(RenderWorldBridge &m, Chunk *c, const char *kind, uint32_t sections = 0)
	{
		m.recordChunkEvent(c, kind, sections);
	}
};

// Publish known frame/scope times through the normal profiler finalizer.
struct ProfilerStateProbe
{
	static void publish(Profiler &profiler, float ms)
	{
		profiler.m_entryCount = 1;
		profiler.m_entries[0] = {"Fixture", 0.f, ms * 0.5f};
		profiler.finalizeFrame(ms);
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
	CHECK(!event.update(false, 1.f), "zero enter still requires a real condition");
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
	VoxelWorld managerWorld(&generator, nullptr); RenderWorldBridge manager(&managerWorld, nullptr, &pool);

	const glm::ivec3 coord{10, 0, 10};
	Chunk *chunk = pool.acquire(glm::vec3(float(coord.x * CHUNK_SIZE), 0.f,
										  float(coord.z * CHUNK_SIZE)));
	CHECK(chunk != nullptr, "pool acquire provides a chunk");
	if (!chunk)
	{
		std::cout << "SKIP (pool exhausted)\n";
		return;
	}
	RenderBridgeProbe::registerChunk(manager, coord, chunk);

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
	RenderBridgeProbe::recordEvent(manager, chunk, "load");
	RenderBridgeProbe::recordEvent(manager, chunk, "genDone");
	RenderBridgeProbe::recordEvent(manager, chunk, "meshDone", 0x5);
	std::vector<ChunkDebugEvent> events = manager.chunkDebugEvents();
	CHECK(events.size() == 3, "events recorded while enabled");
	CHECK(events[0].kind && std::string(events[0].kind) == "load" &&
			  events[2].kind && std::string(events[2].kind) == "meshDone",
		  "events are chronological");
	CHECK(events[0].chunk.x == coord.x && events[0].chunk.z == coord.z,
		  "event carries the chunk coordinate");
	CHECK(events[2].sections == 0x5, "event carries section context");

	// Bounded: overflow evicts the oldest instead of growing.
	for (int i = 0; i < int(RenderWorldBridge::kChunkEventRingSize) + 10; ++i)
		RenderBridgeProbe::recordEvent(manager, chunk, "meshQueued", unsigned(i));
	events = manager.chunkDebugEvents();
	CHECK(events.size() == RenderWorldBridge::kChunkEventRingSize, "ring stays bounded");
	CHECK(std::string(events.front().kind) == "meshQueued" && events.front().sections == 10,
		  "oldest events evicted in order");
	CHECK(std::string(events.back().kind) == "meshQueued" &&
			  events.back().sections == unsigned(RenderWorldBridge::kChunkEventRingSize + 10 - 1),
		  "newest event retained");
	std::cout << "PASS\n";
}

void testUpdateDebugUiState()
{
	std::cout << "[updateDebugUiState] ";
	TerrainGenerator generator(42);
	ChunkPool pool(8);
	VoxelWorld managerWorld(&generator, nullptr); RenderWorldBridge manager(&managerWorld, nullptr, &pool);
	Camera camera;
	camera.setPosition(glm::vec3(160.f, 90.f, 200.f));

	DeveloperUiFrame f{};
	f.hasWorld = true;
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
	// Domain separation (issue #179 review round 2): with only Streaming
	// open, the memory domain (telemetry read + timestamps) stays untouched.
	CHECK(state.memory.sampledAt == 0.0, "memory domain not sampled while only Streaming is open");
	CHECK(std::abs(state.lastTelemetrySample - t0) < 1e-9, "sample timestamp recorded");

	// Opening Overview pulls in the memory domain as well (beyond the 10 Hz
	// window so the throttle allows a fresh sample).
	state.panels.overview = true;
	debugui::updateDebugUiState(state, f, t0 + 0.2);
	CHECK(std::abs(state.memory.sampledAt - (t0 + 0.2)) < 1e-9,
		  "Overview samples the memory domain");
	CHECK(state.activeChunks.count() == 2, "overview sample pushed");

	// Heavy sampling is throttled to 10 Hz: an immediate second call must
	// not re-sample.
	debugui::updateDebugUiState(state, f, t0 + 0.21);
	CHECK(state.activeChunks.count() == 2, "throttled call does not re-sample");

	// With every consumer closed the snapshot is not refreshed at all.
	state.panels.overview = false;
	state.panels.streaming = false;
	debugui::updateDebugUiState(state, f, t0 + 10.0);
	CHECK(state.activeChunks.count() == 2, "closed panels stop sampling");

	// A visibility gap starts a new history and a fresh counter baseline.
	state.cpuFrameTimes.push(100.f);
	state.health.meshBacklog.update(true, 4.f);
	state.eventDelta[telemetry::StagingFailures] = 12;
	state.interruptSampling();
	state.panels.overview = true;
	const auto profilerSamples = prof.historyCount();
	manager.setChunkEventTraceEnabled(true);
	debugui::updateDebugUiState(state, f, t0 + 60.0);
	CHECK(state.cpuFrameTimes.count() == 0 && state.activeChunks.count() == 1,
		  "resume drops old histories and skips the last hidden CPU frame");
	CHECK(state.frame.onePercentLowMs == 0.f && !state.health.meshBacklog.active() &&
		  !state.health.stagingFailures.active() && state.eventDelta[telemetry::StagingFailures] == 0,
		  "resume never interprets the sampling gap as sustained pressure");
	CHECK(prof.historyCount() == profilerSamples && manager.chunkEventTraceEnabled(),
		  "debug statistics leave global capture and bridge configuration alone");

	// A bridge attaches to an already-populated authoritative world. Its
	// initial binds must be traced before the constructor returns.
	VoxelWorld rebootstrapWorld(&generator, nullptr);
	rebootstrapWorld.generateInitialAreaVoxels({0, 90, 0}, 0);
	RenderWorldBridge rebootstrap(&rebootstrapWorld, nullptr, &pool, 1, true);
	CHECK(rebootstrap.chunkEventTraceEnabled() && !rebootstrap.chunkDebugEvents().empty(),
		  "the trace is configured before resident chunks are bound");

	std::cout << "PASS\n";
}
void testScopeStatsLifecycle()
{
	std::cout << "[ScopeStats lifecycle] ";
	DeveloperUiFrame f{};
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

void testCpuCaptureLifecycle()
{
	std::cout << "[CPU capture freshness] ";
	auto &prof = GetProfiler();
	prof.setEnabled(true);
	prof.clearHistory();
	debugui::UiState state;
	state.panels.overview = state.panels.performance = true;
	DeveloperUiFrame frame{};
	const auto draw = [&](double t) { debugui::updateDebugUiState(state, frame, t); };
	const auto capture = [&](double t, float ms) {
		ProfilerStateProbe::publish(prof, ms);
		draw(t);
	};
	capture(700.0, 45.f);
	CHECK(state.cpuFrameTimes.count() == 1 && state.cpuMs.count() == 1 &&
		  state.scopeStats[0].frames == 1 && state.scopeStats[0].history.count() == 1,
		  "a completed frame is sampled into the CPU views and 10 Hz histories");
	for (int i = 1; i <= 4; ++i)
		draw(700.0 + i * 0.25);
	CHECK(state.cpuFrameTimes.count() == 1 && state.cpuMs.count() == 1 &&
		  state.scopeStats[0].frames == 1 && state.scopeStats[0].history.count() == 1,
		  "repeated reads neither fold nor plot the same completed CPU frame");
	CHECK(!state.health.slowFrames.active() && state.health.slowFrames.conditionSeconds() == 0.f,
		  "a repeated slow sample does not create a sustained alert");

	capture(701.01, 50.f); // inside the 10 Hz window, so sampling is pending
	CHECK(state.cpuFrameTimes.count() == 2 && state.frame.avgMs == 45.f,
		  "CPU summaries remain throttled when a new frame arrives");
	const auto serial = prof.completedFrameSerial();
	prof.beginFrame();
	prof.setEnabled(false);
	prof.endFrame();
	CHECK(prof.completedFrameSerial() == serial, "pausing mid-frame discards the partial capture");
	for (int i = 1; i <= 6; ++i)
		draw(701.0 + i * 0.5);
	CHECK(!state.frame.cpuCaptureEnabled && state.cpuFrameTimes.count() == 2 && state.cpuMs.count() == 1 &&
		  state.frame.avgMs == 45.f && state.scopeStats[0].frames == 2 && state.scopeStats[0].history.count() == 1,
		  "Capture off freezes all CPU statistics, including previously pending samples");
	CHECK(!state.health.slowFrames.active(), "Capture off cannot raise a slow-frame alert");

	prof.setEnabled(true);
	draw(704.25);
	CHECK(state.cpuFrameTimes.count() == 2 && state.scopeStats[0].frames == 2,
		  "reenabling does not take a second copy of the frozen frame");
	capture(704.5, 55.f);
	CHECK(state.cpuFrameTimes.count() == 3 && state.cpuMs.count() == 2 &&
		  state.scopeStats[0].frames == 3 && state.scopeStats[0].history.count() == 2,
		  "the next completed frame restarts all enabled sampling");

	state.health.slowFrames.update(true, 3.f);
	prof.clearHistory();
	debugui::syncDebugCpuCapture(state, prof); // the Clear history control uses this immediately
	CHECK(state.cpuFrameTimes.count() == 0 && state.cpuMs.count() == 0 && state.scopeStatCount == 0 &&
		  state.frame.avgMs == 0.f && state.frame.onePercentLowMs == 0.f && state.frame.cpuFrameMs == 0.f &&
		  !state.health.slowFrames.active(), "Clear history immediately resets every local CPU metric and baseline");
	capture(705.0, 10.f);
	CHECK(state.cpuFrameTimes.count() == 1 && state.frame.avgMs == 10.f,
		  "the next epoch starts with one new sample");
	prof.clearHistory(); // external capture-epoch change, as in world reload
	capture(705.25, 20.f);
	CHECK(state.cpuFrameTimes.count() == 1 && state.scopeStats[0].frames == 1 &&
		  state.frame.avgMs == 20.f && state.frame.onePercentLowMs == 20.f,
		  "an external epoch reset does not mix data even when a fresh frame is already published");

	state.interruptSampling();
	ProfilerStateProbe::publish(prof, 60.f);
	draw(706.0);
	CHECK(prof.historyCount() == 2 && state.cpuFrameTimes.count() == 0 && state.scopeStatCount == 0,
		  "reopening skips the prior hidden CPU frame without resetting global capture");
	capture(706.25, 15.f);
	CHECK(prof.historyCount() == 3 && state.cpuFrameTimes.count() == 1,
		  "local sampling starts again as global capture continues");
	prof.clearHistory();
	std::cout << "PASS\n";
}

void testCpuMetricWindows()
{
	std::cout << "[Shared CPU metric windows] ";
	auto &prof = GetProfiler();
	prof.clearHistory();
	debugui::UiState state;
	state.panels.overview = true;
	DeveloperUiFrame frame{};
	double now = 800.0;
	const auto capture = [&](float ms) {
		ProfilerStateProbe::publish(prof, ms);
		debugui::updateDebugUiState(state, frame, now);
		now += 0.125;
		CHECK(state.frame.avgMs == prof.avgFrameMs() && state.frame.onePercentLowMs == prof.onePercentLowMs(),
			  "the same input sequence has identical metrics in both consumers");
	};
	debugui::updateDebugUiState(state, frame, now);
	CHECK(state.frame.avgMs == 0.f && state.frame.onePercentLowMs == 0.f &&
		  prof.avgFrameMs() == 0.f && prof.onePercentLowMs() == 0.f, "empty histories produce zeros");
	now += 0.125;
	capture(10.f);
	capture(15.f);
	capture(20.f);
	CHECK(state.frame.avgMs == 15.f && state.frame.onePercentLowMs == 20.f,
		  "a partial window uses all valid samples and the nearest-rank p99");
	for (int i = 0; i < 60; ++i)
		capture(80.f);
	for (int i = 0; i < 180; ++i)
		capture(16.f);
	for (int i = 0; i < 60; ++i)
		capture(25.f);
	CHECK(state.cpuFrameTimes.count() == debugui::kMetricHistorySize && prof.historyCount() == Profiler::kHistorySize &&
		  state.frame.avgMs == 25.f && state.frame.onePercentLowMs == 25.f,
		  "wrapped rings average the newest 60 and select p99 from the newest 240");
	for (int i = 0; i < 80; ++i)
		capture(25.f); // let the prior warning clear through its normal hysteresis
	capture(200.f);
	CHECK(state.frame.onePercentLowMs == 25.f && !state.health.slowFrames.active(),
		  "an isolated spike is outside p99 for a full window");
	for (int i = 0; i < 20; ++i)
		capture(40.f);
	CHECK(state.frame.onePercentLowMs == 40.f && state.health.slowFrames.active(),
		  "a sustained p99 above 33 ms raises the slow-frame alert");
	prof.clearHistory();
	std::cout << "PASS\n";
}

void testGpuCaptureObservations()
{
	std::cout << "[GPU capture observations] ";
	debugui::UiState state;
	VkGpuProfiler gpu;
	constexpr auto pass = static_cast<size_t>(GpuPass::Frame);
	GpuFrameSample sample;
	sample.serial = 10;
	sample.present[pass] = true;
	sample.ms[pass] = 12.5f;
	const auto observe = [&](bool record = true) {
		debugui::observeGpuCapture(state, gpu.captureGeneration(), &sample, record);
	};
	observe(false);
	CHECK(state.frame.gpuValid && state.gpuMs.count() == 0,
		  "reading a global result for a label does not sample the Overview history");
	observe();
	observe();
	CHECK(state.gpuMs.count() == 1 && state.gpuMs.latest() == 12.5f,
		  "the same serial is observed at most once");
	state.gpuMs.clear(); // the visible-interval reset preserves the consumed serial
	observe();
	CHECK(state.gpuMs.count() == 0, "an already observed serial is not replayed after visibility changes");
	++sample.serial;
	sample.ms[pass] = 16.f;
	observe();
	CHECK(state.gpuMs.count() == 1 && state.gpuMs.latest() == 16.f,
		  "a new delayed global result can be observed in the current interval");

	debugui::syncDebugGpuCapture(state, &gpu, 1); // CPU epoch changed ahead of the GPU owner
	CHECK(!state.frame.gpuValid && state.gpuMs.count() == 0,
		  "a mismatched capture epoch invalidates the previous GPU reading and observations");
	gpu.syncCapture(1);
	++sample.serial;
	observe();
	gpu.setEnabled(false);
	debugui::syncDebugGpuCapture(state, &gpu, 1);
	CHECK(!state.frame.gpuValid && state.gpuMs.count() == 0 && state.frame.gpuFrameMs == 0.f,
		  "disabling GPU capture clears local observations and the latest value");
	gpu.setEnabled(true);
	debugui::syncDebugGpuCapture(state, &gpu, 1);
	CHECK(!state.frame.gpuValid && state.gpuMs.count() == 0,
		  "reenabling waits for a new completed GPU result");
	++sample.serial;
	observe();
	gpu.syncCapture(2);
	debugui::syncDebugGpuCapture(state, &gpu, 2);
	CHECK(!state.frame.gpuValid && state.gpuMs.count() == 0,
		  "a new GPU generation clears local results even when timestamps remain enabled");
	std::cout << "PASS\n";
}
} // namespace

void testPlayerDiagnosticsSnapshot()
{
	std::cout << "[PlayerDiagnostics snapshot] ";
	debugui::UiState state;
	DeveloperUiFrame f{};

	// Closed panel: the snapshot stays untouched (no hidden per-frame cost).
	debugui::updateDebugUiState(state, f, 500.0);
	CHECK(!state.player.valid, "player snapshot not sampled while the panel is closed");

	// Open panel: the solver counters and motion flags are copied from the
	// frame's read-only player snapshot (issue #184).
	state.panels.playerDiagnostics = true;
	f.player.flight = true;
	f.player.grounded = false;
	f.player.swimming = true;
	f.player.waitingForTerrain = false;
	f.player.speed = 42.5f;
	f.player.position = glm::vec3(1.f, 2.f, 3.f);
	f.player.yaw = 91.f;
	f.player.pitch = -17.f;
	f.player.physicsSteps = 7;
	f.player.queriedCells = 1234;
	f.player.queryIterations = 99;
	f.player.droppedSteps = 3;
	f.player.submergedWater = true;
	// Camera view state is snapshot-only (issue #184 review): the panel must
	// be able to render it without a direct Camera path.
	f.player.cameraViewMode = playerui::CameraViewMode::Isometric;
	f.player.cameraMovementSpeed = 125.f;
	f.player.mouseSensitivity = 0.123f;
	f.player.isometricZoom = 64.f;
	debugui::updateDebugUiState(state, f, 500.01);
	CHECK(state.player.valid, "player snapshot sampled once the panel opens");
	CHECK(state.player.flight && state.player.swimming && !state.player.grounded &&
			  !state.player.waitingForTerrain,
		  "motion flags copied");
	CHECK(state.player.speed == 42.5f, "speed copied");
	CHECK(state.player.position.x == 1.f && state.player.position.y == 2.f &&
			  state.player.position.z == 3.f,
		  "position copied");
	CHECK(state.player.physicsSteps == 7 && state.player.queriedCells == 1234 &&
			  state.player.queryIterations == 99 && state.player.droppedSteps == 3,
		  "solver counters copied");
	CHECK(state.player.submergedWater && !state.player.submergedLava, "immersion flags copied");
	CHECK(state.player.cameraViewMode == playerui::CameraViewMode::Isometric,
		  "camera view mode copied");
	CHECK(state.player.cameraMovementSpeed == 125.f && state.player.mouseSensitivity == 0.123f &&
			  state.player.isometricZoom == 64.f,
		  "camera tuning values copied");

	// Values refresh on later frames (not a one-shot latch).
	f.player.queriedCells = 2000;
	debugui::updateDebugUiState(state, f, 500.02);
	CHECK(state.player.queriedCells == 2000, "player snapshot refreshes per frame");

	std::cout << "PASS\n";
}

void testStreamingHealthSubset()
{
	std::cout << "[DebugHealth streaming subset] ";
	debugui::UiState state;

	// Sustained mesh backlog (threshold: pendingMesh > 64 for 3 s) raises
	// through the streaming-only subset so the primary Streaming panel can
	// warn without Overview open (issue #186).
	state.streaming.pendingMesh = 128;
	for (int i = 0; i < 3; ++i)
		state.health.updateStreaming(state, 1.0f);
	CHECK(state.health.meshBacklog.active(), "sustained mesh backlog raises via the streaming subset");

	// A one-frame spike must stay neutral (issue #186 §6): ordinary bursts
	// while moving fast are not testmain::fails.
	debugui::UiState spike;
	spike.streaming.pendingMesh = 128;
	spike.health.updateStreaming(spike, 1.0f);
	CHECK(!spike.health.meshBacklog.active(), "one-frame queue spike stays neutral");

	// Clearing follows the exit hysteresis (6 s), not the first quiet frame.
	state.streaming.pendingMesh = 0;
	state.health.updateStreaming(state, 1.0f);
	CHECK(state.health.meshBacklog.active(), "backlog warning survives the first quiet sample");
	for (int i = 0; i < 6; ++i)
		state.health.updateStreaming(state, 1.0f);
	CHECK(!state.health.meshBacklog.active(), "sustained quiet clears the backlog warning");

	// Memory-derived monitors are never advanced by the streaming subset:
	// their timestamps/histories come from the memory sample and would move
	// on stale data when only Streaming is open.
	state.streaming.pendingMesh = 128;
	state.streaming.uploadBacklog = 32;
	for (int i = 0; i < 10; ++i)
		state.health.updateStreaming(state, 1.0f);
	CHECK(state.health.meshBacklog.active(), "streaming monitors keep advancing");
	CHECK(!state.health.stagingFailures.active(),
		  "staging testmain::fails stay untouched by the streaming subset");
	CHECK(!state.health.gpuMemoryGrowth.active() && !state.health.slowFrames.active() &&
			  !state.health.retiredBacklog.active(),
		  "memory/frame monitors stay untouched by the streaming subset");

	// Overview + Streaming open together (issue #191 review): the monitors
	// must advance exactly once per sample — the sampler's overview branch
	// is exclusive with the streaming subset, so a double advance would show
	// ~2x the nominal first-sample dt (16 ms).
	debugui::UiState both;
	both.panels.overview = true;
	both.panels.streaming = true;
	both.streaming.pendingMesh = 128;
	DeveloperUiFrame frame{};
	debugui::updateDebugUiState(both, frame, 1000.0);
	CHECK(both.health.meshBacklog.conditionSeconds() > 0.f &&
			  both.health.meshBacklog.conditionSeconds() <= 0.02f,
		  "monitors advance exactly once per sample with Overview and Streaming both open");

	std::cout << "PASS\n";
}

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
		testCpuCaptureLifecycle();
		testCpuMetricWindows();
		testGpuCaptureObservations();
		testPlayerDiagnosticsSnapshot();
		testStreamingHealthSubset();
	}
	catch (const std::exception &e)
	{
		std::cerr << "EXCEPTION: " << e.what() << '\n';
		return 1;
	}
	if (testmain::fails)
	{
		std::cerr << testmain::fails << " failure(s)\n";
		return 1;
	}
	std::cout << "PASS: debug ui core (formatting, histories, health, chunk snapshot, trace)\n";
	return 0;
}
