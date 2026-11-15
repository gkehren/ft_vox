#pragma once

// =============================================================================
// VoxelWorld — the CPU-owned AUTHORITATIVE world (issue #206, Epic #204
// Phase B). Single owner of everything gameplay and persistence need:
//
//   * world identity: the terrain generator (seed + kGeneratorVersion) and
//     the WorldPersistence facade (open/close/flush, issue #180 contracts);
//   * authoritative chunk residency: the WorldChunk record map, the pooled
//     voxel/border backing, load queue + streaming footprint, unload scan;
//   * serialized logical edits: the deferred-edit queue with per-coordinate
//     coalescing and neighbor-border mirror semantics (issue #114), applied
//     exactly once, never into a recycled incarnation;
//   * CPU worker publication of generated voxel backing (async terrain
//     dispatch + completion publish) and safe retirement/recycling;
//   * voxel queries with the exact ChunkCollisionView semantics (negative
//     coordinates, unloaded/unknown boundaries, effective pending edits).
//
// The public API and include graph are render-free (enforced by
// tests/check_core_includes.py): no Vulkan/VMA/MeshArena/draw-command/Camera
// types. Streaming takes plain position/forward/settings values; the
// graphical adapter (RenderWorldBridge, issue #207) converts Camera +
// RenderSettings into them.
//
// Derived render state (meshes, light caches, GPU uploads, visibility) stays
// on the render side. The world publishes a small main-thread EVENT STREAM
// (load/unload/gen/edit) that the render adapter consumes to keep its derived
// chunk handles in sync — the "CPU world change/revision stream" of the epic
// architecture. Headless consumers may ignore or drain it.
//
// Threading contract (unchanged from the pre-split ChunkManager):
//   * all lifecycle/edit/persistence entry points are main-thread;
//   * m_mutex (shared) guards the record map, queues and event stream;
//     scoped query views hold it shared for their lifetime;
//   * generation workers own their record exclusively while inTransit is
//     set and publish through m_completedGenerationChunks + the gen counter.
// =============================================================================

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include <Chunk/StreamHelpers.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Core/SimContracts.hpp>
#include <Core/ThreadPool.hpp>
#include <World/WorldChunk.hpp>
#include <World/WorldPersistence.hpp>
#include <utils.hpp>

class WorldCollisionView;

/// Render-free streaming settings: the simulation-relevant subset of the
/// graphical RenderSettings (the Phase A contracts doc records this split).
/// Budgets stay per-call integers owned by the host's frame pacing.
struct WorldStreamSettings
{
	int maxRenderDistance{512}; // streaming / unload radius (blocks)
	int minRenderDistance{192}; // full-quality near range (blocks); LOD threshold = 2x
	float streamFrontBias{0.30f};
};

/// What the last streaming maintenance tick did (issue #108 contract,
/// unchanged vocabulary).
enum class StreamingUpdateKind
{
	None,			// steady state: camera inside the same chunk, anchor and heading
	Incremental,	// chunk-cross or movement-anchor change: O(r) footprint diff
	HeadingRebuild, // heading threshold exceeded: full footprint recompute + diff
	FullRebuild		// startup / teleport / settings change: queue rebuilt from scratch
};

/// Hard cadence floor between out-of-range unload scans (issue #108 review).
constexpr uint32_t kUnloadCheckIntervalFrames = 60;

/// One authoritative-world lifecycle/change event for the derived render
/// adapter (main-thread stream, drained via takeEvents()). EditApplied
/// carries the render invalidation payload (previous type + mirror flag);
/// the world itself never interprets render scheduling.
enum class WorldEventKind : uint8_t
{
	Loaded,		 // record registered at coord (bind a render handle)
	Unloaded,	 // record retired at coord (unbind; external = test-owned record)
	GenQueued,	 // terrain generation dispatched for coord
	Generated,	 // terrain generation published: backing readable at coord
	EditApplied	 // one logical edit piece landed in authoritative storage
};

struct WorldEvent
{
	WorldEventKind kind{WorldEventKind::Loaded};
	glm::ivec3 coord{0, 0, 0};
	uint64_t generation{0};
	uint64_t revision{0};
	bool external{false}; // Unloaded: record owned outside the world (test seam)
	// EditApplied payload:
	int x{0}, y{0}, z{0};
	TextureType type{AIR};
	TextureType previousType{AIR};
	bool borderNeighbor{false};
};

/// Result of one logical edit request (place/delete). geometricMembers lists
/// the target plus every existing mirror neighbor when the edit couples
/// border geometry — the render adapter registers its atomic commit group
/// from it (PR #178 contract; the group itself is derived render state).
struct WorldEditResult
{
	bool accepted{false};
	uint64_t editId{0};
	std::vector<glm::ivec3> geometricMembers;
};

/// Narrow worker-profiling seam: the graphical adapter forwards to the
/// engine Profiler; headless worlds run without one. Two calls per generation
/// job — never per voxel (epic invariant: no per-voxel virtual dispatch).
struct WorldWorkerObserver
{
	virtual ~WorldWorkerObserver() = default;
	virtual uint64_t workerCaptureEpoch() const = 0;
	virtual void addWorkerSample(const char *stage, float ms, uint64_t epoch) = 0;
};

class VoxelWorld
{
public:
	/// The generator is referenced, not owned: the host (Engine today,
	/// GameSimulation in Phase D) keeps its lifetime. The world owns the
	/// world identity derived from it (seed + generator version) and every
	/// authoritative chunk/edit/persistence decision.
	VoxelWorld(TerrainGenerator *terrainGenerator, ThreadPool *threadPool);
	~VoxelWorld();

	VoxelWorld(const VoxelWorld &) = delete;
	VoxelWorld &operator=(const VoxelWorld &) = delete;

	// --- Streaming / residency (plain values, no camera) ---------------------
	/// Enqueue loads / mark far chunks for unload around `center`.
	/// `forwardXZ` is the normalized XZ view direction (fallback +Z).
	StreamingUpdateKind updateStreaming(const glm::vec3 &center, const glm::vec2 &forwardXZ,
										const WorldStreamSettings &settings);
	/// Pull from the load queue (record pool acquire). budget = max chunks.
	void processChunkLoading(int budget);
	/// Dispatch async terrain generation for nearby UNLOADED records.
	/// lodThresholdSq = (2 * fullQualityRange)^2, computed by the host.
	void generatePendingVoxels(const glm::vec3 &center, float lodThresholdSq, int budget);
	/// Grow the record pool toward minCapacity (incremental, capped per call;
	/// same growth contract as ChunkPool).
	bool ensureRecordCapacity(size_t minCapacity, size_t maxGrowPerCall = kChunkPoolMaxGrowPerCall);

	// --- Worker publication / edit application -------------------------------
	/// Publish finished generation jobs (clears inTransit, emits Generated).
	/// Stage 1 of the frame pump; the render adapter publishes its mesh/light
	/// jobs between this and applyPendingEdits() so the superseded-by-edit
	/// checks still see the pending queue (issue #114 ordering).
	void publishFinishedGeneration();
	/// Apply deferred edits whose records are editable now (targets before
	/// mirrors; stale incarnations dropped). Emits EditApplied events.
	void applyPendingEdits();
	/// Headless convenience: publishFinishedGeneration() + applyPendingEdits().
	void pump();

	/// Drain the main-thread event stream (render adapter / tests).
	std::vector<WorldEvent> takeEvents();
	/// Events dropped at the stream cap because a consumer fell behind
	/// (zero while draining at frame cadence). Main thread only, like the
	/// stream itself. Surfaced by RenderWorldBridge::drainWorldEvents as a
	/// one-shot warning on change, so the loss is observable rather than
	/// test-only.
	uint64_t droppedEventCount() const { return m_droppedEvents; }

	// --- Logical edits ---------------------------------------------------------
	WorldEditResult placeVoxel(const glm::vec3 &worldPos, TextureType type);
	WorldEditResult deleteVoxel(const glm::vec3 &worldPos);
	bool isVoxelActive(const glm::vec3 &worldPos) const;
	/// True while accepted edits are still queued against not-yet-readable
	/// records (authoritative state behind accepted input). Main-thread view.
	bool hasPendingLogicalEdits() const;
	bool hasPendingEditsFor(const WorldChunk *chunk) const;

	// --- Queries ---------------------------------------------------------------
	WorldChunk *getChunk(const glm::ivec3 &chunkPos);
	const WorldChunk *getChunk(const glm::ivec3 &chunkPos) const;
	WorldChunk *getChunkAtWorldPos(const glm::vec3 &worldPos);
	/// Canonical world->chunk mapping (floor division; correct for negative
	/// coordinates). The runtime convention (issue #180 review round 7).
	static glm::ivec3 worldToChunkCoord(const glm::vec3 &worldPos);
	size_t chunkCount() const;
	size_t pendingLoadCount() const;
	size_t pendingGenJobs() const;
	/// Gen jobs in flight OR any record worker-owned (mesh/light jobs set
	/// inTransit through their render handles too).
	bool asyncBusy() const;
	std::vector<WorldChunk *> activeChunksSnapshot() const;
	StreamingMaintenanceStats streamingMaintenanceStats() const { return m_streamStats; }

	/// Rebuild a record's neighbor border shell from resident neighbors
	/// (main thread; locks internally). The render adapter calls this at
	/// mesh dispatch; the edit path uses the locked variant internally.
	void ensureShellPopulated(WorldChunk *chunk);

	// --- Bootstrap ---------------------------------------------------------------
	/// Synchronous voxel bootstrap around center: creates + generates the
	/// square (2r+1)^2 footprint. Returns the created coordinates; the render
	/// adapter binds handles, meshes and uploads them.
	std::vector<glm::ivec3> generateInitialAreaVoxels(const glm::vec3 &center, int radiusChunks);
	/// Shared synchronous generation contract (issue #112): prepares storage
	/// on the calling thread immediately before generateTerrain(). Returns
	/// false — and recycles the record — when storage allocation fails.
	bool prepareAndGenerateChunk(WorldChunk *chunk, TerrainGenerator &generator);

	// --- Persistent world lifecycle (issue #180 contracts, unchanged) -----------
	bool openWorld(const std::string &savesRoot, const std::string &worldName, std::string &outError);
	/// Retryable close: quiesce, capture, flush, shutdown. False keeps the
	/// persistence OPEN and usable (failed flush / stranded edits).
	bool closeWorld();
	/// Capture all dirty loaded records + flush (no close). False on I/O failure.
	bool flushWorld();
	bool isWorldOpen() const { return m_persistence != nullptr; }
	WorldPersistence *worldPersistence() { return m_persistence.get(); }
	const WorldPersistence *worldPersistence() const { return m_persistence.get(); }
	/// Destructor-only close: resolve stranded edits, then capture/flush/
	/// shutdown UNCONDITIONALLY. Returns the flush success.
	bool forceCloseWorldForShutdown();
	/// Quiesce world-side async work (gen jobs + transit) and verify every
	/// accepted edit reached authoritative state. The graphical adapter
	/// quiesces its render jobs around this (combined durability barrier).
	bool drainAsyncJobsForPersistence();
	/// Final-shutdown resolution of stranded edits: synchronously generate
	/// UNLOADED records that still hold accepted edits, then pump.
	void resolvePendingEditsForPersistenceShutdown();

	// --- Pools / identity ---------------------------------------------------------
	VoxelPool &voxelPool() { return m_voxelPool; }
	const VoxelPool &voxelPool() const { return m_voxelPool; }
	BorderPool &borderPool() { return m_borderPool; }
	const BorderPool &borderPool() const { return m_borderPool; }
	TerrainGenerator *terrainGenerator() const { return m_terrainGenerator; }
	size_t recordCapacity() const;
	size_t recordFreeCount() const;

	/// Test seam: register an externally-owned record (a standalone Chunk's
	/// WorldChunk) at a coordinate without the streaming load path. The world
	/// never recycles it; unload retires the map entry only.
	void registerExternalChunk(const glm::ivec3 &pos, WorldChunk *record);

	/// Worker profiling seam (null = no profiling). Install before dispatch.
	void setWorkerObserver(WorldWorkerObserver *observer) { m_workerObserver = observer; }

	// Testing hooks (issue #114 review seams, re-pointed at the world).
	friend struct RenderBridgeProbe;
	friend struct RenderBridgeStreamProbe;
	friend struct VoxelWorldProbe;
	friend class WorldCollisionView;

	struct StreamState
	{
		glm::ivec3 lastCamChunk{std::numeric_limits<int>::max(), 0, std::numeric_limits<int>::max()};
		glm::ivec2 lastMovementAnchor{std::numeric_limits<int>::max(), std::numeric_limits<int>::max()};
		glm::vec2 lastCamForwardXZ{0.f, 1.f};
		int lastMaxRenderDistance{-1};
		float lastStreamFrontBias{-1.f};
		bool initialized{false};
	};

private:
	// --- Streaming internals (ported from ChunkManager, issue #108) -----------
	void queueUnloadOutOfRange(const glm::vec3 &center, const WorldStreamSettings &settings);
	StreamingUpdateKind loadChunksAroundPlayer(const glm::ivec3 &centerChunk, const glm::vec3 &center,
											   const glm::vec2 &forwardXZ, const WorldStreamSettings &settings);
	StreamingUpdateKind rebuildStreamingQueueFull(const glm::ivec3 &centerChunk, const glm::vec3 &center,
												  const glm::vec2 &forwardXZ, const WorldStreamSettings &settings);
	StreamingUpdateKind reconcileStreamingIncremental(const glm::ivec3 &centerChunk, const glm::vec3 &center,
													  const glm::vec2 &forwardXZ, const WorldStreamSettings &settings);
	StreamingUpdateKind reconcileStreamingHeading(const glm::ivec3 &centerChunk, const glm::vec3 &center,
												  const glm::vec2 &forwardXZ, const WorldStreamSettings &settings);
	void applyFootprintDiffToQueue(const FootprintDiff &diff, const glm::vec3 &center,
								   const glm::vec2 &forwardXZ, float frontBias);
	TaskPriority calculateTaskPriority(float distanceSq, float lodThresholdSq) const;

	// --- Record pool -----------------------------------------------------------
	WorldChunk *acquireRecord(const glm::vec3 &origin);
	void releaseRecord(WorldChunk *record);
	void growRecords(size_t addCount);

	// --- Deferred edit subsystem (issue #114 review; main-thread only) --------
	bool scheduleLogicalEdit(const glm::ivec3 &chunkPos, int x, int y, int z,
							 TextureType type, WorldEditResult &out);
	bool mustDeferVoxelEdit(const WorldChunk *chunk) const;
	TextureType effectiveVoxelType(const WorldChunk *chunk, int x, int y, int z) const;
	void queueOrApplyEdit(WorldChunk *chunk, const glm::ivec3 &chunkPos, int x, int y,
						  int z, TextureType type, bool borderNeighbor,
						  bool forceDefer, uint64_t editId);
	void enqueueOrApplyMirrorEdits(const glm::ivec3 &chunkPos, int x, int y, int z,
								   TextureType type, bool forceDefer, uint64_t editId,
								   std::vector<glm::ivec3> *geometricMembers);
	void queuePendingEdit(WorldChunk *chunk, const glm::ivec3 &chunkPos, int x, int y, int z,
						  TextureType type, bool borderNeighbor, uint64_t editId);
	void erasePendingEditsFor(const WorldChunk *chunk);
	/// Shell rebuild with m_mutex ALREADY held (edit apply path).
	void ensureShellPopulatedLocked(WorldChunk *chunk);
	void emitEditApplied(WorldChunk *chunk, int x, int y, int z, TextureType type,
						 TextureType previousType, bool borderNeighbor);

	// --- Persistence internals (issue #180) -------------------------------------
	void applyPersistentOverrides(WorldChunk *chunk);
	void captureChunkEditsForUnload(WorldChunk *chunk);
	void captureAllChunkEdits();
	bool anyChunkInTransitForPersistence() const;

	void emitEvent(const WorldEvent &event);

	// --- Authoritative state ------------------------------------------------------
	std::unordered_map<glm::ivec3, WorldChunk *, IVec3Hash> m_chunks;
	std::vector<WorldChunk *> m_activeChunks;
	mutable std::shared_mutex m_mutex;

	/// A voxel edit deferred because its target record was in transit or
	/// UNLOADED (issue #114/#115). One target entry plus one mirror entry per
	/// adjacent neighbor, grouped by editId and coalesced per coordinate.
	struct PendingVoxelEdit
	{
		WorldChunk *chunk{nullptr};
		// Incarnation stamp at queue time: a queued edit whose record was
		// recycled (generation moved on) is dropped at apply time.
		uint64_t generation{0};
		glm::ivec3 chunkPos{0};
		int x{0}, y{0}, z{0};
		TextureType type{AIR};
		bool borderNeighbor{false};
		uint64_t editId{0};
	};
	std::vector<PendingVoxelEdit> m_pendingEdits;
	uint64_t m_nextEditId{1};

	// --- Streaming state -----------------------------------------------------------
	std::vector<LoadCandidate> m_loadQueue;
	std::unordered_set<glm::ivec3, IVec3Hash> m_enqueuedLoads;
	StreamState m_streamState;
	ChunkDesiredFootprint m_desiredFootprint;
	size_t m_loadQueueHead{0};
	bool m_queueNeedsSort{false};
	uint32_t m_streamFramesSinceUnloadCheck{0};
	StreamingMaintenanceStats m_streamStats;

	// --- Generation workers ----------------------------------------------------------
	mutable std::mutex m_completedJobsMutex;
	struct CompletedGenJob
	{
		WorldChunk *chunk{nullptr};
		uint64_t generation{0};
	};
	std::vector<CompletedGenJob> m_completedGenerationChunks;
	std::atomic<size_t> m_pendingGenJobsCount{0};
	WorldWorkerObserver *m_workerObserver{nullptr};

	// --- Events -----------------------------------------------------------------------
	std::deque<WorldEvent> m_events;
	uint64_t m_droppedEvents{0};

	// --- Persistence ---------------------------------------------------------------------
	std::unique_ptr<WorldPersistence> m_persistence;

	// --- Backing pools + record pool ------------------------------------------------------
	// Declared before the record storage so they outlive every WorldChunk
	// (records return their voxel/border backing on destruction).
	VoxelPool m_voxelPool;
	BorderPool m_borderPool;
	std::vector<std::unique_ptr<WorldChunk>> m_recordStorage;
	std::vector<WorldChunk *> m_recordFreeList;
	std::unordered_set<WorldChunk *> m_externalRecords;

	TerrainGenerator *m_terrainGenerator{nullptr};
	ThreadPool *m_threadPool{nullptr};
};
