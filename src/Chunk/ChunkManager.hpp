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
struct MeshBuildResult;

/// A finished async mesh job: the built payload (may be null when the
/// result block could not be acquired) plus the chunk it was built for, so
/// the main thread can validate identity at publish time (issue #104).
struct CompletedMeshJob
{
	Chunk *chunk{nullptr};
	MeshBuildResult *result{nullptr};
};

/// A voxel edit deferred because its target chunk was in transit (its
/// storage/borders were being read by a worker). Applied on the main thread
/// after the in-flight job completes (issue #114 review). An edit is a
/// logical operation: one target entry plus one mirror entry per adjacent
/// neighbor, grouped by editId and coalesced per coordinate.
struct PendingVoxelEdit
{
	Chunk *chunk{nullptr};
	// Incarnation stamp at queue time: a queued edit whose chunk was
	// recycled (generation moved on) is dropped at apply time instead of
	// writing into the new incarnation (issue #114 review item 26).
	uint64_t generation{0};
	// Chunk map index at queue time: needed by ensureShellPopulated for
	// mirror writes, and robust against recycling.
	glm::ivec3 chunkPos{0};
	int x{0};
	int y{0};
	int z{0};
	TextureType type{AIR};
	// True for neighbor mirror writes: the shell is rebuilt before the
	// write (same order as the direct path).
	bool borderNeighbor{false};
	// Logical edit group: one user interaction = one id shared by the
	// target entry and its mirrors.
	uint64_t editId{0};
};

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

	size_t deferredReleaseCount() const { return m_deferredRelease.size(); }
	size_t chunkCount() const;
	size_t pendingLoadCount() const;
	size_t pendingGenJobs() const;
	size_t pendingMeshJobs() const;
	ChunkPool *getChunkPool() const { return m_chunkPool; }

	/// Synchronous bootstrap near spawn so the first frame has terrain.
	void generateInitialArea(const glm::vec3 &center, int radiusChunks, VmaAllocator allocator,
							 ImmediateCommands &imm);

	/// Shared synchronous generation contract (issue #112): prepares voxel
	/// storage on the calling thread immediately before generateTerrain().
	/// Returns false — and releases the chunk back to the pool — when the
	/// storage allocation fails, so no slot is leaked.
	bool prepareAndGenerateChunk(Chunk *chunk, TerrainGenerator &generator);

private:
	void queueUnloadOutOfRange(const Camera &camera, const RenderSettings &settings);
	void loadChunksAroundPlayer(const glm::ivec3 &cameraChunkPos, const Camera &camera,
								const RenderSettings &settings);
	void ensureShellPopulated(Chunk *chunk, const glm::ivec3 &chunkIdx);

	// --- Deferred edit subsystem (issue #114 review). Main-thread only:
	// protected by the engine update/event sequencing, NOT by
	// m_completedJobsMutex. placeVoxel/deleteVoxel, processFinishedJobs and
	// chunk unload all run on the main thread.
	//
	/// Unified entry point (caller holds m_mutex exclusively): resolves the
	/// effective voxel state (storage + pending edits), refuses logical
	/// no-ops, then schedules the target edit and every neighbor mirror as
	/// one logical group. Each piece is applied immediately when its chunk
	/// is free, or queued for the apply phase when it is in transit — so an
	/// in-transit target defers its mirrors too (transaction semantics).
	bool scheduleLogicalEdit(const glm::ivec3 &chunkPos, int x, int y, int z,
							 TextureType type);
	/// Occupancy lifecycle gate (issue #115 blocker): a voxel edit must be
	/// deferred whenever the chunk is missing, still UNLOADED (backing is
	/// stale pool bytes until terrain generation initializes it), or in
	/// transit for any job. UNLOADED defers regardless of transit state.
	bool mustDeferVoxelEdit(const Chunk *chunk) const;
	/// Storage type overlaid with the pending (non-mirror) edits for that
	/// voxel: the state the next apply phase would produce. Read-only, so
	/// safe to evaluate while the chunk is in transit.
	TextureType effectiveVoxelType(const Chunk *chunk, int x, int y, int z) const;
	/// Apply immediately or queue, depending on mustDeferVoxelEdit (and the
	/// group-level forceDefer): never writes into unreadable backing.
	void queueOrApplyEdit(Chunk *chunk, const glm::ivec3 &chunkPos, int x, int y,
						  int z, TextureType type, bool borderNeighbor,
						  bool forceDefer, uint64_t editId);
	/// Schedule the mirror border writes for an edit at local (x, z); a
	/// corner voxel schedules both adjacent neighbors (no diagonal - the
	/// border architecture keeps corner columns only at generation).
	void enqueueOrApplyMirrorEdits(const glm::ivec3 &chunkPos, int x, int y, int z,
								   TextureType type, bool forceDefer, uint64_t editId);
	/// Queue with coalescing: an entry for the same chunk/coordinate/kind
	/// is overwritten (last write wins) instead of accumulating.
	void queuePendingEdit(PendingVoxelEdit edit);
	/// Apply queued edits whose chunk is editable now (backing readable and
	/// not in transit; main thread), targets before mirrors; entries whose
	/// chunk generation has moved on are dropped.
	void applyPendingEdits();
	bool hasPendingEditsFor(const Chunk *chunk) const;
	/// Drop every pending edit targeting this chunk (unload/recycle path).
	void erasePendingEditsFor(const Chunk *chunk);

	TaskPriority calculateTaskPriority(float distanceSq, float lodThresholdSq) const;
	static glm::ivec3 worldToChunkCoord(const glm::vec3 &worldPos);

	// Testing hook (issue #114 review): exposes the deferred-edit queue
	// size without making it public API.
	friend struct ChunkManagerProbe;
	friend class ChunkCollisionView;

	std::unordered_map<glm::ivec3, Chunk *, IVec3Hash> m_chunks;
	std::vector<Chunk *> m_activeChunks;
	/// Distance-prioritized load queue (not FIFO — re-sorted / pruned each stream tick).
	std::vector<LoadCandidate> m_loadQueue;
	std::unordered_set<glm::ivec3, IVec3Hash> m_enqueuedLoads;

	mutable std::mutex m_completedJobsMutex;
	std::vector<Chunk *> m_completedGenerationChunks;
	std::vector<CompletedMeshJob> m_completedMeshJobs;
	std::atomic<size_t> m_pendingGenJobsCount{0};
	std::atomic<size_t> m_pendingMeshJobsCount{0};

	/// Edits deferred while their target chunk was in transit (main-thread
	/// only; user interactions are rare so a small linear queue is plenty).
	std::vector<PendingVoxelEdit> m_pendingEdits;
	/// Logical edit group counter, main-thread only.
	uint64_t m_nextEditId{1};

	mutable std::shared_mutex m_mutex;

	TerrainGenerator *m_terrainGenerator{nullptr};
	ThreadPool *m_threadPool{nullptr};
	ChunkPool *m_chunkPool{nullptr};

	std::vector<Chunk *> m_deferredRelease;
	int m_deferredReleaseAge{0};
};
