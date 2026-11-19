#pragma once

// =============================================================================
// WorldChunk — the AUTHORITATIVE, CPU-only chunk record (issue #206, Epic #204
// Phase B). Owns everything gameplay and persistence need from a chunk:
//
//   * the voxel backing (pooled VoxelStorage) and its occupancy metadata,
//   * the transient neighbor-border shell used by generation and edit mirrors,
//   * the sparse persistent-edit map (issue #180 capture pipeline),
//   * the chunk identity: generation (physical incarnation, bumped on recycle)
//     and revision (logical content, bumped on every content change), exposed
//     as the sim::ChunkIdentity value contract — the SINGLE source of the pair
//     (the MeshBuildResult copy was removed in this phase),
//   * the lifecycle state (UNLOADED / GENERATED / MESHED), terrain publication
//     state and worker in-transit ownership flag,
//   * the per-column generation outputs (biomes, height map, tint colors).
//
// The render-side `Chunk` (Chunk/Chunk.hpp) binds to a WorldChunk and keeps
// only DERIVED state: GPU ranges, draw caches, mesh payloads, light caches,
// visibility and mesh scheduling. It forwards every voxel read/write here.
// A standalone Chunk (tests / tools) owns a private WorldChunk instead.
//
// Dependency rule (enforced by tests/check_core_includes.py): this header is
// part of the ft_vox_core CPU-only closure — glm + std + utils + CPU-only
// Chunk/ headers + Core contracts only. NEVER Renderer/, Vulkan/, Camera/,
// Engine/, SDL3 or ImGui.
//
// Threading contract (unchanged from the pre-split Chunk):
//   * main thread owns lifecycle transitions, edits and persistence capture;
//   * one worker at a time owns the backing while inTransit is set (terrain
//     generation writes; mesh/light jobs read);
//   * state / revision / inTransit are atomic because dispatch-time captures
//     and worker publication observe them across threads.
// =============================================================================

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>

#include <glm/glm.hpp>

#include <Chunk/ChunkBorders.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Chunk/VoxelPool.hpp>
#include <Core/SimContracts.hpp>
#include <Core/WorkloadTelemetry.hpp>
#include <utils.hpp>

class WorldChunk
{
public:
	WorldChunk(const glm::vec3 &position, VoxelPool *voxelPool, BorderPool *borderPool);
	~WorldChunk();

	WorldChunk(const WorldChunk &) = delete;
	WorldChunk &operator=(const WorldChunk &) = delete;
	WorldChunk(WorldChunk &&) = delete;
	WorldChunk &operator=(WorldChunk &&) = delete;

	// --- Identity (issue #206: single source of the generation/revision pair)
	// generation -> physical incarnation; bumped on reset so a job/edit stamped
	//               for a recycled record can never publish into it.
	// revision   -> logical voxel/border content; bumped on every content
	//               change (generation, edit, border rebuild) so in-flight
	//               builds from older content are rejected at publish.
	uint64_t generation() const { return m_generation; }
	uint64_t revision() const { return m_revision.load(std::memory_order_relaxed); }
	sim::ChunkIdentity identity() const
	{
		return {coord().x, coord().z, m_generation, revision()};
	}

	// --- Lifecycle -----------------------------------------------------------
	const glm::vec3 &getPosition() const { return position; }
	/// Canonical world->chunk mapping of this record's origin (floor division).
	glm::ivec3 coord() const;
	ChunkState getState() const { return state.load(std::memory_order_relaxed); }
	void setState(ChunkState newState) { state.store(newState, std::memory_order_relaxed); }
	/// Occupancy lifecycle gate (issue #115): the voxel backing holds stale
	/// pool bytes until terrain generation initializes it. True only once
	/// generated — the gate every direct voxel read/write must pass.
	bool isVoxelBackingReadable() const { return getState() != ChunkState::UNLOADED; }
	bool isInTransit() const { return m_inTransit.load(std::memory_order_relaxed); }
	void setInTransit(bool val) { m_inTransit.store(val, std::memory_order_relaxed); }

	// --- Voxel storage -------------------------------------------------------
	bool hasVoxelStorage() const { return m_storage != nullptr; }
	VoxelPool *getVoxelPool() const { return m_voxelPool; }
	VoxelStorage *voxelStorage() const { return m_storage; }

	/// Prepare all generation backing on the calling thread (voxel storage +
	/// transient neighbor borders). Ownership only: a freshly acquired voxel
	/// block is NOT cleared — its bytes stay stale until generateTerrain()
	/// initializes them (issue #115 lifecycle contract). Returns false on
	/// allocation failure; partially acquired backing is returned first.
	bool prepareVoxelStorageForGeneration();
	/// Release voxel storage upon chunk retirement.
	void releaseVoxelStorageOnRetire();

	/// Strict internal accessor: out-of-range coordinates would index past
	/// the voxel storage. Callers that can see outside the chunk must use
	/// sampleForMeshing()/isVoxelActive(), which answer AIR/false for the
	/// padded neighborhood. Inline so the hot meshing/lighting/editing paths
	/// keep it inlined in every consumer TU.
	const Voxel &getVoxel(uint32_t x, uint32_t y, uint32_t z) const
	{
		assert(x < CHUNK_SIZE && y < CHUNK_HEIGHT && z < CHUNK_SIZE);
		if (!m_storage)
		{
			static constexpr Voxel s_air{static_cast<uint8_t>(AIR)};
			return s_air;
		}
		return m_storage->voxels[index(x, y, z)];
	}
	bool isVoxelActive(int x, int y, int z) const;
	/// Layout-independent border sampling for meshing (issue #103): in-chunk
	/// coordinates read local voxels, x/z = -1 / CHUNK_SIZE read neighbor
	/// faces, diagonals read corner columns, everything else reads AIR.
	TextureType sampleForMeshing(int x, int y, int z) const;

	/// Authoritative voxel write: storage + occupancy + persistent-edit
	/// capture + content revision. Returns the PREVIOUS block type (the
	/// render adapter needs it for light/section invalidation). Does NOT
	/// touch any render scheduling state — the derived side consumes the
	/// world's edit event stream for that (issue #206 boundary).
	/// Accepts the border range x/z in [-1, CHUNK_SIZE] for mirror writes.
	TextureType setVoxel(int x, int y, int z, TextureType type);

	// --- Persistent-edit tracking (issue #180) -------------------------------
	// While armed, every in-chunk setVoxel() write that actually changes the
	// stored voxel records the canonical local index (y*256 + z*16 + x) and
	// the new block type. Border-shell writes and no-op rewrites never record.
	// The map holds the CURRENT value per edited voxel (not an operation log).
	using ChunkEditMap = std::unordered_map<uint32_t, uint8_t>;
	/// Disarming also discards any recorded edits.
	void setTrackPersistentEdits(bool on)
	{
		m_trackPersistentEdits = on;
		if (!on)
			m_persistentEdits.clear();
	}
	bool hasPersistentEdits() const { return !m_persistentEdits.empty(); }
	/// Whether persistent-edit tracking is armed. For a world-resident record
	/// this is exactly "a world is open" (VoxelWorld arms every resident record
	/// on openWorld and disarms on closeWorld); used by the render handle's
	/// bound-reset guard to refuse a Full reset that would discard save state.
	bool tracksPersistentEdits() const { return m_trackPersistentEdits; }
	const ChunkEditMap &persistentEdits() const { return m_persistentEdits; }
	/// Moves the recorded edits out and leaves the record with an empty map.
	ChunkEditMap takePersistentEdits()
	{
		ChunkEditMap out = std::move(m_persistentEdits);
		m_persistentEdits.clear();
		return out;
	}
	void clearPersistentEdits() { m_persistentEdits.clear(); }

	// --- Border shell ---------------------------------------------------------
	bool isShellEmpty() const { return m_borders == nullptr; }
	bool hasBorderStorage() const { return m_borders != nullptr; }
	BorderPool *getBorderPool() const { return m_borderPool; }
	ChunkNeighborBorders *borders() const { return m_borders; }
	void releaseNeighborBorders();
	void rebuildBordersFromNeighbors(const WorldChunk *west, const WorldChunk *east,
									 const WorldChunk *south, const WorldChunk *north);

	// --- Occupancy metadata (issue #105) --------------------------------------
	// One counter per vertical 16-voxel section; maintained by generateTerrain
	// (canonical recount) and setVoxel (incremental delta).
	static constexpr int kOccupancySectionSize = 16;
	static constexpr int kOccupancySections = CHUNK_HEIGHT / kOccupancySectionSize;
	static_assert(CHUNK_HEIGHT % kOccupancySectionSize == 0,
				  "occupancy sections must tile the chunk height exactly");
	static_assert(kOccupancySections <= 16,
				  "the derived occupied-section mask must fit in uint16_t");
	static_assert(static_cast<long>(kOccupancySectionSize) * CHUNK_SIZE * CHUNK_SIZE <=
					  std::numeric_limits<uint16_t>::max(),
				  "a per-section non-air count must fit uint16_t");
	const std::array<uint16_t, kOccupancySections> &sectionNonAir() const { return m_sectionNonAir; }
	void recountOccupancy();
	/// Derived Y span covering every section holding at least one non-air
	/// voxel (section-granular). False when the chunk holds no voxels at all.
	bool occupiedSpanY(int &outMinY, int &outMaxY) const;
	/// Narrow a section-granular span to the first/last layers that hold a
	/// non-air voxel (at most the two boundary sections are scanned).
	void refineOccupiedSpanY(int &occMinY, int &occMaxY) const;
	/// Debug-only sanity check of the counters; compiled out in Release.
	void validateOccupancyMetadata() const;
	/// Bit S set <=> section S holds at least one non-air voxel (derived).
	uint16_t occupiedSectionMask() const;

	// --- Generation -----------------------------------------------------------
	/// Deterministic terrain generation into the prepared backing. Returns
	/// false (no-op) when the record is not UNLOADED or backing is missing.
	/// On success: state = GENERATED, revision bumped, occupancy recounted.
	/// Render scheduling flags are NOT set here — the owner publishes them.
	bool generateTerrain(TerrainGenerator &generator);

	// --- Reset / recycling ------------------------------------------------------
	enum class ResetMode { Full, ForGeneration };
	/// ForGeneration retains voxel storage (if any) until generateTerrain()
	/// overwrites it. Full releases it back to the pool. Both modes bump the
	/// incarnation generation, clear the persistent-edit map and reset the
	/// occupancy metadata + per-column generation state.
	void reset(const glm::vec3 &newPosition, ResetMode mode = ResetMode::Full);

	// --- Per-column generation state (filled by generateTerrain) --------------
	// Public data members: the record is a plain CPU data carrier; the render
	// mesher and the world's generation path read/write these directly.
	std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> biomeGrassColors{};
	std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> biomeFoliageColors{};
	std::array<BiomeType, CHUNK_SIZE * CHUNK_SIZE> biomeTypes{};
	std::array<int, CHUNK_SIZE * CHUNK_SIZE> heightMap{};

	/// Canonical flat voxel index (y-major). Shared by every consumer so the
	/// packed layout has exactly one definition.
	static size_t index(uint32_t x, uint32_t y, uint32_t z)
	{
		return y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x;
	}

private:
	friend class VoxelWorld;
	void ensureVoxelStorageForEdit();
	void publishCpuTelemetry();

	glm::vec3 position;
	std::atomic<ChunkState> state{ChunkState::UNLOADED};
	std::atomic<bool> m_inTransit{false};
	// Owner-published after terrain and overrides are complete. World-resident
	// reads and writes are protected by the world mutex. Workers never set it.
	bool m_terrainPublished{false};

	VoxelPool *m_voxelPool{nullptr};
	VoxelStorage *m_storage{nullptr};
	BorderPool *m_borderPool{nullptr};
	ChunkNeighborBorders *m_borders{nullptr};

	std::array<uint16_t, kOccupancySections> m_sectionNonAir{};

	// Identity pair (see generation()/revision()/identity() above). Mutated
	// only by the owner (main thread) or the generation worker while the
	// record is in transit; atomic revision because dispatch-time captures
	// observe it around worker tasks.
	uint64_t m_generation{0};
	std::atomic<uint64_t> m_revision{0};
	// Owner-only voxel identity. Unlike the render revision, this counts
	// logical target edits, including ones waiting on backing publication.
	uint64_t m_contentGeneration{0}, m_contentRevision{0};

	ChunkEditMap m_persistentEdits;
	bool m_trackPersistentEdits{false};

	// Telemetry publication state (memory gauges; same slots the pre-split
	// Chunk published — voxel/shell/column/occupancy bytes).
	std::array<uint64_t, 13> m_cpuTelemetry{};
	struct MemoryPublication
	{
		WorldChunk &chunk;
		~MemoryPublication() { chunk.publishCpuTelemetry(); }
	};
};
