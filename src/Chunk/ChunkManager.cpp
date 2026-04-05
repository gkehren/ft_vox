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

ChunkManager::ChunkManager(TerrainGenerator *terrainGenerator, ThreadPool *threadPool, RenderTiming &renderTiming)
	: m_terrainGenerator(terrainGenerator), p_threadPool(threadPool), m_renderTiming(renderTiming)
{
}

ChunkManager::~ChunkManager()
{
}

void ChunkManager::updatePlayerPosition(const glm::ivec2 &newPlayerChunkPos, const Camera &camera, const RenderSettings &settings)
{
	unloadOutOfRangeChunks(camera, settings);
	loadChunksAroundPlayer(glm::ivec3(newPlayerChunkPos.x, 0, newPlayerChunkPos.y), camera, settings);
}

void ChunkManager::processChunkLoading(const RenderSettings &settings, int budget)
{
	std::lock_guard<std::shared_mutex> lock(chunkMutex);
	int chunkCount = 0;
	while (!chunkLoadQueue.empty() && chunkCount < budget)
	{
		glm::ivec3 chunkPos = chunkLoadQueue.front();
		chunkLoadQueue.pop();

		if (chunks.find(chunkPos) == chunks.end())
		{
			auto [it, inserted] = chunks.emplace(chunkPos, Chunk(glm::vec3(chunkPos.x * CHUNK_SIZE, 0.0f, chunkPos.z * CHUNK_SIZE)));
			if (inserted)
			{
				activeChunks.insert(&it->second);
			}
		}
		chunkCount++;
	}
}

void ChunkManager::performFrustumCulling(const Camera &camera, int windowWidth, int windowHeight, const RenderSettings &settings)
{
	auto start = std::chrono::high_resolution_clock::now();
	glm::mat4 clipMatrix = camera.getProjectionMatrix(static_cast<float>(windowWidth), static_cast<float>(windowHeight), static_cast<float>(settings.maxRenderDistance)) * camera.getViewMatrix();
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
		// F: Proper AABB–frustum test via the positive-vertex method.
		// For each plane, find the AABB corner that maximises the dot product with the
		// plane normal (the "positive vertex"). If that corner is still outside the plane
		// the entire AABB is outside → cull. This is exact for axis-aligned boxes and
		// eliminates the over-conservative sphere used before (radius ~128 for a 16×256×16 chunk).
		glm::vec3 aabbMin = chunk->getPosition();
		glm::vec3 aabbMax = aabbMin + glm::vec3(CHUNK_SIZE, CHUNK_HEIGHT, CHUNK_SIZE);
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
	if (!m_terrainGenerator)
		return;

	std::lock_guard<std::shared_mutex> lock(chunkMutex);

	// Trier les chunks par distance au joueur pour une génération plus cohérente
	struct ChunkGenInfo
	{
		Chunk *chunk;
		float distance;

		bool operator<(const ChunkGenInfo &other) const
		{
			return distance > other.distance;
		}
	};

	std::priority_queue<ChunkGenInfo> genQueue;

	for (Chunk *chunk : activeChunks)
	{
		if (chunk->isVisible() && chunk->getState() == ChunkState::UNLOADED && chunksInTransit.find(chunk) == chunksInTransit.end())
		{
			glm::vec3 chunkCenter = chunk->getPosition() + glm::vec3(CHUNK_SIZE / 2.0f);
			float distance = glm::distance(
				glm::vec2(chunkCenter.x, chunkCenter.z),
				glm::vec2(camera.getPosition().x, camera.getPosition().z));
			genQueue.push({chunk, distance});
		}
	} // Générer les chunks par ordre de priorité
	int genDispatched = 0;
	while (!genQueue.empty() && genDispatched < budget)
	{
		Chunk *chunk = genQueue.top().chunk;
		float distance = genQueue.top().distance;
		int currentSeed = m_terrainGenerator->getSeed();

		TaskPriority priority = TaskPriority::Normal;
		const float lodThreshold = static_cast<float>(settings.minRenderDistance) * 2.0f;
		if (distance < lodThreshold * 0.5f) {
			priority = TaskPriority::High;
		} else if (distance > lodThreshold) {
			priority = TaskPriority::Low;
		}

		chunksInTransit.insert(chunk);
		auto future = p_threadPool->enqueue(priority, [chunk, currentSeed]()
											{
												TerrainGenerator& localGenerator = TerrainGenerator::getThreadLocal(currentSeed);
												chunk->generateTerrain(localGenerator); });
		pendingGenerationTasks.push_back({std::move(future), chunk});
		genQueue.pop();
		++genDispatched;
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

		bool operator<(const ChunkMeshInfo &other) const
		{
			return distance > other.distance;
		}
	};

	std::priority_queue<ChunkMeshInfo> meshQueue;

	for (Chunk *chunk : activeChunks)
	{
		if (chunk->isVisible() && chunk->getState() == ChunkState::GENERATED && chunksInTransit.find(chunk) == chunksInTransit.end())
		{
			glm::vec3 chunkCenter = chunk->getPosition() + glm::vec3(CHUNK_SIZE / 2.0f);
			float distance = glm::distance(
				glm::vec2(chunkCenter.x, chunkCenter.z),
				glm::vec2(camera.getPosition().x, camera.getPosition().z));
			meshQueue.push({chunk, distance});
		}
	} // Mailler les chunks par ordre de priorité

	// K: Upgrade any LOD-meshed chunks that have since come within normal range.
	// Resetting to GENERATED lets them fall into the dispatch loop below with a
	// full-quality generateMesh() pass.
	const float lodThreshold = static_cast<float>(settings.minRenderDistance) * 2.0f;
	for (Chunk *chunk : activeChunks)
	{
		if (chunk->isLODMesh() && chunk->getState() == ChunkState::MESHED &&
			chunksInTransit.find(chunk) == chunksInTransit.end())
		{
			glm::vec3 cc = chunk->getPosition() + glm::vec3(CHUNK_SIZE / 2.0f);
			float dist = glm::distance(
				glm::vec2(cc.x, cc.z),
				glm::vec2(camera.getPosition().x, camera.getPosition().z));
			if (dist < lodThreshold)
				chunk->setState(ChunkState::GENERATED); // Force full re-mesh
		}
	}

	int meshDispatched = 0;
	while (!meshQueue.empty() && meshDispatched < budget)
	{
		Chunk *chunk = meshQueue.top().chunk;
		float chunkDist = meshQueue.top().distance; // K: distance already computed
		glm::vec3 wp = chunk->getPosition();
		glm::ivec3 ci(static_cast<int>(std::round(wp.x)) / CHUNK_SIZE, 0,
					  static_cast<int>(std::round(wp.z)) / CHUNK_SIZE);
		chunksInTransit.insert(chunk);
		if (chunkDist > lodThreshold)
		{
			// K: Distant chunk — simplified column-top mesh, no shell needed
			auto future = p_threadPool->enqueue(TaskPriority::Low, [chunk]()
												{ chunk->generateLODMesh(); });
			pendingMeshingTasks.push_back({std::move(future), chunk});
		}
		else
		{
			// E: Ensure neighbor shell data is available before the off-thread mesh task runs
			ensureShellPopulated(chunk, ci);
			TaskPriority priority = (chunkDist < lodThreshold * 0.5f) ? TaskPriority::High : TaskPriority::Normal;
			auto future = p_threadPool->enqueue(priority, [chunk]()
												{ chunk->generateMesh(); });
			pendingMeshingTasks.push_back({std::move(future), chunk});
		}
		meshQueue.pop();
		++meshDispatched;
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
	shader.setMat4("projection", camera.getProjectionMatrix(static_cast<float>(windowWidth), static_cast<float>(windowHeight), 3000.0f));
	shader.setMat4("model", glm::mat4(1.0f));
	shader.setMat4("view", camera.getViewMatrix());
	shader.setInt("textureArray", 0);

	glm::vec3 sunPosition = camera.getPosition() + shaderParams.sunDirection * 2000.0f;
	shader.setVec3("lightPos", sunPosition);
	shader.setVec3("viewPos", camera.getPosition());

	// M: fog/lighting uniforms are already set by Engine::renderScene() before this
	// call — removing the duplicate setFloat/setVec3 calls here.

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, textureAtlas);

	// --- Opaque pass ---
	for (Chunk *chunk : activeChunks)
	{
		if (!chunk->isVisible() || chunk->getState() < ChunkState::MESHED)
			continue;

		renderSettings.visibleVoxelsCount += chunk->draw();
		renderSettings.visibleChunksCount++;

		if (renderSettings.chunkBorders && renderer)
		{
			renderer->drawBoundingBox(*chunk, camera);
		}
	}

	// --- Water transparency sub-pass ---
	// H: Rebuild sorted water list only when camera moved > CHUNK_SIZE/2 from last sort.
	glm::vec3 camPos = camera.getPosition();
	constexpr float kResortThreshSq = (CHUNK_SIZE / 2.0f) * (CHUNK_SIZE / 2.0f);
	float camMovedSq = glm::dot(camPos - m_lastWaterSortCamPos, camPos - m_lastWaterSortCamPos);
	if (camMovedSq > kResortThreshSq || m_cachedWaterChunks.empty())
	{
		m_cachedWaterChunks.clear();
		for (Chunk *chunk : activeChunks)
		{
			if (chunk->isVisible() && chunk->getState() >= ChunkState::MESHED && chunk->hasWaterMesh())
				m_cachedWaterChunks.push_back(chunk);
		}
		std::sort(m_cachedWaterChunks.begin(), m_cachedWaterChunks.end(), [&camPos](Chunk *a, Chunk *b)
				  {
					  glm::vec3 centerA = a->getPosition() + glm::vec3(CHUNK_SIZE / 2);
					  glm::vec3 centerB = b->getPosition() + glm::vec3(CHUNK_SIZE / 2);
					  return glm::dot(centerA - camPos, centerA - camPos) > glm::dot(centerB - camPos, centerB - camPos); });
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

	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);

	auto end = std::chrono::high_resolution_clock::now();
	m_renderTiming.chunkRendering = std::chrono::duration<float, std::milli>(end - start).count();
}

void ChunkManager::drawShadows(const Shader &shader) const
{
	std::shared_lock<std::shared_mutex> lock(chunkMutex);
	for (Chunk *chunk : activeChunks)
	{
		if (!chunk->isVisible() || chunk->getState() < ChunkState::MESHED)
			continue;
		chunk->drawShadow(shader);
	}
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
		bool modified = it->second.deleteVoxel(worldPos);
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
		bool modified = it->second.placeVoxel(worldPos, type);
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
	if (it != chunks.end() && it->second.getState() >= ChunkState::GENERATED) // Voxels exist once generated
	{
		// Convert world coordinates to local voxel coordinates
		int localX = static_cast<int>(std::floor(worldPos.x)) - chunkX * CHUNK_SIZE;
		int localY = static_cast<int>(std::floor(worldPos.y));
		int localZ = static_cast<int>(std::floor(worldPos.z)) - chunkZ * CHUNK_SIZE;
		return it->second.getVoxel(localX, localY, localZ).type != AIR;
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
		return &it->second;
	}
	return nullptr;
}

const Chunk *ChunkManager::getChunk(const glm::ivec3 &chunkPos) const
{
	std::shared_lock<std::shared_mutex> lock(chunkMutex);
	auto it = chunks.find(chunkPos);
	if (it != chunks.end())
	{
		return &it->second;
	}
	return nullptr;
}

const std::unordered_map<glm::ivec3, Chunk, IVec3Hash> &ChunkManager::getAllChunks() const
{
	return chunks; // Caller must handle synchronization if iterating and modifying elsewhere
}

void ChunkManager::unloadOutOfRangeChunks(const Camera &camera, const RenderSettings &settings)
{
	std::lock_guard<std::shared_mutex> lock(chunkMutex);
	auto it = chunks.begin();
	while (it != chunks.end())
	{
		Chunk *chunkPtr = &it->second;
		glm::vec3 chunkCenter = chunkPtr->getPosition() + glm::vec3(CHUNK_SIZE / 2.0f);
		float distToPlayer = glm::distance(
			glm::vec2(camera.getPosition().x, camera.getPosition().z),
			glm::vec2(chunkCenter.x, chunkCenter.z));
		// Unload if significantly outside maxRenderDistance (e.g., 1.5x or 2x)
		if (distToPlayer > static_cast<float>(settings.maxRenderDistance) * 1.5f)
		{
			// Do not unload chunks that are currently being processed
			if (chunksInTransit.find(chunkPtr) == chunksInTransit.end())
			{
				activeChunks.erase(chunkPtr);
				it = chunks.erase(it);
				m_cachedWaterChunks.clear(); // H: invalidate water sort cache
			}
			else
			{
				++it;
			}
		}
		else
		{
			++it;
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

		bool operator<(const ChunkLoadInfo &other) const
		{
			return distance > other.distance; // Priorité aux chunks plus proches
		}
	};

	std::priority_queue<ChunkLoadInfo> priorityQueue;

	// Calculer la priorité pour chaque chunk potentiel
	for (int x = -radius; x <= radius; x++)
	{
		for (int z = -radius; z <= radius; z++)
		{
			glm::ivec3 chunkPos = cameraChunkPos + glm::ivec3(x, 0, z);
			glm::vec3 chunkCenterWorld = glm::vec3(
				chunkPos.x * CHUNK_SIZE + CHUNK_SIZE / 2.0f,
				0,
				chunkPos.z * CHUNK_SIZE + CHUNK_SIZE / 2.0f);

			float distToPlayer = glm::distance(
				glm::vec2(camera.getPosition().x, camera.getPosition().z),
				glm::vec2(chunkCenterWorld.x, chunkCenterWorld.z));

			if (distToPlayer <= static_cast<float>(settings.maxRenderDistance))
			{
				priorityQueue.push({chunkPos, distToPlayer});
			}
		}
	}

	// Charger les chunks par ordre de priorité
	std::lock_guard<std::shared_mutex> lock(chunkMutex);
	while (!priorityQueue.empty())
	{
		const auto &info = priorityQueue.top();
		if (chunks.find(info.pos) == chunks.end())
		{
			chunkLoadQueue.push(info.pos);
		}
		priorityQueue.pop();
	}
}

void ChunkManager::processFinishedJobs()
{
	std::lock_guard<std::shared_mutex> lock(chunkMutex);

	// Check generation tasks
	auto genIt = pendingGenerationTasks.begin();
	while (genIt != pendingGenerationTasks.end())
	{
		if (genIt->first.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			genIt->first.get(); // Propagate exceptions if any
			chunksInTransit.erase(genIt->second);
			genIt = pendingGenerationTasks.erase(genIt);
		}
		else
		{
			++genIt;
		}
	}

	// Check meshing tasks
	auto meshIt = pendingMeshingTasks.begin();
	while (meshIt != pendingMeshingTasks.end())
	{
		if (meshIt->first.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			meshIt->first.get();
			Chunk *finishedChunk = meshIt->second;
			chunksInTransit.erase(finishedChunk);
			// H: A newly meshed chunk may have water geometry — invalidate the sorted
			// cache so it appears in the next water transparency pass without waiting
			// for the camera to move.
			if (finishedChunk->hasWaterMesh())
				m_cachedWaterChunks.clear();
			meshIt = pendingMeshingTasks.erase(meshIt);
		}
		else
		{
			++meshIt;
		}
	}
}
