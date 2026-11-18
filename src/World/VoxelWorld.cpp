// VoxelWorld — CPU-owned authoritative world (issue #206, Epic #204 Phase B).
// Ported from the authoritative side of ChunkManager.cpp: streaming
// residency, async terrain publication, the deferred/coalesced logical-edit
// subsystem with border mirrors, and the issue #180 persistence lifecycle.
// Render scheduling (mesh/light dispatch, GPU upload, visibility, commit
// groups, light invalidation) stays in the RenderWorldBridge (issue #207),
// which consumes the world event stream.
#include "VoxelWorld.hpp"

#include <World/WorldSave.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

VoxelWorld::VoxelWorld(TerrainGenerator *terrainGenerator, ThreadPool *threadPool)
	: m_terrainGenerator(terrainGenerator), m_threadPool(threadPool)
{
}

VoxelWorld::~VoxelWorld()
{
	// Persistent-world durability comes FIRST (issue #180 review round 3):
	// while persistence is open, the close path must still be able to
	// publish finished gen jobs, apply deferred edits and capture edits
	// BEFORE any teardown discards state.
	if (m_persistence && !forceCloseWorldForShutdown())
	{
		// Destruction cannot be undone, but a partial save must never be
		// reported as success (issue #180 review round 4).
		std::cerr << "[world] final shutdown did not persist all accepted edits" << std::endl;
	}

	// Best-effort drain of async generation so no worker dereferences a
	// record during teardown.
	while (m_pendingGenJobsCount.load() > 0)
		std::this_thread::yield();
	{
		std::lock_guard<std::mutex> lock(m_completedJobsMutex);
		m_completedGenerationChunks.clear();
	}

	std::lock_guard<std::shared_mutex> lock(m_mutex);
	m_chunks.clear();
	m_activeChunks.clear();
	m_pendingEdits.clear();
	m_events.clear();
	// Records return their backing to the pools on destruction; the pools
	// are declared before m_recordStorage so they outlive every record.
	m_recordFreeList.clear();
	m_recordStorage.clear();
}

glm::ivec3 VoxelWorld::worldToChunkCoord(const glm::vec3 &worldPos)
{
	return ::worldToChunkCoord(worldPos);
}

// ---------------------------------------------------------------------------
// Record pool (pointer-stable, same back-pressure contract as ChunkPool)
// ---------------------------------------------------------------------------

void VoxelWorld::growRecords(size_t addCount)
{
	if (addCount == 0)
		return;
	const size_t cur = m_recordStorage.size();
	if (cur >= kMaxChunkPoolCapacity)
		return;
	const size_t canAdd = std::min(addCount, kMaxChunkPoolCapacity - cur);
	m_recordStorage.reserve(cur + canAdd);
	m_recordFreeList.reserve(m_recordFreeList.size() + canAdd);
	for (size_t i = 0; i < canAdd; ++i)
	{
		auto record = std::make_unique<WorldChunk>(glm::vec3(0.0f), &m_voxelPool, &m_borderPool);
		WorldChunk *ptr = record.get();
		m_recordStorage.push_back(std::move(record));
		m_recordFreeList.push_back(ptr);
	}
}

bool VoxelWorld::ensureRecordCapacity(size_t minCapacity, size_t maxGrowPerCall)
{
	minCapacity = std::min(std::max(minCapacity, kMinChunkPoolCapacity), kMaxChunkPoolCapacity);
	if (m_recordStorage.size() >= minCapacity)
		return false;
	const size_t add = chunkPoolGrowStep(m_recordStorage.size(), minCapacity, maxGrowPerCall);
	const size_t before = m_recordStorage.size();
	growRecords(add);
	return m_recordStorage.size() > before;
}

WorldChunk *VoxelWorld::acquireRecord(const glm::vec3 &origin)
{
	if (m_recordFreeList.empty())
		return nullptr;
	WorldChunk *record = m_recordFreeList.back();
	m_recordFreeList.pop_back();
	record->reset(origin, WorldChunk::ResetMode::ForGeneration);
	return record;
}

void VoxelWorld::releaseRecord(WorldChunk *record)
{
	if (!record)
		return;
	record->reset(glm::vec3(0.0f), WorldChunk::ResetMode::Full);
	m_recordFreeList.push_back(record);
}

size_t VoxelWorld::recordCapacity() const
{
	return m_recordStorage.size();
}

size_t VoxelWorld::recordFreeCount() const
{
	const size_t cap = m_recordStorage.size();
	const size_t free = m_recordFreeList.size();
	return free <= cap ? free : 0;
}

void VoxelWorld::registerExternalChunk(const glm::ivec3 &pos, WorldChunk *record)
{
	assert(record != nullptr);
	std::lock_guard<std::shared_mutex> lock(m_mutex);
	if (m_chunks.find(pos) != m_chunks.end())
		return;
	m_chunks[pos] = record;
	m_activeChunks.push_back(record);
	m_externalRecords.insert(record);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void VoxelWorld::emitEvent(const WorldEvent &event)
{
	// Bounded stream: the render adapter drains every frame, so functional
	// events are never dropped at frame cadence. A consumer that falls
	// behind (e.g. a headless run that never drains) drops the OLDEST events
	// at the cap — O(1) deque pop, counted in m_droppedEvents so the loss is
	// observable rather than silent (issue #206 review).
	constexpr size_t kMaxBufferedEvents = 16384;
	if (m_events.size() >= kMaxBufferedEvents)
	{
		m_events.pop_front();
		++m_droppedEvents;
	}
	m_events.push_back(event);
}

std::vector<WorldEvent> VoxelWorld::takeEvents()
{
	std::vector<WorldEvent> out;
	takeEvents(out);
	return out;
}

void VoxelWorld::takeEvents(std::vector<WorldEvent> &out)
{
	// Buffer-reuse drain (issue #219): the render adapter drains several times
	// per frame, so appending into a caller-owned vector lets it keep one
	// persistent buffer and avoid a fresh heap allocation on every drain. The
	// events and their order are identical to the return-by-value overload;
	// the stream is emptied under the same exclusive lock.
	std::lock_guard<std::shared_mutex> lock(m_mutex);
	out.insert(out.end(), m_events.begin(), m_events.end());
	m_events.clear();
}

// ---------------------------------------------------------------------------
// Streaming (issue #108 contract, ported verbatim with plain-value inputs)
// ---------------------------------------------------------------------------

StreamingUpdateKind VoxelWorld::updateStreaming(const glm::vec3 &center, const glm::vec2 &forwardXZ,
												const WorldStreamSettings &settings)
{
	// Grow the record pool toward the view-distance estimate (incremental,
	// same per-call cap as the render ChunkPool). The world owns its own
	// residency capacity so a headless consumer needs no external growth.
	const size_t poolTarget = estimateChunkPoolCapacity(settings.maxRenderDistance);
	if (m_recordStorage.size() < poolTarget)
		ensureRecordCapacity(poolTarget, kChunkPoolMaxGrowPerCall);

	const glm::ivec3 camChunk = worldToChunkCoord(center);

	// Unload triggers mirror the load-side reconciliation triggers minus
	// intra-chunk anchor moves (issue #108 review: one trigger definition).
	const bool unloadRelevant = !m_streamState.initialized ||
								camChunk != m_streamState.lastCamChunk ||
								settings.maxRenderDistance != m_streamState.lastMaxRenderDistance ||
								streamFrontBiasChanged(settings.streamFrontBias, m_streamState.lastStreamFrontBias);

	++m_streamFramesSinceUnloadCheck;
	if (unloadRelevant || m_streamFramesSinceUnloadCheck >= kUnloadCheckIntervalFrames)
	{
		queueUnloadOutOfRange(center, settings);
		++m_streamStats.unloadScans;
		m_streamFramesSinceUnloadCheck = 0;
	}

	return loadChunksAroundPlayer(camChunk, center, forwardXZ, settings);
}

void VoxelWorld::processChunkLoading(int budget)
{
	if (budget <= 0)
		return;

	// Back-pressure: never pull more candidates than free record slots.
	const size_t freeSlots = recordFreeCount();
	if (freeSlots == 0)
		return;
	budget = std::min(budget, static_cast<int>(freeSlots));

	std::vector<glm::ivec3> toLoad;
	{
		std::lock_guard<std::shared_mutex> lock(m_mutex);
		if (m_queueNeedsSort)
		{
			if (m_loadQueueHead > 0)
			{
				m_loadQueue.erase(m_loadQueue.begin(), m_loadQueue.begin() + m_loadQueueHead);
				m_loadQueueHead = 0;
			}
			sortLoadCandidatesNearestFirst(m_loadQueue);
			++m_streamStats.queueSorts;
			m_queueNeedsSort = false;
		}

		const size_t available = (m_loadQueue.size() >= m_loadQueueHead) ? (m_loadQueue.size() - m_loadQueueHead) : 0;
		const int n = std::min(budget, static_cast<int>(available));
		toLoad.reserve(static_cast<size_t>(n));
		for (int i = 0; i < n; ++i)
		{
			const glm::ivec3 pos = m_loadQueue[m_loadQueueHead + static_cast<size_t>(i)].pos;
			m_enqueuedLoads.erase(pos);
			toLoad.push_back(pos);
		}
		m_loadQueueHead += static_cast<size_t>(n);
		if (m_loadQueueHead == m_loadQueue.size())
		{
			m_loadQueue.clear();
			m_loadQueueHead = 0;
		}
	}
	if (toLoad.empty())
		return;

	std::vector<std::pair<glm::ivec3, WorldChunk *>> acquired;
	std::vector<glm::ivec3> rejected;
	acquired.reserve(toLoad.size());
	for (const auto &chunkPos : toLoad)
	{
		WorldChunk *record = acquireRecord(glm::vec3(
			static_cast<float>(chunkPos.x * CHUNK_SIZE), 0.0f,
			static_cast<float>(chunkPos.z * CHUNK_SIZE)));
		if (record)
		{
			// Persistent-edit tracking (issue #180): arm while a world is
			// open so every in-chunk edit is recorded for the save pipeline.
			record->setTrackPersistentEdits(isWorldOpen());
			acquired.emplace_back(chunkPos, record);
		}
		else
			rejected.push_back(chunkPos); // free list raced empty — re-queue below
	}

	std::lock_guard<std::shared_mutex> lock(m_mutex);
	for (const auto &pair : acquired)
	{
		if (m_chunks.find(pair.first) == m_chunks.end())
		{
			m_chunks[pair.first] = pair.second;
			m_activeChunks.push_back(pair.second);
			WorldEvent e;
			e.kind = WorldEventKind::Loaded;
			e.coord = pair.first;
			e.generation = pair.second->generation();
			e.revision = pair.second->revision();
			emitEvent(e);
		}
		else
		{
			releaseRecord(pair.second);
		}
	}

	// Put rejected loads back (nearest-first will re-sort next tick).
	for (const glm::ivec3 &pos : rejected)
	{
		if (m_chunks.find(pos) != m_chunks.end())
			continue;
		if (m_enqueuedLoads.count(pos) != 0)
			continue;
		m_loadQueue.push_back({pos, 0.f});
		m_enqueuedLoads.insert(pos);
		m_queueNeedsSort = true;
	}
}

void VoxelWorld::queueUnloadOutOfRange(const glm::vec3 &center, const WorldStreamSettings &settings)
{
	const float unloadDist =
		static_cast<float>(settings.maxRenderDistance) * kChunkUnloadDistanceFactor;
	const float unloadDistSq = unloadDist * unloadDist;

	const float camOffsetX = center.x - CHUNK_SIZE * 0.5f;
	const float camOffsetZ = center.z - CHUNK_SIZE * 0.5f;

	std::vector<glm::ivec3> toUnload;
	{
		std::shared_lock<std::shared_mutex> lock(m_mutex);
		for (WorldChunk *chunk : m_activeChunks)
		{
			if (!chunk || chunk->isInTransit())
				continue;

			const glm::vec3 p = chunk->getPosition();
			const float dx = camOffsetX - p.x;
			const float dz = camOffsetZ - p.z;

			if (dx * dx + dz * dz > unloadDistSq)
				toUnload.push_back(chunk->coord());
		}
	}

	if (toUnload.empty())
		return;

	std::lock_guard<std::shared_mutex> lock(m_mutex);
	// Collect the retire set first, then compact the active list in one
	// pass (no per-record linear search; the pre-split manager used the
	// render active index, which records do not carry).
	std::unordered_set<WorldChunk *> retiring;
	for (const auto &pos : toUnload)
	{
		auto it = m_chunks.find(pos);
		if (it == m_chunks.end())
			continue;
		WorldChunk *chunk = it->second;
		if (!chunk || chunk->isInTransit())
			continue;

		m_chunks.erase(it);
		// Durability barrier (review C1): deferred edits targeting a record
		// that never became readable must reach the persistent override set
		// BEFORE the queue entries are erased — otherwise an accepted edit
		// against an UNLOADED record would vanish without a trace when the
		// player walks away before generation is dispatched.
		bakeDeferredEditsForUnload(chunk);
		erasePendingEditsFor(chunk);

		// Capture recorded edits BEFORE recycling wipes the edit map
		// (issue #180): the immutable payload is queued for the save worker,
		// so recycling the record is safe. The pre-split manager captured at
		// the end of the GPU deferred-release window; the edit map is frozen
		// at unload (no edits can target an unregistered coordinate), so
		// capturing here persists exactly the same content.
		captureChunkEditsForUnload(chunk);

		WorldEvent e;
		e.kind = WorldEventKind::Unloaded;
		e.coord = pos;
		e.generation = chunk->generation();
		e.revision = chunk->revision();
		e.external = m_externalRecords.count(chunk) != 0;
		emitEvent(e);

		if (e.external)
			m_externalRecords.erase(chunk);
		else
			releaseRecord(chunk);
		retiring.insert(chunk);
	}
	if (!retiring.empty())
	{
		m_activeChunks.erase(
			std::remove_if(m_activeChunks.begin(), m_activeChunks.end(),
						   [&retiring](WorldChunk *c) { return retiring.count(c) != 0; }),
			m_activeChunks.end());
	}
}

StreamingUpdateKind VoxelWorld::rebuildStreamingQueueFull(const glm::ivec3 &centerChunk, const glm::vec3 &center,
														  const glm::vec2 &forwardXZ, const WorldStreamSettings &settings)
{
	const float frontBias = normalizedStreamFrontBias(settings.streamFrontBias);

	m_desiredFootprint = computeDesiredFootprintFull(centerChunk, center, forwardXZ, frontBias,
													settings.maxRenderDistance);
	m_streamStats.footprintRowsVisited +=
		static_cast<uint64_t>(m_desiredFootprint.maxZ - m_desiredFootprint.minZ + 1);

	std::lock_guard<std::shared_mutex> lock(m_mutex);
	m_loadQueue.clear();
	m_loadQueueHead = 0;
	m_enqueuedLoads.clear();

	for (int z = m_desiredFootprint.minZ; z <= m_desiredFootprint.maxZ; ++z)
	{
		const ChunkRowSpan *s = m_desiredFootprint.spanForZ(z);
		if (!s || s->empty())
			continue;
		for (int x = s->minX; x <= s->maxX; ++x)
		{
			const glm::ivec3 pos(x, 0, z);
			if (m_chunks.find(pos) != m_chunks.end())
				continue;
			const glm::vec3 chunkCenter(
				pos.x * CHUNK_SIZE + CHUNK_SIZE * 0.5f, 0.f,
				pos.z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
			const float distSq = biasedLoadDistSq(center, chunkCenter, forwardXZ, frontBias);
			m_loadQueue.push_back({pos, distSq});
			m_enqueuedLoads.insert(pos);
		}
	}
	sortLoadCandidatesNearestFirst(m_loadQueue);
	++m_streamStats.queueSorts;
	m_queueNeedsSort = false;

	m_streamState.lastCamChunk = centerChunk;
	m_streamState.lastMovementAnchor = streamingMovementAnchor(center);
	m_streamState.lastCamForwardXZ = forwardXZ;
	m_streamState.lastMaxRenderDistance = settings.maxRenderDistance;
	m_streamState.lastStreamFrontBias = normalizedStreamFrontBias(settings.streamFrontBias);
	m_streamState.initialized = true;
	++m_streamStats.fullRebuilds;
	return StreamingUpdateKind::FullRebuild;
}

void VoxelWorld::applyFootprintDiffToQueue(const FootprintDiff &diff, const glm::vec3 &center,
										   const glm::vec2 &forwardXZ, float frontBias)
{
	std::lock_guard<std::shared_mutex> lock(m_mutex);

	for (const glm::ivec3 &pos : diff.exiting)
		m_enqueuedLoads.erase(pos);

	for (const glm::ivec3 &pos : diff.entering)
	{
		if (m_chunks.find(pos) != m_chunks.end())
			continue;
		if (m_enqueuedLoads.count(pos) != 0)
			continue;
		const glm::vec3 chunkCenter(
			pos.x * CHUNK_SIZE + CHUNK_SIZE * 0.5f, 0.f,
			pos.z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
		const float distSq = biasedLoadDistSq(center, chunkCenter, forwardXZ, frontBias);
		m_loadQueue.push_back({pos, distSq});
		m_enqueuedLoads.insert(pos);
	}

	if (m_loadQueueHead > 0)
	{
		m_loadQueue.erase(m_loadQueue.begin(), m_loadQueue.begin() + m_loadQueueHead);
		m_loadQueueHead = 0;
	}
	m_loadQueue.erase(std::remove_if(m_loadQueue.begin(), m_loadQueue.end(),
									 [&](const LoadCandidate &c) {
										 return m_enqueuedLoads.count(c.pos) == 0;
									 }),
					  m_loadQueue.end());

	for (auto &c : m_loadQueue)
	{
		const glm::vec3 chunkCenter(
			c.pos.x * CHUNK_SIZE + CHUNK_SIZE * 0.5f, 0.f,
			c.pos.z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
		c.distSq = biasedLoadDistSq(center, chunkCenter, forwardXZ, frontBias);
	}
	sortLoadCandidatesNearestFirst(m_loadQueue);
	++m_streamStats.queueSorts;
	m_streamStats.enteringCandidates += diff.entering.size();
	m_streamStats.exitingCandidates += diff.exiting.size();
	m_queueNeedsSort = false;
	m_streamState.lastCamForwardXZ = forwardXZ;
}

StreamingUpdateKind VoxelWorld::reconcileStreamingIncremental(const glm::ivec3 &centerChunk, const glm::vec3 &center,
															  const glm::vec2 &forwardXZ, const WorldStreamSettings &settings)
{
	const float frontBias = normalizedStreamFrontBias(settings.streamFrontBias);

	ChunkDesiredFootprint newFootprint =
		computeDesiredFootprintIncremental(m_desiredFootprint, centerChunk, center, forwardXZ,
										   frontBias, settings.maxRenderDistance);
	m_streamStats.footprintRowsVisited +=
		static_cast<uint64_t>(newFootprint.maxZ - newFootprint.minZ + 1);
	FootprintDiff diff = computeFootprintDiff(m_desiredFootprint, newFootprint);
	m_desiredFootprint = std::move(newFootprint);

	applyFootprintDiffToQueue(diff, center, forwardXZ, frontBias);
	m_streamState.lastCamChunk = centerChunk;
	m_streamState.lastMovementAnchor = streamingMovementAnchor(center);
	++m_streamStats.incrementalUpdates;
	return StreamingUpdateKind::Incremental;
}

StreamingUpdateKind VoxelWorld::reconcileStreamingHeading(const glm::ivec3 &centerChunk, const glm::vec3 &center,
														  const glm::vec2 &forwardXZ, const WorldStreamSettings &settings)
{
	const float frontBias = normalizedStreamFrontBias(settings.streamFrontBias);

	ChunkDesiredFootprint newFootprint =
		computeDesiredFootprintFull(centerChunk, center, forwardXZ, frontBias,
									settings.maxRenderDistance);
	m_streamStats.footprintRowsVisited +=
		static_cast<uint64_t>(newFootprint.maxZ - newFootprint.minZ + 1);
	FootprintDiff diff = computeFootprintDiff(m_desiredFootprint, newFootprint);
	m_desiredFootprint = std::move(newFootprint);

	applyFootprintDiffToQueue(diff, center, forwardXZ, frontBias);
	// The heading path also serves chunk-cross frames (heading has dispatch
	// priority), so it must publish the full camera state.
	m_streamState.lastCamChunk = centerChunk;
	m_streamState.lastMovementAnchor = streamingMovementAnchor(center);
	++m_streamStats.headingRebuilds;
	return StreamingUpdateKind::HeadingRebuild;
}

StreamingUpdateKind VoxelWorld::loadChunksAroundPlayer(const glm::ivec3 &centerChunk, const glm::vec3 &center,
													   const glm::vec2 &forwardXZ, const WorldStreamSettings &settings)
{
	const float frontBias = normalizedStreamFrontBias(settings.streamFrontBias);

	if (!m_streamState.initialized ||
		settings.maxRenderDistance != m_streamState.lastMaxRenderDistance ||
		streamFrontBiasChanged(settings.streamFrontBias, m_streamState.lastStreamFrontBias))
	{
		return rebuildStreamingQueueFull(centerChunk, center, forwardXZ, settings);
	}

	const glm::ivec3 chunkDelta = centerChunk - m_streamState.lastCamChunk;
	if (std::abs(chunkDelta.x) > 1 || std::abs(chunkDelta.z) > 1)
		return rebuildStreamingQueueFull(centerChunk, center, forwardXZ, settings); // teleport

	// Explicit priority: settings > teleport > heading > movement (#108).
	if (frontBias > kStreamBiasEpsilon &&
		glm::dot(forwardXZ, m_streamState.lastCamForwardXZ) < kStreamHeadingCosThreshold)
	{
		return reconcileStreamingHeading(centerChunk, center, forwardXZ, settings);
	}

	if (chunkDelta.x != 0 || chunkDelta.z != 0)
		return reconcileStreamingIncremental(centerChunk, center, forwardXZ, settings);

	// Intra-chunk movement: reconcile at movement-anchor granularity.
	const glm::ivec2 anchor = streamingMovementAnchor(center);
	if (anchor != m_streamState.lastMovementAnchor)
		return reconcileStreamingIncremental(centerChunk, center, forwardXZ, settings);

	++m_streamStats.zeroWork;
	return StreamingUpdateKind::None; // exact zero-work steady state
}

// ---------------------------------------------------------------------------
// Generation dispatch + publication
// ---------------------------------------------------------------------------

TaskPriority VoxelWorld::calculateTaskPriority(float distanceSq, float lodThresholdSq) const
{
	if (distanceSq < lodThresholdSq * 0.25f)
		return TaskPriority::High;
	if (distanceSq > lodThresholdSq)
		return TaskPriority::Low;
	return TaskPriority::Normal;
}

void VoxelWorld::generatePendingVoxels(const glm::vec3 &center, float lodThresholdSq, int budget)
{
	if (!m_terrainGenerator || !m_threadPool || budget <= 0)
		return;

	struct Item
	{
		WorldChunk *chunk;
		float distSq;
	};

	// Bounded critical section (review §4.3): the exclusive lock covers
	// selection, storage preparation and dispatch because inTransit must be
	// armed atomically with the state check (an unlocked window would let
	// an unload recycle a record between the two). The work under the lock
	// is bounded by `budget` (per-frame gen rate) and the pool-backed
	// allocations are O(1) amortized; main-thread-only entry points keep
	// readers on the shared lock outside this window.
	std::lock_guard<std::shared_mutex> lock(m_mutex);
	thread_local std::vector<Item> queue;
	queue.clear();
	queue.reserve(m_activeChunks.size());
	const float camOffsetX = center.x - CHUNK_SIZE * 0.5f;
	const float camOffsetZ = center.z - CHUNK_SIZE * 0.5f;

	for (WorldChunk *chunk : m_activeChunks)
	{
		if (chunk->getState() == ChunkState::UNLOADED && !chunk->isInTransit())
		{
			const glm::vec3 p = chunk->getPosition();
			const float dx = p.x - camOffsetX;
			const float dz = p.z - camOffsetZ;
			queue.push_back({chunk, dx * dx + dz * dz});
		}
	}

	const int n = std::min(budget, static_cast<int>(queue.size()));
	if (n <= 0)
		return;

	std::partial_sort(queue.begin(), queue.begin() + n, queue.end(),
					  [](const Item &a, const Item &b) { return a.distSq < b.distSq; });

	const int seed = m_terrainGenerator->getSeed();

	for (int i = 0; i < n; ++i)
	{
		WorldChunk *chunk = queue[i].chunk;
		if (!chunk->prepareVoxelStorageForGeneration())
			continue; // Allocation failed (e.g. OOM), retry next tick
		const TaskPriority prio = calculateTaskPriority(queue[i].distSq, lodThresholdSq);
		chunk->setInTransit(true);
		WorldEvent queued;
		queued.kind = WorldEventKind::GenQueued;
		queued.coord = chunk->coord();
		queued.generation = chunk->generation();
		queued.revision = chunk->revision();
		emitEvent(queued);
		m_pendingGenJobsCount.fetch_add(1);
		const uint64_t captureEpoch = m_workerObserver ? m_workerObserver->workerCaptureEpoch() : 0;
		const uint64_t generation = chunk->generation();
		const auto queuedAt = std::chrono::steady_clock::now();
		WorldWorkerObserver *observer = m_workerObserver;
		// Fire-and-forget (review M4): the result flows through
		// m_completedGenerationChunks, the future would be ignored.
		m_threadPool->submit(prio, [chunk, seed, generation, this, queuedAt, captureEpoch, observer]() {
			const auto t0 = std::chrono::steady_clock::now();
			if (observer)
				observer->addWorkerSample("TerrainQueue",
					std::chrono::duration<float, std::milli>(t0 - queuedAt).count(), captureEpoch);
			TerrainGenerator &localGen = TerrainGenerator::getThreadLocal(seed);
			chunk->generateTerrain(localGen);
			// Saved overrides re-apply deterministically right after
			// generation (issue #180): the job owns the record (in transit,
			// unpublished) and the first mesh after generation covers every
			// touched section, so this is race-free and meshes correctly.
			applyPersistentOverrides(chunk);
			if (observer)
			{
				const float ms = std::chrono::duration<float, std::milli>(
									 std::chrono::steady_clock::now() - t0)
									 .count();
				observer->addWorkerSample("TerrainGen", ms, captureEpoch);
			}

			{
				std::lock_guard<std::mutex> lk(m_completedJobsMutex);
				m_completedGenerationChunks.push_back({chunk, generation});
			}
			m_pendingGenJobsCount.fetch_sub(1);
		});
	}
}

int VoxelWorld::generatePendingVoxelsSync(const glm::vec3 &center, int budget)
{
	if (!m_terrainGenerator || budget <= 0)
		return 0;

	struct Item
	{
		WorldChunk *chunk;
		float distSq;
	};

	// Main-thread-only, like every lifecycle entry point: the exclusive lock
	// covers selection AND generation (no worker ever sees these records —
	// inTransit stays false, so asyncBusy() remains the async-path signal).
	std::lock_guard<std::shared_mutex> lock(m_mutex);
	thread_local std::vector<Item> queue;
	queue.clear();
	queue.reserve(m_activeChunks.size());
	const float camOffsetX = center.x - CHUNK_SIZE * 0.5f;
	const float camOffsetZ = center.z - CHUNK_SIZE * 0.5f;

	for (WorldChunk *chunk : m_activeChunks)
	{
		if (chunk->getState() == ChunkState::UNLOADED && !chunk->isInTransit())
		{
			const glm::vec3 p = chunk->getPosition();
			const float dx = p.x - camOffsetX;
			const float dz = p.z - camOffsetZ;
			queue.push_back({chunk, dx * dx + dz * dz});
		}
	}

	const int n = std::min(budget, static_cast<int>(queue.size()));
	if (n <= 0)
		return 0;

	std::partial_sort(queue.begin(), queue.begin() + n, queue.end(),
					  [](const Item &a, const Item &b) { return a.distSq < b.distSq; });

	int generated = 0;
	for (int i = 0; i < n; ++i)
	{
		WorldChunk *chunk = queue[i].chunk;
		// Unlike the pre-registration bootstrap path, these records are
		// already resident: on allocation failure keep the slot UNLOADED and
		// retry on a later tick instead of recycling a registered record.
		if (!chunk->prepareVoxelStorageForGeneration())
			continue;
		WorldEvent queued;
		queued.kind = WorldEventKind::GenQueued;
		queued.coord = chunk->coord();
		queued.generation = chunk->generation();
		queued.revision = chunk->revision();
		emitEvent(queued);
		if (!chunk->generateTerrain(*m_terrainGenerator))
			continue;
		// Same override contract as the async gen job and the bootstrap:
		// generation is deterministic, saved edits re-apply on top.
		applyPersistentOverrides(chunk);
		WorldEvent done;
		done.kind = WorldEventKind::Generated;
		done.coord = chunk->coord();
		done.generation = chunk->generation();
		done.revision = chunk->revision();
		emitEvent(done);
		++generated;
	}
	return generated;
}

void VoxelWorld::publishFinishedGeneration()
{
	std::vector<CompletedGenJob> finishedGen;
	{
		std::lock_guard<std::mutex> lock(m_completedJobsMutex);
		finishedGen.swap(m_completedGenerationChunks);
	}
	if (finishedGen.empty())
		return;

	std::lock_guard<std::shared_mutex> lock(m_mutex);
	for (const CompletedGenJob &job : finishedGen)
	{
		WorldChunk *chunk = job.chunk;
		if (!chunk)
			continue;
		// Identity validation: a recycled incarnation must never be
		// published into (unload requires !inTransit, so this is a safety
		// net for out-of-order completion, issue #206 acceptance).
		const auto it = m_chunks.find(chunk->coord());
		if (it == m_chunks.end() || it->second != chunk || chunk->generation() != job.generation)
			continue;
		chunk->setInTransit(false);
		WorldEvent e;
		e.kind = WorldEventKind::Generated;
		e.coord = chunk->coord();
		e.generation = chunk->generation();
		e.revision = chunk->revision();
		emitEvent(e);
	}
}

void VoxelWorld::pump()
{
	publishFinishedGeneration();
	applyPendingEdits();
}

// ---------------------------------------------------------------------------
// Deferred logical edits (issue #114 review; ported verbatim)
// ---------------------------------------------------------------------------

WorldEditResult VoxelWorld::placeVoxel(const glm::vec3 &worldPos, TextureType type)
{
	const int chunkX = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE));
	const int chunkZ = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE));
	const glm::ivec3 chunkPos(chunkX, 0, chunkZ);
	const int localX = static_cast<int>(std::floor(worldPos.x)) - chunkX * CHUNK_SIZE;
	const int localY = static_cast<int>(std::floor(worldPos.y));
	const int localZ = static_cast<int>(std::floor(worldPos.z)) - chunkZ * CHUNK_SIZE;

	std::lock_guard<std::shared_mutex> lock(m_mutex);
	WorldEditResult result;
	result.accepted = scheduleLogicalEdit(chunkPos, localX, localY, localZ, type, result);
	return result;
}

WorldEditResult VoxelWorld::deleteVoxel(const glm::vec3 &worldPos)
{
	return placeVoxel(worldPos, AIR);
}

bool VoxelWorld::scheduleLogicalEdit(const glm::ivec3 &chunkPos, int x, int y, int z,
									 TextureType type, WorldEditResult &out)
{
	// Coordinate validation FIRST (issue #114 final review): a raycast hit
	// on the top face of a y = CHUNK_HEIGHT-1 voxel produces y = CHUNK_HEIGHT.
	if (x < 0 || x >= static_cast<int>(CHUNK_SIZE) ||
		y < 0 || y >= static_cast<int>(CHUNK_HEIGHT) ||
		z < 0 || z >= static_cast<int>(CHUNK_SIZE))
		return false;

	// Caller holds m_mutex exclusively.
	auto it = m_chunks.find(chunkPos);
	if (it == m_chunks.end())
		return false;
	WorldChunk *target = it->second;

	// Logical no-op refusal against the EFFECTIVE state (storage overlaid
	// with pending edits), so place-after-pending-delete is not wrongly
	// refused (issue #114 review items 15-17).
	const TextureType effective = effectiveVoxelType(target, x, y, z);
	if (type == AIR ? effective == AIR : effective != AIR)
		return false;

	// One user interaction = one logical group: the target edit plus every
	// neighbor mirror. If the target must defer, its mirrors defer with it
	// (transaction semantics, issue #114 review item 6).
	const uint64_t editId = m_nextEditId++;
	const bool deferAll = mustDeferVoxelEdit(target);
	out.editId = editId;
	out.geometricMembers.push_back(chunkPos);
	queueOrApplyEdit(target, chunkPos, x, y, z, type, false, deferAll, editId);
	enqueueOrApplyMirrorEdits(chunkPos, x, y, z, type, deferAll, editId, &out.geometricMembers);
	return true;
}

TextureType VoxelWorld::effectiveVoxelType(const WorldChunk *chunk, int x, int y, int z) const
{
	assert(chunk != nullptr);
	assert(x >= 0 && x < static_cast<int>(CHUNK_SIZE));
	assert(y >= 0 && y < static_cast<int>(CHUNK_HEIGHT));
	assert(z >= 0 && z < static_cast<int>(CHUNK_SIZE));

	// Occupancy lifecycle (issue #115 review): an UNLOADED record is
	// prepared-for-generation — its backing holds stale pool bytes and is
	// logically empty. The pending-edit overlay still applies.
	TextureType effective = AIR;
	if (chunk->isVoxelBackingReadable())
		effective = static_cast<TextureType>(chunk->getVoxel(static_cast<uint32_t>(x),
															 static_cast<uint32_t>(y),
															 static_cast<uint32_t>(z))
												 .type);
	for (const PendingVoxelEdit &edit : m_pendingEdits)
	{
		if (edit.chunk == chunk && !edit.borderNeighbor &&
			edit.x == x && edit.y == y && edit.z == z)
			effective = edit.type; // last pending write wins
	}
	return effective;
}

bool VoxelWorld::mustDeferVoxelEdit(const WorldChunk *chunk) const
{
	// UNLOADED defers regardless of transit state: the backing is stale
	// pool bytes until terrain generation initializes it (issue #115).
	return !chunk ||
		   chunk->getState() == ChunkState::UNLOADED ||
		   chunk->isInTransit();
}

void VoxelWorld::queueOrApplyEdit(WorldChunk *chunk, const glm::ivec3 &chunkPos, int x,
								  int y, int z, TextureType type, bool borderNeighbor,
								  bool forceDefer, uint64_t editId)
{
	if (!chunk)
		return;
	// One gate for every deferral reason (issue #115 blocker).
	if (forceDefer || mustDeferVoxelEdit(chunk))
	{
		queuePendingEdit(chunk, chunkPos, x, y, z, type, borderNeighbor, editId);
		return;
	}
	// Apply path: the backing is readable and nothing owns the record.
	assert(!chunk->isInTransit() && chunk->isVoxelBackingReadable() &&
		   "edit reached the apply path on unreadable backing");
	if (borderNeighbor)
		ensureShellPopulatedLocked(chunk);
	// Authoritative write; the render adapter consumes the EditApplied
	// event for section/light invalidation and the GENERATED re-arm.
	const TextureType previousType = chunk->setVoxel(x, y, z, type);
	emitEditApplied(chunk, x, y, z, type, previousType, borderNeighbor);
}

void VoxelWorld::emitEditApplied(WorldChunk *chunk, int x, int y, int z, TextureType type,
								 TextureType previousType, bool borderNeighbor)
{
	WorldEvent e;
	e.kind = WorldEventKind::EditApplied;
	e.coord = chunk->coord();
	e.generation = chunk->generation();
	e.revision = chunk->revision();
	e.x = x;
	e.y = y;
	e.z = z;
	e.type = type;
	e.previousType = previousType;
	e.borderNeighbor = borderNeighbor;
	emitEvent(e);
}

void VoxelWorld::enqueueOrApplyMirrorEdits(const glm::ivec3 &chunkPos, int x, int y,
										   int z, TextureType type, bool forceDefer,
										   uint64_t editId, std::vector<glm::ivec3> *geometricMembers)
{
	// Same mirror mapping as the historical dirtyNeighbor path: the
	// neighbor on the -x side holds our x=0 column at its local
	// CHUNK_SIZE, and so on for each face.
	const auto mirror = [&](const glm::ivec3 &neighborPos, int nx, int nz)
	{
		WorldChunk *neighbor = getChunk(neighborPos);
		if (!neighbor)
			return;
		if (geometricMembers)
			geometricMembers->push_back(neighborPos);
		queueOrApplyEdit(neighbor, neighborPos, nx, y, nz, type, true, forceDefer, editId);
	};
	if (x == 0)
		mirror(chunkPos + glm::ivec3(-1, 0, 0), CHUNK_SIZE, z);
	if (x == CHUNK_SIZE - 1)
		mirror(chunkPos + glm::ivec3(1, 0, 0), -1, z);
	if (z == 0)
		mirror(chunkPos + glm::ivec3(0, 0, -1), x, CHUNK_SIZE);
	if (z == CHUNK_SIZE - 1)
		mirror(chunkPos + glm::ivec3(0, 0, 1), x, -1);
}

void VoxelWorld::queuePendingEdit(WorldChunk *chunk, const glm::ivec3 &chunkPos, int x, int y, int z,
								  TextureType type, bool borderNeighbor, uint64_t editId)
{
	const uint64_t generation = chunk->generation();
	// Coalesce: the last logical write per (chunk, coordinate, kind) wins,
	// so place->delete->place during one transit collapses into one entry.
	for (PendingVoxelEdit &existing : m_pendingEdits)
	{
		if (existing.chunk == chunk && existing.borderNeighbor == borderNeighbor &&
			existing.x == x && existing.y == y && existing.z == z)
		{
			existing.type = type;
			existing.generation = generation;
			existing.editId = editId;
			return;
		}
	}
	m_pendingEdits.push_back({chunk, generation, chunkPos, x, y, z, type, borderNeighbor, editId});
}

bool VoxelWorld::hasPendingEditsFor(const WorldChunk *chunk) const
{
	for (const PendingVoxelEdit &edit : m_pendingEdits)
		if (edit.chunk == chunk)
			return true;
	return false;
}

bool VoxelWorld::hasPendingLogicalEdits() const
{
	return !m_pendingEdits.empty();
}

void VoxelWorld::erasePendingEditsFor(const WorldChunk *chunk)
{
	m_pendingEdits.erase(
		std::remove_if(m_pendingEdits.begin(), m_pendingEdits.end(),
					   [chunk](const PendingVoxelEdit &edit) { return edit.chunk == chunk; }),
		m_pendingEdits.end());
}

void VoxelWorld::applyPendingEdits()
{
	// Main-thread only (the edit/pump contract): no other thread mutates
	// m_pendingEdits, so the empty check outside the lock is safe.
	if (m_pendingEdits.empty())
		return;

	std::vector<PendingVoxelEdit> remaining;
	{
		std::lock_guard<std::shared_mutex> lock(m_mutex);
		// Two passes for a deterministic order: target voxel edits first,
		// then mirror border writes (issue #114 review item 8). Each entry
		// waits only on its own record's transit state (item 10).
		for (int pass = 0; pass < 2; ++pass)
		{
			const bool mirrorPass = pass == 1;
			// Frozen-size index loop (issue #206 review): structurally safe
			// even if queueOrApplyEdit ever re-queued — the bound is fixed and
			// each entry is copied by value, so the vector can neither be
			// invalidated nor revisited mid-loop. (Today mustDeferVoxelEdit ==
			// false here guarantees the apply path never re-queues.)
			const size_t n = m_pendingEdits.size();
			for (size_t i = 0; i < n; ++i)
			{
				const PendingVoxelEdit edit = m_pendingEdits[i];
				if (edit.borderNeighbor != mirrorPass)
					continue;
				if (!edit.chunk)
					continue; // dangling entry: drop
				// Staleness first: a recycled incarnation must never
				// receive the queued edit.
				if (edit.chunk->generation() != edit.generation)
					continue; // record recycled since queueing: stale, drop
				// Not readable yet: in transit, or still waiting for terrain
				// generation (issue #115 blocker). Keep waiting.
				if (mustDeferVoxelEdit(edit.chunk))
				{
					remaining.push_back(edit);
					continue;
				}
				queueOrApplyEdit(edit.chunk, edit.chunkPos, edit.x, edit.y,
								 edit.z, edit.type, edit.borderNeighbor,
								 /*forceDefer=*/false, edit.editId);
			}
		}
	}
	m_pendingEdits.swap(remaining);
}

void VoxelWorld::ensureShellPopulated(WorldChunk *chunk)
{
	if (!chunk || !chunk->isShellEmpty())
		return;
	std::lock_guard<std::shared_mutex> lock(m_mutex);
	ensureShellPopulatedLocked(chunk);
}

void VoxelWorld::ensureShellPopulatedLocked(WorldChunk *chunk)
{
	if (!chunk || !chunk->isShellEmpty())
		return;
	const glm::ivec3 chunkIdx = chunk->coord();
	chunk->rebuildBordersFromNeighbors(
		getChunk(chunkIdx + glm::ivec3(-1, 0, 0)),
		getChunk(chunkIdx + glm::ivec3(+1, 0, 0)),
		getChunk(chunkIdx + glm::ivec3(0, 0, -1)),
		getChunk(chunkIdx + glm::ivec3(0, 0, +1)));
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool VoxelWorld::isVoxelActive(const glm::vec3 &worldPos) const
{
	if (worldPos.y < 0.f || worldPos.y >= static_cast<float>(CHUNK_HEIGHT))
		return false;

	const glm::ivec3 chunkPos = worldToChunkCoord(worldPos);

	std::shared_lock<std::shared_mutex> lock(m_mutex);
	auto it = m_chunks.find(chunkPos);
	if (it == m_chunks.end() || it->second->getState() < ChunkState::GENERATED)
		return false;

	const int chunkX = chunkPos.x;
	const int chunkZ = chunkPos.z;
	const int localX = static_cast<int>(std::floor(worldPos.x)) - chunkX * CHUNK_SIZE;
	const int localY = static_cast<int>(std::floor(worldPos.y));
	const int localZ = static_cast<int>(std::floor(worldPos.z)) - chunkZ * CHUNK_SIZE;
	return it->second->getVoxel(localX, localY, localZ).type != AIR;
}

WorldChunk *VoxelWorld::getChunk(const glm::ivec3 &chunkPos)
{
	// Lock-free by contract (review §4.3): every non-const caller is either
	// on the main thread outside concurrent mutation windows or already
	// holds m_mutex (the locked internal helpers). Taking a shared_lock
	// here would deadlock those paths — std::shared_mutex is not recursive.
	// Cross-thread readers must use the const overload, which locks.
	auto it = m_chunks.find(chunkPos);
	return it != m_chunks.end() ? it->second : nullptr;
}

const WorldChunk *VoxelWorld::getChunk(const glm::ivec3 &chunkPos) const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	auto it = m_chunks.find(chunkPos);
	return it != m_chunks.end() ? it->second : nullptr;
}

WorldChunk *VoxelWorld::getChunkAtWorldPos(const glm::vec3 &worldPos)
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	return getChunk(worldToChunkCoord(worldPos));
}

size_t VoxelWorld::chunkCount() const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	return m_chunks.size();
}

size_t VoxelWorld::pendingLoadCount() const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	return (m_loadQueue.size() >= m_loadQueueHead) ? (m_loadQueue.size() - m_loadQueueHead) : 0;
}

size_t VoxelWorld::pendingGenJobs() const
{
	return m_pendingGenJobsCount.load();
}

bool VoxelWorld::asyncBusy() const
{
	return m_pendingGenJobsCount.load() > 0 || anyChunkInTransitForPersistence();
}

std::vector<WorldChunk *> VoxelWorld::activeChunksSnapshot() const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	return m_activeChunks;
}

// ---------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------

bool VoxelWorld::prepareAndGenerateChunk(WorldChunk *chunk, TerrainGenerator &generator)
{
	if (!chunk)
		return false;
	// Shared synchronous bootstrap contract (issue #112): voxel storage is
	// prepared on the calling thread immediately before generation; workers
	// never acquire or release voxel backing. On allocation failure the
	// record is recycled so the slot is not leaked (external test records
	// are never recycled).
	if (!chunk->prepareVoxelStorageForGeneration())
	{
		if (m_externalRecords.count(chunk) == 0)
			releaseRecord(chunk);
		return false;
	}
	if (!chunk->generateTerrain(generator))
	{
		// Generation refused (record not UNLOADED or backing missing): never
		// apply overrides onto ungenerated backing, and never report success
		// (review §4.3). Same recycle discipline as the allocation-failure
		// path above; external test records are never recycled.
		if (m_externalRecords.count(chunk) == 0)
			releaseRecord(chunk);
		return false;
	}
	// Bootstrap path (issue #180): same override contract as the async gen
	// job — generation is deterministic, saved edits re-apply on top.
	applyPersistentOverrides(chunk);
	return true;
}

std::vector<glm::ivec3> VoxelWorld::generateInitialAreaVoxels(const glm::vec3 &center, int radiusChunks)
{
	std::vector<glm::ivec3> created;
	if (!m_terrainGenerator)
		return created;

	// Square footprint (2r+1)^2 plus a little margin for safety. Bootstrap is
	// a one-shot synchronous setup: grow in a single call (maxGrowPerCall = 0).
	const size_t bootstrapNeed =
		static_cast<size_t>(2 * radiusChunks + 1) * static_cast<size_t>(2 * radiusChunks + 1) + 16;
	ensureRecordCapacity(bootstrapNeed, 0);

	const glm::ivec3 camChunk = worldToChunkCoord(center);

	for (int z = -radiusChunks; z <= radiusChunks; ++z)
	{
		for (int x = -radiusChunks; x <= radiusChunks; ++x)
		{
			const glm::ivec3 pos = camChunk + glm::ivec3(x, 0, z);
			{
				std::lock_guard<std::shared_mutex> lock(m_mutex);
				if (m_chunks.find(pos) != m_chunks.end())
					continue;
			}
			WorldChunk *record = acquireRecord(glm::vec3(
				static_cast<float>(pos.x * CHUNK_SIZE), 0.f,
				static_cast<float>(pos.z * CHUNK_SIZE)));
			if (!record)
			{
				// Grow once more and retry (rare if ensureCapacity above is correct).
				ensureRecordCapacity(recordCapacity() + 64, 0);
				record = acquireRecord(glm::vec3(
					static_cast<float>(pos.x * CHUNK_SIZE), 0.f,
					static_cast<float>(pos.z * CHUNK_SIZE)));
				if (!record)
					continue;
			}
			// Bootstrap acquisition (issue #180): arm persistent-edit
			// tracking exactly like processChunkLoading does.
			record->setTrackPersistentEdits(isWorldOpen());
			if (!prepareAndGenerateChunk(record, *m_terrainGenerator))
				continue;
			std::lock_guard<std::shared_mutex> lock(m_mutex);
			m_chunks[pos] = record;
			m_activeChunks.push_back(record);
			WorldEvent e;
			e.kind = WorldEventKind::Loaded;
			e.coord = pos;
			e.generation = record->generation();
			e.revision = record->revision();
			emitEvent(e);
			created.push_back(pos);
		}
	}
	return created;
}

// ---------------------------------------------------------------------------
// Persistent world lifecycle (issue #180, Phase 3+4 — contracts unchanged)
// ---------------------------------------------------------------------------

bool VoxelWorld::openWorld(const std::string &savesRoot, const std::string &worldName,
						   std::string &outError)
{
	outError.clear();
	if (!m_terrainGenerator)
	{
		outError = "no terrain generator";
		return false;
	}
	if (m_persistence)
	{
		outError = "a world is already open";
		return false;
	}

	auto persistence = std::make_unique<WorldPersistence>();
	const WorldPersistence::OpenInfo info = persistence->openOrCreate(
		savesRoot, worldName, m_terrainGenerator->getSeed(), TerrainGenerator::kGeneratorVersion);
	if (!info.ok)
	{
		outError = info.error;
		return false;
	}
	m_persistence = std::move(persistence);

	// Arm tracking on every already-loaded record so edits from this point
	// on are recorded (records acquired later arm in processChunkLoading and
	// the bootstrap path).
	{
		std::lock_guard<std::shared_mutex> lock(m_mutex);
		for (auto &[pos, chunk] : m_chunks)
			if (chunk)
				chunk->setTrackPersistentEdits(true);
	}
	return true;
}

bool VoxelWorld::drainAsyncJobsForPersistence()
{
	// Closing a persistent world is a durability barrier. The loop cannot
	// spin forever on the JOB side (the close path does not feed the queues;
	// workers always finish), but a PendingVoxelEdit whose target record
	// never leaves UNLOADED can stay deferred forever, so that condition is
	// CHECKED afterwards instead of waited on.
	// NOTE: the graphical adapter quiesces its render jobs (which also hold
	// record inTransit) around this call; standalone/headless worlds only
	// have generation jobs, which pump() publishes.
	// Bounded wait (review M9): asyncBusy() also counts render-transit
	// records. If a host ever violated the quiesce-before-drain order, an
	// unbounded loop would hang the process forever; instead we report the
	// stall with per-category diagnostics and return false so closeWorld()
	// stays retryable and the shutdown path proceeds loudly, never silently.
	constexpr auto kDrainTimeout = std::chrono::seconds(30);
	const auto deadline = std::chrono::steady_clock::now() + kDrainTimeout;
	for (;;)
	{
		pump();
		if (!asyncBusy())
			break;
		if (std::chrono::steady_clock::now() >= deadline)
		{
			std::cerr << "[world] persistence drain timed out after "
					  << kDrainTimeout.count() << "s: gen jobs in flight="
					  << pendingGenJobs()
					  << ", records in transit=" << (anyChunkInTransitForPersistence() ? "yes" : "no")
					  << " (render jobs must be quiesced before the durability barrier)"
					  << std::endl;
			return false;
		}
		std::this_thread::sleep_for(std::chrono::microseconds(500));
	}
	// One final publish pass so edits deferred until the very last job land.
	pump();

	// Every edit accepted by gameplay must have reached authoritative voxel
	// state before the capture runs (issue #180 review round 2).
	if (hasPendingLogicalEdits())
		return false;
	return true;
}

bool VoxelWorld::anyChunkInTransitForPersistence() const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	for (const auto &[pos, chunk] : m_chunks)
		if (chunk && chunk->isInTransit())
			return true;
	return false;
}

void VoxelWorld::resolvePendingEditsForPersistenceShutdown()
{
	// Snapshot the distinct coordinates that still hold accepted edits.
	std::vector<glm::ivec3> coords;
	{
		std::lock_guard<std::shared_mutex> lock(m_mutex);
		coords.reserve(m_pendingEdits.size());
		for (const PendingVoxelEdit &edit : m_pendingEdits)
			if (edit.chunk)
				coords.push_back(edit.chunkPos);
	}
	const auto coordLess = [](const glm::ivec3 &a, const glm::ivec3 &b)
	{
		if (a.x != b.x)
			return a.x < b.x;
		if (a.z != b.z)
			return a.z < b.z;
		return a.y < b.y;
	};
	std::sort(coords.begin(), coords.end(), coordLess);
	coords.erase(std::unique(coords.begin(), coords.end(),
							 [](const glm::ivec3 &a, const glm::ivec3 &b)
							 { return a.x == b.x && a.y == b.y && a.z == b.z; }),
				 coords.end());

	for (const glm::ivec3 &coord : coords)
	{
		WorldChunk *chunk = nullptr;
		{
			std::lock_guard<std::shared_mutex> lock(m_mutex);
			const auto it = m_chunks.find(coord);
			chunk = it != m_chunks.end() ? it->second : nullptr;
		}
		if (!chunk || chunk->isInTransit() || chunk->getState() != ChunkState::UNLOADED)
			continue;

		// Deliberately NOT prepareAndGenerateChunk(): its allocation-failure
		// path recycles the record, which would leave m_chunks holding a
		// recycled slot. On failure the record stays UNLOADED and the edit
		// is lost as the documented last resort of a forced shutdown.
		if (!chunk->prepareVoxelStorageForGeneration())
		{
			std::cerr << "[world] shutdown resolve: voxel storage allocation failed for chunk ("
					  << coord.x << ", " << coord.z << "); its accepted edits cannot be saved"
					  << std::endl;
			continue;
		}
		if (m_terrainGenerator)
			chunk->generateTerrain(*m_terrainGenerator);
		applyPersistentOverrides(chunk);
	}

	// Land every edit whose record is now readable.
	pump();
}

bool VoxelWorld::forceCloseWorldForShutdown(const worldsave::CheckpointPayloads *gameplay)
{
	if (!m_persistence)
		return true;

	resolvePendingEditsForPersistenceShutdown();

	const bool drained = drainAsyncJobsForPersistence();
	if (!drained)
	{
		// Accepted logical edits could not be made authoritative. The
		// shutdown continues — destruction has no retry point — but the loss
		// is REPORTED, never silent.
		std::cerr << "[world] final shutdown: accepted logical edits could not be made authoritative"
				  << std::endl;
	}

	captureAllChunkEdits();
	const bool flushed = m_persistence->flush();
	if (!flushed)
		std::cerr << "[world] final shutdown: chunk flush failed" << std::endl;
	// Stabilization PR5b (review correction): the checkpoint precondition is
	// a COMPLETE durability barrier — commit ONLY when the async drain AND
	// the chunk flush both succeeded, so a manifest can never reference an
	// incomplete chunk state. Destruction has no retry point: on any failure
	// the shutdown still releases the service unconditionally, the failure is
	// reported with a distinct diagnostic, and the previous committed
	// checkpoint remains the authoritative recovery point (never a mixture).
	bool checkpointed = false;
	if (drained && flushed)
	{
		checkpointed =
			gameplay ? m_persistence->commitCheckpoint(*gameplay)
					 : m_persistence->commitCarryForwardCheckpoint(nextEditId());
		if (!checkpointed)
			std::cerr << "[world] final shutdown: checkpoint commit failed; the previous committed "
						 "checkpoint remains the recovery point"
					  << std::endl;
	}
	else
	{
		std::cerr << "[world] final shutdown: checkpoint skipped (drain or flush failed); the "
					 "previous committed checkpoint remains the recovery point"
				  << std::endl;
	}
	m_persistence->shutdown();
	// Disarm tracking on the loaded set: after close the world behaves
	// exactly like a never-opened one.
	{
		std::lock_guard<std::shared_mutex> lock(m_mutex);
		for (auto &[pos, chunk] : m_chunks)
			if (chunk)
				chunk->setTrackPersistentEdits(false);
	}
	m_persistence.reset();
	return drained && flushed && checkpointed;
}

bool VoxelWorld::closeWorld()
{
	if (!m_persistence)
		return true;

	// Quiesce BEFORE the final capture (issue #180 review): the capture must
	// see every authoritative edit and no worker may touch m_persistence
	// after shutdown/release.
	if (!drainAsyncJobsForPersistence())
	{
		// Accepted edits are still stranded outside authoritative voxel
		// state. Persistence stays OPEN: a later flushWorld()/closeWorld()
		// retries once the situation resolves.
		std::cerr << "[world] close deferred: logical edits still pending after drain" << std::endl;
		return false;
	}

	captureAllChunkEdits();

	// Retryable close (issue #180 review round 5): a flush failure must NOT
	// tear the persistence down — the desired/durable state machine keeps
	// every failed coordinate dirty, tracking stays armed and the save
	// service is still usable, so the next capture/flush/close retries.
	// Invariant: closeWorld() == false => m_persistence alive and usable.
	if (!m_persistence->flush())
	{
		std::cerr << "[world] close deferred: persistence flush failed" << std::endl;
		return false;
	}

	// Stabilization PR5b: carry-forward checkpoint commit so chunk edits
	// accepted after the last gameplay save are durable at close. A commit
	// failure keeps the persistence OPEN and retryable (same invariant);
	// the previous committed checkpoint stays the recovery point.
	if (!m_persistence->commitCarryForwardCheckpoint(nextEditId()))
	{
		std::cerr << "[world] close deferred: checkpoint commit failed" << std::endl;
		return false;
	}

	m_persistence->shutdown();
	{
		std::lock_guard<std::shared_mutex> lock(m_mutex);
		for (auto &[pos, chunk] : m_chunks)
			if (chunk)
				chunk->setTrackPersistentEdits(false);
	}
	m_persistence.reset();
	return true;
}

bool VoxelWorld::flushWorld()
{
	if (!m_persistence)
		return true;
	captureAllChunkEdits();
	return m_persistence->flush();
}

void VoxelWorld::applyPersistentOverrides(WorldChunk *chunk)
{
	if (!m_persistence || !m_persistence->enabled() || !chunk)
		return;
	const glm::ivec3 cc = chunk->coord();
	// Copy under the persistence mutex: the worker must not iterate a vector
	// that the save pipeline may refine concurrently.
	const std::vector<worldsave::ChunkEdit> overrides = m_persistence->overridesSnapshot(cc.x, cc.z);
	if (overrides.empty())
		return;
	for (const worldsave::ChunkEdit &edit : overrides)
	{
		// Guard against corrupt on-disk payloads: localIndex is the canonical
		// y-major index (y*256 + z*16 + x).
		if (edit.localIndex >= static_cast<uint32_t>(CHUNK_VOLUME))
		{
			std::cerr << "WorldPersistence: ignoring out-of-range localIndex "
					  << edit.localIndex << " in chunk (" << cc.x << ", " << cc.z << ")\n";
			continue;
		}
		const int x = static_cast<int>(edit.localIndex & 15u);
		const int z = static_cast<int>((edit.localIndex >> 4) & 15u);
		const int y = static_cast<int>(edit.localIndex >> 8);
		chunk->setVoxel(x, y, z, static_cast<TextureType>(edit.blockType));
	}
}

void VoxelWorld::bakeDeferredEditsForUnload(WorldChunk *chunk)
{
	// Main thread (the edit/pump contract): the unload caller holds m_mutex,
	// the flush capture caller (stabilization PR2) does not — both are safe
	// because m_pendingEdits and the record lifecycle are main-thread-only.
	// Only the canonical in-chunk entries carry durable content: mirror
	// entries (borderNeighbor) duplicate the owning chunk's edit and the
	// shell is rebuilt from resident neighbors when the coordinate loads
	// again.
	if (!m_persistence || !m_persistence->enabled() || !chunk)
		return;
	std::vector<worldsave::ChunkEdit> values;
	for (const PendingVoxelEdit &edit : m_pendingEdits)
	{
		if (edit.chunk != chunk || edit.borderNeighbor)
			continue;
		// Same staleness rule as applyPendingEdits: a recycled incarnation
		// never receives (or persists) a queued edit.
		if (edit.chunk->generation() != edit.generation)
			continue;
		if (static_cast<uint32_t>(edit.x) >= static_cast<uint32_t>(CHUNK_SIZE) ||
			static_cast<uint32_t>(edit.y) >= static_cast<uint32_t>(CHUNK_HEIGHT) ||
			static_cast<uint32_t>(edit.z) >= static_cast<uint32_t>(CHUNK_SIZE))
			continue;
		// Canonical y-major localIndex (y*256 + z*16 + x), the save format's
		// single index convention.
		const uint32_t localIndex =
			static_cast<uint32_t>(edit.y) * (CHUNK_SIZE * CHUNK_SIZE) +
			static_cast<uint32_t>(edit.z) * CHUNK_SIZE + static_cast<uint32_t>(edit.x);
		values.push_back({localIndex, static_cast<uint8_t>(edit.type)});
	}
	if (values.empty())
		return;
	// captureChunkEdits MERGES into the desired/durable state (delta merge),
	// so previously recorded edits and the on-disk baseline are preserved.
	// Reverts to the procedural base are dropped by the save worker's diff,
	// exactly like recorded reverts.
	const glm::ivec3 cc = chunk->coord();
	m_persistence->captureChunkEdits(cc.x, cc.z, std::move(values));
}

void VoxelWorld::captureChunkEditsForUnload(WorldChunk *chunk)
{
	if (!m_persistence || !m_persistence->enabled() || !chunk || !chunk->hasPersistentEdits())
		return;
	WorldChunk::ChunkEditMap edits = chunk->takePersistentEdits();
	std::vector<worldsave::ChunkEdit> values;
	values.reserve(edits.size());
	for (const auto &[localIndex, type] : edits)
		values.push_back({localIndex, type});
	std::sort(values.begin(), values.end(),
			  [](const worldsave::ChunkEdit &a, const worldsave::ChunkEdit &b)
			  { return a.localIndex < b.localIndex; });
	const glm::ivec3 cc = chunk->coord();
	m_persistence->captureChunkEdits(cc.x, cc.z, std::move(values));
}

void VoxelWorld::captureAllChunkEdits()
{
	if (!m_persistence || !m_persistence->enabled())
		return;
	// Main thread; edits only ever happen on the main thread, so the edit
	// maps of non-transit records are stable. In-transit records are skipped
	// for edit-map capture: their gen/mesh job is mid-flight (the job itself
	// re-applies the stored overrides afterwards), and the map is captured on
	// their own unload or a later flush. Unloaded records were captured at
	// unload.
	std::vector<WorldChunk *> capture;
	std::vector<WorldChunk *> deferred;
	{
		std::lock_guard<std::shared_mutex> lock(m_mutex);
		capture.reserve(m_chunks.size());
		for (auto &[pos, chunk] : m_chunks)
			if (chunk && !chunk->isInTransit() && chunk->hasPersistentEdits())
				capture.push_back(chunk);
		// Stabilization PR2: accepted edits still queued against in-transit /
		// UNLOADED records are authoritative gameplay state (a break already
		// spawned its drop at acceptance). A flush that skipped them would
		// persist the PRE-edit chunk alongside the post-edit gameplay state —
		// a crash before the next flush resurrected the block while its drop
		// survived (duplication on re-break). Collect the distinct records
		// holding canonical pending entries so they are baked below.
		for (const PendingVoxelEdit &edit : m_pendingEdits)
			if (edit.chunk && !edit.borderNeighbor &&
				std::find(deferred.begin(), deferred.end(), edit.chunk) == deferred.end())
				deferred.push_back(edit.chunk);
	}
	for (WorldChunk *chunk : capture)
		captureChunkEditsForUnload(chunk);
	// Bake the canonical pending entries into the desired/durable state
	// WITHOUT touching the backing or the queue: the issue #114 publication
	// ordering stays with the bridge/pump, and each edit still lands exactly
	// once when its record becomes editable (the later edit-map capture
	// delta-merges the same values — idempotent). Mirror entries carry no
	// independent durable content (rebuilt from neighbors on reload), the
	// exact bakeDeferredEditsForUnload contract. Main thread: the pending
	// queue is stable without the world lock here.
	for (WorldChunk *chunk : deferred)
		bakeDeferredEditsForUnload(chunk);
}
