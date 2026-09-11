// Engine-facing snapshot collection for the developer console (issue #179).
// Everything here is read-only with respect to the engine: chunk counters
// are atomic or main-thread-only state, telemetry uses the non-destructive
// sampleLive() path, and the only cross-thread writer touched is
// ChunkManager's opt-in event-trace enable flag.

#include <Engine/DebugUI/DebugUiEngine.hpp>

#include <Engine/Profiler.hpp>
#include <Engine/GpuProfile.hpp>
#include <Chunk/ChunkManager.hpp>
#include <Vulkan/MeshArena.hpp>
#include <Vulkan/StagingRing.hpp>
#include <Vulkan/VkGpuProfiler.hpp>

#include <algorithm>
#include <cstring>

namespace debugui
{

// --- Health aggregation (thresholds rooted in engine constants) ---

void DebugHealth::update(const UiState &state, float dt)
{
	const double now = state.memory.sampledAt;
	const auto &s = state.streaming;
	const auto &live = state.memory.live;

	// Chunk-pool back-pressure: the pool had to refuse an acquire recently.
	poolRejects.update((now - state.lastPoolRejectIncrease) < 2.0, dt);
	// Staging ring could not fit a copy within the last two seconds.
	stagingFailures.update((now - state.lastStagingFailureAt) < 2.0, dt);
	// Persistent job backlogs. 64 sections ~= a full chunk: a healthy
	// pipeline (360 meshes/s, 96 light caches/s default) drains that in a
	// fraction of the 3 s enter window, so only real starvation trips.
	meshBacklog.update(s.pendingMesh > 64, dt);
	lightBacklog.update(s.pendingLight > 64, dt);
	// Uploads drain every frame under normal budgets; a non-empty backlog
	// persisting 5 s means the copy path is staging- or budget-starved.
	uploadBacklog.update(s.uploadBacklog > 0, dt);
	// Retired GPU resources complete within frames-in-flight; 64 MiB held
	// for 3 s means the retire queue is wedged or churning.
	retiredBacklog.update(live.current[telemetry::RetiredBytes] > (64ull << 20), dt);
	// Sustained GPU mesh growth over the trailing ~5 s of history (>5% and
	// >8 MiB) catches arena/pool leaks without flagging normal fill.
	{
		const float nowLive = state.gpuLiveBytes.latest();
		const float thenLive = state.gpuLiveBytes.back(50); // 50 samples @ 10 Hz
		const float growth = nowLive - thenLive;
		const bool growing = thenLive > 0.f && growth > (8.f * 1024.f * 1024.f) &&
							 growth > 0.05f * thenLive;
		gpuMemoryGrowth.update(growing, dt);
	}
	// 1% low is itself a 240-frame windowed statistic: > 33 ms means the
	// recent past held sustained slow frames, not a one-frame spike.
	slowFrames.update(state.frame.onePercentLowMs > 33.f, dt);
}

// --- CPU scope table (bounded by kMaxScopeStats) ---

// Frame-resolution aggregation of the profiler's last-frame buffer: lastMs,
// avgMs, peakMs, frames and depth. Runs every frame while the Performance
// panel is open and capture is enabled; never touches the 10 Hz histories
// (issue #179 review round 2).
static void updateScopeFrameStats(UiState &state)
{
	Profiler &prof = GetProfiler();
	// Never aggregate a frozen last-frame buffer: with capture disabled the
	// previous frame's entries would otherwise be re-folded into the
	// averages indefinitely (issue #179 review).
	if (!prof.enabled())
		return;

	// Aggregate duplicate names within the frame (summed durations), then
	// fold into running averages. Keyed on the profiler's static name
	// pointers; a strcmp fallback keeps the table correct across reloads.
	const int count = prof.lastEntryCount();
	const ProfileEntry *entries = prof.lastEntries();

	constexpr uint32_t kDepthUnknown = UINT32_MAX;
	thread_local std::array<float, 128> frameTotals{};
	thread_local std::array<uint32_t, 128> frameDepth{};
	frameTotals.fill(0.f);
	frameDepth.fill(kDepthUnknown);

	for (int i = 0; i < count; ++i)
	{
		const ProfileEntry &e = entries[i];
		if (!e.name)
			continue;
		size_t slot = kMaxScopeStats;
		for (size_t k = 0; k < state.scopeStatCount; ++k)
		{
			if (state.scopeStats[k].name == e.name ||
				std::strcmp(state.scopeStats[k].name, e.name) == 0)
			{
				slot = k;
				break;
			}
		}
		if (slot == kMaxScopeStats)
		{
			if (state.scopeStatCount >= kMaxScopeStats)
				continue; // bounded: ignore unknown extra scopes
			slot = state.scopeStatCount++;
			state.scopeStats[slot] = ScopeStats{};
			state.scopeStats[slot].name = e.name;
			frameTotals[slot] = 0.f;
			frameDepth[slot] = e.depth;
		}
		else
		{
			frameDepth[slot] = std::min(frameDepth[slot], uint32_t(e.depth));
		}
		frameTotals[slot] += e.durationMs;
	}

	for (size_t k = 0; k < state.scopeStatCount; ++k)
	{
		ScopeStats &st = state.scopeStats[k];
		st.lastMs = frameTotals[k];
		if (frameDepth[k] != kDepthUnknown)
			st.depth = frameDepth[k]; // keep the last known depth otherwise
		if (st.lastMs > st.peakMs)
			st.peakMs = st.lastMs;
		++st.frames;
		st.avgMs += (st.lastMs - st.avgMs) / float(st.frames);
	}
}

// 10 Hz resolution: fold the current per-scope last values into the bounded
// histories. Runs only inside the throttled block, and only while capture is
// on — with capture off nothing new is measured, so nothing new is sampled.
static void sampleScopeHistories(UiState &state)
{
	if (!GetProfiler().enabled())
		return;
	for (size_t i = 0; i < state.scopeStatCount; ++i)
		state.scopeStats[i].history.push(state.scopeStats[i].lastMs);
}

// --- Per-domain heavy snapshots (10 Hz, sampled independently) ---

static void sampleStreamingDomain(UiState &state, const GameUIFrame &frame, double nowSeconds)
{
	auto &s = state.streaming;
	if (frame.chunks)
	{
		const ChunkManager &cm = *frame.chunks;
		s.loadedChunks = cm.chunkCount();
		s.pendingLoad = cm.pendingLoadCount();
		s.pendingGen = cm.pendingGenJobs();
		s.pendingMesh = cm.pendingMeshJobs();
		s.pendingLight = cm.pendingLightJobs();
		s.deferredReleases = cm.deferredReleaseCount();
		s.meshJobsDispatched = cm.meshJobsDispatched();
		s.lightJobsDispatched = cm.lightJobsDispatched();
		s.uploadBacklog = 0;
		for (const Chunk *chunk : cm.getActiveChunks())
			if (chunk && chunk->needsGPUUpload())
				++s.uploadBacklog;
	}
	s.drawCount = frame.drawCount;
	s.viewDistance = frame.render ? frame.render->maxRenderDistance : 0;
	s.nearRange = frame.render ? frame.render->minRenderDistance : 0;
	if (frame.pool)
	{
		const ChunkPool &pool = *frame.pool;
		s.poolCapacity = pool.capacity();
		s.poolFree = pool.freeCount();
		s.poolAcquired = pool.acquiredCount();
		s.poolRejects = pool.rejectCount();
		s.poolGrows = pool.growEvents();
		if (s.poolRejects > state.prevPoolRejects)
			state.lastPoolRejectIncrease = nowSeconds;
		state.prevPoolRejects = s.poolRejects;
	}

	state.pendingMesh.push(float(s.pendingMesh));
	state.pendingLoad.push(float(s.pendingLoad));
	state.pendingGen.push(float(s.pendingGen));
	state.pendingLight.push(float(s.pendingLight));
	state.uploadBacklog.push(float(s.uploadBacklog));
	state.activeChunks.push(float(s.loadedChunks));
}

static void sampleMemoryDomain(UiState &state, const GameUIFrame &frame, double nowSeconds)
{
	auto &m = state.memory;
	m.sampledAt = nowSeconds;
	m.live = telemetry::registry().sampleLive();
	m.telemetryEnabled = m.live.enabled;
	if (frame.worldRenderer)
	{
		const MeshArenas &arenas = frame.worldRenderer->arenas();
		m.arenaLiveBytes = arenas.opaqueVertex.liveBytes() + arenas.opaqueIndex.liveBytes() +
						   arenas.waterVertex.liveBytes() + arenas.waterIndex.liveBytes();
	}
	m.arenaPages = m.live.current[telemetry::ArenaPages];
	m.arenaFreeBytes = m.live.current[telemetry::ArenaFreeBytes];
	m.arenaHighWaterBytes = m.live.current[telemetry::ArenaHighWater];
	if (frame.staging)
	{
		// The sampler runs mid-frame (after beginFrame reset, before this
		// frame's copies): report the completed frame's staging traffic.
		m.stagingUsedBytes = frame.staging->lastFrameUsed();
		m.stagingCapacityBytes = frame.staging->sliceCapacity();
	}
	else
	{
		m.stagingUsedBytes = m.live.current[telemetry::StagingUsed];
	}

	// Event deltas since the previous sample ("recent rate" data).
	if (state.hasPrevEvents)
	{
		for (size_t i = 0; i < telemetry::EventCount; ++i)
		{
			const uint64_t now = m.live.events[i];
			const uint64_t prev = state.prevEvents[i];
			state.eventDelta[i] = now >= prev ? now - prev : now;
		}
	}
	state.hasPrevEvents = true;
	state.prevEvents = m.live.events;
	if (state.eventDelta[telemetry::StagingFailures] > 0)
		state.lastStagingFailureAt = nowSeconds;

	state.gpuLiveBytes.push(float(m.live.current[telemetry::GpuLiveBytes]));
	state.retiredBytes.push(float(m.live.current[telemetry::RetiredBytes]));
	state.stagingUsed.push(float(m.stagingUsedBytes));
	const uint64_t capacity = m.arenaLiveBytes + m.arenaFreeBytes;
	const float util = capacity ? float(m.arenaLiveBytes) / float(capacity) : 0.f;
	state.arenaUtilization.push(util);
}

// --- Per-frame state refresh ---

void updateDebugUiState(UiState &state, const GameUIFrame &frame, double nowSeconds)
{
	// Cheap frame-level snapshot: every frame, main-thread scalar reads.
	{
		// Keep the opt-in chunk lifecycle trace alive across world reloads:
		// the flag lives in UiState and is re-applied every frame, even with
		// the inspector closed, so a recreated ChunkManager inherits it
		// (issue #179 review). Idempotent one-bool store.
		if (frame.chunks)
			frame.chunks->setChunkEventTraceEnabled(state.eventTraceEnabled);

		Profiler &prof = GetProfiler();
		state.frame.fps = frame.fps;
		// CPU frame time comes from the profiler's full-frame scope, not the
		// paced simulation delta (GameUIFrame::frameMs): vsync pacing and
		// stall absorption make the two diverge (issue #179 review).
		state.frame.cpuFrameMs = prof.lastFrameMs();
		state.frame.simulationDtMs = frame.frameMs;
		state.frame.avgMs = prof.avgFrameMs();
		state.frame.onePercentLowMs = prof.onePercentLowMs();
		state.frame.vsync = frame.render ? frame.render->vsyncEnabled : false;
		state.frame.presentMode = frame.presentModeName ? frame.presentModeName : "";
		state.frame.gpuValid = false;
		if (frame.gpu)
		{
			const GpuFrameSample &gpu = frame.gpu->latest();
			if (gpu.serial && gpu.present[size_t(GpuPass::Frame)])
			{
				state.frame.gpuFrameMs = gpu.ms[size_t(GpuPass::Frame)];
				state.frame.gpuValid = true;
			}
		}

		// Player/physics diagnostics (issue #184): plain scalar copies from
		// the frame's read-only snapshot — cheap enough to refresh every
		// frame, but only paid while the console panel consumes them.
		if (state.panels.playerDiagnostics)
		{
			const playerui::PlayerSnapshot &p = frame.player;
			state.player.valid = true;
			state.player.flight = p.flight;
			state.player.grounded = p.grounded;
			state.player.swimming = p.swimming;
			state.player.submergedWater = p.submergedWater;
			state.player.submergedLava = p.submergedLava;
			state.player.waitingForTerrain = p.waitingForTerrain;
			state.player.speed = p.speed;
			state.player.position = p.position;
			state.player.yaw = p.yaw;
			state.player.pitch = p.pitch;
			state.player.physicsSteps = p.physicsSteps;
			state.player.queriedCells = p.queriedCells;
			state.player.queryIterations = p.queryIterations;
			state.player.droppedSteps = p.droppedSteps;
		}
	}

	// Profiler capture lifecycle precedes the throttle: the epoch check and
	// the frame-resolution scope aggregation must see every frame, while
	// only the histories are sampled at 10 Hz (issue #179 review round 2).
	{
		Profiler &prof = GetProfiler();
		const uint64_t epoch = prof.captureEpoch();
		if (epoch != state.lastProfilerEpoch)
		{
			resetScopeStats(state);
			state.lastProfilerEpoch = epoch;
		}
		if (state.panels.performance)
			updateScopeFrameStats(state);
	}

	const bool needStreaming = state.panels.overview || state.panels.streaming;
	const bool needMemory = state.panels.overview || state.panels.memory;
	const bool needPerformance = state.panels.performance;

	if (!(needStreaming || needMemory || needPerformance))
		return;

	// Heavy snapshots, throttled to 10 Hz (issue #179 performance rules) and
	// sampled per domain (issue #179 review round 2): Performance alone must
	// not pay for the streaming walk or the telemetry read. The chunk
	// inspector needs no sampler work — it extracts its own snapshot at draw
	// time.
	if (state.lastTelemetrySample >= 0.0 && nowSeconds - state.lastTelemetrySample < 0.1)
		return;

	const float dt = state.lastTelemetrySample < 0.0
						 ? 0.016f
						 : float(std::min(nowSeconds - state.lastTelemetrySample, 1.0));
	state.dtSinceLastSample = dt;
	state.lastTelemetrySample = nowSeconds;

	if (needPerformance)
		sampleScopeHistories(state);

	if (needStreaming)
		sampleStreamingDomain(state, frame, nowSeconds);

	if (needMemory)
		sampleMemoryDomain(state, frame, nowSeconds);

	// Frame-time histories are Overview consumers; the health monitors read
	// both domains, so they only run when both were freshly sampled.
	if (state.panels.overview)
	{
		state.cpuMs.push(GetProfiler().lastFrameMs());
		if (frame.gpu)
		{
			const GpuFrameSample &gpu = frame.gpu->latest();
			state.gpuMs.push(gpu.serial && gpu.present[size_t(GpuPass::Frame)]
								 ? gpu.ms[size_t(GpuPass::Frame)]
								 : 0.f);
		}
		else
		{
			state.gpuMs.push(0.f);
		}
		state.health.update(state, dt);
	}
}

// --- Chunk inspector snapshot ---

ChunkDebugSnapshot makeChunkDebugSnapshot(const ChunkManager &chunks,
										  const glm::ivec3 &coord,
										  const glm::vec3 &cameraPos)
{
	ChunkDebugSnapshot s;
	s.coord = coord;
	const glm::vec3 center(float(coord.x * CHUNK_SIZE + CHUNK_SIZE / 2), 0.f,
						   float(coord.z * CHUNK_SIZE + CHUNK_SIZE / 2));
	s.distance = glm::length(glm::vec2(cameraPos.x - center.x, cameraPos.z - center.z));

	const Chunk *chunk = chunks.getChunk(coord);
	s.loaded = chunk != nullptr;
	if (!chunk)
		return s;

	s.state = int(chunk->getState());
	s.meshGeneration = chunk->meshGeneration();
	s.meshRevision = chunk->meshRevision();
	s.lodMesh = chunk->isLODMesh();
	s.inTransit = chunk->isInTransit();
	s.uploadPending = chunk->needsGPUUpload();
	s.hasPendingMeshResult = chunk->hasPendingMeshResult();
	s.unuploadedFullMesh = chunk->hasUnuploadedFullMesh();
	s.dirtySections = chunk->dirtySections();
	s.lightCacheWanted = chunk->localLightCacheWanted();
	s.lightCachePresent = chunk->hasLightStorage();
	s.visible = chunk->isVisible();
	s.opaqueDrawCount = chunk->getCachedOpaqueDrawCount();
	s.waterDrawCount = chunk->getCachedWaterDrawCount();
	s.opaqueIndexCount = chunk->getOpaqueIndexCount();
	s.waterIndexCount = chunk->getWaterIndexCount();
	s.liveGpuSections = chunk->liveGpuSections();
	s.activeIndex = uint32_t(chunk->getActiveIndex());
	s.valid = true;
	return s;
}

} // namespace debugui
