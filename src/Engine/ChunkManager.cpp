#include "ChunkManager.hpp"
#include <Camera/Camera.hpp>
#include <Shader/Shader.hpp>
#include <Renderer/Renderer.hpp> // For Renderer and TextureAtlas
#include <Engine/ThreadPool.hpp>
#include <PerlinNoise/PerlinNoise.hpp>
#include <glm/gtc/matrix_access.hpp> // For glm::row

#include <iostream> // For debug messages, consider a logger
#include <chrono>	// For timing
#include <queue>

ChunkManager::ChunkManager(siv::PerlinNoise *noise, ThreadPool *threadPool, RenderTiming &renderTiming)
	: p_noise(noise), p_threadPool(threadPool), m_renderTiming(renderTiming)
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

void ChunkManager::processChunkLoading(const RenderSettings &settings)
{
	std::lock_guard<std::mutex> lock(chunkMutex);
	int chunkCount = 0;
	while (!chunkLoadQueue.empty() && chunkCount < settings.chunkLoadedMax)
	{
		glm::ivec3 chunkPos = chunkLoadQueue.front();
		chunkLoadQueue.pop();

		if (chunks.find(chunkPos) == chunks.end())
		{
			chunks.emplace(chunkPos, Chunk(glm::vec3(chunkPos.x * Chunk::SIZE, 0.0f, chunkPos.z * Chunk::SIZE)));
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

	std::lock_guard<std::mutex> lock(chunkMutex);
	for (auto &chunkPair : chunks)
	{
		Chunk &chunk = chunkPair.second;
		glm::vec3 center = chunk.getPosition() + glm::vec3(Chunk::SIZE / 2.0f, Chunk::HEIGHT / 2.0f, Chunk::SIZE / 2.0f);
		float radius = glm::length(glm::vec3(Chunk::SIZE / 2.0f, Chunk::HEIGHT / 2.0f, Chunk::SIZE / 2.0f)); // AABB radius
		bool isVisible = true;
		for (const auto &plane : frustumPlanes)
		{
			if (glm::dot(glm::vec4(center, 1.0f), plane) < -radius)
			{
				isVisible = false;
				break;
			}
		}
		chunk.setVisible(isVisible);
	}
	auto end = std::chrono::high_resolution_clock::now();
	m_renderTiming.frustumCulling = std::chrono::duration<float, std::milli>(end - start).count();
}

void ChunkManager::generatePendingVoxels(const RenderSettings &settings)
{
	if (!p_threadPool || !p_noise)
		return;
	auto start = std::chrono::high_resolution_clock::now();

	std::vector<std::future<void>> futures;
	{
		std::lock_guard<std::mutex> lock(chunkMutex);

		// Trier les chunks par distance au joueur pour une génération plus cohérente
		struct ChunkGenInfo {
			Chunk* chunk;
			float distance;

			bool operator<(const ChunkGenInfo& other) const {
				return distance > other.distance;
			}
		};

		std::priority_queue<ChunkGenInfo> genQueue;

		for (auto &chunkPair : chunks) {
			Chunk &chunk = chunkPair.second;
			if (chunk.isVisible() && chunk.getState() == ChunkState::UNLOADED) {
				glm::vec3 chunkCenter = chunk.getPosition() + glm::vec3(Chunk::SIZE / 2.0f);
				float distance = glm::distance(
					glm::vec2(chunkCenter.x, chunkCenter.z),
					glm::vec2(0, 0) // Position du joueur (0,0) comme référence
				);
				genQueue.push({&chunk, distance});
			}
		}

		// Générer les chunks par ordre de priorité
		while (!genQueue.empty()) {
			Chunk* chunk = genQueue.top().chunk;
			siv::PerlinNoise *noisePtr = p_noise;
			futures.push_back(p_threadPool->enqueue([chunk, noisePtr]() {
				chunk->generateVoxels(noisePtr);
			}));
			genQueue.pop();
		}
	}

	for (auto &future : futures) {
		future.get();
	}

	auto end = std::chrono::high_resolution_clock::now();
	m_renderTiming.chunkGeneration = std::chrono::duration<float, std::milli>(end - start).count();
}

void ChunkManager::meshPendingChunks(const Camera &camera, const RenderSettings &settings)
{
	if (!p_threadPool || !p_noise)
		return;
	auto start = std::chrono::high_resolution_clock::now();

	std::vector<std::future<void>> futures;
	{
		std::lock_guard<std::mutex> lock(chunkMutex);

		// Trier les chunks par distance au joueur pour un maillage plus cohérent
		struct ChunkMeshInfo {
			Chunk* chunk;
			float distance;

			bool operator<(const ChunkMeshInfo& other) const {
				return distance > other.distance;
			}
		};

		std::priority_queue<ChunkMeshInfo> meshQueue;

		for (auto &chunkPair : chunks) {
			Chunk &chunk = chunkPair.second;
			if (chunk.isVisible() && chunk.getState() == ChunkState::GENERATED) {
				glm::vec3 chunkCenter = chunk.getPosition() + glm::vec3(Chunk::SIZE / 2.0f);
				float distance = glm::distance(
					glm::vec2(chunkCenter.x, chunkCenter.z),
					glm::vec2(camera.getPosition().x, camera.getPosition().z)
				);
				meshQueue.push({&chunk, distance});
			}
		}

		// Mailler les chunks par ordre de priorité
		while (!meshQueue.empty()) {
			Chunk* chunk = meshQueue.top().chunk;
			glm::vec3 playerPos = camera.getPosition();
			siv::PerlinNoise *noisePtr = p_noise;
			futures.push_back(p_threadPool->enqueue([chunk, playerPos, noisePtr]() {
				chunk->generateMesh(playerPos, noisePtr);
			}));
			meshQueue.pop();
		}
	}

	for (auto &future : futures) {
		future.wait();
	}

	auto end = std::chrono::high_resolution_clock::now();
	m_renderTiming.meshGeneration = std::chrono::duration<float, std::milli>(end - start).count();
}

void ChunkManager::drawVisibleChunks(Shader &shader, const Camera &camera, const GLuint &textureAtlas, const ShaderParameters &shaderParams, Renderer *renderer, RenderSettings &renderSettings)
{
	auto start = std::chrono::high_resolution_clock::now();
	std::lock_guard<std::mutex> lock(chunkMutex);
	renderSettings.visibleChunksCount = 0; // Reset here as this is the drawing phase
	renderSettings.visibleVoxelsCount = 0;

	for (auto &chunkPair : chunks)
	{
		Chunk &chunk = chunkPair.second;
		if (!chunk.isVisible() || chunk.getState() < ChunkState::MESHED) // Only draw if mesh is ready
			continue;

		renderSettings.visibleVoxelsCount += chunk.draw(shader, camera, textureAtlas, shaderParams);
		renderSettings.visibleChunksCount++;

		if (renderSettings.chunkBorders && renderer)
		{
			renderer->drawBoundingBox(chunk, camera);
		}
	}
	auto end = std::chrono::high_resolution_clock::now();
	m_renderTiming.chunkRendering = std::chrono::duration<float, std::milli>(end - start).count();
}

bool ChunkManager::deleteVoxel(const glm::vec3 &worldPos)
{
	int chunkX = static_cast<int>(std::floor(worldPos.x / Chunk::SIZE));
	int chunkZ = static_cast<int>(std::floor(worldPos.z / Chunk::SIZE));
	glm::ivec3 chunkPos(chunkX, 0, chunkZ);

	std::lock_guard<std::mutex> lock(chunkMutex);
	auto it = chunks.find(chunkPos);
	if (it != chunks.end())
	{
		bool modified = it->second.deleteVoxel(worldPos);
		if (modified)
		{
			// Mark neighbors for remeshing if the deleted voxel was on a border
			// This is a simplified version. A more robust solution would check specific faces.
			const int localX = static_cast<int>(std::floor(worldPos.x)) - chunkX * Chunk::SIZE;
			const int localZ = static_cast<int>(std::floor(worldPos.z)) - chunkZ * Chunk::SIZE;

			if (localX == 0)
			{
				Chunk *neighbor = getChunk(chunkPos + glm::ivec3(-1, 0, 0));
				if (neighbor)
					neighbor->setState(ChunkState::GENERATED);
			}
			if (localX == Chunk::SIZE - 1)
			{
				Chunk *neighbor = getChunk(chunkPos + glm::ivec3(1, 0, 0));
				if (neighbor)
					neighbor->setState(ChunkState::GENERATED);
			}
			if (localZ == 0)
			{
				Chunk *neighbor = getChunk(chunkPos + glm::ivec3(0, 0, -1));
				if (neighbor)
					neighbor->setState(ChunkState::GENERATED);
			}
			if (localZ == Chunk::SIZE - 1)
			{
				Chunk *neighbor = getChunk(chunkPos + glm::ivec3(0, 0, 1));
				if (neighbor)
					neighbor->setState(ChunkState::GENERATED);
			}
		}
		return modified;
	}
	return false;
}

bool ChunkManager::placeVoxel(const glm::vec3 &worldPos, TextureType type)
{
	int chunkX = static_cast<int>(std::floor(worldPos.x / Chunk::SIZE));
	int chunkZ = static_cast<int>(std::floor(worldPos.z / Chunk::SIZE));
	glm::ivec3 chunkPos(chunkX, 0, chunkZ);

	std::lock_guard<std::mutex> lock(chunkMutex);
	auto it = chunks.find(chunkPos);
	if (it != chunks.end())
	{
		bool modified = it->second.placeVoxel(worldPos, type);
		if (modified)
		{
			// Mark neighbors for remeshing
			const int localX = static_cast<int>(std::floor(worldPos.x)) - chunkX * Chunk::SIZE;
			const int localZ = static_cast<int>(std::floor(worldPos.z)) - chunkZ * Chunk::SIZE;

			if (localX == 0)
			{
				Chunk *neighbor = getChunk(chunkPos + glm::ivec3(-1, 0, 0));
				if (neighbor)
					neighbor->setState(ChunkState::GENERATED);
			}
			if (localX == Chunk::SIZE - 1)
			{
				Chunk *neighbor = getChunk(chunkPos + glm::ivec3(1, 0, 0));
				if (neighbor)
					neighbor->setState(ChunkState::GENERATED);
			}
			if (localZ == 0)
			{
				Chunk *neighbor = getChunk(chunkPos + glm::ivec3(0, 0, -1));
				if (neighbor)
					neighbor->setState(ChunkState::GENERATED);
			}
			if (localZ == Chunk::SIZE - 1)
			{
				Chunk *neighbor = getChunk(chunkPos + glm::ivec3(0, 0, 1));
				if (neighbor)
					neighbor->setState(ChunkState::GENERATED);
			}
		}
		return modified;
	}
	return false;
}

bool ChunkManager::isVoxelActive(const glm::vec3 &worldPos) const
{
	if (worldPos.y < 0 || worldPos.y >= Chunk::HEIGHT)
		return false;

	int chunkX = static_cast<int>(std::floor(worldPos.x / Chunk::SIZE));
	int chunkZ = static_cast<int>(std::floor(worldPos.z / Chunk::SIZE));
	glm::ivec3 chunkPos(chunkX, 0, chunkZ);

	std::lock_guard<std::mutex> lock(chunkMutex); // const method, but mutex still needed for map access
	auto it = chunks.find(chunkPos);
	if (it != chunks.end() && it->second.getState() >= ChunkState::GENERATED) // Voxels exist once generated
	{
		// Convert world coordinates to local voxel coordinates
		int localX = static_cast<int>(std::floor(worldPos.x)) - chunkX * Chunk::SIZE;
		int localY = static_cast<int>(std::floor(worldPos.y));
		int localZ = static_cast<int>(std::floor(worldPos.z)) - chunkZ * Chunk::SIZE;
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
	std::lock_guard<std::mutex> lock(chunkMutex);
	auto it = chunks.find(chunkPos);
	if (it != chunks.end())
	{
		return &it->second;
	}
	return nullptr;
}

const std::unordered_map<glm::ivec3, Chunk, ivec3_hash> &ChunkManager::getAllChunks() const
{
	return chunks; // Caller must handle synchronization if iterating and modifying elsewhere
}

void ChunkManager::unloadOutOfRangeChunks(const Camera &camera, const RenderSettings &settings)
{
	std::lock_guard<std::mutex> lock(chunkMutex);
	auto it = chunks.begin();
	while (it != chunks.end()) {
		const auto& chunk = it->second;
		glm::vec3 chunkCenter = chunk.getPosition() + glm::vec3(Chunk::SIZE / 2.0f);
		float distToPlayer = glm::distance(
			glm::vec2(camera.getPosition().x, camera.getPosition().z),
			glm::vec2(chunkCenter.x, chunkCenter.z)
		);
		// Unload if significantly outside maxRenderDistance (e.g., 1.5x or 2x)
		if (distToPlayer > static_cast<float>(settings.maxRenderDistance) * 1.5f) {
			it = chunks.erase(it);
		} else {
			++it;
		}
	}
}

void ChunkManager::loadChunksAroundPlayer(const glm::ivec3 &cameraChunkPos, const Camera &camera, const RenderSettings &settings)
{
	const int radius = static_cast<int>(std::ceil(static_cast<float>(settings.maxRenderDistance) / Chunk::SIZE));

	// Utiliser une priorité basée sur la distance pour charger les chunks
	struct ChunkLoadInfo {
		glm::ivec3 pos;
		float distance;

		bool operator<(const ChunkLoadInfo& other) const {
			return distance > other.distance; // Priorité aux chunks plus proches
		}
	};

	std::priority_queue<ChunkLoadInfo> priorityQueue;

	// Calculer la priorité pour chaque chunk potentiel
	for (int x = -radius; x <= radius; x++) {
		for (int z = -radius; z <= radius; z++) {
			glm::ivec3 chunkPos = cameraChunkPos + glm::ivec3(x, 0, z);
			glm::vec3 chunkCenterWorld = glm::vec3(
				chunkPos.x * Chunk::SIZE + Chunk::SIZE / 2.0f,
				0,
				chunkPos.z * Chunk::SIZE + Chunk::SIZE / 2.0f
			);

			float distToPlayer = glm::distance(
				glm::vec2(camera.getPosition().x, camera.getPosition().z),
				glm::vec2(chunkCenterWorld.x, chunkCenterWorld.z)
			);

			if (distToPlayer <= static_cast<float>(settings.maxRenderDistance)) {
				priorityQueue.push({chunkPos, distToPlayer});
			}
		}
	}

	// Charger les chunks par ordre de priorité
	std::lock_guard<std::mutex> lock(chunkMutex);
	while (!priorityQueue.empty()) {
		const auto& info = priorityQueue.top();
		if (chunks.find(info.pos) == chunks.end()) {
			chunkLoadQueue.push(info.pos);
		}
		priorityQueue.pop();
	}
}
