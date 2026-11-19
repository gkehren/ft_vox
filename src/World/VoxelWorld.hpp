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
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include <Chunk/StreamHelpers.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Core/SimContracts.hpp>
#include <Core/ThreadPool.hpp>
#include <Sim/PlayerId.hpp>
#include <World/WorldChunk.hpp>
#include <World/WorldPersistence.hpp>
#include <utils.hpp>

class WorldCollisionView;
namespace sim
{
class GameSimulation;
inline constexpr int kMaxPlayerInterestRadiusChunks = 8;
}

/// Shared limits for one authoritative residency tick. A caller can lower
/// any budget, including to zero to pause that stage.
struct PlayerStreamingBudget
{
	static constexpr unsigned kMaxLoads = 128;
	static constexpr unsigned kMaxGenerations = 64;
	static constexpr unsigned kMaxEvictions = 64;
	static constexpr unsigned kMaxOutstandingGenerations = 64;
	unsigned loads{32};
	unsigned generations{8};
	unsigned evictions{16};
};

struct PlayerStreamingStats
{
	size_t observers{0};
	size_t desiredChunks{0};
	size_t residentChunks{0}; // includes registered unknowns and retiring backlog
	size_t waitingForRecord{0};
	size_t waitingForGeneration{0}; // desired, registered, and no job dispatched
	size_t outsideInterest{0};
	size_t pinnedEdits{0}; // out-of-interest chunks protected during this tick's eviction pass
	size_t outstandingGenerations{0};
	size_t transientOverrideChunks{0};
	size_t transientOverrideVoxels{0};
	size_t loaded{0}, generationStarts{0}, evicted{0}; // this tick
};

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

struct ChunkContentVersion
{
	uint64_t generation{}, revision{};
};

struct LogicalVoxelEdit
{
	uint64_t id{};
	glm::ivec3 cell{};
	ChunkContentVersion base;
	TextureType type{AIR};
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
	/// The generator is referenced, not owned: the host (sim::GameSimulation
	/// since Phase D #208) keeps its lifetime. The world owns the world
	/// identity derived from it (seed + generator version) and every
	/// authoritative chunk/edit/persistence decision.
	VoxelWorld(TerrainGenerator *terrainGenerator, ThreadPool *threadPool);
	~VoxelWorld();

	VoxelWorld(const VoxelWorld &) = delete;
	VoxelWorld &operator=(const VoxelWorld &) = delete;

	// --- Streaming / residency (plain values, no camera) ---------------------
	// The solo streaming entry points yield to advancePlayerStreaming once
	// the simulation selects player residency for this world epoch.
	/// Enqueue loads / mark far chunks for unload around `center`.
	/// `forwardXZ` is the normalized XZ view direction (fallback +Z).
	StreamingUpdateKind updateStreaming(const glm::vec3 &center, const glm::vec2 &forwardXZ,
										const WorldStreamSettings &settings);
	/// Pull from the load queue (record pool acquire). budget = max chunks.
	void processChunkLoading(int budget);
	/// Dispatch async terrain generation for nearby UNLOADED records.
	/// lodThresholdSq = (2 * fullQualityRange)^2, computed by the host.
	void generatePendingVoxels(const glm::vec3 &center, float lodThresholdSq, int budget);
	/// Synchronous counterpart for headless hosts without a worker pool
	/// (issue #210, Epic #204 Phase F): generates up to `budget` UNLOADED
	/// records nearest-first on the calling thread, with the same selection,
	/// saved-override re-application and GenQueued/Generated event vocabulary
	/// as the async path. Deterministic by construction — no worker schedule
	/// participates. Storage allocation failures leave the record UNLOADED
	/// for a later tick (registered records are never recycled here, unlike
	/// the pre-registration bootstrap path). Returns the number generated.
	int generatePendingVoxelsSync(const glm::vec3 &center, int budget);
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
	/// Buffer-reuse drain (issue #219): append the pending events into `out`
	/// (keeping its capacity) and empty the stream. Same events/order as the
	/// return-by-value overload; lets a consumer that drains several times per
	/// frame reuse one persistent buffer instead of allocating per drain.
	void takeEvents(std::vector<WorldEvent> &out);
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
	/// Monotonic logical edit counter: the id the NEXT accepted edit will
	/// carry (issue #208 world snapshot seam). Main-thread view.
	uint64_t nextEditId() const { return m_nextEditId; }
	bool hasPendingEditsFor(const WorldChunk *chunk) const;

	// Owner-only observation. Record logical edits at acceptance, before
	// deferred/coalesced writes can affect their publication order.
	void observeLogicalEdits() { m_observeLogicalEdits = true; }
	bool drainLogicalEdits(std::vector<LogicalVoxelEdit> &out);
	std::optional<ChunkContentVersion> chunkContentVersion(glm::ivec3 coord) const;
	// Copy the published backing and its canonical pending overlay at one
	// owner boundary. Returns the next-edit watermark, or zero if unavailable.
	uint64_t captureChunkContent(glm::ivec3 coord, std::span<uint8_t, CHUNK_VOLUME> out,
		ChunkContentVersion &version) const;

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
	/// Owner-thread snapshot. Override counts include evicted coordinates and
	/// reflect edits applied since the last streaming tick.
	PlayerStreamingStats playerStreamingStats() const;
	// Nearest-first disk offsets shared by residency and network interest.
	static std::span<const glm::ivec3> playerInterestOffsets(int radiusChunks);

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
	/// shutdown UNCONDITIONALLY. Returns false when the drain, the flush or
	/// the checkpoint commit failed. Since stabilization PR5b a checkpoint
	/// is committed ONLY after a COMPLETE durability barrier (drain AND
	/// flush both succeeded — review correction): the caller's gameplay
	/// payloads when provided (the GameSimulation shutdown safety net
	/// captures inventory + items; the player payload is carried forward),
	/// else a carry-forward commit. Any failure is REPORTED with a distinct
	/// diagnostic (no retry point at destruction); the service is still
	/// released unconditionally and the previous committed checkpoint
	/// remains the recovery point.
	bool forceCloseWorldForShutdown(const worldsave::CheckpointPayloads *gameplay = nullptr);
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
	friend class sim::GameSimulation;
	struct PlayerInterest
	{
		sim::PlayerId id;
		glm::ivec3 center;
		int radiusChunks;
		bool operator==(const PlayerInterest &) const = default;
	};
	bool advancePlayerStreaming(std::span<const PlayerInterest> interests, PlayerStreamingBudget budget);
	template <typename Eligible>
	std::vector<glm::ivec3> selectPlayerChunks(size_t budget, sim::PlayerId &lastServed,
											 std::span<const glm::ivec3> stranded, Eligible eligible) const;
	void applyTransientPlayerEdits(WorldChunk *chunk);
	bool m_playerStreaming{false};
	std::vector<PlayerInterest> m_playerInterests;
	std::unordered_set<glm::ivec3, IVec3Hash> m_playerDesired;
	sim::PlayerId m_lastPlayerLoad;
	sim::PlayerId m_lastPlayerGeneration;
	PlayerStreamingStats m_playerStreamingStats;
	std::unordered_map<glm::ivec3, WorldChunk::ChunkEditMap, IVec3Hash> m_transientPlayerEdits;
	size_t m_transientPlayerOverrideCount{0};

	std::vector<glm::ivec3> loadChunkRecords(std::span<const glm::ivec3> coords);
	size_t retireChunks(std::span<const glm::ivec3> coords);
	struct GenerationCandidate
	{
		WorldChunk *chunk;
		float distanceSq{0.f};
	};
	size_t dispatchGeneration(std::span<const GenerationCandidate> queue, std::optional<float> lodThresholdSq);
	size_t outstandingGenerationCount() const;
	int generateSynchronously(std::span<const GenerationCandidate> queue);
	void publishTerrainBacking(WorldChunk *chunk);
	void finishGeneration(WorldChunk *chunk);
	bool canReadVoxels(const WorldChunk &chunk) const
	{
		return chunk.isVoxelBackingReadable() && (!m_playerStreaming || chunk.m_terrainPublished);
	}

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
	/// Durability of deferred edits at unload (review C1): edits accepted
	/// against a record that never became readable (UNLOADED) live only in
	/// the deferred queue. Bake them into the persistent override set BEFORE
	/// erasePendingEditsFor() drops them, so the next generation of the
	/// coordinate re-applies them. Mirror (border-shell) entries carry no
	/// independent durable content — they duplicate the owning chunk's
	/// canonical edit and are rebuilt from neighbors on reload.
	void bakeDeferredEditsForUnload(WorldChunk *chunk);
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
	uint64_t m_nextContentGeneration{1};
	bool m_observeLogicalEdits{false}, m_logicalEditGap{false};
	std::vector<LogicalVoxelEdit> m_logicalEdits;

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
		bool succeeded{true};
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
