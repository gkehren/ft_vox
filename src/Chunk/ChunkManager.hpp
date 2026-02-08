#pragma once

#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <mutex>
#include <vector>
#include <future>
#include <memory>

#include <glm/glm.hpp>
#include <Chunk/Chunk.hpp>
#include <utils.hpp>
#include <Engine/EngineDefs.hpp>
#include <Chunk/TerrainGenerator.hpp>

// Forward declarations
class Camera;
class Shader;
class TextureAtlas;
class Renderer;
class ThreadPool;

class ChunkManager
{
public:
	ChunkManager(TerrainGenerator *terrainGenerator, ThreadPool *threadPool, RenderTiming &renderTiming);
	~ChunkManager();

	void updatePlayerPosition(const glm::ivec2 &newPlayerChunkPos, const Camera &camera, const RenderSettings &settings);
	void processChunkLoading(const RenderSettings &settings);
	void processFinishedJobs();
	void performFrustumCulling(const Camera &camera, int windowWidth, int windowHeight, const RenderSettings &settings);

	void generatePendingVoxels(const RenderSettings &settings, unsigned int seed);
	void meshPendingChunks(const Camera &camera, const RenderSettings &settings);

	void drawVisibleChunks(Shader &shader, const Camera &camera, const GLuint &textureAtlas, const ShaderParameters &shaderParams, Renderer *renderer, RenderSettings &renderSettings);

	bool deleteVoxel(const glm::vec3 &worldPos);
	bool placeVoxel(const glm::vec3 &worldPos, TextureType type);
	bool isVoxelActive(const glm::vec3 &worldPos) const;
	Chunk *getChunk(const glm::ivec3 &chunkPos);
	const Chunk *getChunk(const glm::ivec3 &chunkPos) const;

	const std::unordered_map<glm::ivec3, Chunk, IVec3Hash> &getAllChunks() const;

private:
	void unloadOutOfRangeChunks(const Camera &camera, const RenderSettings &settings);
	void loadChunksAroundPlayer(const glm::ivec3 &cameraChunkPos, const Camera &camera, const RenderSettings &settings);

	std::unordered_map<glm::ivec3, Chunk, IVec3Hash> chunks;
	std::queue<glm::ivec3> chunkLoadQueue;

	std::vector<std::pair<std::future<void>, Chunk *>> pendingGenerationTasks;
	std::vector<std::pair<std::future<void>, Chunk *>> pendingMeshingTasks;
	std::unordered_set<Chunk *> chunksInTransit;

	mutable std::mutex chunkMutex;

	TerrainGenerator *m_terrainGenerator;
	ThreadPool *p_threadPool;
	RenderTiming &m_renderTiming;
};
