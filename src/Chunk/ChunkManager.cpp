#include "ChunkManager.hpp"

#include <Camera/Camera.hpp>
#include <Vulkan/VkCommands.hpp>
#include <Vulkan/StagingRing.hpp>
#include <Vulkan/GpuResourceRetire.hpp>

#include <glm/gtc/matrix_access.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>

namespace
{
constexpr int kDeferredReleaseFrames = 3; // >= frames-in-flight
}

ChunkManager::ChunkManager(TerrainGenerator *terrainGenerator, ThreadPool *threadPool, ChunkPool *chunkPool,
						   RenderTiming &renderTiming)
	: m_terrainGenerator(terrainGenerator), m_threadPool(threadPool), m_chunkPool(chunkPool),
	  m_renderTiming(renderTiming)
{
}

ChunkManager::~ChunkManager()
{
	// Best-effort drain of async work so we don't release chunks still in workers.
	for (auto &t : m_pendingGenerationTasks)
	{
		if (t.first.valid())
			t.first.wait();
	}
	for (auto &t : m_pendingMeshingTasks)
	{
		if (t.first.valid())
			t.first.wait();
	}
	m_pendingGenerationTasks.clear();
	m_pendingMeshingTasks.clear();

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

void ChunkManager::updateStreaming(const Camera &camera, const RenderSettings &settings)
{
	queueUnloadOutOfRange(camera, settings);
	const glm::ivec3 camChunk = worldToChunkCoord(camera.getPosition());
	loadChunksAroundPlayer(camChunk, camera, settings);
}

void ChunkManager::processChunkLoading(int budget)
{
	if (budget <= 0 || !m_chunkPool)
		return;

	std::vector<glm::ivec3> toLoad;
	{
		std::lock_guard<std::shared_mutex> lock(m_mutex);
		int n = 0;
		while (!m_loadQueue.empty() && n < budget)
		{
			const glm::ivec3 pos = m_loadQueue.front();
			m_loadQueue.pop();
			m_enqueuedLoads.erase(pos);
			toLoad.push_back(pos);
			++n;
		}
	}
	if (toLoad.empty())
		return;

	std::vector<std::pair<glm::ivec3, Chunk *>> acquired;
	acquired.reserve(toLoad.size());
	for (const auto &chunkPos : toLoad)
	{
		Chunk *chunk = m_chunkPool->acquire(glm::vec3(
			static_cast<float>(chunkPos.x * CHUNK_SIZE), 0.0f,
			static_cast<float>(chunkPos.z * CHUNK_SIZE)));
		if (chunk)
			acquired.emplace_back(chunkPos, chunk);
	}

	std::lock_guard<std::shared_mutex> lock(m_mutex);
	for (const auto &pair : acquired)
	{
		if (m_chunks.find(pair.first) == m_chunks.end())
		{
			m_chunks[pair.first] = pair.second;
			m_activeChunks.push_back(pair.second);
			// Generate even before frustum test so the world fills around the player.
			pair.second->setVisible(true);
		}
		else
		{
			m_chunkPool->release(pair.second);
		}
	}
}

void ChunkManager::updateVisibility(const Camera &camera, int windowWidth, int windowHeight,
									const RenderSettings &settings)
{
	const auto t0 = std::chrono::high_resolution_clock::now();

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
	for (auto &p : planes)
	{
		const float len = glm::length(glm::vec3(p));
		if (len > 1e-6f)
			p /= len;
	}

	std::shared_lock<std::shared_mutex> lock(m_mutex);
	for (Chunk *chunk : m_activeChunks)
	{
		const glm::vec3 aabbMin = chunk->getPosition();
		const glm::vec3 aabbMax = aabbMin + glm::vec3(CHUNK_SIZE, CHUNK_HEIGHT, CHUNK_SIZE);

		bool visible = true;
		for (const auto &plane : planes)
		{
			const glm::vec3 pv(
				plane.x >= 0.f ? aabbMax.x : aabbMin.x,
				plane.y >= 0.f ? aabbMax.y : aabbMin.y,
				plane.z >= 0.f ? aabbMax.z : aabbMin.z);
			if (glm::dot(glm::vec3(plane), pv) + plane.w < 0.f)
			{
				visible = false;
				break;
			}
		}
		chunk->setVisible(visible);
	}
	(void)settings;

	const auto t1 = std::chrono::high_resolution_clock::now();
	m_renderTiming.frustumCulling =
		std::chrono::duration<float, std::milli>(t1 - t0).count();
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
	std::vector<Item> queue;
	queue.reserve(m_activeChunks.size());
	const glm::vec3 camPos = camera.getPosition();

	for (Chunk *chunk : m_activeChunks)
	{
		if (chunk->getState() == ChunkState::UNLOADED && !chunk->isInTransit())
		{
			const glm::vec3 c = chunk->getPosition() + glm::vec3(CHUNK_SIZE * 0.5f, 0.f, CHUNK_SIZE * 0.5f);
			const float dx = c.x - camPos.x;
			const float dz = c.z - camPos.z;
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

	const auto t0 = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < n; ++i)
	{
		Chunk *chunk = queue[i].chunk;
		const TaskPriority prio = calculateTaskPriority(queue[i].distSq, lodThreshSq);
		chunk->setInTransit(true);
		auto future = m_threadPool->enqueue(prio, [chunk, seed]() {
			TerrainGenerator &localGen = TerrainGenerator::getThreadLocal(seed);
			chunk->generateTerrain(localGen);
		});
		m_pendingGenerationTasks.emplace_back(std::move(future), chunk);
	}
	const auto t1 = std::chrono::high_resolution_clock::now();
	m_renderTiming.chunkGeneration =
		std::chrono::duration<float, std::milli>(t1 - t0).count();
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
	std::vector<Item> queue;
	queue.reserve(m_activeChunks.size());
	const glm::vec3 camPos = camera.getPosition();

	const float lodThresh = static_cast<float>(settings.minRenderDistance) * 2.f;
	const float lodThreshSq = lodThresh * lodThresh;

	// Promote distant LOD meshes back to full quality when close enough.
	for (Chunk *chunk : m_activeChunks)
	{
		if (chunk->isLODMesh() && chunk->getState() == ChunkState::MESHED && !chunk->isInTransit())
		{
			const glm::vec3 c = chunk->getPosition() + glm::vec3(CHUNK_SIZE * 0.5f, 0.f, CHUNK_SIZE * 0.5f);
			const float dx = c.x - camPos.x;
			const float dz = c.z - camPos.z;
			if (dx * dx + dz * dz < lodThreshSq)
				chunk->setState(ChunkState::GENERATED);
		}
	}

	for (Chunk *chunk : m_activeChunks)
	{
		if (chunk->getState() == ChunkState::GENERATED && !chunk->isInTransit())
		{
			const glm::vec3 c = chunk->getPosition() + glm::vec3(CHUNK_SIZE * 0.5f, 0.f, CHUNK_SIZE * 0.5f);
			const float dx = c.x - camPos.x;
			const float dz = c.z - camPos.z;
			queue.push_back({chunk, dx * dx + dz * dz});
		}
	}

	const int n = std::min(budget, static_cast<int>(queue.size()));
	if (n <= 0)
		return;

	std::partial_sort(queue.begin(), queue.begin() + n, queue.end(),
					  [](const Item &a, const Item &b) { return a.distSq < b.distSq; });

	const auto t0 = std::chrono::high_resolution_clock::now();
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
			auto future = m_threadPool->enqueue(prio, [chunk]() { chunk->generateLODMesh(); });
			m_pendingMeshingTasks.emplace_back(std::move(future), chunk);
		}
		else
		{
			ensureShellPopulated(chunk, ci);
			auto future = m_threadPool->enqueue(prio, [chunk]() { chunk->generateMesh(); });
			m_pendingMeshingTasks.emplace_back(std::move(future), chunk);
		}
	}
	const auto t1 = std::chrono::high_resolution_clock::now();
	m_renderTiming.meshGeneration =
		std::chrono::duration<float, std::milli>(t1 - t0).count();
}

int ChunkManager::uploadPendingMeshes(VmaAllocator allocator, StagingRing &staging, VkCommandBuffer cmd,
									  GpuResourceRetire &retire, const Camera &camera, int budget)
{
	if (!allocator || budget <= 0 || cmd == VK_NULL_HANDLE || !staging.isValid())
		return 0;

	struct Item
	{
		Chunk *chunk;
		float distSq;
	};
	std::vector<Item> queue;
	{
		std::shared_lock<std::shared_mutex> lock(m_mutex);
		queue.reserve(m_activeChunks.size());
		const glm::vec3 camPos = camera.getPosition();
		for (Chunk *chunk : m_activeChunks)
		{
			if (!chunk)
				continue;
			if (chunk->getState() == ChunkState::MESHED && chunk->needsGPUUpload() && !chunk->isInTransit())
			{
				const glm::vec3 c = chunk->getPosition() + glm::vec3(CHUNK_SIZE * 0.5f, 0.f, CHUNK_SIZE * 0.5f);
				const float dx = c.x - camPos.x;
				const float dz = c.z - camPos.z;
				queue.push_back({chunk, dx * dx + dz * dz});
			}
		}
	}

	if (queue.empty())
		return 0;

	const int n = std::min(budget, static_cast<int>(queue.size()));
	std::partial_sort(queue.begin(), queue.begin() + n, queue.end(),
					  [](const Item &a, const Item &b) { return a.distSq < b.distSq; });

	const auto t0 = std::chrono::high_resolution_clock::now();
	int uploaded = 0;
	for (int i = 0; i < n; ++i)
	{
		if (!queue[i].chunk->uploadToGPUAsync(allocator, staging, cmd, retire))
			break; // staging full — remaining wait next frame
		++uploaded;
	}
	const auto t1 = std::chrono::high_resolution_clock::now();
	m_renderTiming.chunkRendering =
		std::chrono::duration<float, std::milli>(t1 - t0).count(); // reuse slot for upload ms
	return uploaded;
}

void ChunkManager::processDeferredReleases(GpuResourceRetire &retire)
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
		c->releaseGPUDeferred(retire);
		if (m_chunkPool)
			m_chunkPool->release(c);
	}
	m_deferredRelease.clear();
	m_deferredReleaseAge = 0;
}

void ChunkManager::processFinishedJobs()
{
	std::lock_guard<std::shared_mutex> lock(m_mutex);

	for (size_t i = 0; i < m_pendingGenerationTasks.size();)
	{
		if (m_pendingGenerationTasks[i].first.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			m_pendingGenerationTasks[i].first.get();
			m_pendingGenerationTasks[i].second->setInTransit(false);
			m_pendingGenerationTasks[i] = std::move(m_pendingGenerationTasks.back());
			m_pendingGenerationTasks.pop_back();
		}
		else
		{
			++i;
		}
	}

	for (size_t i = 0; i < m_pendingMeshingTasks.size();)
	{
		if (m_pendingMeshingTasks[i].first.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			m_pendingMeshingTasks[i].first.get();
			m_pendingMeshingTasks[i].second->setInTransit(false);
			m_pendingMeshingTasks[i] = std::move(m_pendingMeshingTasks.back());
			m_pendingMeshingTasks.pop_back();
		}
		else
		{
			++i;
		}
	}
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
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	out.reserve(m_activeChunks.size() / 2 + 8);
	for (Chunk *chunk : m_activeChunks)
	{
		if (!chunk)
			continue;
		if (chunk->getOpaqueIndexCount() == 0)
			continue;
		const glm::vec3 c = chunk->getPosition() + glm::vec3(CHUNK_SIZE * 0.5f, 0.f, CHUNK_SIZE * 0.5f);
		const float dx = c.x - cam.x;
		const float dz = c.z - cam.z;
		if (dx * dx + dz * dz > r2)
			continue;
		out.push_back(chunk);
	}
}

void ChunkManager::queueUnloadOutOfRange(const Camera &camera, const RenderSettings &settings)
{
	const float unloadDist = static_cast<float>(settings.maxRenderDistance) * 1.5f;
	const float unloadDistSq = unloadDist * unloadDist;

	std::vector<glm::ivec3> toUnload;
	{
		std::shared_lock<std::shared_mutex> lock(m_mutex);
		for (const auto &pair : m_chunks)
		{
			Chunk *chunk = pair.second;
			if (!chunk || chunk->isInTransit())
				continue;
			const glm::vec3 center = chunk->getPosition() + glm::vec3(CHUNK_SIZE * 0.5f, 0.f, CHUNK_SIZE * 0.5f);
			const float dx = camera.getPosition().x - center.x;
			const float dz = camera.getPosition().z - center.z;
			if (dx * dx + dz * dz > unloadDistSq)
				toUnload.push_back(pair.first);
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

		auto activeIt = std::find(m_activeChunks.begin(), m_activeChunks.end(), chunk);
		if (activeIt != m_activeChunks.end())
		{
			*activeIt = m_activeChunks.back();
			m_activeChunks.pop_back();
		}
		m_chunks.erase(it);
		m_deferredRelease.push_back(chunk);
	}
	if (!toUnload.empty())
		m_deferredReleaseAge = 0; // reset age so new unloads wait full delay
}

void ChunkManager::loadChunksAroundPlayer(const glm::ivec3 &cameraChunkPos, const Camera &camera,
										  const RenderSettings &settings)
{
	const int radius =
		static_cast<int>(std::ceil(static_cast<float>(settings.maxRenderDistance) / static_cast<float>(CHUNK_SIZE)));
	const float maxDistSq = static_cast<float>(settings.maxRenderDistance) * static_cast<float>(settings.maxRenderDistance);
	const glm::vec3 camPos = camera.getPosition();

	struct LoadInfo
	{
		glm::ivec3 pos;
		float distSq;
	};
	std::vector<LoadInfo> candidates;
	candidates.reserve(static_cast<size_t>((2 * radius + 1) * (2 * radius + 1)));

	{
		std::shared_lock<std::shared_mutex> lock(m_mutex);
		for (int x = -radius; x <= radius; ++x)
		{
			for (int z = -radius; z <= radius; ++z)
			{
				const glm::ivec3 chunkPos = cameraChunkPos + glm::ivec3(x, 0, z);
				if (m_chunks.find(chunkPos) != m_chunks.end())
					continue;

				const glm::vec3 center(
					chunkPos.x * CHUNK_SIZE + CHUNK_SIZE * 0.5f, 0.f,
					chunkPos.z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
				const float dx = camPos.x - center.x;
				const float dz = camPos.z - center.z;
				const float distSq = dx * dx + dz * dz;
				if (distSq <= maxDistSq)
					candidates.push_back({chunkPos, distSq});
			}
		}
	}

	std::sort(candidates.begin(), candidates.end(),
			  [](const LoadInfo &a, const LoadInfo &b) { return a.distSq < b.distSq; });

	std::lock_guard<std::shared_mutex> lock(m_mutex);
	for (const auto &info : candidates)
	{
		if (m_chunks.find(info.pos) != m_chunks.end())
			continue;
		if (m_enqueuedLoads.count(info.pos) != 0)
			continue;
		m_loadQueue.push(info.pos);
		m_enqueuedLoads.insert(info.pos);
	}
}

void ChunkManager::ensureShellPopulated(Chunk *chunk, const glm::ivec3 &chunkIdx)
{
	if (!chunk || !chunk->isShellEmpty())
		return;
	// Caller holds exclusive lock.
	chunk->rebuildShellFromNeighbors(
		getChunk(chunkIdx + glm::ivec3(-1, 0, 0)),
		getChunk(chunkIdx + glm::ivec3(+1, 0, 0)),
		getChunk(chunkIdx + glm::ivec3(0, 0, -1)),
		getChunk(chunkIdx + glm::ivec3(0, 0, +1)));
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

	std::lock_guard<std::shared_mutex> lock(m_mutex);
	auto it = m_chunks.find(chunkPos);
	if (it == m_chunks.end())
		return false;

	const bool modified = it->second->deleteVoxel(worldPos);
	if (!modified)
		return false;

	const int localX = static_cast<int>(std::floor(worldPos.x)) - chunkX * CHUNK_SIZE;
	const int localY = static_cast<int>(std::floor(worldPos.y));
	const int localZ = static_cast<int>(std::floor(worldPos.z)) - chunkZ * CHUNK_SIZE;

	auto dirtyNeighbor = [&](const glm::ivec3 &nPos, int sx, int sy, int sz) {
		Chunk *neighbor = getChunk(nPos);
		if (!neighbor)
			return;
		ensureShellPopulated(neighbor, nPos);
		neighbor->setVoxel(sx, sy, sz, AIR);
		neighbor->setState(ChunkState::GENERATED);
	};

	if (localX == 0)
		dirtyNeighbor(chunkPos + glm::ivec3(-1, 0, 0), CHUNK_SIZE, localY, localZ);
	if (localX == CHUNK_SIZE - 1)
		dirtyNeighbor(chunkPos + glm::ivec3(1, 0, 0), -1, localY, localZ);
	if (localZ == 0)
		dirtyNeighbor(chunkPos + glm::ivec3(0, 0, -1), localX, localY, CHUNK_SIZE);
	if (localZ == CHUNK_SIZE - 1)
		dirtyNeighbor(chunkPos + glm::ivec3(0, 0, 1), localX, localY, -1);

	return true;
}

bool ChunkManager::placeVoxel(const glm::vec3 &worldPos, TextureType type)
{
	const int chunkX = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE));
	const int chunkZ = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE));
	const glm::ivec3 chunkPos(chunkX, 0, chunkZ);

	std::lock_guard<std::shared_mutex> lock(m_mutex);
	auto it = m_chunks.find(chunkPos);
	if (it == m_chunks.end())
		return false;

	const bool modified = it->second->placeVoxel(worldPos, type);
	if (!modified)
		return false;

	const int localX = static_cast<int>(std::floor(worldPos.x)) - chunkX * CHUNK_SIZE;
	const int localY = static_cast<int>(std::floor(worldPos.y));
	const int localZ = static_cast<int>(std::floor(worldPos.z)) - chunkZ * CHUNK_SIZE;

	auto dirtyNeighbor = [&](const glm::ivec3 &nPos, int sx, int sy, int sz) {
		Chunk *neighbor = getChunk(nPos);
		if (!neighbor)
			return;
		ensureShellPopulated(neighbor, nPos);
		neighbor->setVoxel(sx, sy, sz, type);
		neighbor->setState(ChunkState::GENERATED);
	};

	if (localX == 0)
		dirtyNeighbor(chunkPos + glm::ivec3(-1, 0, 0), CHUNK_SIZE, localY, localZ);
	if (localX == CHUNK_SIZE - 1)
		dirtyNeighbor(chunkPos + glm::ivec3(1, 0, 0), -1, localY, localZ);
	if (localZ == 0)
		dirtyNeighbor(chunkPos + glm::ivec3(0, 0, -1), localX, localY, CHUNK_SIZE);
	if (localZ == CHUNK_SIZE - 1)
		dirtyNeighbor(chunkPos + glm::ivec3(0, 0, 1), localX, localY, -1);

	return true;
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
	return m_loadQueue.size();
}

size_t ChunkManager::pendingGenJobs() const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	return m_pendingGenerationTasks.size();
}

size_t ChunkManager::pendingMeshJobs() const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	return m_pendingMeshingTasks.size();
}

void ChunkManager::generateInitialArea(const glm::vec3 &center, int radiusChunks, VmaAllocator allocator,
									   ImmediateCommands &imm)
{
	if (!m_terrainGenerator || !m_chunkPool || !allocator)
		return;

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
				continue;
			chunk->generateTerrain(*m_terrainGenerator);
			created.emplace_back(pos, chunk);
		}
	}

	// Shell + mesh on main thread for bootstrap quality.
	{
		std::lock_guard<std::shared_mutex> lock(m_mutex);
		for (const auto &p : created)
		{
			m_chunks[p.first] = p.second;
			m_activeChunks.push_back(p.second);
			p.second->setVisible(true);
		}
		for (const auto &p : created)
			ensureShellPopulated(p.second, p.first);
	}

	for (const auto &p : created)
		p.second->generateMesh();

	// One-time sync upload before the first frame (nothing in flight yet).
	for (const auto &p : created)
		p.second->uploadToGPU(allocator, imm);

	std::cout << "Bootstrap: " << created.size() << " chunks around spawn\n";
}
