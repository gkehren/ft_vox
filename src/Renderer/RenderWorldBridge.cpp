// RenderWorldBridge — renderer-owned derived projection of VoxelWorld
// (issue #207, Epic #204 Phase C; replaces the ChunkManager render adapter).
// The authoritative side (streaming residency, terrain publication, logical
// edits, persistence) lives in World/VoxelWorld.cpp and is owned by the
// host; this unit keeps the derived render pipeline: mesh/light dispatch,
// GPU uploads, commit groups, visibility, light-cache intent, deferred GPU
// retirement and the debug trace. World events are drained on the main
// thread right after each world call so render handles never observe a
// recycled record.
#include "RenderWorldBridge.hpp"

#include <Chunk/ChunkMeshResult.hpp>
#include <Camera/Camera.hpp>
#include <Engine/Profiler.hpp>
#include <Renderer/Lighting.hpp>
#include <Renderer/MinecraftTextures.hpp>
#include <Vulkan/VkCommands.hpp>
#include <Vulkan/StagingRing.hpp>
#include <Vulkan/GpuResourceRetire.hpp>
#include <World/WorldPersistence.hpp>
#include <World/WorldSave.hpp>

#include <glm/gtc/matrix_access.hpp>

#include <algorithm>
#include <array>
#include <unordered_set>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>

namespace
{
constexpr int kDeferredReleaseFrames = 3; // >= frames-in-flight

/// Worker-profiling seam implementation: forwards the world's generation
/// job samples to the engine Profiler (the world itself is profiler-free).
struct ProfilerWorkerObserver final : WorldWorkerObserver
{
	uint64_t workerCaptureEpoch() const override { return GetProfiler().captureEpoch(); }
	void addWorkerSample(const char *stage, float ms, uint64_t epoch) override
	{
		GetProfiler().addWorkerSample(stage, ms, epoch);
	}
};
ProfilerWorkerObserver g_profilerWorkerObserver;

/// Normalized XZ camera forward (fallback +Z when the forward is vertical).
glm::vec2 normalizedForwardXZ(const Camera &camera)
{
	glm::vec2 fwd(camera.getFront().x, camera.getFront().z);
	if (glm::dot(fwd, fwd) < 1e-6f)
		fwd = glm::vec2(0.f, 1.f);
	return glm::normalize(fwd);
}

WorldStreamSettings worldStreamSettings(const RenderSettings &settings)
{
	WorldStreamSettings ws;
	ws.maxRenderDistance = settings.maxRenderDistance;
	ws.minRenderDistance = settings.minRenderDistance;
	ws.streamFrontBias = settings.streamFrontBias;
	return ws;
}
} // namespace

RenderWorldBridge::RenderWorldBridge(VoxelWorld *world, ThreadPool *threadPool, ChunkPool *chunkPool,
									 uint64_t worldGenerationId)
	: m_world(world), m_worldGenerationId(worldGenerationId),
	  m_threadPool(threadPool), m_chunkPool(chunkPool)
{
	assert(m_world != nullptr && "RenderWorldBridge requires a host-owned VoxelWorld");
	// Render-side worker profiling seam: the world itself is profiler-free
	// and headless worlds run without an observer.
	m_world->setWorkerObserver(&g_profilerWorkerObserver);
	// Project the world's current state: bind handles to already-resident
	// records and drain queued change events (issue #207).
	attachToWorld();
}

RenderWorldBridge::~RenderWorldBridge()
{
	// Quiesce DERIVED render work before releasing any handle. When a world is
	// open this is the render half of the durability barrier (issue #180 review
	// round 3): publish finished gen/mesh/light jobs, clear inTransit and apply
	// deferred edits so the host's subsequent close sees complete authoritative
	// state. The bridge does NOT close the world's persistence — it only
	// references the world; the host owns that shutdown (issue #207 review).
	if (m_world->isWorldOpen())
		drainAsyncJobsForPersistence();

	// Generic teardown: best-effort drain of async work so we don't release
	// chunks still in workers. After the quiesce above the completion queues
	// are normally empty; this only sweeps stragglers from a world that was
	// never opened.
	// Progress + bound (review §4.6): the old spin only watched counters —
	// without publishing completions it could never make progress, and a
	// pool stopped before the bridge (host order violation) would hang it
	// forever. Publish inside the loop, and after a generous deadline report
	// loudly and stop waiting: a destroyed ThreadPool has JOINED its workers,
	// so no worker can still reference a handle past that point.
	const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (m_world->pendingGenJobs() > 0 || m_pendingMeshJobsCount.load() > 0 ||
		   m_pendingLightJobsCount.load() > 0)
	{
		processFinishedJobs();
		if (m_world->pendingGenJobs() == 0 && m_pendingMeshJobsCount.load() == 0 &&
			m_pendingLightJobsCount.load() == 0)
			break;
		if (std::chrono::steady_clock::now() >= drainDeadline)
		{
			std::cerr << "[bridge] teardown drain timed out: gen=" << m_world->pendingGenJobs()
					  << " mesh=" << m_pendingMeshJobsCount.load()
					  << " light=" << m_pendingLightJobsCount.load()
					  << " — workers must be quiesced before the bridge is destroyed" << std::endl;
			break;
		}
		std::this_thread::yield();
	}

	std::vector<CompletedMeshJob> completed;
	std::vector<CompletedLightJob> completedLight;
	{
		std::lock_guard<std::mutex> lock(m_completedJobsMutex);
		completed.swap(m_completedMeshJobs);
		completedLight.swap(m_completedLightJobs);
	}
	for (auto &job : completed)
	{
		if (job.result && job.result->homePool)
			job.result->homePool->release(job.result);
	}
	for (auto &job : completedLight)
	{
		if (job.storage && job.pool)
			job.pool->release(job.storage);
	}

	// PendingGroupMember owns every prepared-but-unpublished replacement.
	// Release those ranges while the chunks (and their arena pointers) are
	// still intact; Chunk::reset owns only already-published ranges.
	discardAllCommitGroups();

	for (auto &[pos, chunkPtr] : m_chunks)
	{
		if (chunkPtr)
		{
			chunkPtr->unbindWorldChunk();
			if (m_chunkPool)
				m_chunkPool->release(chunkPtr);
		}
	}
	m_chunks.clear();
	m_activeChunks.clear();

	for (Chunk *c : m_deferredRelease)
	{
		if (c && m_chunkPool)
			m_chunkPool->release(c);
	}
	m_deferredRelease.clear();
	// The world is host-owned and outlives the bridge: restore it to its
	// render-free state (no profiling seam) so it can keep running headless.
	// Its records return their backing to the world pools on ITS destruction,
	// after every render handle is gone.
	m_world->setWorkerObserver(nullptr);
}

glm::ivec3 RenderWorldBridge::worldToChunkCoord(const glm::vec3 &worldPos)
{
	return VoxelWorld::worldToChunkCoord(worldPos);
}

// ---------------------------------------------------------------------------
// World event stream → render handle synchronization (issue #206/#207)
// ---------------------------------------------------------------------------

Chunk *RenderWorldBridge::bindRecord(const glm::ivec3 &coord, WorldChunk *record)
{
	// Idempotent: a duplicate bind for an already-registered coord returns the
	// existing handle instead of acquiring a second one and orphaning the first
	// in m_activeChunks (issue #207 review). Callers may still pre-check.
	if (auto it = m_chunks.find(coord); it != m_chunks.end())
		return it->second;
	if (!m_chunkPool)
		return nullptr;
	Chunk *chunk = m_chunkPool->acquire(glm::vec3(
		static_cast<float>(coord.x * CHUNK_SIZE), 0.0f,
		static_cast<float>(coord.z * CHUNK_SIZE)));
	if (!chunk)
		return nullptr;
	chunk->bindWorldChunk(record);
	// A freshly bound handle carries no derived geometry. If the record already
	// claims MESHED — a live-world projection whose previous bridge released its
	// GPU ranges — that claim is stale: re-arm for a rebuild so mesh dispatch
	// picks it up instead of leaving it resident-but-invisible until an unrelated
	// invalidation (issue #207 review). Fresh loads are UNLOADED/GENERATED here,
	// so this is a no-op on the streaming and bootstrap paths. setState leaves
	// dirtySections()==0, so the rebuild is a FULL mesh via the sectionMask==0
	// fallback in meshPendingChunks — the boundary test asserts sectionsBuilt!=0
	// to catch that fallback ever being tightened.
	if (chunk->getState() == ChunkState::MESHED)
		chunk->setState(ChunkState::GENERATED);
	m_chunks[coord] = chunk;
	chunk->setActiveIndex(m_activeChunks.size());
	m_activeChunks.push_back(chunk);
	// Generate even before frustum test so the world fills around the player.
	chunk->setVisible(true);
	recordChunkEvent(chunk, "load");
	return chunk;
}

void RenderWorldBridge::attachToWorld()
{
	// Bind handles to records already resident in the world. The world does
	// not re-emit Loaded events for them, so a bridge attached to a live world
	// (the host's post-transition attach, issue #209) must project the current
	// residency itself. bindRecord re-arms any stale MESHED claim;
	// pool-exhaustion parks the coord for retryPendingBinds.
	{
		std::lock_guard<std::shared_mutex> lock(m_mutex);
		for (WorldChunk *record : m_world->activeChunksSnapshot())
		{
			if (!record)
				continue;
			const glm::ivec3 coord = record->coord();
			if (m_chunks.find(coord) != m_chunks.end())
				continue;
			if (!m_chunkPool)
				break; // no pool: nothing can bind; don't park every resident
			if (!bindRecord(coord, record))
				m_pendingBinds.push_back(coord);
		}
	}
	// Publish any queued generation/edit render flags for the bound handles.
	drainWorldEvents();
}

void RenderWorldBridge::drainWorldEvents()
{
	// Observability (issue #206 review): the world bounds its event stream and
	// drops the oldest events if a consumer falls behind. Surface that loss
	// once per change instead of leaving droppedEventCount() test-only. At
	// frame cadence this never fires.
	const uint64_t dropped = m_world->droppedEventCount();
	if (dropped != m_lastDroppedEvents)
	{
		std::cerr << "[world] event stream dropped " << dropped
				  << " event(s) — render consumer fell behind the authoritative world\n";
		m_lastDroppedEvents = dropped;
	}
	// Reuse one persistent buffer across the several drains per frame instead
	// of allocating a fresh vector each time (issue #219). clear() keeps the
	// capacity; takeEvents() appends into it under the world lock.
	m_eventDrainBuffer.clear();
	m_world->takeEvents(m_eventDrainBuffer);
	if (m_eventDrainBuffer.empty())
		return;
	std::lock_guard<std::shared_mutex> lock(m_mutex);
	for (const WorldEvent &e : m_eventDrainBuffer)
	{
		switch (e.kind)
		{
		case WorldEventKind::Loaded:
			onWorldLoaded(e);
			break;
		case WorldEventKind::Unloaded:
			onWorldUnloaded(e);
			break;
		case WorldEventKind::GenQueued:
			recordWorldEvent(e.coord, e.revision, "genQueued");
			break;
		case WorldEventKind::Generated:
			onWorldGenerated(e);
			break;
		case WorldEventKind::EditApplied:
			onWorldEditApplied(e);
			break;
		case WorldEventKind::ReplicaChanged:
			onReplicaChanged(e.coord);
			break;
		}
	}
}

void RenderWorldBridge::publishReplicaChanges()
{
	drainWorldEvents();
	retryPendingBinds();
}

void RenderWorldBridge::onReplicaChanged(const glm::ivec3 &coord)
{
	for (int dz = -1; dz <= 1; ++dz)
		for (int dx = -1; dx <= 1; ++dx)
		{
			Chunk *chunk = getChunk(coord + glm::ivec3(dx, 0, dz));
			if (!chunk || !chunk->isVoxelBackingReadable())
				continue;
			chunk->notifyTerrainGenerated();
			if (!chunk->isInTransit())
				chunk->setState(ChunkState::GENERATED);
		}
}

void RenderWorldBridge::onWorldLoaded(const WorldEvent &e)
{
	if (m_chunks.find(e.coord) != m_chunks.end())
		return;
	WorldChunk *record = m_world->getChunk(e.coord);
	if (!record)
		return;
	if (!bindRecord(e.coord, record))
	{
		// Defensive: the load budget is clamped by the render pool free
		// count, so this only fires on an unexpected race. Retry next tick.
		m_pendingBinds.push_back(e.coord);
	}
}

void RenderWorldBridge::retryPendingBinds()
{
	if (m_pendingBinds.empty())
		return;
	if (!m_chunkPool)
		return; // no pool: binds can never succeed; don't rescan every tick
	std::lock_guard<std::shared_mutex> lock(m_mutex);
	std::vector<glm::ivec3> stillPending;
	for (const glm::ivec3 &coord : m_pendingBinds)
	{
		if (m_chunks.find(coord) != m_chunks.end())
			continue;
		WorldChunk *record = m_world->getChunk(coord);
		if (!record)
			continue; // unloaded meanwhile
		if (!bindRecord(coord, record))
			stillPending.push_back(coord);
	}
	m_pendingBinds.swap(stillPending);
}

void RenderWorldBridge::onWorldUnloaded(const WorldEvent &e)
{
	auto it = m_chunks.find(e.coord);
	if (it == m_chunks.end())
		return;
	Chunk *chunk = it->second;
	if (!chunk)
	{
		m_chunks.erase(it);
		return;
	}
	// Local witness of the world-side contract (review §4.6): the world
	// never unloads an in-transit record; assert it here too so a future
	// violation is caught at the boundary that would dereference it.
	assert(!chunk->isInTransit() && "unload event for an in-transit render handle");
	const size_t activeIdx = chunk->getActiveIndex();
	if (activeIdx < m_activeChunks.size() && m_activeChunks[activeIdx] == chunk)
	{
		Chunk *lastChunk = m_activeChunks.back();
		m_activeChunks[activeIdx] = lastChunk;
		lastChunk->setActiveIndex(activeIdx);
		m_activeChunks.pop_back();
		chunk->setActiveIndex(SIZE_MAX);
	}
	m_chunks.erase(it);
	removeChunkFromCommitGroups(chunk);
	// The authoritative record is already retired by the world; drop the
	// binding so the parked render handle never reads a recycled record.
	chunk->unbindWorldChunk();
	if (!e.external)
	{
		m_deferredRelease.push_back(chunk);
		m_deferredReleaseAge = 0; // reset age so new unloads wait full delay
	}
	// External (test-registered) handles are owned by the test: drop the
	// reference only.
}

void RenderWorldBridge::onWorldGenerated(const WorldEvent &e)
{
	auto it = m_chunks.find(e.coord);
	if (it == m_chunks.end() || !it->second)
		return;
	Chunk *chunk = it->second;
	// Publish the render flags the pre-split generateTerrain() set inline
	// (the worker only touches authoritative state now).
	chunk->notifyTerrainGenerated();
	recordChunkEvent(chunk, "genDone");
	// New terrain can carry emissive sources whose light reaches into the
	// side neighbors' halo radius (issue #141 review fix).
	dirtyNeighborsForArrivedLight(chunk, e.coord);
}

void RenderWorldBridge::onWorldEditApplied(const WorldEvent &e)
{
	auto it = m_chunks.find(e.coord);
	if (it == m_chunks.end() || !it->second)
		return;
	Chunk *chunk = it->second;
	// Identity guard: the event must describe the bound incarnation.
	if (chunk->worldChunk() == nullptr || chunk->meshGeneration() != e.generation)
		return;
	chunk->notifyEditApplied(e.x, e.y, e.z, e.type, e.previousType, e.borderNeighbor);
	recordChunkEvent(chunk, e.borderNeighbor ? "editMirror" : "edit", chunk->dirtySections());
	// Light-relevant edits near a border change the neighbors' propagated
	// light too (issue #141 review fix); mirror writes are the owning
	// chunk's edit seen from the other side and skip this.
	if (!e.borderNeighbor)
		markNeighborLightDirty(e.coord, e.x, e.y, e.z, e.previousType, e.type);
}

// ---------------------------------------------------------------------------
// Streaming / loading / generation (delegated to the authoritative world)
// ---------------------------------------------------------------------------

StreamingUpdateKind RenderWorldBridge::updateStreaming(const Camera &camera, const RenderSettings &settings)
{
	// The world grows its own record pool inside updateStreaming (issue
	// #206); the Engine grows the render ChunkPool on the same estimate.
	const StreamingUpdateKind kind =
		m_world->updateStreaming(camera.getPosition(), normalizedForwardXZ(camera),
								 worldStreamSettings(settings));
	drainWorldEvents();
	return kind;
}

void RenderWorldBridge::processChunkLoading(int budget)
{
	if (budget <= 0 || !m_chunkPool)
		return;
	// Back-pressure on BOTH pools: a world record without a render handle
	// cannot mesh, so never load more records than render slots available.
	budget = std::min(budget, static_cast<int>(m_chunkPool->freeCount()));
	if (budget <= 0)
		return;
	m_world->processChunkLoading(budget);
	drainWorldEvents();
	retryPendingBinds();
}

void RenderWorldBridge::generatePendingVoxels(const Camera &camera, const RenderSettings &settings, int budget)
{
	const float lodThresh = static_cast<float>(settings.minRenderDistance) * 2.f;
	m_world->generatePendingVoxels(camera.getPosition(), lodThresh * lodThresh, budget);
	drainWorldEvents();
}

// ---------------------------------------------------------------------------
// Render mesh / light dispatch (derived; unchanged contracts)
// ---------------------------------------------------------------------------

void RenderWorldBridge::meshPendingChunks(const Camera &camera, const RenderSettings &settings, int budget)
{
	if (!m_threadPool || budget <= 0)
		return;

	struct Item
	{
		Chunk *chunk;
		float distSq;
	};

	std::lock_guard<std::shared_mutex> lock(m_mutex);
	thread_local std::vector<Item> queue;
	queue.clear();
	queue.reserve(m_activeChunks.size());
	const glm::vec3 camPos = camera.getPosition();
	const float camOffsetX = camPos.x - CHUNK_SIZE * 0.5f;
	const float camOffsetZ = camPos.z - CHUNK_SIZE * 0.5f;

	const float lodThresh = static_cast<float>(settings.minRenderDistance) * 2.f;
	const float lodThreshSq = lodThresh * lodThresh;

	// Promote distant LOD meshes back to full quality when close enough.
	// Entity light-cache acquisition must NOT re-arm a meshed chunk
	// (issue #172): entering the cache radius schedules a dedicated
	// light-only job in updateEntityLightCaches() instead, so a valid render
	// mesh is never invalidated by cache residency.
	for (Chunk *chunk : m_activeChunks)
	{
		const glm::vec3 p = chunk->getPosition();
		const float dx = p.x - camOffsetX;
		const float dz = p.z - camOffsetZ;
		const float distSq = dx * dx + dz * dz;
		updateEntityLightCacheIntent(*chunk, distSq);

		if (chunk->getState() == ChunkState::MESHED && !chunk->isInTransit())
		{
			if (chunk->isLODMesh() && distSq < lodThreshSq)
			{
				chunk->setState(ChunkState::GENERATED);
			}
		}
	}

	for (Chunk *chunk : m_activeChunks)
	{
		if (chunk->getState() == ChunkState::GENERATED && !chunk->isInTransit() && !chunk->isReplicaRetiring())
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

	for (int i = 0; i < n; ++i)
	{
		Chunk *chunk = queue[i].chunk;
		const float distSq = queue[i].distSq;
		const glm::vec3 wp = chunk->getPosition();
		const glm::ivec3 ci(static_cast<int>(std::round(wp.x)) / CHUNK_SIZE, 0,
							static_cast<int>(std::round(wp.z)) / CHUNK_SIZE);
		const TaskPriority prio = calculateTaskPriority(distSq, lodThreshSq);
		chunk->setInTransit(true);
		recordChunkEvent(chunk, "meshQueued", chunk->dirtySections());

		if (distSq > lodThreshSq)
		{
			m_pendingMeshJobsCount.fetch_add(1);
			m_meshJobsDispatched.fetch_add(1, std::memory_order_relaxed);
			const auto captureEpoch = GetProfiler().captureEpoch();
			const auto queuedAt = std::chrono::steady_clock::now();
			// Identity captured at dispatch (issue #114 review): the worker
			// must not read the counters later, and a result built from
			// content newer than this capture is rejected at publish.
			const uint64_t meshGeneration = chunk->meshGeneration();
			const uint64_t meshRevision = chunk->meshRevision();
			// Fire-and-forget (review M4): results flow through
			// m_completedMeshJobs, the future would be ignored.
			m_threadPool->submit(prio, [chunk, meshGeneration, meshRevision, this, queuedAt, captureEpoch]() {
				const auto t0 = std::chrono::steady_clock::now();
				GetProfiler().addWorkerSample("MeshQueue",
					std::chrono::duration<float, std::milli>(t0 - queuedAt).count(), captureEpoch);
				// Build into a pooled result block (issue #104): the mesh
				// payload never lives on the Chunk itself.
				MeshBuildResult *result = nullptr;
				try
				{
					result = chunk->getMeshResultPool()->acquire();
				}
				catch (const std::bad_alloc &)
				{
					result = nullptr; // retry next dispatch; keep the chunk unstuck
				}
				if (result)
				{
					try
					{
						chunk->buildLODMesh(*result, meshGeneration, meshRevision);
						// Publish sizes/capacities into the pool aggregates
						// after the last vector mutation, outside the
						// MeshSample timing window.
						chunk->getMeshResultPool()->finishBuild(result);
					}
					catch (const std::bad_alloc &)
					{
						result->homePool->release(result);
						result = nullptr;
					}
				}
				const float ms = std::chrono::duration<float, std::milli>(
									 std::chrono::steady_clock::now() - t0)
									 .count();
				GetProfiler().addWorkerSample("MeshLOD", ms, captureEpoch);

				{
					std::lock_guard<std::mutex> lk(m_completedJobsMutex);
					m_completedMeshJobs.push_back({chunk, result});
				}
				m_pendingMeshJobsCount.fetch_sub(1);
			});
		}
		else
		{
			// LOCK ORDER: render lock (held by this method) -> world lock
			// (inside ensureShellPopulated). The reverse order exists in the
			// collision/mob views (world -> render); both are main-thread-only
			// today so neither lock is cross-thread contended. See the lock
			// order note in RenderWorldBridge.hpp before threading either
			// path in Phase D/E (issue #206/#207 review).
			m_world->ensureShellPopulated(chunk->worldChunk());
			ensureLightHalo(chunk, ci);
			// Borrowed for this job only: the worker detaches and returns
			// it when the build ends (success, failure or retry).
			ChunkLightHalo *halo = chunk->lightHalo();
			if (!halo)
			{
				// Halo pool allocation failed: building without cross-chunk
				// light could publish a dark seam that no future edit or
				// arrival would ever invalidate (issue #141 review round 3,
				// P2). The dirty mask was not consumed yet - skip this
				// dispatch and let a later tick retry (same backpressure as
				// the result-pool failure below).
				chunk->setInTransit(false);
				continue;
			}
			m_pendingMeshJobsCount.fetch_add(1);
			m_meshJobsDispatched.fetch_add(1, std::memory_order_relaxed);
			const auto captureEpoch = GetProfiler().captureEpoch();
			const auto queuedAt = std::chrono::steady_clock::now();
			const uint64_t meshGeneration = chunk->meshGeneration();
			const uint64_t meshRevision = chunk->meshRevision();
			// Section-selective remesh (issue #107): the dirty mask is
			// captured at dispatch like the identity counters. It expands to
			// a whole-chunk build when the chunk has no usable sectioned GPU
			// state yet (LOD promotion, first mesh after generation, fresh
			// incarnation) or when an earlier full-quality result is still
			// parked awaiting upload - its sections could not be composed
			// with a partial rebuild.
			uint16_t sectionMask = chunk->takeDirtySections();
			if (sectionMask == 0 || chunk->isLODMesh() || chunk->hasUnuploadedFullMesh())
				sectionMask = kAllSectionMask;
			m_threadPool->submit(prio, [chunk, meshGeneration, meshRevision, sectionMask, halo, this, queuedAt, captureEpoch]() {
				const auto t0 = std::chrono::steady_clock::now();
				GetProfiler().addWorkerSample("MeshQueue",
					std::chrono::duration<float, std::milli>(t0 - queuedAt).count(), captureEpoch);
				MeshBuildResult *result = nullptr;
				try
				{
					result = chunk->getMeshResultPool()->acquire();
				}
				catch (const std::bad_alloc &)
				{
					result = nullptr; // retry next dispatch; keep the chunk unstuck
				}
				if (!result)
				{
					// The mask was consumed at dispatch: re-arm it so the
					// sections are rebuilt on the retry (issue #107).
					chunk->markSectionsDirty(sectionMask);
				}
				if (result)
				{
					try
					{
						chunk->buildMesh(*result, meshGeneration, meshRevision, sectionMask);
						chunk->getMeshResultPool()->finishBuild(result);
					}
					catch (const std::bad_alloc &)
					{
						result->homePool->release(result);
						chunk->markSectionsDirty(sectionMask);
						result = nullptr;
					}
				}
				// Detach the halo borrow while the chunk is still in transit
				// (no main-thread hand can touch it) so the pooled block
				// serves the next job (issue #141 review fix).
				chunk->setLightHalo(nullptr);
				if (halo)
					m_lightHaloPool.release(halo);
				const float ms = std::chrono::duration<float, std::milli>(
									 std::chrono::steady_clock::now() - t0)
									 .count();
				GetProfiler().addWorkerSample("MeshBuild", ms, captureEpoch);

				{
					std::lock_guard<std::mutex> lk(m_completedJobsMutex);
					m_completedMeshJobs.push_back({chunk, result});
				}
				m_pendingMeshJobsCount.fetch_sub(1);
			});
		}
	}
}

void RenderWorldBridge::updateEntityLightCaches(const Camera &camera, const RenderSettings &settings, int budget)
{
	if (!m_threadPool || budget <= 0)
		return;

	struct Item
	{
		Chunk *chunk;
		float distSq;
	};

	std::lock_guard<std::shared_mutex> lock(m_mutex);
	thread_local std::vector<Item> queue;
	queue.clear();
	queue.reserve(m_activeChunks.size());
	const glm::vec3 camPos = camera.getPosition();
	const float camOffsetX = camPos.x - CHUNK_SIZE * 0.5f;
	const float camOffsetZ = camPos.z - CHUNK_SIZE * 0.5f;

	// Cache acquisition is decoupled from meshing (issue #172): only
	// already-MESHED, idle chunks enter. GENERATED chunks populate their
	// cache from the mesh build they are already waiting for, and in-transit
	// chunks are owned by their current worker job.
	for (Chunk *chunk : m_activeChunks)
	{
		if (chunk->getState() == ChunkState::MESHED && !chunk->isInTransit() &&
			!chunk->isReplicaRetiring() && chunk->localLightCacheWanted() && !chunk->hasLightStorage())
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

	const float lodThresh = static_cast<float>(settings.minRenderDistance) * 2.f;
	const float lodThreshSq = lodThresh * lodThresh;

	for (int i = 0; i < n; ++i)
	{
		Chunk *chunk = queue[i].chunk;
		const glm::vec3 wp = chunk->getPosition();
		const glm::ivec3 ci(static_cast<int>(std::round(wp.x)) / CHUNK_SIZE, 0,
							static_cast<int>(std::round(wp.z)) / CHUNK_SIZE);
		const TaskPriority prio = calculateTaskPriority(queue[i].distSq, lodThreshSq);
		// Cross-chunk light context for the light field (issue #172
		// equivalence): computeLightField reads own voxels + the halo ring
		// only, so unlike a mesh dispatch there is no neighbor shell to
		// populate - the border layout exists for greedy face culling.
		// haloFill timing reports under lightCache.* (issue #173 review).
		ensureLightHalo(chunk, ci, telemetry::LightCacheFamily);
		ChunkLightHalo *halo = chunk->lightHalo();
		if (!halo)
		{
			// Halo pool exhausted: retry a later tick (same backpressure as
			// the mesh path). The chunk keeps its valid mesh and stays
			// MESHED - only the cache fill waits.
			continue;
		}
		// inTransit doubles as the exclusive worker-ownership flag here: no
		// mesh job, edit or unload can touch this chunk until the light
		// result is published (or discarded) on the main thread.
		chunk->setInTransit(true);
		m_pendingLightJobsCount.fetch_add(1);
		m_lightJobsDispatched.fetch_add(1, std::memory_order_relaxed);
		const auto captureEpoch = GetProfiler().captureEpoch();
		const auto queuedAt = std::chrono::steady_clock::now();
		const sim::ChunkIdentity identity = chunk->chunkIdentity();
		m_threadPool->submit(prio, [chunk, identity, halo, this, queuedAt, captureEpoch]() {
			const auto t0 = std::chrono::steady_clock::now();
			// Own queue bucket (issue #173 review): light-cache wait time is
			// observable separately from real mesh jobs, so pool starvation
			// of one pipeline cannot hide inside the other's numbers.
			GetProfiler().addWorkerSample("LightCacheQueue",
				std::chrono::duration<float, std::milli>(t0 - queuedAt).count(), captureEpoch);
			// Worker stays read-only on published chunk state (issue #172):
			// the only products are the pooled storage block and telemetry.
			ChunkLightPool *pool = chunk->getLightPool();
			ChunkLightStorage *storage = nullptr;
			if (pool)
			{
				try
				{
					storage = pool->acquire();
				}
				catch (const std::bad_alloc &)
				{
					storage = nullptr; // retry a later tick once published
				}
			}
			if (storage)
			{
				try
				{
					chunk->buildLightCache(*storage);
				}
				catch (const std::bad_alloc &)
				{
					pool->release(storage);
					storage = nullptr;
				}
			}
			// Detach the halo borrow while the chunk is still in transit
			// (no main-thread hand can touch it), same contract as mesh jobs.
			chunk->setLightHalo(nullptr);
			if (halo)
				m_lightHaloPool.release(halo);
			const float ms = std::chrono::duration<float, std::milli>(
								 std::chrono::steady_clock::now() - t0)
								 .count();
			GetProfiler().addWorkerSample("LightCache", ms, captureEpoch);

			{
				std::lock_guard<std::mutex> lk(m_completedJobsMutex);
				m_completedLightJobs.push_back({chunk, storage, pool, identity});
			}
			m_pendingLightJobsCount.fetch_sub(1);
		});
	}
}

// ---------------------------------------------------------------------------
// GPU uploads (derived; unchanged)
// ---------------------------------------------------------------------------

int RenderWorldBridge::uploadPendingMeshes(VmaAllocator allocator, StagingRing &staging, VkCommandBuffer cmd,
                                      GpuResourceRetire &retire, MeshArenas &arenas, const Camera &camera, int budget,
                                      bool *outCopiesRecorded)
{
	// Copy tracking (review M2): set on EVERY recorded transfer, independent
	// of publication — the host barrier keys on this flag.
	if (outCopiesRecorded)
		*outCopiesRecorded = false;
	if (!allocator || budget <= 0 || cmd == VK_NULL_HANDLE || !staging.isValid())
		return 0;

	struct Item
	{
		Chunk *chunk;
		float distSq;
	};
	thread_local std::vector<Item> queue;
	queue.clear();
	{
		std::shared_lock<std::shared_mutex> lock(m_mutex);
		queue.reserve(m_activeChunks.size());
		const glm::vec3 camPos = camera.getPosition();
		const float camOffsetX = camPos.x - CHUNK_SIZE * 0.5f;
		const float camOffsetZ = camPos.z - CHUNK_SIZE * 0.5f;
		for (Chunk *chunk : m_activeChunks)
		{
			if (!chunk || chunk->isReplicaRetiring())
				continue;
			if (chunk->getState() == ChunkState::MESHED && chunk->needsGPUUpload() && !chunk->isInTransit())
			{
				const glm::vec3 p = chunk->getPosition();
				const float dx = p.x - camOffsetX;
				const float dz = p.z - camOffsetZ;
				queue.push_back({chunk, dx * dx + dz * dz});
			}
		}
	}

	if (queue.empty())
		return 0;

	// Select only the nearest candidates needed to spend this frame's COPY
	// budget. Group members are added as an allowance because several queue
	// entries may collapse to one group, or a not-ready group may be skipped;
	// they must not prevent an independent chunk just beyond the base budget
	// from progressing. This restores the bounded historical partial sort
	// without weakening the Round 4 non-blocking group scheduler.
	size_t candidateCount = std::min(queue.size(), static_cast<size_t>(budget));
	for (const PendingMeshCommitGroup &group : m_commitGroups)
	{
		candidateCount += std::min(group.members.size(), queue.size() - candidateCount);
		if (candidateCount == queue.size())
			break;
	}
	std::partial_sort(queue.begin(), queue.begin() + candidateCount, queue.end(),
	                  [](const Item &a, const Item &b) { return a.distSq < b.distSq; });

	// Distance-prioritized. Budget units gate GPU COPY work: a group's
	// members record their copies progressively across frames (one member
	// per unit, as staging allows), and the publication itself is CPU-only
	// and free, so per-sec upload settings can neither make an atomic
	// border commit structurally impossible nor starve it once every
	// replacement is resident (PR #178 review round 4).
	int budgetUnits = budget;
	int uploaded = 0;
	// Optimization: For small collections in hot loops, std::vector + linear search
	// yields better cache locality and avoids dynamic heap allocations for hash nodes.
	thread_local std::vector<Chunk *> published;
	published.clear();
	thread_local std::vector<uint64_t> processedGroups;
	processedGroups.clear();
	for (size_t i = 0; i < candidateCount; ++i)
	{
		Chunk *chunk = queue[i].chunk;
		if (std::find(published.begin(), published.end(), chunk) != published.end())
			continue;

		PendingMeshCommitGroup *group = commitGroupFor(chunk);
		if (!group)
		{
			if (budgetUnits <= 0)
				continue; // budget exhausted: remaining independents wait next frame
			if (!chunk->uploadToGPUAsync(allocator, staging, cmd, retire, arenas))
				break; // staging full — remaining wait next frame
			if (outCopiesRecorded)
				*outCopiesRecorded = true;
			recordChunkEvent(chunk, "gpuCommit");
			uploaded += 1;
			--budgetUnits;
			continue;
		}
		if (std::find(processedGroups.begin(), processedGroups.end(), group->groupId) != processedGroups.end())
			continue; // every group state machine advances at most once per frame
		processedGroups.push_back(group->groupId);

		// Only stable values needed after publication are snapshotted: the
		// state machine mutates the live member entries (replacements), and
		// nothing below touches `group` after the publish (and therefore
		// the group erase) has run.
		std::vector<PendingGroupMember> &members = group->members;
		const uint64_t groupId = group->groupId;

		// Supersession (round 4, item 8): a member re-edited after its
		// replacement was prepared invalidates that replacement. Discard it
		// and drop the group back to WaitingForMeshes - the newest result
		// will prepare, upload and publish in its place.
		bool superseded = false;
		for (PendingGroupMember &member : members)
		{
			if (member.replacement.valid &&
			    (member.chunk->meshGeneration() != member.replacement.generation ||
			     member.chunk->meshRevision() != member.replacement.revision ||
			     member.chunk->getState() != ChunkState::MESHED))
			{
				member.chunk->discardGPUUpload(member.replacement);
				superseded = true;
			}
		}
		if (superseded)
			group->state = CommitGroupState::WaitingForMeshes;

		if (group->state == CommitGroupState::WaitingForMeshes)
		{
			// Gate: every geometric member must carry a publishable newest
			// result. Until then the group is skipped (without consuming
			// budget) and the renderer keeps showing old A + old B; the
			// blocked group never stops independent chunks.
			bool ready = true;
			for (const PendingGroupMember &member : members)
			{
				if (!member.chunk || member.chunk->getState() != ChunkState::MESHED ||
				    !member.chunk->needsGPUUpload() || member.chunk->isInTransit() ||
				    !member.chunk->hasPendingMeshResult())
				{
					ready = false;
					break;
				}
			}
			if (!ready)
				continue;

			// ALLOCATE for every member without a live replacement (each
			// fully rolled back on its own failure; members that already hold
			// a valid replacement keep it).
			std::vector<size_t> freshlyPrepared;
			bool allPrepared = true;
			for (size_t k = 0; k < members.size() && allPrepared; ++k)
			{
				if (!members[k].replacement.valid)
				{
					if (members[k].chunk->prepareGPUUpload(arenas, members[k].replacement))
						freshlyPrepared.push_back(k);
					else
						allPrepared = false;
				}
			}
			if (!allPrepared)
			{
				for (size_t k : freshlyPrepared)
					members[k].chunk->discardGPUUpload(members[k].replacement);
				telemetry::registry().add(telemetry::UploadDeferred);
				continue; // arena exhaustion — the group waits next frame
			}
			group->state = CommitGroupState::Uploading;
		}

		if (group->state == CommitGroupState::Uploading)
		{
			// COPY: each successfully recorded member consumes one budget
			// unit. Members that do not fit, or for which the per-frame budget
			// is exhausted, stay for a later frame while old meshes render.
			bool allRecorded = true;
			for (PendingGroupMember &member : members)
			{
				if (member.replacement.recorded)
					continue;
				if (budgetUnits <= 0)
				{
					allRecorded = false;
					continue;
				}
				if (!member.chunk->recordGPUUpload(allocator, staging, cmd, arenas,
				                                   member.replacement))
				{
					allRecorded = false; // no staging room for this member yet
					continue;
				}
				if (outCopiesRecorded)
					*outCopiesRecorded = true;
				--budgetUnits;
			}
			if (allRecorded)
				group->state = CommitGroupState::ReadyToPublish;
			if (group->state != CommitGroupState::ReadyToPublish)
				continue; // still uploading — old meshes render everywhere
		}

		// ReadyToPublish: CPU-only, consumes no budget, atomic across all
		// members - the previous frame drew old A..E, this frame draws
		// new A..E, with no mixed state in between.
		for (PendingGroupMember &member : members)
		{
			member.chunk->publishGPUUpload(retire, arenas, member.replacement);
			recordChunkEvent(member.chunk, "gpuCommit");
			telemetry::registry().add(telemetry::UploadChunks);
			published.push_back(member.chunk);
		}
		// The fused-group invariant makes this exact: no other active group
		// shares a member with the one just published.
		const int publishedCount = static_cast<int>(members.size());
		eraseCommitGroup(groupId);
		uploaded += publishedCount;
	}
	return uploaded;
}

// ---------------------------------------------------------------------------
// Deferred GPU retirement (render-only; edit capture moved to world unload)
// ---------------------------------------------------------------------------

void RenderWorldBridge::processDeferredReleases()
{
	constexpr size_t kMaxRetainedLightBlocks = 64;
	if (m_chunkPool && m_chunkPool->lightStorageFree() > kMaxRetainedLightBlocks)
	{
		m_chunkPool->lightPool().trim(kMaxRetainedLightBlocks);
	}

	if (m_deferredRelease.empty())
	{
		m_deferredReleaseAge = 0;
		return;
	}

	++m_deferredReleaseAge;
	if (m_deferredReleaseAge < kDeferredReleaseFrames)
		return;

	// No device idle: hand GPU buffers to the retire queue, then recycle the
	// render handle. The authoritative edits were captured by the world at
	// unload (issue #206); the record is already recycled.
	for (Chunk *c : m_deferredRelease)
	{
		if (!c)
			continue;
		recordChunkEvent(c, "unload");
		c->releaseGPUDeferred();
		if (m_chunkPool)
			m_chunkPool->release(c);
	}
	m_deferredRelease.clear();
	m_deferredReleaseAge = 0;
}

// ---------------------------------------------------------------------------
// Job publication: world generation → render mesh/light → deferred edits
// (the issue #114 ordering is preserved: mesh publish sees the pending-edit
// queue BEFORE applyPendingEdits lands the edits)
// ---------------------------------------------------------------------------

void RenderWorldBridge::processFinishedJobs()
{
	// Stage 1: authoritative generation publication (+ Generated events →
	// render flags and cross-chunk light arrival invalidation).
	m_world->publishFinishedGeneration();
	drainWorldEvents();

	// Stage 2: render mesh/light publication.
	std::vector<CompletedMeshJob> finishedMesh;
	std::vector<CompletedLightJob> finishedLight;
	{
		std::lock_guard<std::mutex> lock(m_completedJobsMutex);
		finishedMesh.swap(m_completedMeshJobs);
		finishedLight.swap(m_completedLightJobs);
	}

	for (const CompletedMeshJob &job : finishedMesh)
	{
		// Publish before clearing in-transit so a recycled chunk can never
		// race this decision (issue #104): stale results are refused and
		// returned to their pool; a null result just unsticks the chunk.
		// A queued edit made while this job ran supersedes the result
		// outright: the mesh was built from pre-edit content (issue #114).
		const bool supersededByEdit =
			job.chunk && m_world->hasPendingEditsFor(job.chunk->worldChunk());
		if (job.result)
		{
			if (job.chunk && !supersededByEdit)
			{
				job.chunk->publishMeshResult(job.result);
				recordChunkEvent(job.chunk, "meshDone");
			}
			else
			{
				// A superseded section build must re-arm the sections it
				// rebuilt (PR #117 review): the queued edit only re-arms
				// its own sections when it is applied below, so without
				// this the dropped job's sections would keep their stale
				// GPU meshes (old dirty A + new edit B => next mask A|B).
				if (job.chunk)
					job.chunk->markSectionsDirty(job.result->sectionsBuilt);
				job.result->homePool->release(job.result);
			}
		}
		if (job.chunk)
			job.chunk->setInTransit(false);
		// Neighbor-light invalidation may land while a mesh job owns the
		// chunk (an edit racing the build, a chunk arriving mid-mesh - issue
		// #141 review round 2): the section mask persisted through the
		// in-flight job, so re-arm scheduling now that publish is done.
		if (job.chunk && job.chunk->getState() == ChunkState::MESHED &&
			job.chunk->dirtySections() != 0)
			job.chunk->setState(ChunkState::GENERATED);
		// Entity light-cache residency is no longer a mesh invalidation
		// (issue #172): a MESHED chunk that still lacks its cache is served
		// by updateEntityLightCaches()'s dedicated light-only job, never by
		// a GENERATED re-arm here.
	}
	// Publish completed entity light-cache jobs (issue #172). Same identity
	// contract as mesh results: storage built for a retired incarnation,
	// from older voxel/border content, or superseded by a queued edit is
	// discarded; the block returns to its pool and the chunk keeps its
	// valid mesh untouched.
	for (const CompletedLightJob &job : finishedLight)
	{
		if (!job.chunk)
		{
			if (job.storage && job.pool)
				job.pool->release(job.storage);
			continue;
		}
		if (job.storage)
		{
			const bool valid = job.chunk->meshGeneration() == job.identity.generation &&
							   job.chunk->meshRevision() == job.identity.revision &&
							   job.chunk->localLightCacheWanted() &&
							   !job.chunk->hasLightStorage() &&
							   !m_world->hasPendingEditsFor(job.chunk->worldChunk());
			if (valid)
			{
				job.chunk->attachLightStorage(job.storage);
				recordChunkEvent(job.chunk, "lightCacheDone");
			}
			else
				job.pool->release(job.storage);
		}
		job.chunk->setInTransit(false);
		// An edit may have dirtied sections while this job owned the chunk:
		// the mask persisted through transit, so re-arm scheduling now that
		// the chunk is free again (same rule as the mesh path above).
		if (job.chunk->getState() == ChunkState::MESHED &&
			job.chunk->dirtySections() != 0)
			job.chunk->setState(ChunkState::GENERATED);
	}

	// Stage 3: land deferred authoritative edits (+ EditApplied events →
	// render section/light invalidation and the GENERATED re-arm).
	m_world->applyPendingEdits();
	drainWorldEvents();
}

// ---------------------------------------------------------------------------
// Visibility / draw lists (derived; unchanged)
// ---------------------------------------------------------------------------

void RenderWorldBridge::updateVisibility(const Camera &camera, int windowWidth, int windowHeight,
									const RenderSettings &settings)
{
	// Timed by Engine PROFILE_SCOPE("Visibility") — keep body lean.

	const float farPlane = computeCameraFarPlane(settings.maxRenderDistance);
	const glm::mat4 clipMatrix =
		camera.getProjectionMatrix(static_cast<float>(windowWidth), static_cast<float>(windowHeight),
								   farPlane) *
		camera.getViewMatrix();

	std::array<glm::vec4, 6> planes{};
	planes[0] = glm::row(clipMatrix, 3) + glm::row(clipMatrix, 0);
	planes[1] = glm::row(clipMatrix, 3) - glm::row(clipMatrix, 0);
	planes[2] = glm::row(clipMatrix, 3) + glm::row(clipMatrix, 1);
	planes[3] = glm::row(clipMatrix, 3) - glm::row(clipMatrix, 1);
	planes[4] = glm::row(clipMatrix, 3) + glm::row(clipMatrix, 2);
	planes[5] = glm::row(clipMatrix, 3) - glm::row(clipMatrix, 2);

	std::array<glm::vec3, 6> planeNormals;
	std::array<float, 6> planeOffsets;
	for (size_t i = 0; i < 6; ++i)
	{
		auto &p = planes[i];
		const float len = glm::length(glm::vec3(p));
		if (len > 1e-6f)
			p /= len;

		planeNormals[i] = glm::vec3(p);
		const glm::vec3 optOffset(
			p.x >= 0.f ? static_cast<float>(CHUNK_SIZE) : 0.f,
			p.y >= 0.f ? static_cast<float>(CHUNK_HEIGHT) : 0.f,
			p.z >= 0.f ? static_cast<float>(CHUNK_SIZE) : 0.f
		);
		planeOffsets[i] = p.w + glm::dot(planeNormals[i], optOffset);
	}

	const glm::vec3 camPos = camera.getPosition();
	const float camOffsetX = camPos.x - CHUNK_SIZE * 0.5f;
	const float camOffsetZ = camPos.z - CHUNK_SIZE * 0.5f;

	std::shared_lock<std::shared_mutex> lock(m_mutex);
	for (Chunk *chunk : m_activeChunks)
	{
		const glm::vec3 aabbMin = chunk->getPosition();

		bool visible = true;
		for (size_t i = 0; i < 6; ++i)
		{
			if (glm::dot(planeNormals[i], aabbMin) + planeOffsets[i] < 0.f)
			{
				visible = false;
				break;
			}
		}
		chunk->setVisible(visible);

		const float dx = aabbMin.x - camOffsetX;
		const float dz = aabbMin.z - camOffsetZ;
		updateEntityLightCacheIntent(*chunk, dx * dx + dz * dz);
	}
}

void RenderWorldBridge::updateEntityLightCacheIntent(Chunk &chunk, float distSq)
{
	// Acquire/release hysteresis (issue #172): outside chunks acquire at the
	// 128m radius; held caches are retained until the chunk leaves the
	// larger release radius, so camera movement around the acquire edge
	// cannot oscillate allocate/free cycles of the 128 KiB blocks.
	const bool previous = chunk.localLightCacheWanted();
	const bool wanted = previous ? (distSq <= kEntityLightCacheReleaseRadiusSq)
								 : (distSq <= kEntityLightCacheAcquireRadiusSq);
	if (previous && !wanted)
	{
		chunk.releaseLightStorage();
	}
	chunk.setLocalLightCacheWanted(wanted);
}

void RenderWorldBridge::collectDrawList(std::vector<Chunk *> &out) const
{
	out.clear();
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	out.reserve(m_activeChunks.size());
	for (Chunk *chunk : m_activeChunks)
	{
		if (!chunk || chunk->isReplicaRetiring())
			continue;
		// Keep drawing the last GPU mesh while a remesh/upload is pending so
		// break/place doesn't make the chunk pop out of existence.
		if (chunk->getOpaqueIndexCount() == 0 && !chunk->hasWaterMesh())
			continue;
		if (!chunk->isVisible())
			continue;
		out.push_back(chunk);
	}
}

void RenderWorldBridge::collectShadowList(std::vector<Chunk *> &out, const Camera &camera,
									 float shadowRadius) const
{
	out.clear();
	const float r2 = shadowRadius * shadowRadius;
	const glm::vec3 cam = camera.getPosition();
	const float camOffsetX = cam.x - CHUNK_SIZE * 0.5f;
	const float camOffsetZ = cam.z - CHUNK_SIZE * 0.5f;
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	out.reserve(m_activeChunks.size() / 2 + 8);
	for (Chunk *chunk : m_activeChunks)
	{
		if (!chunk || chunk->isReplicaRetiring())
			continue;
		if (chunk->getOpaqueIndexCount() == 0)
			continue;
		const glm::vec3 p = chunk->getPosition();
		const float dx = p.x - camOffsetX;
		const float dz = p.z - camOffsetZ;
		if (dx * dx + dz * dz > r2)
			continue;
		out.push_back(chunk);
	}
}

// ---------------------------------------------------------------------------
// Logical edits (delegated; commit groups stay render-side)
// ---------------------------------------------------------------------------

bool RenderWorldBridge::deleteVoxel(const glm::vec3 &worldPos)
{
	const WorldEditResult result = m_world->deleteVoxel(worldPos);
	publishEditResult(result);
	return result.accepted;
}

bool RenderWorldBridge::placeVoxel(const glm::vec3 &worldPos, TextureType type)
{
	const WorldEditResult result = m_world->placeVoxel(worldPos, type);
	publishEditResult(result);
	return result.accepted;
}

void RenderWorldBridge::publishEditResult(const WorldEditResult &result)
{
	drainWorldEvents();
	// A border edit couples the geometry of the target and its mirrors:
	// their GPU publications must commit together or not at all (PR #178
	// review). Light-only invalidated neighbors never join. Overlapping
	// groups (edits sharing chunks) are fused at registration.
	if (result.accepted && result.geometricMembers.size() > 1)
	{
		std::vector<PendingGroupMember> members;
		for (const glm::ivec3 &coord : result.geometricMembers)
		{
			Chunk *member = getChunk(coord);
			if (member)
				members.push_back({member, {}});
		}
		if (members.size() > 1)
			registerCommitGroup(result.editId, std::move(members));
	}
}

bool RenderWorldBridge::isVoxelActive(const glm::vec3 &worldPos) const
{
	return m_world->isVoxelActive(worldPos);
}

Chunk *RenderWorldBridge::getChunk(const glm::ivec3 &chunkPos)
{
	// Lock-free by contract (review §4.6, the VoxelWorld pattern): non-const
	// callers are main-thread paths that already hold m_mutex or run outside
	// concurrent mutation windows; a shared_lock here would deadlock the
	// locked helpers (std::shared_mutex is not recursive). Cross-thread
	// readers use the const overload, which locks.
	auto it = m_chunks.find(chunkPos);
	return it != m_chunks.end() ? it->second : nullptr;
}

const Chunk *RenderWorldBridge::getChunk(const glm::ivec3 &chunkPos) const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	auto it = m_chunks.find(chunkPos);
	return it != m_chunks.end() ? it->second : nullptr;
}

Chunk *RenderWorldBridge::getChunkAtWorldPos(const glm::vec3 &worldPos)
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	return getChunk(worldToChunkCoord(worldPos));
}

// ---------------------------------------------------------------------------
// Entity light sampling (derived caches; unchanged)
// ---------------------------------------------------------------------------

lighting::LocalVoxelLight RenderWorldBridge::sampleVoxelLightUnlocked(const glm::ivec3 &p) const
{
	if (p.y >= static_cast<int>(CHUNK_HEIGHT))
		return {1.0f, glm::vec3(0.0f)};
	if (p.y < 0)
		return {0.0f, glm::vec3(0.0f)};
	if (p.x < SHRT_MIN || p.x >= SHRT_MAX || p.z < SHRT_MIN || p.z >= SHRT_MAX)
		return {0.0f, glm::vec3(0.0f)};

	const int size = static_cast<int>(CHUNK_SIZE);
	const int cx = p.x / size - (p.x % size < 0 ? 1 : 0);
	const int cz = p.z / size - (p.z % size < 0 ? 1 : 0);
	const auto it = m_chunks.find({cx, 0, cz});
	if (it == m_chunks.end() || !it->second)
		return {0.0f, glm::vec3(0.0f)};

	const Chunk *chunk = it->second;
	if (!chunk->hasLightStorage())
		return {0.0f, glm::vec3(0.0f)};

	const int lx = p.x - cx * size;
	const int lz = p.z - cz * size;
	return chunk->sampleLight(lx, p.y, lz);
}

lighting::LocalVoxelLight RenderWorldBridge::sampleVoxelLight(const glm::ivec3 &blockPos) const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	return sampleVoxelLightUnlocked(blockPos);
}

lighting::LocalVoxelLight RenderWorldBridge::sampleSmoothedLightUnlocked(const glm::vec3 &worldPos) const
{
	const glm::vec3 s = worldPos - glm::vec3(0.5f);
	const int x0 = static_cast<int>(std::floor(s.x));
	const int y0 = static_cast<int>(std::floor(s.y));
	const int z0 = static_cast<int>(std::floor(s.z));
	const int x1 = x0 + 1;
	const int y1 = y0 + 1;
	const int z1 = z0 + 1;

	const float fx = s.x - static_cast<float>(x0);
	const float fy = s.y - static_cast<float>(y0);
	const float fz = s.z - static_cast<float>(z0);

	const auto c000 = sampleVoxelLightUnlocked({x0, y0, z0});
	const auto c100 = sampleVoxelLightUnlocked({x1, y0, z0});
	const auto c010 = sampleVoxelLightUnlocked({x0, y1, z0});
	const auto c110 = sampleVoxelLightUnlocked({x1, y1, z0});
	const auto c001 = sampleVoxelLightUnlocked({x0, y0, z1});
	const auto c101 = sampleVoxelLightUnlocked({x1, y0, z1});
	const auto c011 = sampleVoxelLightUnlocked({x0, y1, z1});
	const auto c111 = sampleVoxelLightUnlocked({x1, y1, z1});

	const auto c00 = lighting::lerpLocalVoxelLight(c000, c100, fx);
	const auto c10 = lighting::lerpLocalVoxelLight(c010, c110, fx);
	const auto c01 = lighting::lerpLocalVoxelLight(c001, c101, fx);
	const auto c11 = lighting::lerpLocalVoxelLight(c011, c111, fx);

	const auto c0 = lighting::lerpLocalVoxelLight(c00, c10, fy);
	const auto c1 = lighting::lerpLocalVoxelLight(c01, c11, fy);

	return lighting::lerpLocalVoxelLight(c0, c1, fz);
}

lighting::LocalVoxelLight RenderWorldBridge::sampleSmoothedLight(const glm::vec3 &worldPos) const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	return sampleSmoothedLightUnlocked(worldPos);
}

// ---------------------------------------------------------------------------
// Counters / inspection
// ---------------------------------------------------------------------------

size_t RenderWorldBridge::chunkCount() const
{
	return m_world->chunkCount();
}

size_t RenderWorldBridge::pendingLoadCount() const
{
	return m_world->pendingLoadCount();
}

size_t RenderWorldBridge::pendingGenJobs() const
{
	return m_world->pendingGenJobs();
}

size_t RenderWorldBridge::pendingMeshJobs() const
{
	return m_pendingMeshJobsCount.load();
}

size_t RenderWorldBridge::pendingLightJobs() const
{
	return m_pendingLightJobsCount.load();
}

// ---------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------

bool RenderWorldBridge::prepareAndGenerateChunk(Chunk *chunk, TerrainGenerator &generator)
{
	if (!chunk)
		return false;
	WorldChunk *record = chunk->worldChunk();
	// World-resident records (bound handles AND adopted test records) use
	// the authoritative contract: prepare + generate + saved overrides.
	const bool worldResident = record && m_world->getChunk(record->coord()) == record;
	if (worldResident)
	{
		if (!m_world->prepareAndGenerateChunk(record, generator))
		{
			// Allocation failure: the world recycled the record; return the
			// render handle to the pool so no slot is leaked (issue #112).
			chunk->unbindWorldChunk();
			if (m_chunkPool)
				m_chunkPool->release(chunk);
			return false;
		}
		chunk->notifyTerrainGenerated();
		return true;
	}
	// Standalone (tests/tools): shared synchronous bootstrap contract
	// (issue #112) on the privately owned record; no persistence.
	if (!chunk->prepareVoxelStorageForGeneration())
	{
		if (m_chunkPool)
			m_chunkPool->release(chunk);
		return false;
	}
	chunk->generateTerrain(generator);
	return true;
}

void RenderWorldBridge::generateInitialArea(const glm::vec3 &center, int radiusChunks, VmaAllocator allocator,
						 ImmediateCommands &imm, MeshArenas &arenas)
{
	if (!m_chunkPool || !allocator)
		return;

	// Square footprint (2r+1)^2 plus a little margin for safety. Bootstrap is
	// a one-shot synchronous setup: grow in a single call (maxGrowPerCall = 0)
	// instead of the incremental per-tick cap the streaming path uses.
	const size_t bootstrapNeed =
		static_cast<size_t>(2 * radiusChunks + 1) * static_cast<size_t>(2 * radiusChunks + 1) + 16;
	m_chunkPool->ensureCapacity(bootstrapNeed, 0);

	// Authoritative voxel bootstrap (world-owned), then render binding.
	const std::vector<glm::ivec3> created = m_world->generateInitialAreaVoxels(center, radiusChunks);
	buildBootstrapMeshes(center, created, allocator, imm, arenas);
}

void RenderWorldBridge::meshResidentArea(const glm::vec3 &center, int radiusChunks, VmaAllocator allocator,
	ImmediateCommands &imm, MeshArenas &arenas)
{
	if (!m_chunkPool || !allocator)
		return;
	std::vector<glm::ivec3> resident;
	const auto origin = worldToChunkCoord(center);
	for (int z = -radiusChunks; z <= radiusChunks; ++z)
		for (int x = -radiusChunks; x <= radiusChunks; ++x)
		{
			const auto pos = origin + glm::ivec3(x, 0, z);
			const auto *record = m_world->getChunk(pos);
			if (record && record->getState() >= ChunkState::GENERATED)
				resident.push_back(pos);
		}
	buildBootstrapMeshes(center, resident, allocator, imm, arenas);
}

void RenderWorldBridge::buildBootstrapMeshes(const glm::vec3 &center,
	const std::vector<glm::ivec3> &positions, VmaAllocator allocator, ImmediateCommands &imm, MeshArenas &arenas)
{
	std::vector<std::pair<glm::ivec3, Chunk *>> bound;
	{
		std::lock_guard<std::shared_mutex> lock(m_mutex);
		for (const glm::ivec3 &pos : positions)
		{
			const auto found = m_chunks.find(pos);
			Chunk *chunk = found != m_chunks.end() ? found->second : nullptr;
			if (!chunk)
			{
				WorldChunk *record = m_world->getChunk(pos);
				if (!record)
					continue;
				chunk = bindRecord(pos, record);
				if (!chunk)
				{
					m_chunkPool->ensureCapacity(m_chunkPool->capacity() + 64);
					chunk = bindRecord(pos, record);
					if (!chunk)
						continue;
				}
			}
			const glm::vec3 p = chunk->getPosition();
			const float dx = p.x - center.x;
			const float dz = p.z - center.z;
			updateEntityLightCacheIntent(*chunk, dx * dx + dz * dz);
			bound.emplace_back(pos, chunk);
		}
		for (const auto &p : bound)
			m_world->ensureShellPopulated(p.second->worldChunk());
		// Bootstrap meshing gets cross-chunk light too: every created chunk
		// is generated and registered by now, so the halo snapshots see the
		// full neighborhood (issue #141 review fix).
		for (const auto &p : bound)
			ensureLightHalo(p.second, p.first);
	}
	// Consume the Loaded events emitted by the world bootstrap (the handles
	// are already bound above; drain keeps the stream empty and records the
	// lifecycle trace).
	drainWorldEvents();

	for (const auto &p : bound)
	{
		p.second->generateMesh();
		ChunkLightHalo *halo = p.second->lightHalo();
		p.second->setLightHalo(nullptr);
		if (halo)
			m_lightHaloPool.release(halo);
	}

	// One-time sync upload before the first frame (nothing in flight yet).
	for (const auto &p : bound)
		p.second->uploadToGPU(allocator, imm, arenas);

	std::cout << "Bootstrap: " << bound.size() << " chunks around spawn\n";
}

// ---------------------------------------------------------------------------
// Persistent world lifecycle (delegated; issue #180 contracts preserved)
// ---------------------------------------------------------------------------

bool RenderWorldBridge::openWorld(const std::string &savesRoot, const std::string &worldName,
                             std::string &outError)
{
	return m_world->openWorld(savesRoot, worldName, outError);
}

bool RenderWorldBridge::renderJobsBusy() const
{
	return m_pendingMeshJobsCount.load() > 0 || m_pendingLightJobsCount.load() > 0;
}

void RenderWorldBridge::drainAsyncJobsForPersistence()
{
	// Combined durability barrier (issue #180 review round 2): publish every
	// finished gen/mesh/light job, apply every deferred voxel edit and age
	// deferred GPU releases, so the final persistence capture sees the
	// complete authoritative voxel state and no worker can dereference the
	// persistence facade after it is released. Deliberately NO timeout —
	// closing a persistent world is a durability barrier.
	for (;;)
	{
		processFinishedJobs();     // world gen + render mesh/light + deferred edits
		processDeferredReleases(); // ages deferred GPU releases
		if (!renderJobsBusy() && !m_world->asyncBusy())
			break;
		std::this_thread::sleep_for(std::chrono::microseconds(500));
	}
	// One final publish pass so edits deferred until the very last job land.
	processFinishedJobs();
}

bool RenderWorldBridge::closeWorld()
{
	if (!m_world->isWorldOpen())
		return true;
	// Quiesce render + world BEFORE the world's capture/flush/shutdown.
	drainAsyncJobsForPersistence();
	return m_world->closeWorld();
}

bool RenderWorldBridge::flushWorld()
{
	return m_world->flushWorld();
}

// ---------------------------------------------------------------------------
// Cross-chunk light invalidation (derived render scheduling; unchanged)
// ---------------------------------------------------------------------------

void RenderWorldBridge::ensureLightHalo(Chunk *chunk, const glm::ivec3 &chunkIdx,
								   telemetry::Family telemetryFamily)
{
	if (!chunk)
		return;
	ChunkLightHalo *halo = chunk->lightHalo();
	if (!halo)
	{
		try
		{
			halo = m_lightHaloPool.acquire();
		}
		catch (const std::bad_alloc &)
		{
			return; // caller skips the dispatch; a later tick retries
		}
		chunk->setLightHalo(halo);
	}
	// Caller holds exclusive lock.
	{
		// The ring snapshot is main-thread dispatch cost (issue #141 review
		// round 2, P2): measured as its own stage so the benchmark reports
		// haloFill avg/p95 next to the worker-side mesh stages. Routed to the
		// calling pipeline's telemetry family (issue #173 review).
		telemetry::StageSample haloFillSample(telemetry::HaloFill, telemetryFamily);
		fillLightHaloFromNeighbors(
			*halo, chunk,
			getChunk(chunkIdx + glm::ivec3(-1, 0, 0)),
			getChunk(chunkIdx + glm::ivec3(+1, 0, 0)),
			getChunk(chunkIdx + glm::ivec3(0, 0, -1)),
			getChunk(chunkIdx + glm::ivec3(0, 0, +1)),
			getChunk(chunkIdx + glm::ivec3(-1, 0, -1)),
			getChunk(chunkIdx + glm::ivec3(+1, 0, -1)),
			getChunk(chunkIdx + glm::ivec3(-1, 0, +1)),
			getChunk(chunkIdx + glm::ivec3(+1, 0, +1)));
	}
}

void RenderWorldBridge::markNeighborLightDirty(const glm::ivec3 &chunkPos, int x, int y, int z,
										  TextureType previousType, TextureType type)
{
	// Same light-relevance predicate as Chunk::markEditDirtySections: only
	// emissive changes and sky-transmission flips can change a propagated
	// light field.
	const bool lightRelevant =
		blockTransmitsSkyLight(previousType) != blockTransmitsSkyLight(type) ||
		lighting::blockLightEmission(static_cast<uint8_t>(previousType)) > 0 ||
		lighting::blockLightEmission(static_cast<uint8_t>(type)) > 0;
	if (!lightRelevant)
		return;

	// The edit dirties every neighbor it can still reach with nonzero
	// propagated light: those within the halo radius of that border
	// (conservative - the 15th step carries value 0 and is included).
	constexpr int R = ChunkLightHalo::kRadius;
	const bool west = x + 1 <= R;
	const bool east = CHUNK_SIZE - x <= R;
	const bool south = z + 1 <= R;
	const bool north = CHUNK_SIZE - z <= R;
	const bool southWest = west && south && (x + 1) + (z + 1) <= R;
	const bool southEast = east && south && (CHUNK_SIZE - x) + (z + 1) <= R;
	const bool northWest = west && north && (x + 1) + (CHUNK_SIZE - z) <= R;
	const bool northEast = east && north && (CHUNK_SIZE - x) + (CHUNK_SIZE - z) <= R;
	if (!west && !east && !south && !north && !southWest && !southEast && !northWest && !northEast)
		return;

	uint16_t yMask = 0;
	const int yLo = std::max(0, y - R) / Chunk::kOccupancySectionSize;
	const int yHi = std::min(static_cast<int>(CHUNK_HEIGHT) - 1, y + R) /
					Chunk::kOccupancySectionSize;
	for (int s = yLo; s <= yHi; ++s)
		yMask |= static_cast<uint16_t>(1u << s);

	auto dirty = [&](const glm::ivec3 &pos) {
		Chunk *n = getChunk(pos);
		// UNLOADED chunks have no mesh to invalidate (generation re-arms all
		// sections anyway). The section mask is recorded even when the
		// neighbor is IN TRANSIT: the atomic mask persists across the
		// in-flight job and processFinishedJobs re-arms GENERATED after
		// publish, so an edit racing a mesh build can never lose its
		// invalidation (issue #141 review round 2). Only the scheduling
		// state change is withheld while a job owns the chunk - publish
		// would overwrite it.
		if (!n || n->getState() == ChunkState::UNLOADED)
			return;
		n->markSectionsDirty(yMask);
		if (!n->isInTransit() && n->getState() == ChunkState::MESHED)
			n->setState(ChunkState::GENERATED);
	};
	if (west) dirty(chunkPos + glm::ivec3(-1, 0, 0));
	if (east) dirty(chunkPos + glm::ivec3(+1, 0, 0));
	if (south) dirty(chunkPos + glm::ivec3(0, 0, -1));
	if (north) dirty(chunkPos + glm::ivec3(0, 0, +1));
	if (southWest) dirty(chunkPos + glm::ivec3(-1, 0, -1));
	if (southEast) dirty(chunkPos + glm::ivec3(+1, 0, -1));
	if (northWest) dirty(chunkPos + glm::ivec3(-1, 0, +1));
	if (northEast) dirty(chunkPos + glm::ivec3(+1, 0, +1));
}

void RenderWorldBridge::dirtyNeighborsForArrivedLight(Chunk *chunk, const glm::ivec3 &chunkIdx)
{
	if (!chunk || !chunk->isVoxelBackingReadable())
		return;
	constexpr int R = ChunkLightHalo::kRadius;
	// One pass classifies every light-relevant cell into the neighbor bands
	// it can reach - sides AND diagonals, with the same Manhattan reach rule
	// as markNeighborLightDirty. Light-relevant here means emissive sources
	// (new light) OR non-air-like content (the halo assumed AIR for this
	// missing chunk, so arriving blockers/liquids change BFS paths even
	// without a single emitter - issue #141 review round 2). ~65K cheap
	// checks per arrival, once per chunk lifetime; a band without relevant
	// cells dirties nothing.
	constexpr int kBandCount = 8; // W E S N SW SE NW NE
	int bandMinY[kBandCount] = {CHUNK_HEIGHT, CHUNK_HEIGHT, CHUNK_HEIGHT, CHUNK_HEIGHT,
								CHUNK_HEIGHT, CHUNK_HEIGHT, CHUNK_HEIGHT, CHUNK_HEIGHT};
	int bandMaxY[kBandCount] = {-1, -1, -1, -1, -1, -1, -1, -1};
	auto note = [&](int side, int y) {
		if (y < bandMinY[side])
			bandMinY[side] = y;
		if (y > bandMaxY[side])
			bandMaxY[side] = y;
	};
	for (uint32_t y = 0; y < CHUNK_HEIGHT; ++y)
		for (uint32_t z = 0; z < CHUNK_SIZE; ++z)
			for (uint32_t x = 0; x < CHUNK_SIZE; ++x)
			{
				const auto t = static_cast<TextureType>(chunk->getVoxel(x, y, z).type);
				const bool lightRelevant =
					lighting::isBlockLightSource(t) || !blockTransmitsSkyLight(t);
				if (!lightRelevant)
					continue;
				const int xi = static_cast<int>(x);
				const int zi = static_cast<int>(z);
				if (xi + 1 <= R) note(0, static_cast<int>(y));
				if (CHUNK_SIZE - xi <= R) note(1, static_cast<int>(y));
				if (zi + 1 <= R) note(2, static_cast<int>(y));
				if (CHUNK_SIZE - zi <= R) note(3, static_cast<int>(y));
				if (xi + 1 + zi + 1 <= R) note(4, static_cast<int>(y));
				if (CHUNK_SIZE - xi + zi + 1 <= R) note(5, static_cast<int>(y));
				if (xi + 1 + (CHUNK_SIZE - zi) <= R) note(6, static_cast<int>(y));
				if (CHUNK_SIZE - xi + (CHUNK_SIZE - zi) <= R) note(7, static_cast<int>(y));
			}

	// Same mask-persists-through-transit rule as markNeighborLightDirty.
	auto dirty = [&](int side, const glm::ivec3 &pos) {
		if (bandMinY[side] > bandMaxY[side])
			return;
		Chunk *n = getChunk(pos);
		if (!n || n->getState() == ChunkState::UNLOADED)
			return;
		uint16_t yMask = 0;
		const int yLo = std::max(0, bandMinY[side] - R) / Chunk::kOccupancySectionSize;
		const int yHi = std::min(static_cast<int>(CHUNK_HEIGHT) - 1, bandMaxY[side] + R) /
						Chunk::kOccupancySectionSize;
		for (int s = yLo; s <= yHi; ++s)
			yMask |= static_cast<uint16_t>(1u << s);
		n->markSectionsDirty(yMask);
		if (!n->isInTransit() && n->getState() == ChunkState::MESHED)
			n->setState(ChunkState::GENERATED);
	};
	dirty(0, chunkIdx + glm::ivec3(-1, 0, 0));
	dirty(1, chunkIdx + glm::ivec3(+1, 0, 0));
	dirty(2, chunkIdx + glm::ivec3(0, 0, -1));
	dirty(3, chunkIdx + glm::ivec3(0, 0, +1));
	dirty(4, chunkIdx + glm::ivec3(-1, 0, -1));
	dirty(5, chunkIdx + glm::ivec3(+1, 0, -1));
	dirty(6, chunkIdx + glm::ivec3(-1, 0, +1));
	dirty(7, chunkIdx + glm::ivec3(+1, 0, +1));
}

TaskPriority RenderWorldBridge::calculateTaskPriority(float distanceSq, float lodThresholdSq) const
{
	if (distanceSq < lodThresholdSq * 0.25f)
		return TaskPriority::High;
	if (distanceSq > lodThresholdSq)
		return TaskPriority::Low;
	return TaskPriority::Normal;
}

// ---------------------------------------------------------------------------
// Geometric commit groups (PR #178 review; unchanged)
// ---------------------------------------------------------------------------

namespace
{
bool memberListContains(const std::vector<PendingGroupMember> &members, Chunk *chunk)
{
	for (const PendingGroupMember &member : members)
		if (member.chunk == chunk)
			return true;
	return false;
}

bool groupsIntersect(const std::vector<PendingGroupMember> &a,
	                 const std::vector<PendingGroupMember> &b)
{
	for (const PendingGroupMember &member : a)
		if (memberListContains(b, member.chunk))
			return true;
	return false;
}

// Add a newly registered member. New edit registrations carry no prepared
// replacement; ownership from an already active group is transferred later.
void appendMemberUnique(std::vector<PendingGroupMember> &dst, PendingGroupMember member)
{
	if (!memberListContains(dst, member.chunk))
		dst.push_back(std::move(member));
}

// Fusion dedup: the member from the EXISTING active group wins. It may own
// allocated or already-recorded ranges which must survive the container
// erase; a later revision check discards them if the new edit made them stale.
void absorbExistingMember(std::vector<PendingGroupMember> &dst, PendingGroupMember &member)
{
	for (PendingGroupMember &existing : dst)
	{
		if (existing.chunk != member.chunk)
			continue;
		if (existing.replacement.valid && existing.chunk)
			existing.chunk->discardGPUUpload(existing.replacement);
		existing = std::move(member);
		member.replacement = {};
		return;
	}
	dst.push_back(std::move(member));
	member.replacement = {};
}

void appendUnique(std::vector<uint64_t> &dst, uint64_t id)
{
	for (uint64_t existing : dst)
		if (existing == id)
			return;
	dst.push_back(id);
}
} // namespace

// Registers the geometric members of one border edit as a commit group.
// Overlapping active groups are fused transitively (a group sharing any
// chunk with the new member set is absorbed, and absorption repeats until
// a full pass merges nothing more), so the active groups always form
// disjoint chunk sets: a chunk belongs to at most one group, which is what
// makes the per-group atomic commit exact.
void RenderWorldBridge::registerCommitGroup(uint64_t editId, std::vector<PendingGroupMember> members)
{
	std::vector<PendingGroupMember> merged;
	std::vector<uint64_t> editIds{editId};
	for (PendingGroupMember &member : members)
		appendMemberUnique(merged, std::move(member));

	// Transitive fusion: absorb every active group sharing a chunk with the
	// merged set, repeating until a full pass merges nothing more. Shared
	// chunks keep their existing member entry (an in-flight recorded
	// replacement survives the fusion; the per-frame revision check
	// discards it if the new edit made it stale).
	bool changed = false;
	do
	{
		changed = false;
		for (auto it = m_commitGroups.begin(); it != m_commitGroups.end();)
		{
			if (groupsIntersect(merged, it->members))
			{
				for (PendingGroupMember &member : it->members)
					absorbExistingMember(merged, member);
				for (uint64_t absorbed : it->editIds)
					appendUnique(editIds, absorbed);
				it = m_commitGroups.erase(it);
				changed = true;
			}
			else
			{
				++it;
			}
		}
	} while (changed);

	m_commitGroups.push_back(
		{m_nextGroupId++, CommitGroupState::WaitingForMeshes, std::move(merged),
		 std::move(editIds)});
}

PendingMeshCommitGroup *RenderWorldBridge::commitGroupFor(Chunk *chunk)
{
	PendingMeshCommitGroup *found = nullptr;
#ifndef NDEBUG
	size_t matches = 0;
#endif
	for (PendingMeshCommitGroup &group : m_commitGroups)
	{
		for (PendingGroupMember &member : group.members)
		{
			if (member.chunk == chunk)
			{
				found = &group;
#ifndef NDEBUG
				++matches;
#endif
			}
		}
	}
	// Registration fuses overlapping groups, so a chunk can never belong to
	// two active groups at once.
	assert(matches <= 1 && "active commit groups must form disjoint chunk sets");
	return found;
}

void RenderWorldBridge::eraseCommitGroup(uint64_t groupId)
{
	m_commitGroups.erase(std::remove_if(m_commitGroups.begin(), m_commitGroups.end(),
										[groupId](const PendingMeshCommitGroup &group) {
											return group.groupId == groupId;
										}),
						 m_commitGroups.end());
}

void RenderWorldBridge::discardAllCommitGroups()
{
	for (PendingMeshCommitGroup &group : m_commitGroups)
	{
		for (PendingGroupMember &member : group.members)
		{
			if (member.chunk)
				member.chunk->discardGPUUpload(member.replacement);
		}
	}
	m_commitGroups.clear();
	// (review §4.6: the assert(m_commitGroups.empty()) that used to sit here
	// was trivially true after clear() and documented nothing.)
}

// A member chunk is unloading: drop it from its group (discarding any
// unpublished replacement it still owns) and keep the remaining atomic
// dependency alive while at least two chunks remain. A group falling to a
// single member has no coupling left to protect and dissolves - the
// remaining chunk reverts to normal independent uploads.
void RenderWorldBridge::removeChunkFromCommitGroups(Chunk *chunk)
{
	for (auto it = m_commitGroups.begin(); it != m_commitGroups.end();)
	{
		std::vector<PendingGroupMember> &members = it->members;
		for (auto memberIt = members.begin(); memberIt != members.end();)
		{
			if (memberIt->chunk == chunk)
			{
				chunk->discardGPUUpload(memberIt->replacement);
				memberIt = members.erase(memberIt);
			}
			else
			{
				++memberIt;
			}
		}
		if (members.size() < 2)
		{
			for (PendingGroupMember &survivor : members)
				if (survivor.chunk)
					survivor.chunk->discardGPUUpload(survivor.replacement);
			it = m_commitGroups.erase(it);
		}
		else
			++it;
	}
}

// ---------------------------------------------------------------------------
// Chunk lifecycle debug trace (issue #179; unchanged ring contract)
// ---------------------------------------------------------------------------

void RenderWorldBridge::recordChunkEvent(Chunk *chunk, const char *kind, uint32_t sections)
{
	if (!m_chunkTraceEnabled || !chunk)
		return;
	const glm::vec3 wp = chunk->getPosition();
	recordWorldEvent(glm::ivec3(static_cast<int>(std::round(wp.x)) / CHUNK_SIZE, 0,
								static_cast<int>(std::round(wp.z)) / CHUNK_SIZE),
					 chunk->meshRevision(), kind, sections);
}

void RenderWorldBridge::recordWorldEvent(const glm::ivec3 &coord, uint64_t revision, const char *kind,
									uint32_t sections)
{
	if (!m_chunkTraceEnabled)
		return;
	ChunkDebugEvent &e = m_chunkEventRing[m_chunkEventWrite];
	e.timeSec = std::chrono::duration<double>(
					std::chrono::steady_clock::now().time_since_epoch())
					.count();
	e.chunk = coord;
	e.kind = kind;
	e.revision = revision;
	e.sections = sections;
	m_chunkEventWrite = (m_chunkEventWrite + 1) % kChunkEventRingSize;
	if (m_chunkEventCount < kChunkEventRingSize)
		++m_chunkEventCount;
}

std::vector<ChunkDebugEvent> RenderWorldBridge::chunkDebugEvents() const
{
	std::vector<ChunkDebugEvent> out;
	out.reserve(m_chunkEventCount);
	const size_t start = (m_chunkEventCount < kChunkEventRingSize) ? 0 : m_chunkEventWrite;
	for (size_t i = 0; i < m_chunkEventCount; ++i)
		out.push_back(m_chunkEventRing[(start + i) % kChunkEventRingSize]);
	return out;
}
