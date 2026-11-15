#pragma once

// =============================================================================
// RenderWorldBridge — the renderer-owned DERIVED projection of the
// authoritative CPU voxel world (issue #207, Epic #204 Phase C).
//
// Ownership (the Phase C inversion):
//   * VoxelWorld (owned by sim::GameSimulation since Phase D #208; the
//     graphical Engine hosts the simulation) is the SINGLE owner of
//     authoritative voxel state: chunk records, residency/streaming, logical
//     edits, generation workers and WorldPersistence. The bridge only
//     REFERENCES it (world()) and never owns, moves or destroys simulation
//     state.
//   * RenderWorldBridge owns everything DERIVED: render chunk handles bound
//     to world records, mesh/light job dispatch, GPU uploads, geometric
//     commit groups, visibility, entity light-cache intent + sampling,
//     deferred GPU retirement and the chunk lifecycle debug trace.
//
// This replaces the temporary ChunkManager render adapter (issue #206 §10):
// the render pipeline moved here unchanged, and the authoritative world
// moved OUT to the host. A headless process constructs VoxelWorld alone —
// no bridge is registered and no visual-only job (mesh, GPU cache, draw
// visibility) is ever scheduled.
//
// Change channel: the bridge drains the world's bounded main-thread event
// stream (Loaded / Unloaded / GenQueued / Generated / EditApplied) right
// after each world call, so the render handle map never observes a recycled
// record.
//
// Worker lease contract (the "safe read-only chunk snapshots/leases" of the
// epic): dispatch captures the record's sim::ChunkIdentity (generation +
// revision, the single source on WorldChunk) and takes the exclusive
// in-transit lease on the record. While the lease is held the world defers
// edits and unloads targeting the record, so the worker reads a stable
// read-only snapshot of the voxel/border content; every async result is
// re-validated against the identity at publish time and late results from
// unloaded/reused chunks are discarded back to their pools.
//
// Synchronization: the render mutex guards only render state (handle map,
// light caches); the world mutex guards authoritative state. Both are
// main-thread-only today (workers take completion-queue and pool mutexes
// only). Lock order note (issue #208, Phase D): the former world -> render
// nesting lived in the removed ChunkMobWorld adapter; mob queries now hold
// ONLY the world lock (WorldMobView) and the host samples entity light from
// this bridge AFTER the mob snapshot, so no path holds both map locks at
// once. Mesh dispatch still acquires render -> world (ensureShellPopulated).
// Phase E (#209) kept both paths main-thread-only; revisit the lock order
// before threading either path.
// =============================================================================

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
#include <Core/ThreadPool.hpp>
#include <Core/WorkloadTelemetry.hpp>
#include <Chunk/LightSample.hpp>
#include <World/VoxelWorld.hpp>
#include <utils.hpp>

#include <memory>

class Camera;
class ImmediateCommands;
class StagingRing;
class GpuResourceRetire;
struct MeshBuildResult;
class MeshResultPool;
class WorldPersistence;

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
	sim::ChunkIdentity identity{};
};

/// Lifecycle of a geometric commit group (PR #178 review round 4):
///
///   WaitingForMeshes  some member has no publishable newest result yet;
///                     old meshes keep rendering for every member
///   Uploading         every member's replacement is allocated; members
///                     record their GPU copies progressively across frames
///                     as staging room allows
///   ReadyToPublish    every replacement is resident: all members publish
///                     atomically in one CPU frame, then the group erases
enum class CommitGroupState
{
	WaitingForMeshes,
	Uploading,
	ReadyToPublish
};

/// One chunk inside a geometric commit group. `replacement` survives
/// frames (fresh arena ranges, future slot values, identity stamps) so a
/// member can record in one frame and publish several frames later;
/// staging scratch never survives the frame that recorded the copies.
struct PendingGroupMember
{
	Chunk *chunk{nullptr};
	Chunk::GpuReplacement replacement{};
};

/// Geometric commit group (PR #178 review): the chunks whose border
/// geometry was coupled by border voxel edits - the target plus its
/// shell-mirror neighbors. One edit contributes at most three members
/// (target + two sides; no diagonals), but transitive fusion can grow the
/// resulting group beyond three chunks. Light-only invalidated neighbors
/// never join. Overlapping groups are fused at registration (transitively),
/// so active groups always form disjoint chunk sets: one chunk belongs to
/// at most one group.
struct PendingMeshCommitGroup
{
	// Stable group identity: distinct from editId because a fused group
	// represents several logical edits. 0 is reserved (no group).
	uint64_t groupId{0};
	CommitGroupState state{CommitGroupState::WaitingForMeshes};
	std::vector<PendingGroupMember> members;
	// Debug provenance: the logical edits absorbed into this group.
	std::vector<uint64_t> editIds;
};

/// One chunk lifecycle debug event (issue #179 chunk inspector). Recorded
/// on the main thread into a globally bounded ring, only while tracing is
/// enabled; `kind` points at a static string literal.
struct ChunkDebugEvent
{
	double timeSec{0.0};
	glm::ivec3 chunk{0, 0, 0};
	const char *kind{nullptr};
	uint64_t revision{0};
	uint32_t sections{0}; // section mask / count context, event-dependent
};

/// Renderer-owned derived world projection: consumes the authoritative
/// world's change stream and produces meshes, lighting caches, GPU uploads
/// and draw/shadow lists for WorldRenderer. Never owns simulation state.
class RenderWorldBridge
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

	/// The world is REFERENCED, not owned: the host (sim::GameSimulation
	/// since Phase D #208) keeps its lifetime and must outlive the bridge.
	/// The bridge installs the render-side worker profiling seam on the world
	/// (headless worlds run without one).
	/// `worldGenerationId` is the host's world epoch (the simulation's
	/// session reload counter, issue #209): it stamps WHICH authoritative
	/// world incarnation this projection belongs to. The host's epoch
	/// barrier (Engine::resetWorldProjection) destroys the bridge before the
	/// world is replaced, so a projection never outlives its epoch; the
	/// stamp makes the pairing explicit for tests and diagnostics.
	/// Standalone worlds/tests may leave it 0.
	RenderWorldBridge(VoxelWorld *world, ThreadPool *threadPool, ChunkPool *chunkPool,
					  uint64_t worldGenerationId = 0);
	~RenderWorldBridge();

	RenderWorldBridge(const RenderWorldBridge &) = delete;
	RenderWorldBridge &operator=(const RenderWorldBridge &) = delete;

	/// The authoritative world this bridge projects (issue #207). The
	/// bridge never owns it; the accessor is the read seam for derived
	/// scheduling, UI telemetry and tests.
	VoxelWorld &world() { return *m_world; }
	const VoxelWorld &world() const { return *m_world; }

	/// The world epoch this projection was attached at (issue #209). Equal
	/// to sim::GameSimulation::worldGenerationId() for the live projection;
	/// 0 for standalone/test bridges.
	uint64_t worldGenerationId() const { return m_worldGenerationId; }

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
	/// Geometric commit groups publish atomically, while each member COPY
	/// consumes one upload-budget unit and may be recorded across frames.
	/// Publication consumes no budget. Returns the number of chunks published
	/// this call (a committed group contributes all its members).
	int uploadPendingMeshes(VmaAllocator allocator, StagingRing &staging, VkCommandBuffer cmd,
							GpuResourceRetire &retire, MeshArenas &arenas, const Camera &camera, int budget);

	/// After frames-in-flight delay, retire arena ranges and return chunks to the pool.
	void processDeferredReleases();

	/// Join finished worker jobs and clear in-transit flags (world generation
	/// + render mesh/light), then apply deferred authoritative edits.
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
	StreamingMaintenanceStats streamingMaintenanceStats() const { return m_world->streamingMaintenanceStats(); }

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

	/// Opt-in, globally bounded chunk lifecycle trace (issue #179): records
	/// load / gen / mesh / edit / upload / retire events for the chunk
	/// inspector. Main-thread recording only; disabled by default (zero
	/// cost beyond one branch at the event sites) and never grows beyond
	/// kChunkEventRingSize entries.
	static constexpr size_t kChunkEventRingSize = 256;
	void setChunkEventTraceEnabled(bool enabled) { m_chunkTraceEnabled = enabled; }
	bool chunkEventTraceEnabled() const { return m_chunkTraceEnabled; }
	/// Chronological copy of the recorded events (oldest first). Main thread.
	std::vector<ChunkDebugEvent> chunkDebugEvents() const;

	/// Synchronous bootstrap near spawn so the first frame has terrain.
	void generateInitialArea(const glm::vec3 &center, int radiusChunks, VmaAllocator allocator,
							 ImmediateCommands &imm, MeshArenas &arenas);

	/// Shared synchronous generation contract (issue #112): prepares voxel
	/// storage on the calling thread immediately before generateTerrain().
	/// Returns false — and releases the chunk back to the pool — when the
	/// storage allocation fails, so no slot is leaked.
	bool prepareAndGenerateChunk(Chunk *chunk, TerrainGenerator &generator);

	// --- Persistent world lifecycle (issue #180, Phase 3+4) ---
	// Authoritative state lives in VoxelWorld; these are convenience
	// forwards so the graphical host keeps one call surface. closeWorld()
	// additionally orchestrates the render-side quiesce around the world's
	// durability barrier (render jobs hold record leases).
	bool openWorld(const std::string &savesRoot, const std::string &worldName, std::string &outError);
	bool closeWorld();
	/// True while gameplay edits accepted against not-yet-readable chunks
	/// are still queued - authoritative voxel state is behind accepted input.
	bool hasPendingLogicalEdits() const { return m_world->hasPendingLogicalEdits(); }
	/// Canonical world->chunk mapping (floor division; correct for negative
	/// coordinates). Public so tools/benchmarks use the runtime convention.
	static glm::ivec3 worldToChunkCoord(const glm::vec3 &worldPos);
	/// Test/benchmark accessor: snapshot of the active render chunk set.
	std::vector<Chunk *> activeChunksSnapshot() const
	{
		std::shared_lock<std::shared_mutex> lock(m_mutex);
		return m_activeChunks;
	}
	/// Capture all dirty loaded chunks + flush (no close). False on I/O failure.
	bool flushWorld();
	bool isWorldOpen() const { return m_world->isWorldOpen(); }
	/// For UI/status; may be null (no world open). Const-correct pair.
	WorldPersistence *worldPersistence() { return m_world->worldPersistence(); }
	const WorldPersistence *worldPersistence() const { return m_world->worldPersistence(); }

private:
	void ensureLightHalo(Chunk *chunk, const glm::ivec3 &chunkIdx,
						 telemetry::Family telemetryFamily = telemetry::MeshFamily);
	/// Light-aware cross-chunk invalidation (issue #141 review fix): when a
	/// light-relevant edit lands within the 15-voxel halo radius of a border,
	/// the neighbor(s) on that side must remesh too. Derived render state.
	void markNeighborLightDirty(const glm::ivec3 &chunkPos, int x, int y, int z,
								TextureType previousType, TextureType type);
	/// A chunk just finished generating: scan its halo-radius border bands
	/// for emissive sources and dirty the side neighbors' affected sections.
	void dirtyNeighborsForArrivedLight(Chunk *chunk, const glm::ivec3 &chunkIdx);

	// --- World event stream consumption (issue #206/#207) --------------------
	/// Drain the authoritative event stream and keep the render handle map
	/// in sync: bind/unbind handles, publish generation render flags, apply
	/// edit invalidation. Main thread, immediately after each world call.
	void drainWorldEvents();
	/// Attach to a possibly-live world: bind render handles to records already
	/// resident (no Loaded event is re-emitted for them) and drain any queued
	/// change events, so the projection reflects current authoritative state
	/// even when attached after loading began (issue #207; the graphical host
	/// attaches a fresh bridge to the running GameSimulation after every
	/// world transition, issue #209). Main thread, constructor only.
	void attachToWorld();
	void onWorldLoaded(const WorldEvent &e);
	void onWorldUnloaded(const WorldEvent &e);
	void onWorldGenerated(const WorldEvent &e);
	void onWorldEditApplied(const WorldEvent &e);
	void retryPendingBinds();
	/// Single bind contract shared by every handle-registration path (attach,
	/// Loaded event, bind retry, bootstrap): acquire a render handle, bind it
	/// to the record, re-arm a stale MESHED claim, and register it in the
	/// render map. Returns nullptr when the pool is exhausted — the caller
	/// decides whether to park / grow / skip. Caller holds m_mutex.
	Chunk *bindRecord(const glm::ivec3 &coord, WorldChunk *record);

	// --- Geometric commit groups (PR #178 review) ---
	void registerCommitGroup(uint64_t editId, std::vector<PendingGroupMember> members);
	PendingMeshCommitGroup *commitGroupFor(Chunk *chunk);
	void eraseCommitGroup(uint64_t groupId);
	void removeChunkFromCommitGroups(Chunk *chunk);
	void discardAllCommitGroups();
	std::vector<PendingMeshCommitGroup> m_commitGroups;
	uint64_t m_nextGroupId{1};

	TaskPriority calculateTaskPriority(float distanceSq, float lodThresholdSq) const;

	/// Bounded trace write (no-op while tracing is disabled). Main thread.
	void recordChunkEvent(Chunk *chunk, const char *kind, uint32_t sections = 0);
	void recordWorldEvent(const glm::ivec3 &coord, uint64_t revision, const char *kind,
						  uint32_t sections = 0);

	/// Maintain chunk local light cache desire based on distance to camera
	/// (issue #128), with acquire/release hysteresis (issue #172).
	void updateEntityLightCacheIntent(Chunk &chunk, float distSq);

	/// Combined render+world quiesce for the persistence durability barrier.
	void drainAsyncJobsForPersistence();
	bool renderJobsBusy() const;

	// Testing hooks (issue #114 review): expose render internals to the
	// lifecycle tests; authoritative seams live on VoxelWorld's friends.
	friend struct RenderBridgeProbe;
	friend struct RenderBridgeStreamProbe;

	// --- The authoritative world (REFERENCED, host-owned; issue #207) ---
	VoxelWorld *m_world{nullptr};
	/// Host world epoch at attach (issue #209); 0 for standalone/test use.
	uint64_t m_worldGenerationId{0};
	/// Last observed world event-drop count (one-shot warning on change).
	uint64_t m_lastDroppedEvents{0};
	/// Persistent event-drain scratch (issue #219): reused across the several
	/// drainWorldEvents() calls per frame so the stream copy allocates only
	/// until the buffer reaches its steady-state capacity. Main-thread only.
	std::vector<WorldEvent> m_eventDrainBuffer;

	// --- Render handle map (derived; mirrors world residency) ---
	std::unordered_map<glm::ivec3, Chunk *, IVec3Hash> m_chunks;
	std::vector<Chunk *> m_activeChunks;
	mutable std::shared_mutex m_mutex;
	/// Defensive: world records whose render handle acquire failed (pool
	/// exhaustion race). Retried on the next load tick; normally empty.
	std::vector<glm::ivec3> m_pendingBinds;

	mutable std::mutex m_completedJobsMutex;
	std::vector<CompletedMeshJob> m_completedMeshJobs;
	std::vector<CompletedLightJob> m_completedLightJobs;
	std::atomic<size_t> m_pendingMeshJobsCount{0};
	std::atomic<size_t> m_pendingLightJobsCount{0};
	std::atomic<uint64_t> m_meshJobsDispatched{0};
	std::atomic<uint64_t> m_lightJobsDispatched{0};

	ThreadPool *m_threadPool{nullptr};
	ChunkPool *m_chunkPool{nullptr};
	/// Pooled halo blocks lent to in-flight mesh jobs (issue #141 review
	/// fix); one block is ~530 KiB, so a small pool serves the dispatch
	/// budget + worker count and grows on demand.
	LightHaloPool m_lightHaloPool{4};

	std::vector<Chunk *> m_deferredRelease;
	int m_deferredReleaseAge{0};

	// Chunk lifecycle event trace (issue #179). Main-thread only.
	bool m_chunkTraceEnabled{false};
	std::array<ChunkDebugEvent, kChunkEventRingSize> m_chunkEventRing{};
	size_t m_chunkEventWrite{0};
	size_t m_chunkEventCount{0};
};
