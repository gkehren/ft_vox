#pragma once

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif

#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <glm/gtx/hash.hpp>

#include <chrono>

#include <Chunk/TerrainGenerator.hpp>
#include <Chunk/VoxelPool.hpp>
#include <Chunk/ChunkBorders.hpp>
#include <Chunk/ChunkLightHalo.hpp>
#include <Chunk/ChunkLightPool.hpp>
#include <Vulkan/VkBuffer.hpp>
#include <Vulkan/MeshArena.hpp>
#include <Camera/Camera.hpp>
#include <utils.hpp>
#include <Engine/EngineDefs.hpp>
#include <Engine/WorkloadTelemetry.hpp>

// TextureManager only for static isTransparent — no GL dependency in mesh gen.
#include <Renderer/TextureManager.hpp>

class ImmediateCommands;
class StagingRing;
class GpuResourceRetire;
struct MeshBuildResult;
class MeshResultPool;

class Chunk
{
public:
	Chunk(const glm::vec3 &position, ChunkState state = ChunkState::UNLOADED,
		  VoxelPool *voxelPool = nullptr, BorderPool *borderPool = nullptr,
		  MeshResultPool *meshPool = nullptr, ChunkLightPool *lightPool = nullptr);
	Chunk(Chunk &&other) noexcept;
	Chunk &operator=(Chunk &&other) noexcept;
	~Chunk();

	const glm::vec3 &getPosition() const;
	bool isVisible() const;
	void setVisible(bool visible);
	void setState(ChunkState state);
	ChunkState getState() const;

	const Voxel &getVoxel(uint32_t x, uint32_t y, uint32_t z) const;
	bool isVoxelActive(int x, int y, int z) const;
	void setVoxel(int x, int y, int z, TextureType type);

	bool deleteVoxel(const glm::vec3 &position);
	bool placeVoxel(const glm::vec3 &position, TextureType type);

	bool hasVoxelStorage() const { return m_storage != nullptr; }
	VoxelPool *getVoxelPool() const { return m_voxelPool; }
	bool hasBorderStorage() const { return m_borders != nullptr; }
	BorderPool *getBorderPool() const { return m_borderPool; }
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

	/// Prepare all generation backing on the calling thread:
	/// - voxel storage
	/// - transient neighbor borders
	/// Ownership only: a freshly acquired voxel block is NOT cleared - its
	/// bytes stay stale/unspecified until generateTerrain() initializes
	/// them. Returns false if an allocation fails (e.g. std::bad_alloc); any
	/// partially acquired backing is returned to its pool first.
	///
	/// Occupancy lifecycle contract (issue #115 review), the three states a
	/// chunk moves through:
	///   1. No storage              - m_storage == nullptr, occupancy
	///                                metadata == 0.
	///   2. Prepared for generation - m_storage != nullptr, backing contents
	///                                stale/unspecified, occupancy metadata
	///                                == 0. No consumer may inspect voxel
	///                                contents in this state (edits targeting
	///                                the chunk are deferred while it is in
	///                                transit, and answer "logically empty").
	///   3. Generated/editable      - backing valid, occupancy metadata
	///                                exactly matches the backing; edits
	///                                keep them in lockstep.
	bool prepareVoxelStorageForGeneration();

	/// Release voxel storage upon chunk retirement.
	void releaseVoxelStorageOnRetire();

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

	void generateTerrain(TerrainGenerator &generator);

	// Mesh building (issue #104): the CPU mesh payload no longer lives on
	// the Chunk. Workers build into a pooled MeshBuildResult and publish it
	// for upload; the Chunk keeps only GPU handles/counts.
	//
	// Worker contract (issue #114 review): buildMesh/buildLODMesh may READ
	// Chunk generation data (voxels, borders, biome metadata, position) but
	// must not mutate any Chunk lifecycle/render state. The completed result
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
	uint64_t meshGeneration() const { return m_meshGeneration; }
	uint64_t meshRevision() const { return m_meshRevision.load(std::memory_order_relaxed); }
	MeshResultPool *getMeshResultPool() const { return m_resultPool; }

	// Section dirty tracking (issue #107): bit s set => section s needs a
	// mesh rebuild. Edits mark the affected sections (own + Y-boundary
	// neighbors + a conservative light range); mesh dispatch takes and
	// clears the mask, and a rejected publish re-arms it.
	void markSectionsDirty(uint16_t mask) { m_dirtySections.fetch_or(mask, std::memory_order_relaxed); }
	uint16_t takeDirtySections() { return m_dirtySections.exchange(0, std::memory_order_relaxed); }
	uint16_t dirtySections() const { return m_dirtySections.load(std::memory_order_relaxed); }

	// Vertical occupancy granularity (issue #105): 16 sections of 16 voxels.
	static constexpr int kOccupancySectionSize = 16;
	static constexpr int kOccupancySections = CHUNK_HEIGHT / kOccupancySectionSize;
	static_assert(CHUNK_HEIGHT % kOccupancySectionSize == 0,
				  "occupancy sections must tile the chunk height exactly");
	static_assert(kOccupancySections <= 16,
				  "the derived occupied-section mask must fit in uint16_t");
	static_assert(static_cast<long>(kOccupancySectionSize) * CHUNK_SIZE * CHUNK_SIZE <=
					  std::numeric_limits<uint16_t>::max(),
				  "a per-section non-air count must fit uint16_t");
	bool hasWaterMesh() const { return waterIndexCount > 0; }
	bool isLODMesh() const { return m_isLODMesh; }
	bool needsGPUUpload() const { return meshNeedsUpdate.load(); }
	bool isInTransit() const { return m_inTransit.load(); }
	void setInTransit(bool val) { m_inTransit.store(val); }

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

	/// Prepared-but-uncommitted GPU upload transaction (PR #178 review):
	/// every failure-capable step of an upload (staging reservation, arena
	/// allocation) runs in prepareGPUUpload() and is fully rolled back on
	/// failure; commitGPUUpload() then applies the plan with no failure
	/// path. A geometric commit group prepares ALL its members before
	/// committing ANY of them, so a staging shortfall on one chunk can
	/// never publish a replacement whose border-coupled neighbor stays
	/// stale.
	struct PreparedMeshUpload
	{
		MeshBuildResult *result{nullptr}; // attached payload, still Chunk-owned
		StagingRing *staging{nullptr};    // frame path (null on the imm path)
		bool lod{false};
		// Whole-chunk LOD plan (issue #109).
		bool needOpaque{false};
		bool needWater{false};
		MeshArena::Range newOV{}, newOI{}, newWV{}, newWI{};
		VkDeviceSize stageVOff{0}, stageIOff{0}, stageWVOff{0}, stageWIOff{0};
		void *stageVPtr{nullptr}, *stageIPtr{nullptr}, *stageWVPtr{nullptr}, *stageWIPtr{nullptr};
		// Sectioned plan (issue #107/#109 section transaction).
		struct SectionPlan
		{
			bool touched{false}; // present in sectionsBuilt
			bool active{false};  // payload is non-empty
			uint32_t vertexBytes{0};
			uint32_t indexBytes{0};
			SectionGpuSlot old{}; // the slot as currently published
			MeshArena::Range newV{};
			MeshArena::Range newI{};
			uint32_t vertexBase{0};
			VkDeviceSize stageVOff{0};
			void *stageVPtr{nullptr};
			VkDeviceSize stageIOff{0};
			void *stageIPtr{nullptr};
		};
		struct StreamPlan
		{
			SectionPlan sections[kOccupancySections]{};
		};
		StreamPlan opaque, water;
	};

	/// Failure-capable upload phase: staging reservation + fresh arena
	/// allocation for the attached pending result. Fully rolled back on
	/// failure - every published slot, range and draw count stays exactly
	/// as published and the result stays attached. On success only
	/// commitGPUUpload() remains.
	bool prepareGPUUpload(VmaAllocator allocator, StagingRing &staging, VkCommandBuffer cmd,
	                      GpuResourceRetire &retire, MeshArenas &arenas, PreparedMeshUpload &out);
	/// Cannot-fail commit phase: record the staging copies, swap the
	/// prepared plan in, retire replaced ranges frame-aware and consume the
	/// pending result. Requires a successful prepareGPUUpload() with the
	/// same arguments; the prepared plan is consumed.
	void commitGPUUpload(VkCommandBuffer cmd, GpuResourceRetire &retire, PreparedMeshUpload &prepared);
	/// Release the fresh arena ranges of a prepared-but-uncommitted plan
	/// (group rollback: another member failed its prepare). No copy command
	/// references these ranges until commit runs, so immediate free is safe
	/// and the published mesh is untouched.
	void rollbackGPUUpload(PreparedMeshUpload &prepared);

	/// Immediate destroy (shutdown / destructor only — not while frames may reference buffers).
	void releaseGPU();
	/// Frame-aware release of every arena range (safe during streaming).
	void releaseGPUDeferred();

	bool isShellEmpty() const { return m_borders == nullptr; }
	/// Transient cross-chunk block-light context (issue #141 review fix).
	/// Non-owning and main-thread-bound: attached by the ChunkManager at
	/// mesh dispatch (like the neighbor borders), read by computeLightField
	/// on the worker, detached and returned to the pool when the job
	/// finishes. Null (no halo) builds keep the historical in-chunk-only
	/// block light - tests and the LOD path never attach one.
	void setLightHalo(ChunkLightHalo *halo) { m_lightHalo = halo; }
	ChunkLightHalo *lightHalo() const { return m_lightHalo; }
	/// Occupancy lifecycle (issue #115): the voxel backing holds stale pool
	/// bytes until terrain generation initializes it. True only in the
	/// generated/editable states (GENERATED or MESHED) - the gate every
	/// direct voxel read/write must pass.
	bool isVoxelBackingReadable() const
	{
		return state.load() != ChunkState::UNLOADED;
	}

	/// Layout-independent border sampling for meshing (issue #103).
	/// Accepts the full padded range used by the greedy mesher: in-chunk
	/// coordinates read local voxels, x/z = -1 / CHUNK_SIZE read neighbor
	/// faces, diagonal coordinates read corner columns, and everything else
	/// (including vertical padding) reads as AIR. Missing borders - freed
	/// after upload or never built - also read as AIR.
	TextureType sampleForMeshing(int x, int y, int z) const;
	void releaseNeighborBorders();
	void rebuildBordersFromNeighbors(const Chunk *west, const Chunk *east,
								   const Chunk *south, const Chunk *north);

	enum class ResetMode { Full, ForGeneration };
	/// ForGeneration retains voxel storage (if any) until generateTerrain()
	/// overwrites it, avoiding deallocation/reallocation during pool recycling.
	/// Do not read/mesh voxels in this chunk before generation completes.
	/// Full (the default) releases voxel storage back to the pool, for retirement without regeneration.
	/// Both modes reset the occupancy metadata to 0: after a ForGeneration
	/// reset the chunk sits in the "prepared for generation" state of the
	/// prepareVoxelStorageForGeneration() lifecycle contract (backing may be
	/// stale, metadata empty, contents unreadable).
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

private:
    // Published by the exclusive chunk owner at mutation boundaries.
    std::array<uint64_t, 13> m_cpuTelemetry{};
    void publishCpuTelemetry();
    struct MemoryPublication {
        Chunk& chunk;
        ~MemoryPublication() { chunk.publishCpuTelemetry(); }
    };
	glm::vec3 position;
	bool visible;
	std::atomic<ChunkState> state;

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
	// Live byte extent of the sectioned layout is gone (issue #109): the
	// arenas own their pages; per-section ranges above are the only state.

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

	VoxelPool *m_voxelPool{nullptr};
	VoxelStorage *m_storage{nullptr};
	void ensureVoxelStorageForEdit();
	// Compact occupancy metadata (issue #105). The raw voxel type is the
	// canonical per-cell occupancy source (`type != AIR`); this one array
	// replaces the former 8 KiB activeVoxels bitset duplicate. It counts
	// non-air voxels per vertical section (16 sections of 16 voxels), so
	// generation/meshing can skip empty slabs without touching the whole
	// volume. Maintained by generateTerrain() (one canonical recount) and
	// setVoxel() (incremental delta); verified against brute force by the
	// chunk lifecycle tests.
	std::array<uint16_t, kOccupancySections> m_sectionNonAir{};
	void recountOccupancy();
	/// Derived Y span [outMinY, outMaxY] covering every section that holds
	/// at least one non-air voxel (section-granular). Returns false when the
	/// chunk holds no voxels at all.
	bool occupiedSpanY(int &outMinY, int &outMaxY) const;
	/// Narrow a section-granular span to the first/last layers that hold a
	/// non-air voxel (at most the two boundary sections are scanned).
	void refineOccupiedSpanY(int &occMinY, int &occMaxY) const;
	/// Debug-only sanity check of the counters (no silent desync can hide
	/// behind an out-of-range count); compiled out in Release.
	void validateOccupancyMetadata() const;
	/// Bit S set <=> section S holds at least one non-air voxel (derived).
	uint16_t occupiedSectionMask() const;
	// Section dirtying for one voxel edit (issue #107): own section,
	// Y-boundary neighbors, a conservative light-dirty Y range for in-chunk
	// edits, and exactly the y/16 section for border (mirror) writes.
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
	// Failure-capable phases of the two upload paths (see
	// PreparedMeshUpload): staging reservation + fresh arena allocation
	// with full rollback; then the cannot-fail slot swap / copy recording /
	// retirement.
	bool prepareLodUpload(MeshBuildResult &result, StagingRing &staging, MeshArenas &arenas,
	                      PreparedMeshUpload &out);
	void commitLodUpload(VkCommandBuffer cmd, PreparedMeshUpload &prepared);
	bool prepareSectionUpload(MeshBuildResult &result, StagingRing *staging, MeshArenas &arenas,
	                          PreparedMeshUpload &out);
	void commitSectionUpload(VmaAllocator allocator, VkCommandBuffer cmd, GpuResourceRetire *retire,
	                         ImmediateCommands *imm, MeshArenas &arenas, PreparedMeshUpload &prepared);
	// Retire every range described by a section slot table and reset the
	// slots (issue #109 review: single retirement path for full->LOD and
	// unload transitions). immediate=true for bootstrap/shutdown paths.
	void retireSectionSlots(std::array<SectionGpuSlot, kOccupancySections> &slots,
							MeshArena &vertexArena, MeshArena &indexArena, bool immediate);
	// Retire LOD stream ranges and reset them to empty.
	void retireLodRanges(MeshArena &vertexArena, MeshArena &indexArena,
						 MeshArena::Range &vertices, MeshArena::Range &indices, bool immediate);
	// Borrowed from a BorderPool for generation/meshing and returned after
	// upload (issue #103): no per-chunk border memory is retained.
	ChunkNeighborBorders *m_borders{nullptr};
	BorderPool *m_borderPool{nullptr};
	// Borrowed from the ChunkManager's LightHaloPool for one mesh job
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
	// Identity of the meshable content (issue #114 review):
	//   m_meshGeneration - physical chunk incarnation; bumped on reset so a
	//     result built for a recycled chunk pointer can never publish.
	//   m_meshRevision   - logical voxel/border content revision; bumped on
	//     every content invalidation (generation, successful voxel/border
	//     edit, border rebuild) so an in-flight result built from older
	//     content is rejected at publish instead of overwriting newer data.
	// A build result must match BOTH to be published. Mutated only on the
	// main thread; atomic because dispatch-time captures observe it around
	// worker tasks.
	uint64_t m_meshGeneration{0};
	std::atomic<uint64_t> m_meshRevision{0};
	// Section dirty mask (issue #107): bit s set => section s needs a mesh
	// rebuild. Mutated from the main thread only (edits, dispatch, publish
	// rejection); atomic because dispatch-time captures observe it around
	// worker tasks, same rule as m_meshRevision.
	std::atomic<uint16_t> m_dirtySections{0};
	void releasePendingMeshResult();

	std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> biomeGrassColors{};
	std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> biomeFoliageColors{};

	// Per-column generation state, filled by generateTerrain() (biomes feed
	// vegetation/border passes; heightMap mirrors ChunkData::heightMap).
	// Fully reset on every generation and pool recycle.
	std::array<BiomeType, CHUNK_SIZE * CHUNK_SIZE> biomeTypes{};
	std::array<int, CHUNK_SIZE * CHUNK_SIZE> heightMap{};

	uint32_t opaqueIndexCount{0};
	uint32_t waterIndexCount{0};

	std::atomic<bool> meshNeedsUpdate;
	bool m_isLODMesh{false};
	std::atomic<bool> m_inTransit{false};
	std::atomic<bool> m_localLightCacheWanted{false};

	size_t getIndex(uint32_t x, uint32_t y, uint32_t z) const;

private:
	size_t m_activeIndex{SIZE_MAX};

	// Testing hook (issue #78 review): lets the chunk lifecycle test verify
	// full move semantics - including per-column generation state - without
	// exposing that state in the public API.
	friend struct ChunkStateProbe;
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
