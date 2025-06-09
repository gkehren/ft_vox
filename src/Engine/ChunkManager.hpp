#pragma once

#include <unordered_map>
#include <queue>
#include <mutex>
#include <vector>
#include <future>
#include <memory>

#include <glm/glm.hpp>
#include <Chunk/Chunk.hpp>
#include <utils.hpp>
#include <Engine/EngineDefs.hpp>
#include <PerlinNoise/PerlinNoise.hpp>

// Forward declarations
class Camera;
class Shader;
class TextureAtlas;
class Renderer;
class ThreadPool;

class ChunkManager
{
public:
	ChunkManager(siv::PerlinNoise *noise, ThreadPool *threadPool, RenderTiming &renderTiming);
	~ChunkManager();

	void updatePlayerPosition(const glm::ivec2 &newPlayerChunkPos, const Camera &camera, const RenderSettings &settings);
	void processChunkLoading(const RenderSettings &settings);
	void performFrustumCulling(const Camera &camera, int windowWidth, int windowHeight, const RenderSettings &settings);

	void generatePendingVoxels(const RenderSettings &settings);
	void meshPendingChunks(const Camera &camera, const RenderSettings &settings);

	void drawVisibleChunks(Shader &shader, const Camera &camera, const GLuint &textureAtlas, const ShaderParameters &shaderParams, Renderer *renderer, RenderSettings &renderSettings);

	bool deleteVoxel(const glm::vec3 &worldPos);
	bool placeVoxel(const glm::vec3 &worldPos, TextureType type);
	bool isVoxelActive(const glm::vec3 &worldPos) const;
	Chunk *getChunk(const glm::ivec3 &chunkPos);
	const Chunk *getChunk(const glm::ivec3 &chunkPos) const;

	const std::unordered_map<glm::ivec3, Chunk, ivec3_hash> &getAllChunks() const;

private:
	void addChunkToGenerationQueue(const glm::ivec3 &chunkPos);
	void unloadOutOfRangeChunks(const Camera &camera, const RenderSettings &settings);
	void loadChunksAroundPlayer(const glm::ivec3 &cameraChunkPos, const Camera &camera, const RenderSettings &settings);

	std::unordered_map<glm::ivec3, Chunk, ivec3_hash> chunks;
	std::queue<glm::ivec3> chunkLoadQueue; // For chunks that need to be created

	mutable std::mutex chunkMutex;

	siv::PerlinNoise *p_noise;	  // External, owned by Engine
	ThreadPool *p_threadPool;	  // External, owned by Engine
	RenderTiming &m_renderTiming; // Reference to Engine's RenderTiming
};
