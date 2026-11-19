#pragma once

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif

#include <vector>
#include <cassert>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <glm/gtx/hash.hpp>

#include <chrono>

#include <Chunk/TerrainGenerator.hpp>
#include <Chunk/VoxelPool.hpp>
#include <Chunk/ChunkBorders.hpp>
#include <Chunk/ChunkLightHalo.hpp>
#include <Chunk/ChunkLightPool.hpp>
#include <Chunk/ChunkMeshResult.hpp>
#include <Vulkan/VkBuffer.hpp>
#include <Vulkan/MeshArena.hpp>
// (CI fix: the Camera/Camera.hpp include here was dead — Chunk never uses
// Camera — and dragged SDL3 into every header-only consumer of Chunk.hpp,
// breaking the Vulkan-only test targets on Linux where SDL3 headers are not
// globally visible. Consumers that need Camera include it themselves.)
#include <utils.hpp>
#include <Engine/EngineDefs.hpp>
#include <Core/WorkloadTelemetry.hpp>
#include <World/WorldChunk.hpp>

// TextureManager only for static isTransparent — no GL dependency in mesh gen.
#include <Renderer/TextureManager.hpp>

class ImmediateCommands;
class StagingRing;
class GpuResourceRetire;
struct MeshBuildResult;
class MeshResultPool;

/// Render-side chunk handle (issue #206, Epic #204 Phase B).
///
/// The AUTHORITATIVE voxel state (backing, occupancy, borders, persistent
/// edits, identity, lifecycle) lives in a WorldChunk owned by VoxelWorld.
/// This class keeps only DERIVED render state — GPU arena ranges, indirect
/// draw caches, pending mesh payloads, entity light caches, visibility and
/// mesh scheduling — and forwards every voxel read/write to its bound record.
///
/// Binding:
///   * In the game, the RenderWorldBridge acquires a render Chunk from
///     ChunkPool and binds it to the world's record for the same coordinate.
///     The world owns the record's lifetime; the generation stamp rejects any
///     stale access.
///   * Standalone (tests / tools), a Chunk owns a private WorldChunk created
///     with the pools passed to the constructor, so the full historical
///     single-object API keeps working without a world.
///
/// A bound Chunk must never be moved or reset through ChunkPool — the world
/// drives its record's lifecycle. Pool acquire/release only ever see unbound
/// chunks.
class Chunk
{
public:
	Chunk(const glm::vec3 &position, ChunkState state = ChunkState::UNLOADED,
		  VoxelPool *voxelPool = nullptr, BorderPool *borderPool = nullptr,
		  MeshResultPool *meshPool = nullptr, ChunkLightPool *lightPool = nullptr);
	Chunk(Chunk &&other) noexcept;
	Chunk &operator=(Chunk &&other) noexcept;
	~Chunk();

private:
	/// Empty-state construction for the move constructor to delegate to
	/// (review M11): every member carries a default initializer, so the
	/// transfer lives in ONE place (moveFrom) instead of two diverging
	/// ~80-line copies.
	Chunk() noexcept = default;
	/// Single move-transfer implementation (review M11): copies every field
	/// — including m_activeIndex, which the old ctor/assign pair dropped —
	/// then zeroes the moved-from render state. The caller releases the
	/// destination's prior resources first (move assignment) or starts from
	/// the empty default state (move construction).
	void moveFrom(Chunk &&other) noexcept;

public:

	// --- Authoritative record binding (issue #206) ---------------------------
	/// The record this chunk reads/writes voxels through: the bound world
	/// record, or the privately owned one for standalone chunks. Null on a
	/// moved-from chunk OR a freshly constructed handle that has not been
	/// bound or used standalone yet (the owned record is allocated lazily —
	/// see recordForWrite — so idle pool handles carry no per-column backing).
	WorldChunk *worldChunk() const { return m_active; }
	/// Mutable record access for standalone tools/tests that edit per-column
	/// state directly (e.g. biome tint fixtures). Lazily creates the owned
	/// record for an unbound handle. Do NOT use this to mutate a bound
	/// handle's world record outside the world's edit path.
	WorldChunk *mutableWorldChunk() { return recordForWrite(); }
	/// Bind to a world-owned record. Any privately owned record must be
	/// retired (no voxel storage) — pool chunks always are.
	void bindWorldChunk(WorldChunk *record)
	{
		assert(record != nullptr);
		assert(!m_ownedWorld || !m_ownedWorld->hasVoxelStorage());
		m_world = record;
		m_active = record;
	}
	/// Drop the world binding (unload path); voxel access falls back to the
	/// owned record (created lazily on next standalone use). The render state
	/// (GPU ranges awaiting retirement) is untouched.
	void unbindWorldChunk()
	{
		if (m_active)
			m_position = m_active->getPosition();
		m_world = nullptr;
		m_active = m_ownedWorld.get();
	}
	bool isWorldBound() const { return m_world != nullptr; }

	// --- Forwarded authoritative state ---------------------------------------
	const glm::vec3 &getPosition() const
	{
		return m_active ? m_active->getPosition() : m_position;
	}
	void setState(ChunkState state)
	{
		// Render scheduling side effect of the lifecycle transition (the
		// pre-split behavior): GENERATED/UNLOADED re-arm meshing.
		if (state == ChunkState::GENERATED || state == ChunkState::UNLOADED)
			meshNeedsUpdate = true;
		recordForWrite()->setState(state);
	}
	ChunkState getState() const
	{
		return m_active ? m_active->getState() : ChunkState::UNLOADED;
	}
	bool isVoxelBackingReadable() const { return m_active && m_active->isVoxelBackingReadable(); }
	bool isInTransit() const { return m_active && m_active->isInTransit(); }
	bool isReplicaRetiring() const { return m_active && m_active->isReplicaRetiring(); }
	void setInTransit(bool val)
	{
		recordForWrite()->setInTransit(val);
	}

	const Voxel &getVoxel(uint32_t x, uint32_t y, uint32_t z) const
	{
		assert(x < CHUNK_SIZE && y < CHUNK_HEIGHT && z < CHUNK_SIZE);
		if (!m_active)
		{
			static constexpr Voxel s_air{static_cast<uint8_t>(AIR)};
			return s_air;
		}
		return m_active->getVoxel(x, y, z);
	}
	bool isVoxelActive(int x, int y, int z) const
	{
		return m_active && m_active->isVoxelActive(x, y, z);
	}
	/// Authoritative write + render section invalidation. The world's edit
	/// path calls WorldChunk::setVoxel directly and invalidates through the
	/// edit event stream instead (same net effect, one owner per side).
	void setVoxel(int x, int y, int z, TextureType type);

	bool hasVoxelStorage() const { return m_active && m_active->hasVoxelStorage(); }
	VoxelPool *getVoxelPool() const { return m_active ? m_active->getVoxelPool() : nullptr; }
	bool hasBorderStorage() const { return m_active && m_active->hasBorderStorage(); }
	BorderPool *getBorderPool() const { return m_active ? m_active->getBorderPool() : nullptr; }
	bool isShellEmpty() const { return !m_active || m_active->isShellEmpty(); }
	TextureType sampleForMeshing(int x, int y, int z) const
	{
		return m_active ? m_active->sampleForMeshing(x, y, z) : AIR;
	}
	bool prepareVoxelStorageForGeneration()
	{
		return recordForWrite()->prepareVoxelStorageForGeneration();
	}
	void releaseVoxelStorageOnRetire()
	{
		if (m_active)
			m_active->releaseVoxelStorageOnRetire();
	}
	void releaseNeighborBorders()
	{
		if (m_active)
			m_active->releaseNeighborBorders();
	}
	void rebuildBordersFromNeighbors(const Chunk *west, const Chunk *east,
									 const Chunk *south, const Chunk *north);
	/// Deterministic generation + render re-arm (bootstrap / standalone path).
	/// The world's async path generates the record directly and publishes the
	/// render flags when it drains the generation completion event.
	void generateTerrain(TerrainGenerator &generator);

	// --- Render-side publication of authoritative world events (issue #206) --
	/// The world's async generation completed for the bound record: re-arm
	/// meshing exactly as the pre-split generateTerrain() did inline.
	void notifyTerrainGenerated()
	{
		meshNeedsUpdate = true;
		m_dirtySections.store(kAllSectionMask, std::memory_order_relaxed);
	}
	/// One authoritative edit piece landed in the bound record (world edit
	/// event stream): section invalidation + GENERATED re-arm, matching the
	/// pre-split queueOrApplyEdit() render side effects.
	void notifyEditApplied(int x, int y, int z, TextureType type,
						   TextureType previousType, bool borderWrite)
	{
		markEditDirtySections(x, y, z, type, previousType, borderWrite);
		setState(ChunkState::GENERATED);
	}

	// Persistent-edit tracking (issue #180) — forwarded to the record.
	using ChunkEditMap = WorldChunk::ChunkEditMap;
	void setTrackPersistentEdits(bool on)
	{
		recordForWrite()->setTrackPersistentEdits(on);
	}
	bool hasPersistentEdits() const { return m_active && m_active->hasPersistentEdits(); }
	const ChunkEditMap &persistentEdits() const
	{
		static const ChunkEditMap s_empty{};
		return m_active ? m_active->persistentEdits() : s_empty;
	}
	ChunkEditMap takePersistentEdits()
	{
		return m_active ? m_active->takePersistentEdits() : ChunkEditMap{};
	}
	void clearPersistentEdits()
	{
		if (m_active)
			m_active->clearPersistentEdits();
	}

	bool deleteVoxel(const glm::vec3 &position);
	bool placeVoxel(const glm::vec3 &position, TextureType type);

	// --- Identity (issue #206: single source = the WorldChunk record) --------
	uint64_t meshGeneration() const { return m_active ? m_active->generation() : 0; }
	uint64_t meshRevision() const { return m_active ? m_active->revision() : 0; }
	sim::ChunkIdentity chunkIdentity() const
	{
		return m_active ? m_active->identity() : sim::ChunkIdentity{};
	}

	// --- Light caches (derived, render-owned) --------------------------------
	bool hasLightStorage() const { return m_lightStorage != nullptr; }
	ChunkLightPool *getLightPool() const { return m_lightPool; }
	const ChunkLightStorage *getLightStorage() const { return m_lightStorage; }
	void releaseLightStorage();
	/// Main-thread commit of a light-cache-only job result (issue #172):
	/// takes ownership of `storage` (acquired from this chunk's light pool).
	/// Any storage already held is released first - ownership stays exclusive.
	void attachLightStorage(ChunkLightStorage *storage);
	bool localLightCacheWanted() const { return m_localLightCacheWanted.load(std::memory_order_relaxed); }
	void setLocalLightCacheWanted(bool wanted) { m_localLightCacheWanted.store(wanted, std::memory_order_relaxed); }
	uint16_t sampleLightRaw(int x, int y, int z) const;
	lighting::LocalVoxelLight sampleLight(int x, int y, int z) const;

	// Draw command collection (issue #109): the passes bind the shared mesh
	// arenas once per page pair and submit one indirect draw per live
	// section. Each collected entry carries the arena pages its range lives
	// in so the pass can group contiguous commands under one bind.
	struct IndirectDraw
	{
		VkDrawIndexedIndirectCommand cmd{};
		uint32_t vertexPage{MeshArena::kNoPage};
		uint32_t indexPage{MeshArena::kNoPage};
		glm::ivec3 chunkOrigin{0};
	};
	/// Appends one IndirectDraw per live opaque section (or the single LOD
	/// range). Returns the number appended. Stale-until-replaced (issue
	/// #177): while a replacement upload is pending the committed draw
	/// cache stays collectable and drawable - only chunks with no
	/// committed mesh at all are skipped.
	size_t collectOpaqueDraws(std::vector<IndirectDraw> &out) const;
	size_t collectWaterDraws(std::vector<IndirectDraw> &out) const;

	// Mesh building (issue #104): the CPU mesh payload no longer lives on the
	// Chunk. Workers build into a pooled MeshBuildResult and publish it
	// for upload; the Chunk keeps only GPU handles/counts.
	//
	// Worker contract (issue #114 review): buildMesh/buildLODMesh may READ
	// the authoritative record (voxels, borders, biome metadata, position)
	// but must not mutate any lifecycle/render state. The completed result
	// is committed exclusively by publishMeshResult() on the main thread.
	// `generation`/`revision` must be captured at dispatch time on the main
	// thread (see meshPendingChunks) and are stamped into the result.
	void buildMesh(MeshBuildResult &out, uint64_t generation, uint64_t revision);
	// Section-selective full-quality build (issue #107): bit s set in
	// `sectionMask` rebuilds that vertical 16^3 section into
	// out.sections[s] (section-local indices); unset sections keep their
	// previously published GPU state. Lighting is recomputed chunk-wide for
	// the job, so every rebuilt section sees the same light field a whole
	// build would produce.
	void buildMesh(MeshBuildResult &out, uint64_t generation, uint64_t revision,
				   uint16_t sectionMask);
	void buildLODMesh(MeshBuildResult &out, uint64_t generation, uint64_t revision);
	// Light-cache-only build (issue #172): computes the chunk-wide light field
	// (same computeLightField + halo semantics as buildMesh) and packs it into
	// `out` without producing any mesh geometry. Used by the dedicated
	// light-cache worker path for already-MESHED chunks entering the entity
	// light radius, so cache acquisition never invalidates a valid render
	// mesh. Worker-side and read-only with respect to published chunk state;
	// the result is committed exclusively on the main thread via
	// attachLightStorage() after generation/revision/intent validation.
	void buildLightCache(ChunkLightStorage &out);
	// Ranged bodies used by buildMesh/buildLODMesh with occupancy bounds
	// derived from the section metadata. Exposed (private, probe-tested) so
	// tests can force a full [0, CHUNK_HEIGHT-1] range and verify the
	// metadata-driven bounds produce byte-identical meshes. The caller must
	// have run out.beginBuild() (the wrappers do it; bounds outside the
	// occupied span simply iterate empty slices).
	void buildMeshRanged(MeshBuildResult &out, uint64_t generation, uint64_t revision,
						 int occMinY, int occMaxY);
	void buildLODMeshRanged(MeshBuildResult &out, int scanTopY);
	// Main-thread commit of a completed build result (the ONLY place a mesh
	// becomes official). Rejects - returning false and returning the block
	// to its pool without touching any chunk state - when the result is
	// superseded: built by another chunk, for a retired incarnation
	// (generation), or for older voxel/border content (revision). On success
	// the result is attached and m_isLODMesh/state/meshNeedsUpdate are
	// committed from it; any previously attached result is replaced.
	bool publishMeshResult(MeshBuildResult *result);
	// Synchronous convenience path (bootstrap / tests): acquire a pooled
	// block, build, finish accounting, publish. Returns false when nothing
	// was published (allocation failure or superseded result); other
	// exceptions propagate.
	bool generateMesh();
	bool generateLODMesh();

	bool hasPendingMeshResult() const { return m_pendingResult != nullptr; }
	// True when a full-quality (sectioned) build result is published but not
	// yet uploaded: a further partial section build could not compose with
	// the unuploaded sections, so dispatch expands its mask to all sections
	// (issue #107).
	bool hasUnuploadedFullMesh() const;
	MeshResultPool *getMeshResultPool() const { return m_resultPool; }

	// Section dirty tracking (issue #107): bit s set => section s needs a
	// mesh rebuild. Edits mark the affected sections (own + Y-boundary
	// neighbors + a conservative light range); mesh dispatch takes and
	// clears the mask, and a rejected publish re-arms it. Render scheduling
	// state: stays on the render side of the issue #206 boundary.
	void markSectionsDirty(uint16_t mask) { m_dirtySections.fetch_or(mask, std::memory_order_relaxed); }
	uint16_t takeDirtySections() { return m_dirtySections.exchange(0, std::memory_order_relaxed); }
	uint16_t dirtySections() const { return m_dirtySections.load(std::memory_order_relaxed); }

	// Vertical occupancy granularity (issue #105) — forwarded constants.
	static constexpr int kOccupancySectionSize = WorldChunk::kOccupancySectionSize;
	static constexpr int kOccupancySections = WorldChunk::kOccupancySections;
	bool hasWaterMesh() const { return waterIndexCount > 0; }
	bool isLODMesh() const { return m_isLODMesh; }
	bool needsGPUUpload() const { return meshNeedsUpdate.load(); }

	/// Synchronous upload (bootstrap / tests). Prefer uploadToGPUAsync on the hot path.
	void uploadToGPU(VmaAllocator allocator, ImmediateCommands &imm, MeshArenas &arenas);

	/// Suballocate arena ranges and record staging copies into cmd (issue
	/// #109). Returns false if staging is full or the arenas cannot back a
	/// required range (CPU mesh kept; try again next frame). On failure no
	/// arena range, slot or draw count is modified.
	bool uploadToGPUAsync(VmaAllocator allocator, StagingRing &staging, VkCommandBuffer cmd,
	                      GpuResourceRetire &retire, MeshArenas &arenas);

	/// Published per-section GPU ranges (issue #107/#109). Immutable once
	/// published: a rebuilt section allocates fresh ranges and swaps the
	/// replacement slot in atomically.
	struct SectionGpuSlot
	{
		uint32_t vertexPage{MeshArena::kNoPage};
		uint32_t vertexOffset{0}; // bytes within its arena page
		uint32_t vertexSlotBytes{0};
		uint32_t vertexUsedBytes{0};
		uint32_t vertexBase{0}; // vertexOffset / sizeof(Vertex)
		uint32_t indexPage{MeshArena::kNoPage};
		uint32_t indexOffset{0}; // bytes within its arena page
		uint32_t indexSlotBytes{0};
		uint32_t indexUsedBytes{0};
		uint32_t indexCount{0}; // live indices (0 = empty section)

		bool empty() const
		{
			return vertexPage == MeshArena::kNoPage && indexPage == MeshArena::kNoPage;
		}
		bool hasVertexRange() const { return vertexPage != MeshArena::kNoPage; }
		bool hasIndexRange() const { return indexPage != MeshArena::kNoPage; }
	};

	/// Prepared-but-unpublished GPU replacement for one chunk (PR #178
	/// review round 4): fresh arena ranges are allocated once and survive
	/// frames inside a commit group until every member's replacement is
	/// resident on the GPU; only then are the published slots swapped
	/// atomically. Staging scratch is frame-bound and never stored here.
	struct GpuReplacement
	{
		bool valid{false};
		bool lod{false};
		MeshBuildResult *result{nullptr}; // attached payload, still Chunk-owned
		uint64_t generation{0};           // chunk identity at prepare time
		uint64_t revision{0};
		bool recorded{false};             // copy commands recorded for submission
		// Sectioned plan (issue #107/#109): future slots are precomputed;
		// the currently published slots stay live until publish.
		struct SectionPlan
		{
			bool touched{false};
			bool active{false};
			uint32_t vertexBytes{0};
			uint32_t indexBytes{0};
			uint32_t vertexBase{0};
			SectionGpuSlot future{};
			MeshArena::Range newV{}, newI{};
		};
		struct StreamPlan
		{
			SectionPlan sections[kOccupancySections]{};
		};
		StreamPlan opaque, water;
		// Whole-chunk LOD plan (issue #109).
		bool needOpaque{false};
		bool needWater{false};
		MeshArena::Range newOV{}, newOI{}, newWV{}, newWI{};
	};

	/// ALLOCATE phase (frame-independent, failure-capable): plan and
	/// allocate every fresh arena range the attached pending result needs.
	/// Fully rolled back on failure; on success the replacement stays
	/// private to the caller - the published mesh is untouched and keeps
	/// rendering.
	bool prepareGPUUpload(MeshArenas &arenas, GpuReplacement &out);
	/// COPY phase (frame-bound): preflight + reserve staging, memcpy and
	/// record the copies into cmd. Returns false when the ring lacks room
	/// this frame - the caller retries next frame; the allocated
	/// replacement is untouched and stays private. Nothing here survives
	/// the frame: after the submit, the fresh ranges hold the payload.
	bool recordGPUUpload(VmaAllocator allocator, StagingRing &staging, VkCommandBuffer cmd,
	                     MeshArenas &arenas, GpuReplacement &replacement);
	/// PUBLISH phase (cannot fail, CPU-only): swap every prepared slot in,
	/// retire the replaced ranges frame-aware, rebuild draw caches and
	/// consume the pending result. Requires a successful prepare (and, for
	/// frame-path uploads, a matching record).
	void publishGPUUpload(GpuResourceRetire &retire, MeshArenas &arenas, GpuReplacement &replacement);
	/// Release a replacement's fresh ranges without publishing (group
	/// rollback, supersession, unload). Ranges whose copies were already
	/// submitted retire frame-aware; never-recorded ranges free
	/// immediately.
	void discardGPUUpload(GpuReplacement &replacement);

	/// Immediate destroy (shutdown / destructor only — not while frames may reference buffers).
	void releaseGPU();
	/// Frame-aware release of every arena range (safe during streaming).
	void releaseGPUDeferred();

	/// Transient cross-chunk block-light context (issue #141 review fix).
	/// Non-owning and main-thread-bound: attached by the RenderWorldBridge at
	/// mesh dispatch (like the neighbor borders), read by computeLightField
	/// on the worker, detached and returned to the pool when the job
	/// finishes. Null (no halo) builds keep the historical in-chunk-only
	/// block light - tests and the LOD path never attach one.
	void setLightHalo(ChunkLightHalo *halo) { m_lightHalo = halo; }
	ChunkLightHalo *lightHalo() const { return m_lightHalo; }

	enum class ResetMode { Full, ForGeneration };
	/// Render-side reset + (standalone or in-place bound) record reset.
	/// ForGeneration retains voxel storage until generateTerrain() overwrites
	/// it; Full releases it. Pool acquire/release only ever see unbound
	/// chunks; a world-bound reset must keep the coordinate (the record is
	/// keyed by it in VoxelWorld::m_chunks — asserted) and is only legitimate
	/// as an explicit recycle-in-place. The world's own unload path captures
	/// edits and resets the record itself, so a Full reset of a LIVE bound
	/// record bypasses that capture and discards its persistent edits.
	void reset(const glm::vec3 &newPosition, ResetMode mode = ResetMode::Full);

	uint32_t getOpaqueIndexCount() const { return opaqueIndexCount; }
	uint32_t getWaterIndexCount() const { return waterIndexCount; }
	uint32_t getCachedOpaqueDrawCount() const { return m_cachedOpaqueDrawCount; }
	uint32_t getCachedWaterDrawCount() const { return m_cachedWaterDrawCount; }
	const IndirectDraw *cachedOpaqueDraws() const { return m_cachedOpaqueDraws.data(); }
	const IndirectDraw *cachedWaterDraws() const { return m_cachedWaterDraws.data(); }

	/// Renderable-cache contract shared by every consuming pass (issue
	/// #177): non-empty cache AND live indices, i.e. a committed GPU mesh
	/// exists. meshNeedsUpdate only marks a pending newer mesh — it must
	/// not hide the committed one: the cached descriptors keep describing
	/// valid, already-uploaded ranges until the replacement commits
	/// (stale-until-replaced), so an edit never blanks the chunk. Chunks
	/// with no committed GPU mesh stay non-renderable because the cache
	/// and index counters remain zero until the first successful upload.
	bool hasRenderableOpaqueDraws() const
	{
		return m_cachedOpaqueDrawCount > 0 && opaqueIndexCount > 0;
	}
	bool hasRenderableWaterDraws() const
	{
		return m_cachedWaterDrawCount > 0 && waterIndexCount > 0;
	}

	size_t getActiveIndex() const { return m_activeIndex; }
	void setActiveIndex(size_t index) { m_activeIndex = index; }

	/// Number of 16^3 sections with a live committed opaque GPU range
	/// (issue #179 chunk inspector). Main-thread read of published slots.
	uint32_t liveGpuSections() const
	{
		uint32_t n = 0;
		for (const SectionGpuSlot &s : m_sectionGpu)
			if (!s.empty())
				++n;
		return n;
	}

	bool isVisible() const { return visible; }
	void setVisible(bool v) { visible = v; } // param renamed: no member shadow (review M21)

private:
	// --- Authoritative record binding (issue #206) ---------------------------
	// World-bound chunks point at the VoxelWorld-owned record; standalone
	// chunks own a private record. m_active is the single dereference target
	// for every voxel access (bound record or owned record). The owned record
	// is allocated LAZILY (recordForWrite) so an idle pool handle — which is
	// always bound before use in the game — carries no per-column backing and
	// does not double-count column.bytes/occupancy.bytes telemetry. m_position
	// holds the construction/reset position until a record exists. The pool
	// pointers survive moves so a recycled handle can re-create its record.
	std::unique_ptr<WorldChunk> m_ownedWorld;
	WorldChunk *m_world{nullptr};
	WorldChunk *m_active{nullptr};
	glm::vec3 m_position{0.0f};
	VoxelPool *m_voxelPool{nullptr};
	BorderPool *m_borderPool{nullptr};

	/// Lazily create the privately owned record at the current fallback
	/// position (standalone use only; never called for a bound handle).
	WorldChunk *ensureOwnedRecord()
	{
		if (!m_ownedWorld)
			m_ownedWorld = std::make_unique<WorldChunk>(m_position, m_voxelPool, m_borderPool);
		return m_ownedWorld.get();
	}
	/// The record a write targets: the bound world record, or the lazily
	/// created owned record for a standalone handle. Keeps m_active cached so
	/// the read hot path stays a single dereference.
	WorldChunk *recordForWrite()
	{
		if (m_world)
			return m_world;
		WorldChunk *record = ensureOwnedRecord();
		m_active = record;
		return record;
	}

	// Occupancy helpers forwarded to the record (meshing + probes).
	bool occupiedSpanY(int &outMinY, int &outMaxY) const
	{
		return m_active && m_active->occupiedSpanY(outMinY, outMaxY);
	}
	void refineOccupiedSpanY(int &occMinY, int &occMaxY) const
	{
		if (m_active)
			m_active->refineOccupiedSpanY(occMinY, occMaxY);
	}
	uint16_t occupiedSectionMask() const
	{
		return m_active ? m_active->occupiedSectionMask() : 0;
	}
	void recountOccupancy()
	{
		if (m_active)
			m_active->recountOccupancy();
	}

	bool visible{false};

	VmaAllocator m_allocator{VK_NULL_HANDLE};

	// Shared mesh arena ranges (issue #109): opaque/water geometry lives in
	// the WorldRenderer-owned MeshArenas instead of per-chunk Vulkan
	// buffers. The LOD whole-chunk mesh owns four ranges; the sectioned
	// full-quality mesh owns one range pair per 16^3 section below.
	// VmaAllocator/m_allocator stays only as the bootstrap-path allocator
	// handle for ImmediateCommands uploads.

	// Section ranges inside the shared mesh arenas (issue #107/#109).
	// Published ranges are immutable: any rebuilt non-empty section
	// allocates fresh vertex/index ranges, uploads into them, atomically
	// publishes the replacement slot, then retires the previous ranges
	// frame-aware. A rebuilt empty section retires its old ranges and
	// becomes slotless - no stale range is ever referenced. Index ranges
	// need no gap-zeroing here: indirect draws reference live ranges only.
	// (SectionGpuSlot itself is public: it appears in PreparedMeshUpload.)
	std::array<SectionGpuSlot, kOccupancySections> m_sectionGpu{};
	std::array<SectionGpuSlot, kOccupancySections> m_sectionGpuWater{};

	// Whole-chunk LOD ranges (issue #109): the simplified LOD mesh lives in
	// the same shared arenas as the sectioned meshes.
	MeshArena::Range m_lodOpaqueVertices{};
	MeshArena::Range m_lodOpaqueIndices{};
	MeshArena::Range m_lodWaterVertices{};
	MeshArena::Range m_lodWaterIndices{};

	// Per-chunk indirect draw cache (issue #109 / issue #122): rebuilt only
	// when GPU mesh state changes (upload commit paths, LOD transitions,
	// reset/release). Eliminates per-pass per-frame reconstruction of draw
	// descriptors across OpaquePass, ShadowPass (3 cascades), and WaterPass.
	void rebuildIndirectDrawCache();
	uint32_t m_cachedOpaqueDrawCount{0};
	uint32_t m_cachedWaterDrawCount{0};
	std::array<IndirectDraw, kOccupancySections> m_cachedOpaqueDraws{};
	std::array<IndirectDraw, kOccupancySections> m_cachedWaterDraws{};

	// Section dirtying for one voxel edit (issue #107): own section,
	// Y-boundary neighbors, a conservative light-dirty Y range for in-chunk
	// edits, and exactly the y/16 section for border (mirror) writes.
	// Render scheduling: consumes the authoritative write's previous type.
	void markEditDirtySections(int x, int y, int z, TextureType type,
							   TextureType previousType, bool borderWrite);
	// Chunk-wide sky/block light field for one mesh job (extracted from the
	// former monolithic buildMeshRanged so section-selective builds pay it
	// exactly once).
	void computeLightField(telemetry::MeshSample &meshSample);
	void populateLightStorage(MeshBuildResult &out);
	// Pack the thread-local light-field scratch into a storage block: the
	// shared body of populateLightStorage (mesh path) and buildLightCache
	// (light-only path, issue #172), so both produce byte-identical caches.
	void packLightField(ChunkLightStorage &out) const;
	// Greedy meshing of ONE vertical section into out.sections[section]
	// (issue #107): faces owned by voxels in [ownerMinY, ownerMaxY] only,
	// with full one-voxel chunk/border context for faces, AO and light.
	void buildSectionGreedy(MeshBuildResult &out, int section, int ownerMinY,
							int ownerMaxY, telemetry::MeshSample &meshSample);
	// Shared upload logic: (re)place the built sections' ranges in the
	// shared arenas. Returns false when staging space is missing or an
	// arena cannot back a required range (async path retries later).
	bool uploadSectionSlots(MeshBuildResult &result, VmaAllocator allocator,
							StagingRing *staging, VkCommandBuffer cmd,
							GpuResourceRetire *retire, ImmediateCommands *imm,
							MeshArenas &arenas);
	// ALLOCATE / COPY / PUBLISH / discard phases of the two upload paths
	// (see GpuReplacement): ALLOCATE plans and reserves fresh arena ranges
	// with full rollback; COPY reserves staging, memcpys and records the
	// frame's copies; PUBLISH swaps slots, retires replaced ranges and
	// rebuilds draw caches without any failure path.
	bool prepareLodReplacement(MeshBuildResult &result, MeshArenas &arenas,
	                           GpuReplacement &out);
	bool prepareSectionReplacement(MeshBuildResult &result, MeshArenas &arenas,
	                               GpuReplacement &out);
	bool recordLodReplacement(VmaAllocator allocator, StagingRing &staging, VkCommandBuffer cmd,
	                          MeshArenas &arenas, GpuReplacement &replacement);
	bool recordSectionUpload(VmaAllocator allocator, StagingRing *staging, VkCommandBuffer cmd,
	                         ImmediateCommands *imm, MeshArenas &arenas,
	                         GpuReplacement &replacement);
	void publishLodReplacement(MeshArenas &arenas, GpuReplacement &replacement);
	void publishSectionUpload(GpuResourceRetire *retire, MeshArenas &arenas,
	                          GpuReplacement &replacement);
	void discardLodReplacement(MeshArenas &arenas, GpuReplacement &replacement);
	void discardSectionReplacement(MeshArenas &arenas, GpuReplacement &replacement);
	// Retire every range described by a section slot table and reset the
	// slots (issue #109 review: single retirement path for full->LOD and
	// unload transitions). immediate=true for bootstrap/shutdown paths.
	void retireSectionSlots(std::array<SectionGpuSlot, kOccupancySections> &slots,
							MeshArena &vertexArena, MeshArena &indexArena, bool immediate);
	// Retire LOD stream ranges and reset them to empty.
	void retireLodRanges(MeshArena &vertexArena, MeshArena &indexArena,
						 MeshArena::Range &vertices, MeshArena::Range &indices, bool immediate);
	// Borrowed from the RenderWorldBridge's LightHaloPool for one mesh job
	// (issue #141 review fix): neighbor ring voxels feeding the cross-chunk
	// block-light BFS. Non-owning; null outside a job.
	ChunkLightHalo *m_lightHalo{nullptr};
	// Mesh build buffers are pooled too (issue #104): m_pendingResult holds
	// a completed CPU mesh awaiting upload, borrowed from m_resultPool.
	// Released on upload, reset, and moves - no per-chunk mesh capacity.
	MeshResultPool *m_resultPool{nullptr};
	MeshBuildResult *m_pendingResult{nullptr};
	ChunkLightStorage *m_lightStorage{nullptr};
	ChunkLightPool *m_lightPool{nullptr};
	// Non-owning handle to the WorldRenderer-owned shared arenas (issue
	// #109): set by every upload, used to retire ranges on release/unload.
	MeshArenas *m_arenas{nullptr};
	// Section dirty mask (issue #107): bit s set => section s needs a mesh
	// rebuild. Mutated from the main thread only (edits, dispatch, publish
	// rejection); atomic because dispatch-time captures observe it around
	// worker tasks.
	std::atomic<uint16_t> m_dirtySections{0};
	void releasePendingMeshResult();

	uint32_t opaqueIndexCount{0};
	uint32_t waterIndexCount{0};

	std::atomic<bool> meshNeedsUpdate{true};
	bool m_isLODMesh{false};
	std::atomic<bool> m_localLightCacheWanted{false};

	// Hot inner-loop arithmetic (meshing/lighting/editing all call it);
	// inline so the implementation split (issue #181) keeps it inlinable
	// in every consumer TU. Delegates to the canonical record layout.
	size_t getIndex(uint32_t x, uint32_t y, uint32_t z) const
	{
		return WorldChunk::index(x, y, z);
	}

private:
	size_t m_activeIndex{SIZE_MAX};

	// Testing hook (issue #78 review): lets the chunk lifecycle test verify
	// full move semantics - including per-column generation state - without
	// exposing that state in the public API.
	friend struct ChunkStateProbe;
	// Testing hook (issue #207 review): lets the render-bridge boundary test
	// assert the pending mesh payload actually carries geometry (sectionsBuilt),
	// not merely that a result was published.
	friend struct RenderBridgeProbe;
};

inline size_t Chunk::collectOpaqueDraws(std::vector<IndirectDraw> &out) const
{
	if (!hasRenderableOpaqueDraws())
		return 0;
	out.insert(out.end(), m_cachedOpaqueDraws.data(),
	           m_cachedOpaqueDraws.data() + m_cachedOpaqueDrawCount);
	return m_cachedOpaqueDrawCount;
}

inline size_t Chunk::collectWaterDraws(std::vector<IndirectDraw> &out) const
{
	if (!hasRenderableWaterDraws())
		return 0;
	out.insert(out.end(), m_cachedWaterDraws.data(),
	           m_cachedWaterDraws.data() + m_cachedWaterDrawCount);
	return m_cachedWaterDrawCount;
}
