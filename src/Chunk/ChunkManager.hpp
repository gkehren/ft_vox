#pragma once

#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <future>
#include <functional>
#include <limits>
#include <cstddef>

#include <glm/glm.hpp>
#include <Chunk/Chunk.hpp>
#include <Chunk/ChunkPool.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Engine/EngineDefs.hpp>
#include <Engine/ThreadPool.hpp>
#include <utils.hpp>

class Camera;
class ImmediateCommands;

/// Streams chunks around the player: load → async terrain → async mesh → main-thread GPU upload.
class ChunkManager
{
public:
	ChunkManager(TerrainGenerator *terrainGenerator, ThreadPool *threadPool, ChunkPool *chunkPool,
				 RenderTiming &renderTiming);
	~ChunkManager();

	/// Enqueue loads / mark far chunks for unload based on camera position.
	void updateStreaming(const Camera &camera, const RenderSettings &settings);

	/// Pull from the load queue (pool acquire). budget = max chunks this frame.
	void processChunkLoading(int budget);

	/// Dispatch async terrain generation for nearby UNLOADED chunks.
	void generatePendingVoxels(const Camera &camera, const RenderSettings &settings, int budget);

	/// Dispatch async meshing for GENERATED chunks (neighbor shell filled on main thread).
	void meshPendingChunks(const Camera &camera, const RenderSettings &settings, int budget);

	/// Main-thread GPU upload for meshed chunks. waitGpu must idle the device before buffer destroy.
	void uploadPendingMeshes(VmaAllocator allocator, ImmediateCommands &imm, int budget,
							 const std::function<void()> &waitGpu);

	/// After frames-in-flight delay, release unloaded chunks back to the pool.
	void processDeferredReleases(const std::function<void()> &waitGpu);

	/// Join finished worker jobs and clear in-transit flags.
	void processFinishedJobs();

	/// Distance + frustum visibility flags (affects optional draw filtering).
	void updateVisibility(const Camera &camera, int windowWidth, int windowHeight,
						  const RenderSettings &settings);

	/// Draw list: GPU-ready chunks (not pending upload).
	void collectDrawList(std::vector<Chunk *> &out) const;

	bool deleteVoxel(const glm::vec3 &worldPos);
	bool placeVoxel(const glm::vec3 &worldPos, TextureType type);
	bool isVoxelActive(const glm::vec3 &worldPos) const;
	Chunk *getChunkAtWorldPos(const glm::vec3 &worldPos);
	Chunk *getChunk(const glm::ivec3 &chunkPos);
	const Chunk *getChunk(const glm::ivec3 &chunkPos) const;

	size_t chunkCount() const;
	size_t pendingLoadCount() const;
	size_t pendingGenJobs() const;
	size_t pendingMeshJobs() const;
	ChunkPool *getChunkPool() const { return m_chunkPool; }

	/// Synchronous bootstrap near spawn so the first frame has terrain.
	void generateInitialArea(const glm::vec3 &center, int radiusChunks, VmaAllocator allocator,
							 ImmediateCommands &imm, const std::function<void()> &waitGpu);

private:
	void queueUnloadOutOfRange(const Camera &camera, const RenderSettings &settings);
	void loadChunksAroundPlayer(const glm::ivec3 &cameraChunkPos, const Camera &camera,
								const RenderSettings &settings);
	void ensureShellPopulated(Chunk *chunk, const glm::ivec3 &chunkIdx);
	TaskPriority calculateTaskPriority(float distanceSq, float lodThresholdSq) const;
	static glm::ivec3 worldToChunkCoord(const glm::vec3 &worldPos);

	std::unordered_map<glm::ivec3, Chunk *, IVec3Hash> m_chunks;
	std::vector<Chunk *> m_activeChunks;
	std::queue<glm::ivec3> m_loadQueue;
	std::unordered_set<glm::ivec3, IVec3Hash> m_enqueuedLoads;

	std::vector<std::pair<std::future<void>, Chunk *>> m_pendingGenerationTasks;
	std::vector<std::pair<std::future<void>, Chunk *>> m_pendingMeshingTasks;

	mutable std::shared_mutex m_mutex;

	TerrainGenerator *m_terrainGenerator{nullptr};
	ThreadPool *m_threadPool{nullptr};
	ChunkPool *m_chunkPool{nullptr};
	RenderTiming &m_renderTiming;

	std::vector<Chunk *> m_deferredRelease;
	int m_deferredReleaseAge{0};
};
