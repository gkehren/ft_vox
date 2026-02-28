#pragma once

#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <future>
#include <memory>

#include <glm/glm.hpp>
#include <Chunk/Chunk.hpp>
#include <utils.hpp>
#include <Engine/EngineDefs.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <limits>

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
	void processChunkLoading(const RenderSettings &settings, int budget);
	void processFinishedJobs();
	void performFrustumCulling(const Camera &camera, int windowWidth, int windowHeight, const RenderSettings &settings);

	void generatePendingVoxels(const Camera &camera, const RenderSettings &settings, unsigned int seed, int budget);
	void meshPendingChunks(const Camera &camera, const RenderSettings &settings, int budget);

	void drawVisibleChunks(Shader &shader, const Camera &camera, const GLuint &textureAtlas, const ShaderParameters &shaderParams, Renderer *renderer, RenderSettings &renderSettings, int windowWidth, int windowHeight);
	void drawShadows(const Shader &shader) const;
	void uploadPendingMeshes(int budget);

	bool deleteVoxel(const glm::vec3 &worldPos);
	bool placeVoxel(const glm::vec3 &worldPos, TextureType type);
	bool isVoxelActive(const glm::vec3 &worldPos) const;
	Chunk *getChunk(const glm::ivec3 &chunkPos);
	const Chunk *getChunk(const glm::ivec3 &chunkPos) const;

	const std::unordered_map<glm::ivec3, Chunk, IVec3Hash> &getAllChunks() const;

private:
	void unloadOutOfRangeChunks(const Camera &camera, const RenderSettings &settings);
	void loadChunksAroundPlayer(const glm::ivec3 &cameraChunkPos, const Camera &camera, const RenderSettings &settings);
	void ensureShellPopulated(Chunk *chunk, const glm::ivec3 &chunkIdx);

	std::unordered_map<glm::ivec3, Chunk, IVec3Hash> chunks;
	std::unordered_set<Chunk *> activeChunks;
	std::queue<glm::ivec3> chunkLoadQueue;

	std::vector<std::pair<std::future<void>, Chunk *>> pendingGenerationTasks;
	std::vector<std::pair<std::future<void>, Chunk *>> pendingMeshingTasks;
	std::unordered_set<Chunk *> chunksInTransit;

	mutable std::shared_mutex chunkMutex;

	// H: water-sort cache — rebuilt only when camera moves > CHUNK_SIZE/2
	mutable glm::vec3 m_lastWaterSortCamPos{std::numeric_limits<float>::max()};
	mutable std::vector<Chunk *> m_cachedWaterChunks;

	TerrainGenerator *m_terrainGenerator;
	ThreadPool *p_threadPool;
	RenderTiming &m_renderTiming;
};
