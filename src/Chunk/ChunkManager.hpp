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
#include <Chunk/ChunkLightHalo.hpp>
#include <Chunk/ChunkPool.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Chunk/StreamHelpers.hpp>
#include <Engine/EngineDefs.hpp>
#include <Engine/ThreadPool.hpp>
#include <Engine/WorkloadTelemetry.hpp>
#include <Chunk/LightSample.hpp>
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

/// A finished async entity light-cache job (issue #172): the packed light
/// storage (null when the pool could not provide a block) plus the identity
/// captured at dispatch, so the main thread can reject stale results at
/// publish time with the same lifetime safety as mesh results.
struct CompletedLightJob
{
	Chunk *chunk{nullptr};
	ChunkLightStorage *storage{nullptr};
	ChunkLightPool *pool{nullptr}; // pool the storage was acquired from
	uint64_t generation{0};
	uint64_t revision{0};
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

/// What the last streaming maintenance tick did (issue #108 review): lets
/// tests and telemetry assert the zero-work / incremental / rebuild contract.
enum class StreamingUpdateKind
{
	None,			// steady state: camera inside the same chunk, anchor and heading
	Incremental,	// chunk-cross or movement-anchor change: O(r) footprint diff
	HeadingRebuild, // heading threshold exceeded: full footprint recompute + diff
	FullRebuild		// startup / teleport / settings change: queue rebuilt from scratch
};

/// Hard cadence floor between out-of-range unload scans (issue #108 review):
/// unload checks run on chunk-cross/teleport or settings changes, and at
/// least every this many frames otherwise.
constexpr uint32_t kUnloadCheckIntervalFrames = 60;

/// Streams chunks around the player: load → async terrain → async mesh → main-thread GPU upload.
class ChunkManager
{
public:
	// Entity local light cache retention around camera (issue #128), with
	// acquire/release hysteresis (issue #172). Dynamic entities spawn and
	// wander within ~128m (kMobDespawnDist), so caches are acquired at that
	// radius; the larger release radius keeps camera jitter around the
	// acquire edge from oscillating allocate/release cycles. Cache
	// acquisition is decoupled from meshing (issue #172): it never
	// invalidates a valid render mesh.
	static constexpr float kEntityLightCacheAcquireRadius = 128.0f;
	static constexpr float kEntityLightCacheAcquireRadiusSq =
		kEntityLightCacheAcquireRadius * kEntityLightCacheAcquireRadius;
	static constexpr float kEntityLightCacheReleaseRadius = 144.0f;
	static constexpr float kEntityLightCacheReleaseRadiusSq =
		kEntityLightCacheReleaseRadius * kEntityLightCacheReleaseRadius;

	ChunkManager(TerrainGenerator *terrainGenerator, ThreadPool *threadPool, ChunkPool *chunkPool);
	~ChunkManager();

	/// Enqueue loads / mark far chunks for unload based on camera position.
	/// Returns what the load-side maintenance did this tick (the out-of-range
	/// unload scan is a separate concern driven by the same triggers).
	StreamingUpdateKind updateStreaming(const Camera &camera, const RenderSettings &settings);

	/// Pull from the load queue (pool acquire). budget = max chunks this frame.
	void processChunkLoading(int budget);

	/// Dispatch async terrain generation for nearby UNLOADED chunks.
	void generatePendingVoxels(const Camera &camera, const RenderSettings &settings, int budget);

	/// Dispatch async meshing for GENERATED chunks (neighbor shell filled on main thread).
	void meshPendingChunks(const Camera &camera, const RenderSettings &settings, int budget);

	/// Dispatch async entity light-cache-only builds for MESHED chunks inside
	/// the acquire radius that lack storage (issue #172). The worker computes
	/// the chunk-wide light field and publishes only a ChunkLightStorage -
	/// chunk state, dirty sections, mesh payloads, indirect draw caches and
	/// GPU upload flags are never touched. budget = max dispatches this call.
	void updateEntityLightCaches(const Camera &camera, const RenderSettings &settings, int budget);

	/// Record mesh uploads into cmd (staging ring). No device idle. Distance-prioritized.
	/// Returns number of chunks uploaded this call.
	int uploadPendingMeshes(VmaAllocator allocator, StagingRing &staging, VkCommandBuffer cmd,
							GpuResourceRetire &retire, MeshArenas &arenas, const Camera &camera, int budget);

	/// After frames-in-flight delay, retire arena ranges and return chunks to the pool.
	void processDeferredReleases();

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

	/// Sample published local lighting at an integer voxel coordinate.
	lighting::LocalVoxelLight sampleVoxelLight(const glm::ivec3 &blockPos) const;
	/// Sample smoothed local lighting via trilinear interpolation of the 8 surrounding voxel centers.
	lighting::LocalVoxelLight sampleSmoothedLight(const glm::vec3 &worldPos) const;

	lighting::LocalVoxelLight sampleVoxelLightUnlocked(const glm::ivec3 &blockPos) const;
	lighting::LocalVoxelLight sampleSmoothedLightUnlocked(const glm::vec3 &worldPos) const;

	/// Snapshot of the streaming maintenance counters (issue #108). Main
	/// thread writes and reads them, so no lock is taken.
	StreamingMaintenanceStats streamingMaintenanceStats() const { return m_streamStats; }

	size_t deferredReleaseCount() const { return m_deferredRelease.size(); }
	size_t chunkCount() const;
	size_t pendingLoadCount() const;
	size_t pendingGenJobs() const;
	size_t pendingMeshJobs() const;
	size_t pendingLightJobs() const;
	/// Cumulative dispatch counters (issue #172): let tests and benchmarks
	/// prove that cache-radius crossings dispatch light-only jobs instead of
	/// render mesh rebuilds.
	uint64_t meshJobsDispatched() const { return m_meshJobsDispatched.load(std::memory_order_relaxed); }
	uint64_t lightJobsDispatched() const { return m_lightJobsDispatched.load(std::memory_order_relaxed); }
	ChunkPool *getChunkPool() const { return m_chunkPool; }
	/// Test/inspection access to the active chunk set (unordered).
	const std::vector<Chunk *> &getActiveChunks() const { return m_activeChunks; }

	/// Synchronous bootstrap near spawn so the first frame has terrain.
	void generateInitialArea(const glm::vec3 &center, int radiusChunks, VmaAllocator allocator,
							 ImmediateCommands &imm, MeshArenas &arenas);

	/// Shared synchronous generation contract (issue #112): prepares voxel
	/// storage on the calling thread immediately before generateTerrain().
	/// Returns false — and releases the chunk back to the pool — when the
	/// storage allocation fails, so no slot is leaked.
	bool prepareAndGenerateChunk(Chunk *chunk, TerrainGenerator &generator);

private:
	void queueUnloadOutOfRange(const Camera &camera, const RenderSettings &settings);
	StreamingUpdateKind loadChunksAroundPlayer(const glm::ivec3 &cameraChunkPos, const Camera &camera,
											   const RenderSettings &settings);
	StreamingUpdateKind rebuildStreamingQueueFull(const glm::ivec3 &cameraChunkPos, const Camera &camera,
												  const RenderSettings &settings);
	StreamingUpdateKind reconcileStreamingIncremental(const glm::ivec3 &cameraChunkPos, const Camera &camera,
													  const RenderSettings &settings);
	StreamingUpdateKind reconcileStreamingHeading(const glm::ivec3 &cameraChunkPos, const Camera &camera,
												  const RenderSettings &settings);
	/// Shared tail of the incremental streaming updates: apply a footprint
	/// diff to the load queue (enqueue entering coords, purge exiting ones,
	/// compact consumed entries, refresh biased distances, re-sort).
	void applyFootprintDiffToQueue(const FootprintDiff &diff, const glm::vec3 &camPos,
								   const glm::vec2 &camForwardXZ, float frontBias);
	void ensureShellPopulated(Chunk *chunk, const glm::ivec3 &chunkIdx);
	/// Cross-chunk block-light context (issue #141 review fix): borrow (or
	/// reuse) a pooled halo for `chunk` and snapshot the 15-voxel neighbor
	/// ring into it. Caller holds m_mutex exclusively; same dispatch-time
	/// contract as ensureShellPopulated. Gracefully degrades to no halo
	/// (in-chunk-only light) when the pool allocation fails. The telemetry
	/// family routes the haloFill stage timing to the calling pipeline
	/// (mesh vs light-cache, issue #173 review).
	void ensureLightHalo(Chunk *chunk, const glm::ivec3 &chunkIdx,
						 telemetry::Family telemetryFamily = telemetry::MeshFamily);
	/// Light-aware cross-chunk invalidation (issue #141 review fix): when a
	/// light-relevant edit (emission or sky-transmission flip - the same
	/// predicate as markEditDirtySections) lands within the 15-voxel halo
	/// radius of a border, the neighbor(s) on that side must remesh too or
	/// their propagated light goes stale. Caller holds m_mutex exclusively.
	void markNeighborLightDirty(const glm::ivec3 &chunkPos, int x, int y, int z,
								TextureType previousType, TextureType type);
	/// A chunk just finished generating: scan its halo-radius border bands
	/// for emissive sources and dirty the side neighbors' affected sections
	/// so their meshes pick up the newly available light. Cheap early-out
	/// when the bands hold no sources - the common case. Caller holds
	/// m_mutex exclusively.
	void dirtyNeighborsForArrivedLight(Chunk *chunk, const glm::ivec3 &chunkIdx);

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

	/// Maintain chunk local light cache desire based on distance to camera
	/// (issue #128), with acquire/release hysteresis (issue #172): a cache is
	/// acquired when the chunk enters the acquire radius and retained until
	/// it leaves the (larger) release radius.
	void updateEntityLightCacheIntent(Chunk &chunk, float distSq);

	// Testing hook (issue #114 review): exposes the deferred-edit queue
	// size without making it public API.
	friend struct ChunkManagerProbe;
	friend struct ChunkManagerStreamProbe;
	friend class ChunkCollisionView;

	struct StreamState
	{
		glm::ivec3 lastCamChunk{std::numeric_limits<int>::max(), 0, std::numeric_limits<int>::max()};
		glm::ivec2 lastMovementAnchor{std::numeric_limits<int>::max(), std::numeric_limits<int>::max()};
		glm::vec2 lastCamForwardXZ{0.f, 1.f};
		int lastMaxRenderDistance{-1};
		float lastStreamFrontBias{-1.f};
		bool initialized{false};
	};

	std::unordered_map<glm::ivec3, Chunk *, IVec3Hash> m_chunks;
	std::vector<Chunk *> m_activeChunks;
	/// Distance-prioritized load queue (not FIFO — re-sorted / pruned each stream tick).
	std::vector<LoadCandidate> m_loadQueue;
	std::unordered_set<glm::ivec3, IVec3Hash> m_enqueuedLoads;
	StreamState m_streamState;
	ChunkDesiredFootprint m_desiredFootprint;
	size_t m_loadQueueHead{0};
	bool m_queueNeedsSort{false};
	uint32_t m_streamFramesSinceUnloadCheck{0};
	StreamingMaintenanceStats m_streamStats;

	mutable std::mutex m_completedJobsMutex;
	std::vector<Chunk *> m_completedGenerationChunks;
	std::vector<CompletedMeshJob> m_completedMeshJobs;
	std::vector<CompletedLightJob> m_completedLightJobs;
	std::atomic<size_t> m_pendingGenJobsCount{0};
	std::atomic<size_t> m_pendingMeshJobsCount{0};
	std::atomic<size_t> m_pendingLightJobsCount{0};
	std::atomic<uint64_t> m_meshJobsDispatched{0};
	std::atomic<uint64_t> m_lightJobsDispatched{0};

	/// Edits deferred while their target chunk was in transit (main-thread
	/// only; user interactions are rare so a small linear queue is plenty).
	std::vector<PendingVoxelEdit> m_pendingEdits;
	/// Logical edit group counter, main-thread only.
	uint64_t m_nextEditId{1};

	mutable std::shared_mutex m_mutex;

	TerrainGenerator *m_terrainGenerator{nullptr};
	ThreadPool *m_threadPool{nullptr};
	ChunkPool *m_chunkPool{nullptr};
	/// Pooled halo blocks lent to in-flight mesh jobs (issue #141 review
	/// fix); one block is ~530 KiB, so a small pool serves the dispatch
	/// budget + worker count and grows on demand.
	LightHaloPool m_lightHaloPool{4};

	std::vector<Chunk *> m_deferredRelease;
	int m_deferredReleaseAge{0};
};
