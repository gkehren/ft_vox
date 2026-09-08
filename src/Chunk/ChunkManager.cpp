#include "ChunkManager.hpp"

#include <Chunk/ChunkMeshResult.hpp>
#include <Camera/Camera.hpp>
#include <Engine/Profiler.hpp>
#include <Renderer/Lighting.hpp>
#include <Renderer/MinecraftTextures.hpp>
#include <Vulkan/VkCommands.hpp>
#include <Vulkan/StagingRing.hpp>
#include <Vulkan/GpuResourceRetire.hpp>

#include <glm/gtc/matrix_access.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>

namespace
{
constexpr int kDeferredReleaseFrames = 3; // >= frames-in-flight
}

ChunkManager::ChunkManager(TerrainGenerator *terrainGenerator, ThreadPool *threadPool, ChunkPool *chunkPool)
	: m_terrainGenerator(terrainGenerator), m_threadPool(threadPool), m_chunkPool(chunkPool)
{
}

ChunkManager::~ChunkManager()
{
	// Best-effort drain of async work so we don't release chunks still in workers.
	while (m_pendingGenJobsCount.load() > 0 || m_pendingMeshJobsCount.load() > 0)
	{
		std::this_thread::yield();
	}

	for (auto &[pos, chunkPtr] : m_chunks)
	{
		if (chunkPtr && m_chunkPool)
			m_chunkPool->release(chunkPtr);
	}
	m_chunks.clear();
	m_activeChunks.clear();

	for (Chunk *c : m_deferredRelease)
	{
		if (c && m_chunkPool)
			m_chunkPool->release(c);
	}
	m_deferredRelease.clear();
}

glm::ivec3 ChunkManager::worldToChunkCoord(const glm::vec3 &worldPos)
{
	return {
		static_cast<int>(std::floor(worldPos.x / static_cast<float>(CHUNK_SIZE))),
		0,
		static_cast<int>(std::floor(worldPos.z / static_cast<float>(CHUNK_SIZE)))};
}

StreamingUpdateKind ChunkManager::updateStreaming(const Camera &camera, const RenderSettings &settings)
{
	const glm::ivec3 camChunk = worldToChunkCoord(camera.getPosition());

	// Unload triggers mirror the load-side reconciliation triggers minus
	// intra-chunk anchor moves: the 1.5x unload hysteresis makes 4-block
	// precision worthless and the m_activeChunks scan is the expensive part.
	// Read BEFORE loadChunksAroundPlayer publishes the new camera state
	// (issue #108 review: one trigger definition, no duplicated logic).
	const bool unloadRelevant = !m_streamState.initialized ||
								camChunk != m_streamState.lastCamChunk ||
								settings.maxRenderDistance != m_streamState.lastMaxRenderDistance ||
								streamFrontBiasChanged(settings.streamFrontBias, m_streamState.lastStreamFrontBias);

	++m_streamFramesSinceUnloadCheck;
	if (unloadRelevant || m_streamFramesSinceUnloadCheck >= kUnloadCheckIntervalFrames)
	{
		queueUnloadOutOfRange(camera, settings);
		++m_streamStats.unloadScans;
		m_streamFramesSinceUnloadCheck = 0;
	}

	return loadChunksAroundPlayer(camChunk, camera, settings);
}

void ChunkManager::processChunkLoading(int budget)
{
	if (budget <= 0 || !m_chunkPool)
		return;

	// Back-pressure: never pull more candidates than free pool slots.
	const size_t freeSlots = m_chunkPool->freeCount();
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

	std::vector<std::pair<glm::ivec3, Chunk *>> acquired;
	std::vector<glm::ivec3> rejected;
	acquired.reserve(toLoad.size());
	for (const auto &chunkPos : toLoad)
	{
		Chunk *chunk = m_chunkPool->acquire(glm::vec3(
			static_cast<float>(chunkPos.x * CHUNK_SIZE), 0.0f,
			static_cast<float>(chunkPos.z * CHUNK_SIZE)));
		if (chunk)
			acquired.emplace_back(chunkPos, chunk);
		else
			rejected.push_back(chunkPos); // free list raced empty — re-queue below
	}

	std::lock_guard<std::shared_mutex> lock(m_mutex);
	for (const auto &pair : acquired)
	{
		if (m_chunks.find(pair.first) == m_chunks.end())
		{
			m_chunks[pair.first] = pair.second;
			pair.second->setActiveIndex(m_activeChunks.size());
			m_activeChunks.push_back(pair.second);
			// Generate even before frustum test so the world fills around the player.
			pair.second->setVisible(true);
		}
		else
		{
			m_chunkPool->release(pair.second);
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

void ChunkManager::updateVisibility(const Camera &camera, int windowWidth, int windowHeight,
									const RenderSettings &settings)
{
	// Timed by Engine PROFILE_SCOPE("Visibility") — keep body lean.

	const glm::mat4 clipMatrix =
		camera.getProjectionMatrix(static_cast<float>(windowWidth), static_cast<float>(windowHeight),
								   static_cast<float>(settings.maxRenderDistance)) *
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
	}
	(void)settings;
}

void ChunkManager::generatePendingVoxels(const Camera &camera, const RenderSettings &settings, int budget)
{
	if (!m_terrainGenerator || !m_threadPool || budget <= 0)
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

	for (Chunk *chunk : m_activeChunks)
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
	const float lodThresh = static_cast<float>(settings.minRenderDistance) * 2.f;
	const float lodThreshSq = lodThresh * lodThresh;

	for (int i = 0; i < n; ++i)
	{
		Chunk *chunk = queue[i].chunk;
		if (!chunk->prepareVoxelStorageForGeneration())
			continue; // Allocation failed (e.g. OOM), retry next tick
		const TaskPriority prio = calculateTaskPriority(queue[i].distSq, lodThreshSq);
		chunk->setInTransit(true);
		m_pendingGenJobsCount.fetch_add(1);
		const auto captureEpoch = GetProfiler().captureEpoch();
		const auto queuedAt = std::chrono::steady_clock::now();
		m_threadPool->enqueue(prio, [chunk, seed, this, queuedAt, captureEpoch]() {
			const auto t0 = std::chrono::steady_clock::now();
			GetProfiler().addWorkerSample("TerrainQueue",
				std::chrono::duration<float, std::milli>(t0 - queuedAt).count(), captureEpoch);
			TerrainGenerator &localGen = TerrainGenerator::getThreadLocal(seed);
			chunk->generateTerrain(localGen);
			const float ms = std::chrono::duration<float, std::milli>(
								 std::chrono::steady_clock::now() - t0)
								 .count();
			GetProfiler().addWorkerSample("TerrainGen", ms, captureEpoch);

			{
				std::lock_guard<std::mutex> lk(m_completedJobsMutex);
				m_completedGenerationChunks.push_back(chunk);
			}
			m_pendingGenJobsCount.fetch_sub(1);
		});
	}
}

void ChunkManager::meshPendingChunks(const Camera &camera, const RenderSettings &settings, int budget)
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
	for (Chunk *chunk : m_activeChunks)
	{
		if (chunk->isLODMesh() && chunk->getState() == ChunkState::MESHED && !chunk->isInTransit())
		{
			const glm::vec3 p = chunk->getPosition();
			const float dx = p.x - camOffsetX;
			const float dz = p.z - camOffsetZ;
			if (dx * dx + dz * dz < lodThreshSq)
				chunk->setState(ChunkState::GENERATED);
		}
	}

	for (Chunk *chunk : m_activeChunks)
	{
		if (chunk->getState() == ChunkState::GENERATED && !chunk->isInTransit())
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

		if (distSq > lodThreshSq)
		{
			m_pendingMeshJobsCount.fetch_add(1);
			const auto captureEpoch = GetProfiler().captureEpoch();
			const auto queuedAt = std::chrono::steady_clock::now();
			// Identity captured at dispatch (issue #114 review): the worker
			// must not read the counters later, and a result built from
			// content newer than this capture is rejected at publish.
			const uint64_t meshGeneration = chunk->meshGeneration();
			const uint64_t meshRevision = chunk->meshRevision();
			m_threadPool->enqueue(prio, [chunk, meshGeneration, meshRevision, this, queuedAt, captureEpoch]() {
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
			ensureShellPopulated(chunk, ci);
			ensureLightHalo(chunk, ci);
			// Borrowed for this job only: the worker detaches and returns
			// it when the build ends (success, failure or retry).
			ChunkLightHalo *halo = chunk->lightHalo();
			m_pendingMeshJobsCount.fetch_add(1);
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
			m_threadPool->enqueue(prio, [chunk, meshGeneration, meshRevision, sectionMask, halo, this, queuedAt, captureEpoch]() {
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

int ChunkManager::uploadPendingMeshes(VmaAllocator allocator, StagingRing &staging, VkCommandBuffer cmd,
									  GpuResourceRetire &retire, MeshArenas &arenas, const Camera &camera, int budget)
{
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
			if (!chunk)
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

	const int n = std::min(budget, static_cast<int>(queue.size()));
	std::partial_sort(queue.begin(), queue.begin() + n, queue.end(),
					  [](const Item &a, const Item &b) { return a.distSq < b.distSq; });

	int uploaded = 0;
	for (int i = 0; i < n; ++i)
	{
		if (!queue[i].chunk->uploadToGPUAsync(allocator, staging, cmd, retire, arenas))
			break; // staging full — remaining wait next frame
		++uploaded;
	}
	return uploaded;
}

void ChunkManager::processDeferredReleases()
{
	if (m_deferredRelease.empty())
	{
		m_deferredReleaseAge = 0;
		return;
	}

	++m_deferredReleaseAge;
	if (m_deferredReleaseAge < kDeferredReleaseFrames)
		return;

	// No device idle: hand GPU buffers to the retire queue, then recycle CPU chunk.
	for (Chunk *c : m_deferredRelease)
	{
		if (!c)
			continue;
		c->releaseGPUDeferred();
		if (m_chunkPool)
			m_chunkPool->release(c);
	}
	m_deferredRelease.clear();
	m_deferredReleaseAge = 0;
}

void ChunkManager::processFinishedJobs()
{
	std::vector<Chunk *> finishedGen;
	std::vector<CompletedMeshJob> finishedMesh;
	{
		std::lock_guard<std::mutex> lock(m_completedJobsMutex);
		finishedGen.swap(m_completedGenerationChunks);
		finishedMesh.swap(m_completedMeshJobs);
	}

	for (Chunk *chunk : finishedGen)
	{
		if (chunk)
			chunk->setInTransit(false);
	}
	// New terrain can carry emissive sources whose light reaches into the
	// side neighbors' halo radius (issue #141 review fix): dirty the
	// affected neighbors so their next remesh imports it.
	{
		std::lock_guard<std::shared_mutex> lock(m_mutex);
		for (Chunk *chunk : finishedGen)
		{
			if (!chunk)
				continue;
			const glm::vec3 wp = chunk->getPosition();
			const glm::ivec3 ci(static_cast<int>(std::round(wp.x)) / CHUNK_SIZE, 0,
								static_cast<int>(std::round(wp.z)) / CHUNK_SIZE);
			dirtyNeighborsForArrivedLight(chunk, ci);
		}
	}
	for (const CompletedMeshJob &job : finishedMesh)
	{
		// Publish before clearing in-transit so a recycled chunk can never
		// race this decision (issue #104): stale results are refused and
		// returned to their pool; a null result just unsticks the chunk.
		// A queued edit made while this job ran supersedes the result
		// outright: the mesh was built from pre-edit content (issue #114).
		const bool supersededByEdit = job.chunk && hasPendingEditsFor(job.chunk);
		if (job.result)
		{
			if (job.chunk && !supersededByEdit)
			{
				job.chunk->publishMeshResult(job.result);
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
	}
	// Apply edits that were deferred while their chunk was in transit; they
	// bump the mesh revision and mark the chunk GENERATED for a remesh.
	applyPendingEdits();
}

bool ChunkManager::scheduleLogicalEdit(const glm::ivec3 &chunkPos, int x, int y, int z,
									   TextureType type)
{
	// Coordinate validation FIRST (issue #114 final review): effectiveVoxelType()
	// reads the voxel backing and must never see an out-of-range coordinate -
	// a raycast hit on the top face of a y = CHUNK_HEIGHT-1 voxel produces
	// exactly y = CHUNK_HEIGHT here. Local x/z stay in [0, CHUNK_SIZE) by
	// construction of the world-to-chunk mapping; y has no such wrap.
	if (x < 0 || x >= static_cast<int>(CHUNK_SIZE) ||
		y < 0 || y >= static_cast<int>(CHUNK_HEIGHT) ||
		z < 0 || z >= static_cast<int>(CHUNK_SIZE))
		return false;

	// Caller holds m_mutex exclusively.
	auto it = m_chunks.find(chunkPos);
	if (it == m_chunks.end())
		return false;
	Chunk *target = it->second;

	// Logical no-op refusal against the EFFECTIVE state (storage overlaid
	// with pending edits), so place-after-pending-delete is not wrongly
	// refused (issue #114 review items 15-17).
	const TextureType effective = effectiveVoxelType(target, x, y, z);
	if (type == AIR ? effective == AIR : effective != AIR)
		return false;

	// One user interaction = one logical group: the target edit plus every
	// neighbor mirror. If the target must defer (in transit, or UNLOADED
	// and waiting for terrain generation - issue #115 blocker), its mirrors
	// defer with it so the whole group lands in one apply phase
	// (transaction semantics, issue #114 review item 6). When the target
	// applies immediately, each mirror still decides on its own chunk's
	// state inside queueOrApplyEdit.
	const uint64_t editId = m_nextEditId++;
	const bool deferAll = mustDeferVoxelEdit(target);
	queueOrApplyEdit(target, chunkPos, x, y, z, type, false, deferAll, editId);
	enqueueOrApplyMirrorEdits(chunkPos, x, y, z, type, deferAll, editId);
	return true;
}

TextureType ChunkManager::effectiveVoxelType(const Chunk *chunk, int x, int y, int z) const
{
	// Strict contract: scheduleLogicalEdit() validates coordinates before
	// this helper runs. Debug asserts make any future caller violation
	// immediately visible; no silent AIR fallback, which would make an
	// invalid coordinate look legitimately placeable.
	assert(chunk != nullptr);
	assert(x >= 0 && x < static_cast<int>(CHUNK_SIZE));
	assert(y >= 0 && y < static_cast<int>(CHUNK_HEIGHT));
	assert(z >= 0 && z < static_cast<int>(CHUNK_SIZE));

	// Occupancy lifecycle (issue #115 review): an UNLOADED chunk is
	// prepared-for-generation - its backing holds stale pool bytes and is
	// not readable. It is logically empty: a placement defers (the chunk is
	// in transit while generation runs), a deletion is the no-op it claims
	// to be. The pending-edit overlay still applies so a second edit racing
	// an already-queued one sees the queued state.
	TextureType effective = AIR;
	if (chunk->isVoxelBackingReadable())
	{
		// getVoxel is read-only and answers AIR without storage, so this is
		// safe to evaluate while the chunk is being meshed/generated.
		effective = static_cast<TextureType>(chunk->getVoxel(static_cast<uint32_t>(x),
															static_cast<uint32_t>(y),
															static_cast<uint32_t>(z))
												 .type);
	}
	for (const PendingVoxelEdit &edit : m_pendingEdits)
	{
		if (edit.chunk == chunk && !edit.borderNeighbor &&
			edit.x == x && edit.y == y && edit.z == z)
			effective = edit.type; // last pending write wins
	}
	return effective;
}

bool ChunkManager::mustDeferVoxelEdit(const Chunk *chunk) const
{
	// UNLOADED defers regardless of transit state: the backing is stale
	// pool bytes until terrain generation initializes it (issue #115).
	return !chunk ||
	       chunk->getState() == ChunkState::UNLOADED ||
	       chunk->isInTransit();
}

void ChunkManager::queueOrApplyEdit(Chunk *chunk, const glm::ivec3 &chunkPos, int x,
									int y, int z, TextureType type, bool borderNeighbor,
									bool forceDefer, uint64_t editId)
{
	if (!chunk)
		return;
	// One gate for every deferral reason (issue #115 blocker): in transit
	// for any job, or still UNLOADED - stale backing is never edited, even
	// when no job currently owns the chunk.
	if (forceDefer || mustDeferVoxelEdit(chunk))
	{
		queuePendingEdit({chunk, chunk->meshGeneration(), chunkPos, x, y, z,
						  type, borderNeighbor, editId});
		return;
	}
	// Apply path: the backing is readable and nothing owns the chunk.
	assert(!chunk->isInTransit() && chunk->isVoxelBackingReadable() &&
		   "edit reached the apply path on unreadable backing");
	if (borderNeighbor)
		ensureShellPopulated(chunk, chunkPos);
	// setVoxel bumps the mesh revision; GENERATED re-arms meshing
	// (setState also raises meshNeedsUpdate).
	const TextureType previousType =
		static_cast<TextureType>(chunk->getVoxel(static_cast<uint32_t>(x),
												 static_cast<uint32_t>(y),
												 static_cast<uint32_t>(z))
									 .type);
	chunk->setVoxel(x, y, z, type);
	chunk->setState(ChunkState::GENERATED);
	// Light-relevant edits near a border change the neighbors' propagated
	// light too (issue #141 review fix); mirror writes are the owning
	// chunk's edit seen from the other side and skip this.
	if (!borderNeighbor)
		markNeighborLightDirty(chunkPos, x, y, z, previousType, type);
}

void ChunkManager::enqueueOrApplyMirrorEdits(const glm::ivec3 &chunkPos, int x, int y,
											 int z, TextureType type, bool forceDefer,
											 uint64_t editId)
{
	// Same mirror mapping as the historical dirtyNeighbor path: the
	// neighbor on the -x side holds our x=0 column at its local
	// CHUNK_SIZE, and so on for each face.
	if (x == 0)
		queueOrApplyEdit(getChunk(chunkPos + glm::ivec3(-1, 0, 0)),
						 chunkPos + glm::ivec3(-1, 0, 0), CHUNK_SIZE, y, z,
						 type, true, forceDefer, editId);
	if (x == CHUNK_SIZE - 1)
		queueOrApplyEdit(getChunk(chunkPos + glm::ivec3(1, 0, 0)),
						 chunkPos + glm::ivec3(1, 0, 0), -1, y, z,
						 type, true, forceDefer, editId);
	if (z == 0)
		queueOrApplyEdit(getChunk(chunkPos + glm::ivec3(0, 0, -1)),
						 chunkPos + glm::ivec3(0, 0, -1), x, y, CHUNK_SIZE,
						 type, true, forceDefer, editId);
	if (z == CHUNK_SIZE - 1)
		queueOrApplyEdit(getChunk(chunkPos + glm::ivec3(0, 0, 1)),
						 chunkPos + glm::ivec3(0, 0, 1), x, y, -1,
						 type, true, forceDefer, editId);
}

void ChunkManager::queuePendingEdit(PendingVoxelEdit edit)
{
	// Coalesce: the last logical write per (chunk, coordinate, kind) wins,
	// so place->delete->place during one transit collapses into one entry.
	for (PendingVoxelEdit &existing : m_pendingEdits)
	{
		if (existing.chunk == edit.chunk && !existing.borderNeighbor == !edit.borderNeighbor &&
			existing.x == edit.x && existing.y == edit.y && existing.z == edit.z)
		{
			existing.type = edit.type;
			existing.generation = edit.generation;
			existing.editId = edit.editId;
			return;
		}
	}
	m_pendingEdits.push_back(edit);
}

bool ChunkManager::hasPendingEditsFor(const Chunk *chunk) const
{
	for (const PendingVoxelEdit &edit : m_pendingEdits)
	{
		if (edit.chunk == chunk)
			return true;
	}
	return false;
}

void ChunkManager::erasePendingEditsFor(const Chunk *chunk)
{
	m_pendingEdits.erase(
		std::remove_if(m_pendingEdits.begin(), m_pendingEdits.end(),
					   [chunk](const PendingVoxelEdit &edit) { return edit.chunk == chunk; }),
		m_pendingEdits.end());
}

void ChunkManager::applyPendingEdits()
{
	if (m_pendingEdits.empty())
		return;

	std::vector<PendingVoxelEdit> remaining;
	{
		// ensureShellPopulated expects the exclusive lock (same contract as
		// the direct edit path).
		std::lock_guard<std::shared_mutex> lock(m_mutex);
		// Two passes for a deterministic order: target voxel edits first,
		// then mirror border writes (issue #114 review item 8). Target and
		// mirrors complete independently: each entry waits only on its own
		// chunk's transit state (item 10).
		for (int pass = 0; pass < 2; ++pass)
		{
			const bool mirrorPass = pass == 1;
			for (const PendingVoxelEdit &edit : m_pendingEdits)
			{
				if (edit.borderNeighbor != mirrorPass)
					continue;
				if (!edit.chunk)
					continue; // dangling entry: drop
				// Staleness first: a recycled incarnation must never
				// receive the queued edit, even though it now sits in the
				// UNLOADED state.
				if (edit.chunk->meshGeneration() != edit.generation)
					continue; // chunk recycled since queueing: stale, drop
				// Not readable yet: in transit for a job, or still waiting
				// for terrain generation (issue #115 blocker - do not
				// apply onto stale backing just because no job owns the
				// chunk right now). Keep waiting.
				if (mustDeferVoxelEdit(edit.chunk))
				{
					remaining.push_back(edit);
					continue;
				}
				// The shell rebuild inside a mirror write also bumps the
				// revision (before setVoxel bumps it again): revision
				// monotonicity is what matters, not exact +1 semantics.
				queueOrApplyEdit(edit.chunk, edit.chunkPos, edit.x, edit.y,
								 edit.z, edit.type, edit.borderNeighbor,
								 /*forceDefer=*/false, edit.editId);
			}
		}
	}
	m_pendingEdits.swap(remaining);
}

void ChunkManager::collectDrawList(std::vector<Chunk *> &out) const
{
	out.clear();
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	out.reserve(m_activeChunks.size());
	for (Chunk *chunk : m_activeChunks)
	{
		if (!chunk)
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

void ChunkManager::collectShadowList(std::vector<Chunk *> &out, const Camera &camera,
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
		if (!chunk)
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

void ChunkManager::queueUnloadOutOfRange(const Camera &camera, const RenderSettings &settings)
{
	const float unloadDist =
		static_cast<float>(settings.maxRenderDistance) * kChunkUnloadDistanceFactor;
	const float unloadDistSq = unloadDist * unloadDist;

	std::vector<glm::ivec3> toUnload;
	{
		std::shared_lock<std::shared_mutex> lock(m_mutex);
		for (Chunk *chunk : m_activeChunks)
		{
			if (!chunk || chunk->isInTransit())
				continue;
			const glm::vec3 center = chunk->getPosition() + glm::vec3(CHUNK_SIZE * 0.5f, 0.f, CHUNK_SIZE * 0.5f);
			const float dx = camera.getPosition().x - center.x;
			const float dz = camera.getPosition().z - center.z;
			if (dx * dx + dz * dz > unloadDistSq)
			{
				const glm::ivec3 chunkPos(
					static_cast<int>(std::floor(chunk->getPosition().x / static_cast<float>(CHUNK_SIZE))),
					0,
					static_cast<int>(std::floor(chunk->getPosition().z / static_cast<float>(CHUNK_SIZE))));
				toUnload.push_back(chunkPos);
			}
		}
	}

	if (toUnload.empty())
		return;

	std::lock_guard<std::shared_mutex> lock(m_mutex);
	for (const auto &pos : toUnload)
	{
		auto it = m_chunks.find(pos);
		if (it == m_chunks.end())
			continue;
		Chunk *chunk = it->second;
		if (!chunk || chunk->isInTransit())
			continue;

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
		m_deferredRelease.push_back(chunk);
		erasePendingEditsFor(chunk);
	}
	if (!toUnload.empty())
		m_deferredReleaseAge = 0; // reset age so new unloads wait full delay
}

namespace
{
/// Normalized XZ camera forward (fallback +Z when the forward is vertical).
glm::vec2 normalizedForwardXZ(const Camera &camera)
{
	glm::vec2 fwd(camera.getFront().x, camera.getFront().z);
	if (glm::dot(fwd, fwd) < 1e-6f)
		fwd = glm::vec2(0.f, 1.f);
	return glm::normalize(fwd);
}
} // namespace

StreamingUpdateKind ChunkManager::rebuildStreamingQueueFull(const glm::ivec3 &cameraChunkPos, const Camera &camera,
															const RenderSettings &settings)
{
	const float frontBias = normalizedStreamFrontBias(settings.streamFrontBias);
	const glm::vec3 camPos = camera.getPosition();
	const glm::vec2 camForwardXZ = normalizedForwardXZ(camera);

	m_desiredFootprint = computeDesiredFootprintFull(cameraChunkPos, camPos, camForwardXZ, frontBias,
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
			const glm::vec3 center(
				pos.x * CHUNK_SIZE + CHUNK_SIZE * 0.5f, 0.f,
				pos.z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
			const float distSq = biasedLoadDistSq(camPos, center, camForwardXZ, frontBias);
			m_loadQueue.push_back({pos, distSq});
			m_enqueuedLoads.insert(pos);
		}
	}
	sortLoadCandidatesNearestFirst(m_loadQueue);
	++m_streamStats.queueSorts;
	m_queueNeedsSort = false;

	m_streamState.lastCamChunk = cameraChunkPos;
	m_streamState.lastMovementAnchor = streamingMovementAnchor(camPos);
	m_streamState.lastCamForwardXZ = camForwardXZ;
	m_streamState.lastMaxRenderDistance = settings.maxRenderDistance;
	m_streamState.lastStreamFrontBias = normalizedStreamFrontBias(settings.streamFrontBias);
	m_streamState.initialized = true;
	++m_streamStats.fullRebuilds;
	return StreamingUpdateKind::FullRebuild;
}

void ChunkManager::applyFootprintDiffToQueue(const FootprintDiff &diff, const glm::vec3 &camPos,
											 const glm::vec2 &camForwardXZ, float frontBias)
{
	std::lock_guard<std::shared_mutex> lock(m_mutex);

	for (const glm::ivec3 &pos : diff.exiting)
	{
		m_enqueuedLoads.erase(pos);
	}

	for (const glm::ivec3 &pos : diff.entering)
	{
		if (m_chunks.find(pos) != m_chunks.end())
			continue;
		if (m_enqueuedLoads.count(pos) != 0)
			continue;
		const glm::vec3 center(
			pos.x * CHUNK_SIZE + CHUNK_SIZE * 0.5f, 0.f,
			pos.z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
		const float distSq = biasedLoadDistSq(camPos, center, camForwardXZ, frontBias);
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
		const glm::vec3 center(
			c.pos.x * CHUNK_SIZE + CHUNK_SIZE * 0.5f, 0.f,
			c.pos.z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
		c.distSq = biasedLoadDistSq(camPos, center, camForwardXZ, frontBias);
	}
	sortLoadCandidatesNearestFirst(m_loadQueue);
	++m_streamStats.queueSorts;
	m_streamStats.enteringCandidates += diff.entering.size();
	m_streamStats.exitingCandidates += diff.exiting.size();
	m_queueNeedsSort = false;
	m_streamState.lastCamForwardXZ = camForwardXZ;
}

StreamingUpdateKind ChunkManager::reconcileStreamingIncremental(const glm::ivec3 &cameraChunkPos, const Camera &camera,
																const RenderSettings &settings)
{
	const float frontBias = normalizedStreamFrontBias(settings.streamFrontBias);
	const glm::vec3 camPos = camera.getPosition();
	const glm::vec2 camForwardXZ = normalizedForwardXZ(camera);

	ChunkDesiredFootprint newFootprint =
		computeDesiredFootprintIncremental(m_desiredFootprint, cameraChunkPos, camPos, camForwardXZ,
										   frontBias, settings.maxRenderDistance);
	m_streamStats.footprintRowsVisited +=
		static_cast<uint64_t>(newFootprint.maxZ - newFootprint.minZ + 1);
	FootprintDiff diff = computeFootprintDiff(m_desiredFootprint, newFootprint);
	m_desiredFootprint = std::move(newFootprint);

	applyFootprintDiffToQueue(diff, camPos, camForwardXZ, frontBias);
	m_streamState.lastCamChunk = cameraChunkPos;
	m_streamState.lastMovementAnchor = streamingMovementAnchor(camPos);
	++m_streamStats.incrementalUpdates;
	return StreamingUpdateKind::Incremental;
}

StreamingUpdateKind ChunkManager::reconcileStreamingHeading(const glm::ivec3 &cameraChunkPos, const Camera &camera,
															const RenderSettings &settings)
{
	const float frontBias = normalizedStreamFrontBias(settings.streamFrontBias);
	const glm::vec3 camPos = camera.getPosition();
	const glm::vec2 camForwardXZ = normalizedForwardXZ(camera);

	ChunkDesiredFootprint newFootprint =
		computeDesiredFootprintFull(cameraChunkPos, camPos, camForwardXZ, frontBias,
									settings.maxRenderDistance);
	m_streamStats.footprintRowsVisited +=
		static_cast<uint64_t>(newFootprint.maxZ - newFootprint.minZ + 1);
	FootprintDiff diff = computeFootprintDiff(m_desiredFootprint, newFootprint);
	m_desiredFootprint = std::move(newFootprint);

	applyFootprintDiffToQueue(diff, camPos, camForwardXZ, frontBias);
	// The heading path also serves chunk-cross frames (heading has dispatch
	// priority), so it must publish the full camera state.
	m_streamState.lastCamChunk = cameraChunkPos;
	m_streamState.lastMovementAnchor = streamingMovementAnchor(camPos);
	++m_streamStats.headingRebuilds;
	return StreamingUpdateKind::HeadingRebuild;
}

StreamingUpdateKind ChunkManager::loadChunksAroundPlayer(const glm::ivec3 &cameraChunkPos, const Camera &camera,
														 const RenderSettings &settings)
{
	const float frontBias = normalizedStreamFrontBias(settings.streamFrontBias);
	const glm::vec2 camForwardXZ = normalizedForwardXZ(camera);

	if (!m_streamState.initialized ||
		settings.maxRenderDistance != m_streamState.lastMaxRenderDistance ||
		streamFrontBiasChanged(settings.streamFrontBias, m_streamState.lastStreamFrontBias))
	{
		return rebuildStreamingQueueFull(cameraChunkPos, camera, settings);
	}

	const glm::ivec3 chunkDelta = cameraChunkPos - m_streamState.lastCamChunk;
	if (std::abs(chunkDelta.x) > 1 || std::abs(chunkDelta.z) > 1)
		return rebuildStreamingQueueFull(cameraChunkPos, camera, settings); // teleport

	// A heading change beyond the threshold reshapes the whole footprint, so
	// it supersedes a simultaneous chunk-cross/anchor move (explicit priority:
	// settings > teleport > heading > movement — issue #108 review).
	if (frontBias > kStreamBiasEpsilon &&
		glm::dot(camForwardXZ, m_streamState.lastCamForwardXZ) < kStreamHeadingCosThreshold)
	{
		return reconcileStreamingHeading(cameraChunkPos, camera, settings);
	}

	if (chunkDelta.x != 0 || chunkDelta.z != 0)
		return reconcileStreamingIncremental(cameraChunkPos, camera, settings);

	// Intra-chunk movement: the footprint is a function of the exact camera
	// position, so reconcile at movement-anchor granularity instead of letting
	// it go stale for up to a full 16-block chunk (issue #108 review).
	const glm::ivec2 anchor = streamingMovementAnchor(camera.getPosition());
	if (anchor != m_streamState.lastMovementAnchor)
		return reconcileStreamingIncremental(cameraChunkPos, camera, settings);

	++m_streamStats.zeroWork;
	return StreamingUpdateKind::None; // exact zero-work steady state
}

void ChunkManager::ensureShellPopulated(Chunk *chunk, const glm::ivec3 &chunkIdx)
{
	if (!chunk || !chunk->isShellEmpty())
		return;
	// Caller holds exclusive lock.
	chunk->rebuildBordersFromNeighbors(
		getChunk(chunkIdx + glm::ivec3(-1, 0, 0)),
		getChunk(chunkIdx + glm::ivec3(+1, 0, 0)),
		getChunk(chunkIdx + glm::ivec3(0, 0, -1)),
		getChunk(chunkIdx + glm::ivec3(0, 0, +1)));
}

void ChunkManager::ensureLightHalo(Chunk *chunk, const glm::ivec3 &chunkIdx)
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
			return; // in-chunk-only light for this build; retried next dispatch
		}
		chunk->setLightHalo(halo);
	}
	// Caller holds exclusive lock.
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

void ChunkManager::markNeighborLightDirty(const glm::ivec3 &chunkPos, int x, int y, int z,
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
		// In-transit neighbors are skipped: their mesh job already captured
		// its mask, and setting GENERATED under it would be overwritten by
		// publish (stuck dirty state). The edit path defers the target the
		// same way.
		if (!n || n->getState() == ChunkState::UNLOADED || n->isInTransit())
			return;
		n->markSectionsDirty(yMask);
		// MESHED chunks are only re-picked by mesh dispatch in GENERATED.
		if (n->getState() == ChunkState::MESHED)
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

void ChunkManager::dirtyNeighborsForArrivedLight(Chunk *chunk, const glm::ivec3 &chunkIdx)
{
	if (!chunk || !chunk->isVoxelBackingReadable())
		return;
	constexpr int R = ChunkLightHalo::kRadius;
	// One pass over the chunk classifies every emissive cell into the side
	// bands it can reach: a cell at local x matters to the west neighbor
	// iff x + 1 <= R and to the east one iff CHUNK_SIZE - x <= R (same for
	// z). ~65K cheap checks per arrival, once per chunk lifetime. A band
	// without sources dirties nothing - the common case.
	int bandMinY[4] = {CHUNK_HEIGHT, CHUNK_HEIGHT, CHUNK_HEIGHT, CHUNK_HEIGHT};
	int bandMaxY[4] = {-1, -1, -1, -1};
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
				if (!lighting::isBlockLightSource(t))
					continue;
				if (static_cast<int>(x) + 1 <= R) note(0, static_cast<int>(y));
				if (static_cast<int>(CHUNK_SIZE - x) <= R) note(1, static_cast<int>(y));
				if (static_cast<int>(z) + 1 <= R) note(2, static_cast<int>(y));
				if (static_cast<int>(CHUNK_SIZE - z) <= R) note(3, static_cast<int>(y));
			}

	// Same skip rules as markNeighborLightDirty.
	auto dirty = [&](int side, const glm::ivec3 &pos) {
		if (bandMinY[side] > bandMaxY[side])
			return;
		Chunk *n = getChunk(pos);
		if (!n || n->getState() == ChunkState::UNLOADED || n->isInTransit())
			return;
		uint16_t yMask = 0;
		const int yLo = std::max(0, bandMinY[side] - R) / Chunk::kOccupancySectionSize;
		const int yHi = std::min(static_cast<int>(CHUNK_HEIGHT) - 1, bandMaxY[side] + R) /
						Chunk::kOccupancySectionSize;
		for (int s = yLo; s <= yHi; ++s)
			yMask |= static_cast<uint16_t>(1u << s);
		n->markSectionsDirty(yMask);
		if (n->getState() == ChunkState::MESHED)
			n->setState(ChunkState::GENERATED);
	};
	dirty(0, chunkIdx + glm::ivec3(-1, 0, 0));
	dirty(1, chunkIdx + glm::ivec3(+1, 0, 0));
	dirty(2, chunkIdx + glm::ivec3(0, 0, -1));
	dirty(3, chunkIdx + glm::ivec3(0, 0, +1));
}

TaskPriority ChunkManager::calculateTaskPriority(float distanceSq, float lodThresholdSq) const
{
	if (distanceSq < lodThresholdSq * 0.25f)
		return TaskPriority::High;
	if (distanceSq > lodThresholdSq)
		return TaskPriority::Low;
	return TaskPriority::Normal;
}

bool ChunkManager::deleteVoxel(const glm::vec3 &worldPos)
{
	const int chunkX = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE));
	const int chunkZ = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE));
	const glm::ivec3 chunkPos(chunkX, 0, chunkZ);
	const int localX = static_cast<int>(std::floor(worldPos.x)) - chunkX * CHUNK_SIZE;
	const int localY = static_cast<int>(std::floor(worldPos.y));
	const int localZ = static_cast<int>(std::floor(worldPos.z)) - chunkZ * CHUNK_SIZE;

	std::lock_guard<std::shared_mutex> lock(m_mutex);
	return scheduleLogicalEdit(chunkPos, localX, localY, localZ, AIR);
}

bool ChunkManager::placeVoxel(const glm::vec3 &worldPos, TextureType type)
{
	const int chunkX = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE));
	const int chunkZ = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE));
	const glm::ivec3 chunkPos(chunkX, 0, chunkZ);
	const int localX = static_cast<int>(std::floor(worldPos.x)) - chunkX * CHUNK_SIZE;
	const int localY = static_cast<int>(std::floor(worldPos.y));
	const int localZ = static_cast<int>(std::floor(worldPos.z)) - chunkZ * CHUNK_SIZE;

	std::lock_guard<std::shared_mutex> lock(m_mutex);
	return scheduleLogicalEdit(chunkPos, localX, localY, localZ, type);
}

bool ChunkManager::isVoxelActive(const glm::vec3 &worldPos) const
{
	if (worldPos.y < 0.f || worldPos.y >= static_cast<float>(CHUNK_HEIGHT))
		return false;

	const int chunkX = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE));
	const int chunkZ = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE));
	const glm::ivec3 chunkPos(chunkX, 0, chunkZ);

	std::shared_lock<std::shared_mutex> lock(m_mutex);
	auto it = m_chunks.find(chunkPos);
	if (it == m_chunks.end() || it->second->getState() < ChunkState::GENERATED)
		return false;

	const int localX = static_cast<int>(std::floor(worldPos.x)) - chunkX * CHUNK_SIZE;
	const int localY = static_cast<int>(std::floor(worldPos.y));
	const int localZ = static_cast<int>(std::floor(worldPos.z)) - chunkZ * CHUNK_SIZE;
	return it->second->getVoxel(localX, localY, localZ).type != AIR;
}

Chunk *ChunkManager::getChunk(const glm::ivec3 &chunkPos)
{
	auto it = m_chunks.find(chunkPos);
	return it != m_chunks.end() ? it->second : nullptr;
}

const Chunk *ChunkManager::getChunk(const glm::ivec3 &chunkPos) const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	auto it = m_chunks.find(chunkPos);
	return it != m_chunks.end() ? it->second : nullptr;
}

Chunk *ChunkManager::getChunkAtWorldPos(const glm::vec3 &worldPos)
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	return getChunk(worldToChunkCoord(worldPos));
}

size_t ChunkManager::chunkCount() const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	return m_chunks.size();
}

size_t ChunkManager::pendingLoadCount() const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	return (m_loadQueue.size() >= m_loadQueueHead) ? (m_loadQueue.size() - m_loadQueueHead) : 0;
}

size_t ChunkManager::pendingGenJobs() const
{
	return m_pendingGenJobsCount.load();
}

size_t ChunkManager::pendingMeshJobs() const
{
	return m_pendingMeshJobsCount.load();
}

bool ChunkManager::prepareAndGenerateChunk(Chunk *chunk, TerrainGenerator &generator)
{
	if (!chunk)
		return false;
	// Shared synchronous bootstrap contract (issue #112): voxel storage is
	// prepared on the calling thread immediately before generation; workers
	// never acquire or release voxel backing. On allocation failure the
	// chunk is returned to the pool so the slot is not leaked.
	if (!chunk->prepareVoxelStorageForGeneration())
	{
		if (m_chunkPool)
			m_chunkPool->release(chunk);
		return false;
	}
	chunk->generateTerrain(generator);
	return true;
}

void ChunkManager::generateInitialArea(const glm::vec3 &center, int radiusChunks, VmaAllocator allocator,
						 ImmediateCommands &imm, MeshArenas &arenas)
{
	if (!m_terrainGenerator || !m_chunkPool || !allocator)
		return;

	// Square footprint (2r+1)^2 plus a little margin for safety.
	const size_t bootstrapNeed =
		static_cast<size_t>(2 * radiusChunks + 1) * static_cast<size_t>(2 * radiusChunks + 1) + 16;
	m_chunkPool->ensureCapacity(bootstrapNeed);

	const glm::ivec3 camChunk = worldToChunkCoord(center);
	std::vector<std::pair<glm::ivec3, Chunk *>> created;

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
			Chunk *chunk = m_chunkPool->acquire(glm::vec3(
				static_cast<float>(pos.x * CHUNK_SIZE), 0.f,
				static_cast<float>(pos.z * CHUNK_SIZE)));
			if (!chunk)
			{
				// Grow once more and retry (should be rare if ensureCapacity above is correct).
				m_chunkPool->ensureCapacity(m_chunkPool->capacity() + 64);
				chunk = m_chunkPool->acquire(glm::vec3(
					static_cast<float>(pos.x * CHUNK_SIZE), 0.f,
					static_cast<float>(pos.z * CHUNK_SIZE)));
				if (!chunk)
					continue;
			}
			if (!prepareAndGenerateChunk(chunk, *m_terrainGenerator))
				continue;
			created.emplace_back(pos, chunk);
		}
	}

	// Shell + mesh on main thread for bootstrap quality.
	{
		std::lock_guard<std::shared_mutex> lock(m_mutex);
		for (const auto &p : created)
		{
			m_chunks[p.first] = p.second;
			p.second->setActiveIndex(m_activeChunks.size());
			m_activeChunks.push_back(p.second);
			p.second->setVisible(true);
		}
		for (const auto &p : created)
			ensureShellPopulated(p.second, p.first);
		// Bootstrap meshing gets cross-chunk light too: every created chunk
		// is generated and registered by now, so the halo snapshots see the
		// full neighborhood (issue #141 review fix).
		for (const auto &p : created)
			ensureLightHalo(p.second, p.first);
	}

	for (const auto &p : created)
	{
		p.second->generateMesh();
		ChunkLightHalo *halo = p.second->lightHalo();
		p.second->setLightHalo(nullptr);
		if (halo)
			m_lightHaloPool.release(halo);
	}

	// One-time sync upload before the first frame (nothing in flight yet).
	for (const auto &p : created)
		p.second->uploadToGPU(allocator, imm, arenas);

	std::cout << "Bootstrap: " << created.size() << " chunks around spawn\n";
}
