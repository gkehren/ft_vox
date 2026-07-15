#pragma once

#include <unordered_map>
#include <unordered_set>
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
#include <Chunk/StreamHelpers.hpp>
#include <Engine/EngineDefs.hpp>
#include <Engine/ThreadPool.hpp>
#include <utils.hpp>

class Camera;
class ImmediateCommands;
class StagingRing;
class GpuResourceRetire;

/// Streams chunks around the player: load → async terrain → async mesh → main-thread GPU upload.
class ChunkManager
{
public:
	ChunkManager(TerrainGenerator *terrainGenerator, ThreadPool *threadPool, ChunkPool *chunkPool);
	~ChunkManager();

	/// Enqueue loads / mark far chunks for unload based on camera position.
	void updateStreaming(const Camera &camera, const RenderSettings &settings);

	/// Pull from the load queue (pool acquire). budget = max chunks this frame.
	void processChunkLoading(int budget);

	/// Dispatch async terrain generation for nearby UNLOADED chunks.
	void generatePendingVoxels(const Camera &camera, const RenderSettings &settings, int budget);

	/// Dispatch async meshing for GENERATED chunks (neighbor shell filled on main thread).
	void meshPendingChunks(const Camera &camera, const RenderSettings &settings, int budget);

	/// Record mesh uploads into cmd (staging ring). No device idle. Distance-prioritized.
	/// Returns number of chunks uploaded this call.
	int uploadPendingMeshes(VmaAllocator allocator, StagingRing &staging, VkCommandBuffer cmd,
							GpuResourceRetire &retire, const Camera &camera, int budget);

	/// After frames-in-flight delay, retire GPU buffers and return chunks to the pool.
	void processDeferredReleases(GpuResourceRetire &retire);

	/// Join finished worker jobs and clear in-transit flags.
	void processFinishedJobs();

	/// Pure frustum visibility (used by collectDrawList).
	void updateVisibility(const Camera &camera, int windowWidth, int windowHeight,
						  const RenderSettings &settings);

	/// Draw list: GPU-ready + frustum-visible chunks.
	void collectDrawList(std::vector<Chunk *> &out) const;

	/// Shadow casters: GPU-ready chunks within shadowRadius (blocks, XZ).
	void collectShadowList(std::vector<Chunk *> &out, const Camera &camera, float shadowRadius) const;

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
							 ImmediateCommands &imm);

private:
	void queueUnloadOutOfRange(const Camera &camera, const RenderSettings &settings);
	void loadChunksAroundPlayer(const glm::ivec3 &cameraChunkPos, const Camera &camera,
								const RenderSettings &settings);
	void ensureShellPopulated(Chunk *chunk, const glm::ivec3 &chunkIdx);
	TaskPriority calculateTaskPriority(float distanceSq, float lodThresholdSq) const;
	static glm::ivec3 worldToChunkCoord(const glm::vec3 &worldPos);

	std::unordered_map<glm::ivec3, Chunk *, IVec3Hash> m_chunks;
	std::vector<Chunk *> m_activeChunks;
	/// Distance-prioritized load queue (not FIFO — re-sorted / pruned each stream tick).
	std::vector<LoadCandidate> m_loadQueue;
	std::unordered_set<glm::ivec3, IVec3Hash> m_enqueuedLoads;

	std::vector<std::pair<std::future<void>, Chunk *>> m_pendingGenerationTasks;
	std::vector<std::pair<std::future<void>, Chunk *>> m_pendingMeshingTasks;

	mutable std::shared_mutex m_mutex;

	TerrainGenerator *m_terrainGenerator{nullptr};
	ThreadPool *m_threadPool{nullptr};
	ChunkPool *m_chunkPool{nullptr};

	std::vector<Chunk *> m_deferredRelease;
	int m_deferredReleaseAge{0};
};
