#include "ChunkManager.hpp"
#include <Camera/Camera.hpp>
#include <Shader/Shader.hpp>
#include <Renderer/Renderer.hpp>
#include <Engine/ThreadPool.hpp>
#include <Chunk/ChunkManager.hpp>
#include <glm/gtc/matrix_access.hpp>

#include <iostream>
#include <algorithm>
#include <cmath>
#include <execution>

ChunkManager::ChunkManager(TerrainGenerator *terrainGenerator, ThreadPool *threadPool, ChunkPool *chunkPool, RenderTiming &renderTiming)
	: m_terrainGenerator(terrainGenerator), p_threadPool(threadPool), m_chunkPool(chunkPool), m_renderTiming(renderTiming)
{
}

ChunkManager::~ChunkManager()
{
	// Release all chunks back to the pool
	for (auto &[pos, chunkPtr] : chunks)
	{
		if (chunkPtr && m_chunkPool)
			m_chunkPool->release(chunkPtr);
	}
	chunks.clear();
	activeChunks.clear();
}

void ChunkManager::updatePlayerPosition(const glm::ivec2 &newPlayerChunkPos, const Camera &camera, const RenderSettings &settings)
{
	unloadOutOfRangeChunks(camera, settings);
	loadChunksAroundPlayer(glm::ivec3(newPlayerChunkPos.x, 0, newPlayerChunkPos.y), camera, settings);
}

void ChunkManager::processChunkLoading(const RenderSettings &settings, int budget)
{
	thread_local std::vector<glm::ivec3> toLoad;
	toLoad.clear();
	{
		std::lock_guard<std::shared_mutex> lock(chunkMutex);
		int chunkCount = 0;
		while (!chunkLoadQueue.empty() && chunkCount < budget)
		{
			toLoad.push_back(chunkLoadQueue.front());
			chunkLoadQueue.pop();
			chunkCount++;
		}
	}

	if (toLoad.empty()) return;

	// Acquire chunks from pool (thread-safe, does not need chunkMutex)
	thread_local std::vector<std::pair<glm::ivec3, Chunk*>> acquiredChunks;
	acquiredChunks.clear();
	for (const auto& chunkPos : toLoad)
	{
		Chunk *chunk = m_chunkPool->acquire(glm::vec3(chunkPos.x * CHUNK_SIZE, 0.0f, chunkPos.z * CHUNK_SIZE));
		if (chunk)
		{
			acquiredChunks.push_back({chunkPos, chunk});
		}
	}

	if (acquiredChunks.empty()) return;

	// Insert into map and active set
	{
		std::lock_guard<std::shared_mutex> lock(chunkMutex);
		for (const auto& pair : acquiredChunks)
		{
			const glm::ivec3& chunkPos = pair.first;
			Chunk* chunk = pair.second;
			
			if (chunks.find(chunkPos) == chunks.end())
			{
				chunks[chunkPos] = chunk;
				activeChunks.push_back(chunk);
			}
			else
			{
				// In rare case it was already loaded somehow, release it
				m_chunkPool->release(chunk);
			}
		}
	}
}

void ChunkManager::performFrustumCulling(const Camera &camera, int windowWidth, int windowHeight, const RenderSettings &settings)
{
	auto start = std::chrono::high_resolution_clock::now();
	glm::mat4 clipMatrix = camera.getProjectionMatrix(static_cast<float>(windowWidth), static_cast<float>(windowHeight), static_cast<float>(settings.maxRenderDistance)) * camera.getViewMatrix();
	
	// Compute Frustum AABB in world space for fast broad-phase culling
	glm::mat4 invClip = glm::inverse(clipMatrix);
	glm::vec3 fMin(std::numeric_limits<float>::max());
	glm::vec3 fMax(std::numeric_limits<float>::lowest());
	for (int x = -1; x <= 1; x += 2) {
		for (int y = -1; y <= 1; y += 2) {
			for (int z = -1; z <= 1; z += 2) {
				glm::vec4 pt = invClip * glm::vec4(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), 1.0f);
				glm::vec3 wpt = glm::vec3(pt) / pt.w;
				fMin = glm::min(fMin, wpt);
				fMax = glm::max(fMax, wpt);
			}
		}
	}

	std::array<glm::vec4, 6> frustumPlanes;

	frustumPlanes[0] = glm::row(clipMatrix, 3) + glm::row(clipMatrix, 0); // Left
	frustumPlanes[1] = glm::row(clipMatrix, 3) - glm::row(clipMatrix, 0); // Right
	frustumPlanes[2] = glm::row(clipMatrix, 3) + glm::row(clipMatrix, 1); // Bottom
	frustumPlanes[3] = glm::row(clipMatrix, 3) - glm::row(clipMatrix, 1); // Top
	frustumPlanes[4] = glm::row(clipMatrix, 3) + glm::row(clipMatrix, 2); // Near
	frustumPlanes[5] = glm::row(clipMatrix, 3) - glm::row(clipMatrix, 2); // Far

	for (auto &plane : frustumPlanes)
	{
		plane = plane / glm::length(glm::vec3(plane));
	}

	std::shared_lock<std::shared_mutex> lock(chunkMutex);
	for (Chunk *chunk : activeChunks)
	{
		glm::vec3 aabbMin = chunk->getPosition();
		glm::vec3 aabbMax = aabbMin + glm::vec3(CHUNK_SIZE, CHUNK_HEIGHT, CHUNK_SIZE);

		// Broad-phase: Frustum AABB test
		if (aabbMax.x < fMin.x || aabbMin.x > fMax.x || 
			aabbMax.y < fMin.y || aabbMin.y > fMax.y || 
			aabbMax.z < fMin.z || aabbMin.z > fMax.z)
		{
			chunk->setVisible(false);
			continue;
		}

		bool isVisible = true;
		for (const auto &plane : frustumPlanes)
		{
			glm::vec3 pv(
				plane.x >= 0.0f ? aabbMax.x : aabbMin.x,
				plane.y >= 0.0f ? aabbMax.y : aabbMin.y,
				plane.z >= 0.0f ? aabbMax.z : aabbMin.z);
			if (glm::dot(glm::vec3(plane), pv) + plane.w < 0.0f)
			{
				isVisible = false;
				break;
			}
		}
		chunk->setVisible(isVisible);
	}
	auto end = std::chrono::high_resolution_clock::now();
	m_renderTiming.frustumCulling = std::chrono::duration<float, std::milli>(end - start).count();
}

void ChunkManager::generatePendingVoxels(const Camera &camera, const RenderSettings &settings, unsigned int seed, int budget)
{
	if (!m_terrainGenerator || !p_threadPool)
		return;

	std::lock_guard<std::shared_mutex> lock(chunkMutex);

	// Trier les chunks par distance au joueur pour une génération plus cohérente
	struct ChunkGenInfo
	{
		Chunk *chunk;
		float distance;
	};

	thread_local std::vector<ChunkGenInfo> genQueueVec;
	genQueueVec.clear();
	genQueueVec.reserve(activeChunks.size());
	const glm::vec3 camPos = camera.getPosition();

	for (Chunk *chunk : activeChunks)
	{
		if (chunk->isVisible() && chunk->getState() == ChunkState::UNLOADED && chunksInTransit.find(chunk) == chunksInTransit.end())
		{
			glm::vec3 chunkCenter = chunk->getPosition() + glm::vec3(CHUNK_SIZE / 2.0f);
			float dx = chunkCenter.x - camPos.x;
			float dz = chunkCenter.z - camPos.z;
			float distanceSq = dx * dx + dz * dz;
			genQueueVec.push_back({chunk, distanceSq});
		}
	} // Générer les chunks par ordre de priorité

	const int chunksToProcess = std::min(budget, static_cast<int>(genQueueVec.size()));
	if (chunksToProcess <= 0)
		return;

	std::partial_sort(genQueueVec.begin(), genQueueVec.begin() + chunksToProcess, genQueueVec.end(),
					  [](const ChunkGenInfo &a, const ChunkGenInfo &b)
					  {
						  return a.distance < b.distance;
					  });

	const int currentSeed = m_terrainGenerator->getSeed();
	const float lodThreshold = static_cast<float>(settings.minRenderDistance) * 2.0f;
	const float lodThresholdSq = lodThreshold * lodThreshold;
	for (int i = 0; i < chunksToProcess; ++i)
	{
		Chunk *chunk = genQueueVec[i].chunk;
		float distanceSq = genQueueVec[i].distance;
		TaskPriority priority = calculateTaskPriority(distanceSq, lodThresholdSq);

		chunksInTransit.insert(chunk);
		auto future = p_threadPool->enqueue(priority, [chunk, currentSeed]()
											{
												TerrainGenerator& localGenerator = TerrainGenerator::getThreadLocal(currentSeed);
												chunk->generateTerrain(localGenerator); });
		pendingGenerationTasks.push_back({std::move(future), chunk});
	}
}

void ChunkManager::meshPendingChunks(const Camera &camera, const RenderSettings &settings, int budget)
{
	if (!p_threadPool)
		return;

	std::lock_guard<std::shared_mutex> lock(chunkMutex);

	// Trier les chunks par distance au joueur pour un maillage plus cohérent
	struct ChunkMeshInfo
	{
		Chunk *chunk;
		float distance;
	};

	thread_local std::vector<ChunkMeshInfo> meshQueueVec;
	meshQueueVec.clear();
	meshQueueVec.reserve(activeChunks.size());
	const glm::vec3 camPos = camera.getPosition();

	for (Chunk *chunk : activeChunks)
	{
		if (chunk->isVisible() && chunk->getState() == ChunkState::GENERATED && chunksInTransit.find(chunk) == chunksInTransit.end())
		{
			glm::vec3 chunkCenter = chunk->getPosition() + glm::vec3(CHUNK_SIZE / 2.0f);
			float dx = chunkCenter.x - camPos.x;
			float dz = chunkCenter.z - camPos.z;
			float distanceSq = dx * dx + dz * dz;
			meshQueueVec.push_back({chunk, distanceSq});
		}
	} // Mailler les chunks par ordre de priorité

	// K: Upgrade any LOD-meshed chunks that have since come within normal range.
	// Resetting to GENERATED lets them fall into the dispatch loop below with a
	// full-quality generateMesh() pass.
	const float lodThreshold = static_cast<float>(settings.minRenderDistance) * 2.0f;
	const float lodThresholdSq = lodThreshold * lodThreshold;
	for (Chunk *chunk : activeChunks)
	{
		if (chunk->isLODMesh() && chunk->getState() == ChunkState::MESHED &&
			chunksInTransit.find(chunk) == chunksInTransit.end())
		{
			glm::vec3 cc = chunk->getPosition() + glm::vec3(CHUNK_SIZE / 2.0f);
			float dx = cc.x - camPos.x;
			float dz = cc.z - camPos.z;
			float distSq = dx * dx + dz * dz;
			if (distSq < lodThresholdSq)
				chunk->setState(ChunkState::GENERATED); // Force full re-mesh
		}
	}

	const int chunksToProcess = std::min(budget, static_cast<int>(meshQueueVec.size()));
	if (chunksToProcess <= 0)
		return;

	std::partial_sort(meshQueueVec.begin(), meshQueueVec.begin() + chunksToProcess, meshQueueVec.end(),
					  [](const ChunkMeshInfo &a, const ChunkMeshInfo &b)
					  {
						  return a.distance < b.distance;
					  });

	for (int i = 0; i < chunksToProcess; ++i)
	{
		Chunk *chunk = meshQueueVec[i].chunk;
		float chunkDistSq = meshQueueVec[i].distance; // K: distance already computed
		glm::vec3 wp = chunk->getPosition();
		glm::ivec3 ci(static_cast<int>(std::round(wp.x)) / CHUNK_SIZE, 0,
					  static_cast<int>(std::round(wp.z)) / CHUNK_SIZE);
		chunksInTransit.insert(chunk);
		TaskPriority priority = calculateTaskPriority(chunkDistSq, lodThresholdSq);
		if (chunkDistSq > lodThresholdSq)
		{
			// K: Distant chunk — simplified column-top mesh, no shell needed
			auto future = p_threadPool->enqueue(priority, [chunk]()
												{ chunk->generateLODMesh(); });
			pendingMeshingTasks.push_back({std::move(future), chunk});
		}
		else
		{
			// E: Ensure neighbor shell data is available before the off-thread mesh task runs
			ensureShellPopulated(chunk, ci);
			auto future = p_threadPool->enqueue(priority, [chunk]()
												{ chunk->generateMesh(); });
			pendingMeshingTasks.push_back({std::move(future), chunk});
		}
	}
}

void ChunkManager::drawVisibleChunks(Shader &shader, const Camera &camera, const GLuint &textureAtlas, const ShaderParameters &shaderParams, Renderer *renderer, RenderSettings &renderSettings, int windowWidth, int windowHeight)
{
	auto start = std::chrono::high_resolution_clock::now();
	std::shared_lock<std::shared_mutex> lock(chunkMutex);
	renderSettings.visibleChunksCount = 0;
	renderSettings.visibleVoxelsCount = 0;

	// --- Set all frame-constant uniforms once ---
	shader.use();
	shader.setMat4("projection", camera.getProjectionMatrix(static_cast<float>(windowWidth), static_cast<float>(windowHeight), static_cast<float>(renderSettings.maxRenderDistance)));
	shader.setMat4("model", glm::mat4(1.0f));
	shader.setMat4("view", camera.getViewMatrix());
	shader.setInt("textureArray", 0);

	shader.setVec3("viewPos", camera.getPosition());

	// M: fog/lighting uniforms are already set by Engine::renderScene() before this
	// call — removing the duplicate setFloat/setVec3 calls here.

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, textureAtlas);

	// --- Opaque pass ---
	// Cache distances before sorting to reduce complexity from O(N log N) to O(N) operations.
	m_visibleOpaquePairs.clear();
	m_visibleOpaquePairs.reserve(activeChunks.size());

	glm::vec3 camPos = camera.getPosition();
	for (Chunk *chunk : activeChunks)
	{
		if (chunk->isVisible() && chunk->getState() >= ChunkState::MESHED)
		{
			glm::vec3 center = chunk->getPosition() + glm::vec3(CHUNK_SIZE / 2.0f);
			float distSq = glm::dot(center - camPos, center - camPos);
			m_visibleOpaquePairs.push_back({distSq, chunk});
		}
	}

	// Tri front-to-back (plus proche au plus lointain)
	std::sort(m_visibleOpaquePairs.begin(), m_visibleOpaquePairs.end(), [](const auto& a, const auto& b)
			  {
				  return a.first < b.first;
			  });

	for (const auto& pair : m_visibleOpaquePairs)
	{
		Chunk *chunk = pair.second;
		renderSettings.visibleVoxelsCount += chunk->draw();
		renderSettings.visibleChunksCount++;

		if (renderSettings.chunkBorders && renderer)
		{
			renderer->drawBoundingBox(*chunk, camera);
		}
	}
	glBindVertexArray(0);

	// --- Water transparency sub-pass ---
	// H: Rebuild sorted water list only when camera moved > CHUNK_SIZE/2 from last sort.
	constexpr float kResortThreshSq = (CHUNK_SIZE / 2.0f) * (CHUNK_SIZE / 2.0f);
	float camMovedSq = glm::dot(camPos - m_lastWaterSortCamPos, camPos - m_lastWaterSortCamPos);
	if (camMovedSq > kResortThreshSq || m_cachedWaterChunks.empty())
	{
		// Cache distances before sorting to reduce complexity from O(N log N) to O(N) operations.
		m_waterPairs.clear();
		for (Chunk *chunk : activeChunks)
		{
			if (chunk->isVisible() && chunk->getState() >= ChunkState::MESHED && chunk->hasWaterMesh())
			{
				glm::vec3 center = chunk->getPosition() + glm::vec3(CHUNK_SIZE / 2.0f);
				float distSq = glm::dot(center - camPos, center - camPos);
				m_waterPairs.push_back({distSq, chunk});
			}
		}
		std::sort(m_waterPairs.begin(), m_waterPairs.end(), [](const auto& a, const auto& b)
				  {
					  return a.first > b.first; // back-to-front
				  });

		m_cachedWaterChunks.clear();
		m_cachedWaterChunks.reserve(m_waterPairs.size());
		for (const auto& pair : m_waterPairs)
		{
			m_cachedWaterChunks.push_back(pair.second);
		}
		m_lastWaterSortCamPos = camPos;
	}

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE); // V1: Allow seeing water from below

	for (Chunk *chunk : m_cachedWaterChunks)
	{
		if (chunk->isVisible() && chunk->getState() >= ChunkState::MESHED)
			chunk->drawWater();
	}
	glBindVertexArray(0);

	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);

	auto end = std::chrono::high_resolution_clock::now();
	m_renderTiming.chunkRendering = std::chrono::duration<float, std::milli>(end - start).count();
}

void ChunkManager::drawShadows(const Shader &shader, const glm::vec3 &cameraPos) const
{
	std::shared_lock<std::shared_mutex> lock(chunkMutex);
	shader.use();
	// Set model matrix once for all chunks to avoid redundant per-chunk API overhead
	shader.setMat4("model", glm::mat4(1.0f));

	// Rayon de couverture de la shadow map (512.0f) + marge pour la diagonale du chunk (~250.0f)
	constexpr float kShadowCullDistanceSq = (512.0f + 250.0f) * (512.0f + 250.0f);

	for (Chunk *chunk : activeChunks)
	{
		if (chunk->getState() < ChunkState::MESHED)
			continue;

		// Culling de distance pour couvrir la boîte de projection d'ombres centrée sur la caméra
		glm::vec3 chunkCenter = chunk->getPosition() + glm::vec3(CHUNK_SIZE / 2.0f);
		float dx = chunkCenter.x - cameraPos.x;
		float dy = chunkCenter.y - cameraPos.y;
		float dz = chunkCenter.z - cameraPos.z;
		float distSq = dx * dx + dy * dy + dz * dz;

		if (distSq > kShadowCullDistanceSq)
			continue;

		chunk->drawShadow();
	}
	glBindVertexArray(0);
}

void ChunkManager::uploadPendingMeshes(int budget)
{
	std::shared_lock<std::shared_mutex> lock(chunkMutex);
	int uploaded = 0;
	for (Chunk *chunk : activeChunks)
	{
		if (uploaded >= budget)
			break;
		if (chunk->getState() == ChunkState::MESHED && chunk->needsGPUUpload() && chunksInTransit.find(chunk) == chunksInTransit.end())
		{
			chunk->uploadToGPU();
			++uploaded;
		}
	}
}

void ChunkManager::ensureShellPopulated(Chunk *chunk, const glm::ivec3 &chunkIdx)
{
	if (!chunk->isShellEmpty())
		return;
	chunk->rebuildShellFromNeighbors(
		getChunk(chunkIdx + glm::ivec3(-1, 0, 0)),
		getChunk(chunkIdx + glm::ivec3(+1, 0, 0)),
		getChunk(chunkIdx + glm::ivec3(0, 0, -1)),
		getChunk(chunkIdx + glm::ivec3(0, 0, +1)));
}

bool ChunkManager::deleteVoxel(const glm::vec3 &worldPos)
{
	int chunkX = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE));
	int chunkZ = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE));
	glm::ivec3 chunkPos(chunkX, 0, chunkZ);

	std::lock_guard<std::shared_mutex> lock(chunkMutex);
	auto it = chunks.find(chunkPos);
	if (it != chunks.end())
	{
		bool modified = it->second->deleteVoxel(worldPos);
		if (modified)
		{
			const int localX = static_cast<int>(std::floor(worldPos.x)) - chunkX * CHUNK_SIZE;
			const int localY = static_cast<int>(std::floor(worldPos.y));
			const int localZ = static_cast<int>(std::floor(worldPos.z)) - chunkZ * CHUNK_SIZE;

			// Update the neighbor's shell voxels so its mesh reflects the change
			if (localX == 0)
			{
				glm::ivec3 nPos = chunkPos + glm::ivec3(-1, 0, 0);
				Chunk *neighbor = getChunk(nPos);
				if (neighbor)
				{
					ensureShellPopulated(neighbor, nPos);
					neighbor->setVoxel(CHUNK_SIZE, localY, localZ, AIR);
					neighbor->setState(ChunkState::GENERATED);
				}
			}
			if (localX == CHUNK_SIZE - 1)
			{
				glm::ivec3 nPos = chunkPos + glm::ivec3(1, 0, 0);
				Chunk *neighbor = getChunk(nPos);
				if (neighbor)
				{
					ensureShellPopulated(neighbor, nPos);
					neighbor->setVoxel(-1, localY, localZ, AIR);
					neighbor->setState(ChunkState::GENERATED);
				}
			}
			if (localZ == 0)
			{
				glm::ivec3 nPos = chunkPos + glm::ivec3(0, 0, -1);
				Chunk *neighbor = getChunk(nPos);
				if (neighbor)
				{
					ensureShellPopulated(neighbor, nPos);
					neighbor->setVoxel(localX, localY, CHUNK_SIZE, AIR);
					neighbor->setState(ChunkState::GENERATED);
				}
			}
			if (localZ == CHUNK_SIZE - 1)
			{
				glm::ivec3 nPos = chunkPos + glm::ivec3(0, 0, 1);
				Chunk *neighbor = getChunk(nPos);
				if (neighbor)
				{
					ensureShellPopulated(neighbor, nPos);
					neighbor->setVoxel(localX, localY, -1, AIR);
					neighbor->setState(ChunkState::GENERATED);
				}
			}
		}
		return modified;
	}
	return false;
}

bool ChunkManager::placeVoxel(const glm::vec3 &worldPos, TextureType type)
{
	int chunkX = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE));
	int chunkZ = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE));
	glm::ivec3 chunkPos(chunkX, 0, chunkZ);

	std::lock_guard<std::shared_mutex> lock(chunkMutex);
	auto it = chunks.find(chunkPos);
	if (it != chunks.end())
	{
		bool modified = it->second->placeVoxel(worldPos, type);
		if (modified)
		{
			const int localX = static_cast<int>(std::floor(worldPos.x)) - chunkX * CHUNK_SIZE;
			const int localY = static_cast<int>(std::floor(worldPos.y));
			const int localZ = static_cast<int>(std::floor(worldPos.z)) - chunkZ * CHUNK_SIZE;

			// Update the neighbor's shell voxels so its mesh reflects the change
			if (localX == 0)
			{
				glm::ivec3 nPos = chunkPos + glm::ivec3(-1, 0, 0);
				Chunk *neighbor = getChunk(nPos);
				if (neighbor)
				{
					ensureShellPopulated(neighbor, nPos);
					neighbor->setVoxel(CHUNK_SIZE, localY, localZ, type);
					neighbor->setState(ChunkState::GENERATED);
				}
			}
			if (localX == CHUNK_SIZE - 1)
			{
				glm::ivec3 nPos = chunkPos + glm::ivec3(1, 0, 0);
				Chunk *neighbor = getChunk(nPos);
				if (neighbor)
				{
					ensureShellPopulated(neighbor, nPos);
					neighbor->setVoxel(-1, localY, localZ, type);
					neighbor->setState(ChunkState::GENERATED);
				}
			}
			if (localZ == 0)
			{
				glm::ivec3 nPos = chunkPos + glm::ivec3(0, 0, -1);
				Chunk *neighbor = getChunk(nPos);
				if (neighbor)
				{
					ensureShellPopulated(neighbor, nPos);
					neighbor->setVoxel(localX, localY, CHUNK_SIZE, type);
					neighbor->setState(ChunkState::GENERATED);
				}
			}
			if (localZ == CHUNK_SIZE - 1)
			{
				glm::ivec3 nPos = chunkPos + glm::ivec3(0, 0, 1);
				Chunk *neighbor = getChunk(nPos);
				if (neighbor)
				{
					ensureShellPopulated(neighbor, nPos);
					neighbor->setVoxel(localX, localY, -1, type);
					neighbor->setState(ChunkState::GENERATED);
				}
			}
		}
		return modified;
	}
	return false;
}

bool ChunkManager::isVoxelActive(const glm::vec3 &worldPos) const
{
	if (worldPos.y < 0 || worldPos.y >= CHUNK_HEIGHT)
		return false;

	int chunkX = static_cast<int>(std::floor(worldPos.x / CHUNK_SIZE));
	int chunkZ = static_cast<int>(std::floor(worldPos.z / CHUNK_SIZE));
	glm::ivec3 chunkPos(chunkX, 0, chunkZ);

	std::shared_lock<std::shared_mutex> lock(chunkMutex); // const method, shared read lock
	auto it = chunks.find(chunkPos);
	if (it != chunks.end() && it->second->getState() >= ChunkState::GENERATED) // Voxels exist once generated
	{
		// Convert world coordinates to local voxel coordinates
		int localX = static_cast<int>(std::floor(worldPos.x)) - chunkX * CHUNK_SIZE;
		int localY = static_cast<int>(std::floor(worldPos.y));
		int localZ = static_cast<int>(std::floor(worldPos.z)) - chunkZ * CHUNK_SIZE;
		return it->second->getVoxel(localX, localY, localZ).type != AIR;
	}
	return false; // Chunk not found or not generated
}

Chunk *ChunkManager::getChunk(const glm::ivec3 &chunkPos)
{
	// This internal getChunk is called by place/deleteVoxel, which already holds the lock.
	// If called externally, it would need its own lock.
	// For simplicity, assume internal calls for now. If external access is needed, add lock.
	auto it = chunks.find(chunkPos);
	if (it != chunks.end())
	{
		return it->second;
	}
	return nullptr;
}

const Chunk *ChunkManager::getChunk(const glm::ivec3 &chunkPos) const
{
	std::shared_lock<std::shared_mutex> lock(chunkMutex);
	auto it = chunks.find(chunkPos);
	if (it != chunks.end())
	{
		return it->second;
	}
	return nullptr;
}

const std::unordered_map<glm::ivec3, Chunk *, IVec3Hash> &ChunkManager::getAllChunks() const
{
	return chunks;
}

void ChunkManager::unloadOutOfRangeChunks(const Camera &camera, const RenderSettings &settings)
{
	const float unloadDist = static_cast<float>(settings.maxRenderDistance) * 1.5f;
	const float unloadDistSq = unloadDist * unloadDist;

	thread_local std::vector<glm::ivec3> chunksToUnload;
	chunksToUnload.clear();

	// Phase 1: Identify chunks to unload (using a shared lock, since we only read)
	{
		std::shared_lock<std::shared_mutex> lock(chunkMutex);
		for (const auto& pair : chunks)
		{
			const glm::ivec3& pos = pair.first;
			Chunk* chunkPtr = pair.second;
			
			glm::vec3 chunkCenter = chunkPtr->getPosition() + glm::vec3(CHUNK_SIZE / 2.0f);
			float dx = camera.getPosition().x - chunkCenter.x;
			float dz = camera.getPosition().z - chunkCenter.z;
			float distToPlayerSq = dx * dx + dz * dz;

			// Unload if significantly outside maxRenderDistance (e.g., 1.5x)
			if (distToPlayerSq > unloadDistSq)
			{
				// Do not unload chunks that are currently being processed
				if (chunksInTransit.find(chunkPtr) == chunksInTransit.end())
				{
					chunksToUnload.push_back(pos);
				}
			}
		}
	}

	// Phase 2: Actually remove them (using an exclusive lock)
	if (!chunksToUnload.empty())
	{
		std::lock_guard<std::shared_mutex> lock(chunkMutex);
		for (const auto& pos : chunksToUnload)
		{
			auto it = chunks.find(pos);
			if (it != chunks.end())
			{
				Chunk* chunkPtr = it->second;
				// Re-check in_transit just in case state changed
				if (chunksInTransit.find(chunkPtr) == chunksInTransit.end())
				{
					auto activeIt = std::find(activeChunks.begin(), activeChunks.end(), chunkPtr);
					if (activeIt != activeChunks.end())
					{
						*activeIt = activeChunks.back();
						activeChunks.pop_back();
					}
					m_chunkPool->release(chunkPtr);
					chunks.erase(it);
					m_cachedWaterChunks.clear(); // H: invalidate water sort cache
				}
			}
		}
	}
}

void ChunkManager::loadChunksAroundPlayer(const glm::ivec3 &cameraChunkPos, const Camera &camera, const RenderSettings &settings)
{
	const int radius = static_cast<int>(std::ceil(static_cast<float>(settings.maxRenderDistance) / CHUNK_SIZE));

	// Utiliser une priorité basée sur la distance pour charger les chunks
	struct ChunkLoadInfo
	{
		glm::ivec3 pos;
		float distance;
	};

	thread_local std::vector<ChunkLoadInfo> priorityQueueVec;
	priorityQueueVec.clear();
	priorityQueueVec.reserve((2 * radius + 1) * (2 * radius + 1));

	// Calculer la priorité pour chaque chunk potentiel
	const float maxDistSq = static_cast<float>(settings.maxRenderDistance) * static_cast<float>(settings.maxRenderDistance);
	const glm::vec3 camPos = camera.getPosition();

	{
		std::shared_lock<std::shared_mutex> lock(chunkMutex);
		for (int x = -radius; x <= radius; x++)
		{
			for (int z = -radius; z <= radius; z++)
			{
				glm::ivec3 chunkPos = cameraChunkPos + glm::ivec3(x, 0, z);

				// Skip immediately if already loaded
				if (chunks.find(chunkPos) != chunks.end())
					continue;

				glm::vec3 chunkCenterWorld = glm::vec3(
					chunkPos.x * CHUNK_SIZE + CHUNK_SIZE / 2.0f,
					0,
					chunkPos.z * CHUNK_SIZE + CHUNK_SIZE / 2.0f);

				float dx = camPos.x - chunkCenterWorld.x;
				float dz = camPos.z - chunkCenterWorld.z;
				float distToPlayerSq = dx * dx + dz * dz;

				if (distToPlayerSq <= maxDistSq)
				{
					priorityQueueVec.push_back({chunkPos, distToPlayerSq});
				}
			}
		}
	}

	// Charger les chunks par ordre de priorité
	std::sort(priorityQueueVec.begin(), priorityQueueVec.end(), [](const ChunkLoadInfo &a, const ChunkLoadInfo &b)
			  {
				  return a.distance < b.distance;
			  });

	std::lock_guard<std::shared_mutex> writeLock(chunkMutex);
	for (const auto &info : priorityQueueVec)
	{
		if (chunks.find(info.pos) == chunks.end())
		{
			chunkLoadQueue.push(info.pos);
		}
	}
}

void ChunkManager::processFinishedJobs()
{
	std::lock_guard<std::shared_mutex> lock(chunkMutex);

	// Check generation tasks
	for (size_t i = 0; i < pendingGenerationTasks.size(); )
	{
		if (pendingGenerationTasks[i].first.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			pendingGenerationTasks[i].first.get(); // Propagate exceptions if any
			chunksInTransit.erase(pendingGenerationTasks[i].second);
			pendingGenerationTasks[i] = std::move(pendingGenerationTasks.back());
			pendingGenerationTasks.pop_back();
		}
		else
		{
			++i;
		}
	}

	// Check meshing tasks
	for (size_t i = 0; i < pendingMeshingTasks.size(); )
	{
		if (pendingMeshingTasks[i].first.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			pendingMeshingTasks[i].first.get();
			Chunk *finishedChunk = pendingMeshingTasks[i].second;
			chunksInTransit.erase(finishedChunk);
			// H: A newly meshed chunk may have water geometry — invalidate the sorted
			// cache so it appears in the next water transparency pass without waiting
			// for the camera to move.
			if (finishedChunk->hasWaterMesh())
				m_cachedWaterChunks.clear();
			pendingMeshingTasks[i] = std::move(pendingMeshingTasks.back());
			pendingMeshingTasks.pop_back();
		}
		else
		{
			++i;
		}
	}
}

TaskPriority ChunkManager::calculateTaskPriority(float distanceSq, float lodThresholdSq) const
{
	if (distanceSq < lodThresholdSq * 0.25f) // (0.5f)^2
		return TaskPriority::High;
	if (distanceSq > lodThresholdSq)
		return TaskPriority::Low;
	return TaskPriority::Normal;
}
