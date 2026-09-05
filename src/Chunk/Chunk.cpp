#include <Engine/WorkloadTelemetry.hpp>
#include "Chunk.hpp"
#include <Chunk/ChunkMeshResult.hpp>
#include <Chunk/StreamHelpers.hpp>
#include <Renderer/ShadowCascades.hpp>
#include <Renderer/Lighting.hpp>
#include <Renderer/TextureManager.hpp>
#include <Renderer/MinecraftTextures.hpp>
#include <Vulkan/VkUpload.hpp>
#include <Vulkan/VkCommands.hpp>
#include <Vulkan/StagingRing.hpp>
#include <Vulkan/GpuResourceRetire.hpp>
#include <stdexcept>
#include <cassert>
#include <algorithm>
#include <cstring>
#include <glm/gtx/hash.hpp>
#include <utils.hpp>
#include <vector>

// Packed ABGR water tint colour shared by the full mesh and LOD mesh generators.
static constexpr uint32_t WATER_COLOR = 0xFF'E6804D;

struct MeshWorkspace
{
  std::vector<uint8_t> mask;
  // Issue #106: face-key planes, one entry per mask cell. Written once per
  // slice by the classification pass; the greedy probes then read key
  // fields instead of re-sampling voxels/biome arrays. Thread-local like
  // the mask: reserved once per worker, no heap churn afterwards.
  std::vector<uint64_t> faceKeyLo;
  std::vector<uint64_t> faceKeyHi;
  std::vector<glm::ivec3> skyQ;
  std::vector<glm::ivec3> blockQ;

  MeshWorkspace()
  {
    mask.reserve(CHUNK_HEIGHT * CHUNK_SIZE);
    faceKeyLo.reserve(CHUNK_HEIGHT * CHUNK_SIZE);
    faceKeyHi.reserve(CHUNK_HEIGHT * CHUNK_SIZE);
    skyQ.reserve(512);
    blockQ.reserve(256);
  }
};

static thread_local MeshWorkspace s_meshWorkspace;

// Issue #106: per-cell materialization of the greedy mesher's face inputs.
// The classification pass (phase 1) samples the two voxels of a slice cell
// exactly once and packs them with their biome owner colors; the greedy
// width/height expansion (phase 2) then evaluates the merge rules on these
// key fields instead of re-sampling voxels and biome arrays per probe.
//
// The merge semantics require the voxel PAIR, not just the cell's own
// classified face:
//  - a +q face fronts AIR in one cell and a transparent block in the next
//    (stone top over air merges with stone top over water);
//  - a -q face may merge with a cell whose own first-match classification
//    is the opposite face of a different transparent block (a water
//    underside merging across a cell fronting kelp/leaves/ice).
// A single "same face" equality key cannot express those boundaries, so
// each cell stores its raw pair and both owner colors:
//
//   lo: bits 0..7    type at the cell (the -q side voxel, may be a border)
//       bits 8..15   type at cell+q (the +q side voxel, may be a border)
//       bits 16..47  owner color of the cell-side type: biome color of its
//                    column for grass/foliage, the constant water tint for
//                    WATER, 0 otherwise
//       bit  48      a visible face exists on this cell (classified once)
//       bit  49      set when the face is owned by the cell+q voxel (-q)
//   hi: bits 0..31   owner color of the cell+q type (same rules, at its
//                    own column)
// AO and light are intentionally NOT part of the key: they are sampled at
// the final merged quad corners and never prevent a merge, exactly as
// before (issue #106 acceptance criteria).
inline constexpr uint64_t kFaceKeyHasFace = 1ull << 48;
inline constexpr uint64_t kFaceKeyNegSide = 1ull << 49;
inline uint64_t makeFaceKeyLo(uint8_t typeAtCell, uint8_t typeAtCellQ,
                              uint32_t colorAtCell, uint64_t flags = 0)
{
  return static_cast<uint64_t>(typeAtCell) |
         (static_cast<uint64_t>(typeAtCellQ) << 8) |
         (static_cast<uint64_t>(colorAtCell) << 16) | flags;
}

// Owner-color kind per block type, precomputed once: keeps the per-cell
// classification to a table load instead of a type switch in the hot loop.
enum FaceColorKind : uint8_t { kColorNone, kColorGrass, kColorFoliage, kColorWater };
inline const std::array<uint8_t, 256> &faceColorKindTable()
{
  static const std::array<uint8_t, 256> table = [] {
    std::array<uint8_t, 256> kinds{};
    for (int i = 0; i < 256; ++i)
    {
      const TextureType type = static_cast<TextureType>(i);
      if (type == GRASS_TOP || type == GRASS_SIDE)
        kinds[static_cast<size_t>(i)] = kColorGrass;
      else if (blockIsFoliage(type))
        kinds[static_cast<size_t>(i)] = kColorFoliage;
      else if (type == WATER)
        kinds[static_cast<size_t>(i)] = kColorWater;
    }
    return kinds;
  }();
  return table;
}

Chunk::Chunk(const glm::vec3 &position, ChunkState state, VoxelPool *voxelPool,
             BorderPool *borderPool, MeshResultPool *meshPool)
    : position(position), visible(false), state(state),
      opaqueIndexCount(0), waterIndexCount(0),
      m_voxelPool(voxelPool ? voxelPool : &VoxelPool::defaultPool()),
      m_storage(nullptr),
      m_borderPool(borderPool ? borderPool : &BorderPool::defaultPool()),
      m_borders(nullptr),
      m_resultPool(meshPool ? meshPool : &MeshResultPool::defaultPool()),
      m_pendingResult(nullptr),
      meshNeedsUpdate(true) { publishCpuTelemetry(); }

Chunk::Chunk(Chunk &&other) noexcept
    : position(std::move(other.position)), visible(other.visible),
      state(other.state.load()),
      m_allocator(other.m_allocator),
      vertexBuffer(other.vertexBuffer),
      indexBuffer(other.indexBuffer),
      waterVertexBuffer(other.waterVertexBuffer),
      waterIndexBuffer(other.waterIndexBuffer),
      opaqueIndexCount(other.opaqueIndexCount), waterIndexCount(other.waterIndexCount),
      meshNeedsUpdate(other.meshNeedsUpdate.load()),
      m_voxelPool(other.m_voxelPool),
      m_storage(other.m_storage),
      m_sectionNonAir(other.m_sectionNonAir),
      m_borderPool(other.m_borderPool),
      m_borders(other.m_borders),
      m_resultPool(other.m_resultPool),
      m_meshGeneration(other.m_meshGeneration),
      m_meshRevision(other.m_meshRevision.load(std::memory_order_relaxed)),
      biomeGrassColors(other.biomeGrassColors),
      biomeFoliageColors(other.biomeFoliageColors),
      biomeTypes(other.biomeTypes),
      heightMap(other.heightMap),
      m_isLODMesh(other.m_isLODMesh)
{
  other.m_storage = nullptr;
  other.m_borders = nullptr;
  // Invariant: no storage => empty occupancy metadata (issue #115 review).
  other.m_sectionNonAir.fill(0);
  other.publishCpuTelemetry();
  publishCpuTelemetry();
  other.m_allocator = VK_NULL_HANDLE;
  other.vertexBuffer = {};
  other.indexBuffer = {};
  other.waterVertexBuffer = {};
  other.waterIndexBuffer = {};
  other.opaqueIndexCount = 0;
  other.waterIndexCount = 0;
  // A pending build result belongs to the moved-from incarnation: release
  // it rather than transferring (issue #104). Its owner pointer names the
  // source chunk, and the destination identity is carried by the transfer
  // of m_meshGeneration above - a build still in flight for that same
  // generation remains publishable on the destination.
  other.releasePendingMeshResult();
}
Chunk &Chunk::operator=(Chunk &&other) noexcept
{
  if (this != &other)
  {
    releaseGPU();

    // Release the destination's current backing through ITS current pools.
    // This must happen before the pool pointers are overwritten by the
    // transfer below, otherwise the destination's border block leaks
    // (issue #113 review).
    releaseNeighborBorders();
    releaseVoxelStorageOnRetire();
    releasePendingMeshResult();
    other.releasePendingMeshResult();

    position = std::move(other.position);
    visible = other.visible;
    state.store(other.state.load());

    // A move transfers complete ownership (issue #112 review): the voxel
    // storage and the pool that owns it travel together. The destination
    // may end up referencing the source's VoxelPool — re-acquiring from
    // the destination pool here would put an allocation and a 64 KiB copy
    // inside a noexcept move, and zeroing the source before acquiring
    // would mutilate it if that allocation threw.
    m_voxelPool = other.m_voxelPool;
    m_storage = other.m_storage;
    other.m_storage = nullptr;

    // Transfer border ownership together with its owning pool (issue #113).
    m_borderPool = other.m_borderPool;
    m_borders = other.m_borders;
    other.m_borders = nullptr;

    m_resultPool = other.m_resultPool;
    m_meshGeneration = other.m_meshGeneration;
    m_meshRevision.store(other.m_meshRevision.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);

    m_sectionNonAir = other.m_sectionNonAir;
    // Invariant: no storage => empty occupancy metadata (issue #115 review).
    // Zero the moved-from side only AFTER the transfer above.
    other.m_sectionNonAir.fill(0);
    biomeGrassColors = other.biomeGrassColors;
    biomeFoliageColors = other.biomeFoliageColors;
    biomeTypes = other.biomeTypes;
    heightMap = other.heightMap;
    m_allocator = other.m_allocator;
    vertexBuffer = other.vertexBuffer;
    indexBuffer = other.indexBuffer;
    waterVertexBuffer = other.waterVertexBuffer;
    waterIndexBuffer = other.waterIndexBuffer;
    opaqueIndexCount = other.opaqueIndexCount;
    waterIndexCount = other.waterIndexCount;
    meshNeedsUpdate.store(other.meshNeedsUpdate.load());
    m_isLODMesh = other.m_isLODMesh;
    m_inTransit.store(other.m_inTransit.load());

    other.publishCpuTelemetry();
    publishCpuTelemetry();
    other.m_allocator = VK_NULL_HANDLE;
    other.vertexBuffer = {};
    other.indexBuffer = {};
    other.waterVertexBuffer = {};
    other.waterIndexBuffer = {};
    other.opaqueIndexCount = 0;
    other.waterIndexCount = 0;
  }
  return *this;
}

Chunk::~Chunk()
{
  // Release all borrowed backing through the pools that own it, including
  // an attached mesh build result (issue #104).
  releasePendingMeshResult();
  releaseNeighborBorders();
  releaseVoxelStorageOnRetire();
  telemetry::registry().replaceCpu(m_cpuTelemetry, std::array<uint64_t, 13>{});
  releaseGPU();
}

bool Chunk::prepareVoxelStorageForGeneration()
{
  // Borders are borrowed on the same main-thread step (issue #103): workers
  // stay allocation-free and generation writes into the chunk's border block.
  //
  // Ownership ONLY (issue #115 review): a freshly acquired voxel block keeps
  // whatever bytes its previous incarnation left - it is NOT cleared here.
  // generateChunkInto() remains the single authoritative reset (one
  // CHUNK_VOLUME air fill before generating, issue #93). See the lifecycle
  // contract on ResetMode::ForGeneration for what may be read when.
  if (m_storage && m_borders)
    return true;
  try
  {
    if (!m_storage)
    {
      m_storage = m_voxelPool->acquire();
      // Logical occupancy is empty until generation initializes the backing.
      m_sectionNonAir.fill(0);
      publishCpuTelemetry();
    }
    if (!m_borders)
    {
      m_borders = m_borderPool->acquire();
      publishCpuTelemetry();
    }
    return true;
  }
  catch (const std::bad_alloc &)
  {
    // Return whatever was acquired so partially prepared state cannot leak.
    releaseNeighborBorders();
    releaseVoxelStorageOnRetire();
    return false;
  }
}

void Chunk::releaseVoxelStorageOnRetire()
{
  if (!m_storage)
    return;
  VoxelStorage *st = m_storage;
  m_storage = nullptr;
  m_voxelPool->release(st);
  // Invariant: no storage => empty occupancy metadata (issue #105).
  m_sectionNonAir.fill(0);
  publishCpuTelemetry();
}

void Chunk::ensureVoxelStorageForEdit()
{
  if (!m_storage)
  {
    m_storage = m_voxelPool->acquire();
    std::fill(m_storage->voxels.begin(), m_storage->voxels.end(), Voxel{static_cast<uint8_t>(AIR)});
    // Fresh storage is all-air: pin the occupancy metadata to it.
    m_sectionNonAir.fill(0);
    publishCpuTelemetry();
  }
}

const glm::vec3 &Chunk::getPosition() const { return position; }

size_t Chunk::getIndex(uint32_t x, uint32_t y, uint32_t z) const
{
  return y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x;
}

bool Chunk::isVisible() const { return visible; }

void Chunk::setVisible(bool visible) { this->visible = visible; }

void Chunk::setState(ChunkState state)
{
  if (state == ChunkState::GENERATED || state == ChunkState::UNLOADED)
  {
    meshNeedsUpdate = true;
  }
  this->state = state;
}

ChunkState Chunk::getState() const { return state; }

const Voxel &Chunk::getVoxel(uint32_t x, uint32_t y, uint32_t z) const
{
	// Strict internal accessor (issue #114 final review): out-of-range
	// coordinates would index past the voxel storage. Callers that can
	// see outside the chunk must use sampleForMeshing()/isVoxelActive(),
	// which answer AIR/false for the padded neighborhood.
	assert(x < CHUNK_SIZE && y < CHUNK_HEIGHT && z < CHUNK_SIZE);
	if (!m_storage)
	{
		static constexpr Voxel s_air{static_cast<uint8_t>(AIR)};
		return s_air;
	}
	return m_storage->voxels[getIndex(x, y, z)];
}

void Chunk::setVoxel(int x, int y, int z, TextureType type)
{
  if (static_cast<uint32_t>(x) < CHUNK_SIZE && static_cast<uint32_t>(y) < CHUNK_HEIGHT &&
      static_cast<uint32_t>(z) < CHUNK_SIZE)
  {
    size_t index = getIndex(x, y, z);
    const TextureType previousType =
        m_storage ? static_cast<TextureType>(m_storage->voxels[index].type) : AIR;
    const bool wasAir = !m_storage ||
                        m_storage->voxels[index].type == static_cast<uint8_t>(AIR);
    if (type != AIR)
    {
      ensureVoxelStorageForEdit();
      m_storage->voxels[index].type = static_cast<uint8_t>(type);
    }
    else
    {
      if (m_storage)
        m_storage->voxels[index].type = static_cast<uint8_t>(AIR);
    }
    // Incremental occupancy (issue #105): a cell moved in or out of its
    // vertical section; type->type rewrites leave the count untouched.
    // Explicit +/- with Debug range asserts: a desync shows up as a
    // hard failure instead of a silent uint16 wraparound.
    const bool nowAir = (type == AIR);
    if (wasAir != nowAir)
    {
      constexpr uint16_t kSectionVolume =
          static_cast<uint16_t>(kOccupancySectionSize) * CHUNK_SIZE * CHUNK_SIZE;
      const int section = y / kOccupancySectionSize;
      uint16_t &count = m_sectionNonAir[section];
      if (nowAir)
      {
        assert(count > 0 && "occupancy section underflow");
        --count;
      }
      else
      {
        assert(count < kSectionVolume && "occupancy section overflow");
        ++count;
      }
    }
    // Section-local remeshing (issue #107): dirty only the affected
    // vertical sections instead of the whole chunk.
    markEditDirtySections(x, y, z, type, previousType, false);
    // Content invalidation (issue #114 review): any in-flight mesh built
    // from the previous content is rejected at publish time.
    m_meshRevision.fetch_add(1, std::memory_order_relaxed);
  }
    else if (x >= -1 && x <= CHUNK_SIZE && z >= -1 && z <= CHUNK_SIZE &&
             y >= 0 && y < static_cast<int>(CHUNK_HEIGHT))
    {
        // Lazily re-borrow borders if they were freed after a previous GPU
        // upload. Vertical padding (y = -1 / CHUNK_HEIGHT) is not representable
        // in the compact storage and was never written by generation either.
        // Lazily re-borrow border storage if it was freed after a previous GPU
        // upload. Acquisition may throw on OOM; a failed edit write is not
        // fatal, so ignore it (the block itself is unchanged).
        if (!m_borders)
        {
            try
            {
                m_borders = m_borderPool->acquire();
                // A freshly acquired pool block holds stale bytes: initialize it so
                // every other sampled border coordinate reads AIR (issue #113).
                m_borders->resetToAir();
                publishCpuTelemetry();
            }
            catch (const std::bad_alloc &)
            {
                return;
            }
        }
        m_borders->mutableAt(x, y, z) = static_cast<uint8_t>(type);
        // Border content invalidation (issue #114 review): mirror
        // writes from a neighbor's boundary edit must invalidate in-flight
        // meshes of THIS chunk just like in-chunk edits do.
        // Section-local dirtying too (issue #107): the neighbor's border
        // strip face ownership lives in exactly the y/16 section.
        markEditDirtySections(x, y, z, type, AIR, true);
        m_meshRevision.fetch_add(1, std::memory_order_relaxed);
    }
}

// Section dirtying for one voxel edit (issue #107). The mesh state is one
// payload per vertical 16^3 section, so an edit re-arms only the sections
// whose quads can change:
//   - the edited section always;
//   - the section above/below when the voxel sits on a section Y boundary
//     (faces at that plane are owned by both neighbors);
//   - for in-chunk edits, a conservative light-dirty Y range. The light
//     FIELD is recomputed chunk-wide by every build (no seams by
//     construction); this range only decides which section meshes refresh
//     their light bits in the same job: emissive edits spread their radius,
//     other edits change the skylight column down to the first blocker.
//     Deep horizontal flood beyond that range is the documented frontier
//     for regional lighting.
//   - border (mirror) writes change face ownership only at the written
//     voxel's own y/16 section (cross-chunk light/AO do not exist).
void Chunk::markEditDirtySections(int x, int y, int z, TextureType type,
                                  TextureType previousType, bool borderWrite)
{
  if (y < 0 || y >= static_cast<int>(CHUNK_HEIGHT))
    return; // vertical padding has no mesh state
  const int section = y / kOccupancySectionSize;
  uint16_t mask = static_cast<uint16_t>(1u << section);
  if (!borderWrite)
  {
    const int inSection = y % kOccupancySectionSize;
    if (inSection == 0 && section > 0)
      mask |= static_cast<uint16_t>(1u << (section - 1));
    if (inSection == kOccupancySectionSize - 1 && section + 1 < kOccupancySections)
      mask |= static_cast<uint16_t>(1u << (section + 1));

    int lightMinY = y;
    int lightMaxY = y;
    if (lighting::blockLightEmission(static_cast<uint8_t>(type)) > 0 ||
        lighting::blockLightEmission(static_cast<uint8_t>(previousType)) > 0)
    {
      lightMinY = std::max(0, y - 14);
      lightMaxY = std::min(CHUNK_HEIGHT - 1, y + 14);
    }
    else
    {
      // Skylight column below the edit: cells between the first blocker
      // and the edit change brightness (place blocks the shaft, delete
      // re-opens it). The scan reads the freshly written voxel, so a
      // delete passes through y itself.
      const int scanStart = (type == AIR) ? y : y - 1;
      int blocker = scanStart;
      while (blocker >= 0 &&
             blockTransmitsSkyLight(static_cast<TextureType>(getVoxel(
                 static_cast<uint32_t>(x), static_cast<uint32_t>(blocker),
                 static_cast<uint32_t>(z))
                                     .type)))
        --blocker;
      lightMinY = blocker + 1;
    }
    const int lightMinSection =
        std::max(0, lightMinY / kOccupancySectionSize);
    const int lightMaxSection =
        std::min(kOccupancySections - 1, lightMaxY / kOccupancySectionSize);
    for (int s = lightMinSection; s <= lightMaxSection; ++s)
      mask |= static_cast<uint16_t>(1u << s);
  }
  m_dirtySections.fetch_or(mask, std::memory_order_relaxed);
}

bool Chunk::deleteVoxel(const glm::vec3 &position)
{
  if (!m_storage)
    return false;

  int x = static_cast<int>(position.x - this->position.x);
  int y = static_cast<int>(position.y - this->position.y);
  int z = static_cast<int>(position.z - this->position.z);
  if (x < 0)
    x += CHUNK_SIZE;
  if (z < 0)
    z += CHUNK_SIZE;

  if (isVoxelActive(x, y, z))
  {
    setVoxel(x, y, z, AIR);
    meshNeedsUpdate = true;
    state = ChunkState::GENERATED;
    return true;
  }
  return false;
}

bool Chunk::placeVoxel(const glm::vec3 &position, TextureType type)
{
  int x = static_cast<int>(position.x - this->position.x);
  int y = static_cast<int>(position.y - this->position.y);
  int z = static_cast<int>(position.z - this->position.z);
  if (x < 0)
    x += CHUNK_SIZE;
  if (z < 0)
    z += CHUNK_SIZE;

  if (!isVoxelActive(x, y, z))
  {
    setVoxel(x, y, z, type);
    meshNeedsUpdate = true;
    state = ChunkState::GENERATED;
    return true;
  }
  return false;
}

TextureType Chunk::sampleForMeshing(int x, int y, int z) const
{
  if (static_cast<uint32_t>(x) < CHUNK_SIZE && static_cast<uint32_t>(y) < CHUNK_HEIGHT &&
      static_cast<uint32_t>(z) < CHUNK_SIZE)
  {
    return static_cast<TextureType>(getVoxel(x, y, z).type);
  }
  if (x < -1 || x > CHUNK_SIZE || z < -1 || z > CHUNK_SIZE)
    return AIR; // outside the padded neighborhood
  if (!m_borders)
    return AIR; // borders freed after upload or never built (documented default)
  return static_cast<TextureType>(m_borders->at(x, y, z));
}

bool Chunk::isVoxelActive(int x, int y, int z) const
{
  if (static_cast<uint32_t>(x) < CHUNK_SIZE && static_cast<uint32_t>(y) < CHUNK_HEIGHT &&
      static_cast<uint32_t>(z) < CHUNK_SIZE)
  {
    if (!m_storage)
      return false;
    // Canonical occupancy (issue #105): the voxel type itself, no bitset.
    return m_storage->voxels[getIndex(x, y, z)].type != static_cast<uint8_t>(AIR);
  }
  else if (x >= -1 && x <= CHUNK_SIZE && z >= -1 && z <= CHUNK_SIZE)
  {
    return m_borders && m_borders->at(x, y, z) != static_cast<uint8_t>(AIR);
  }
  return false; // Outside known boundaries
}

void Chunk::recountOccupancy()
{
  m_sectionNonAir.fill(0);
  if (!m_storage)
    return;
  constexpr size_t kSectionVolume =
      static_cast<size_t>(kOccupancySectionSize) * CHUNK_SIZE * CHUNK_SIZE;
  for (int section = 0; section < kOccupancySections; ++section)
  {
    const Voxel *cells = m_storage->voxels.data() + section * kSectionVolume;
    uint16_t count = 0;
    for (size_t i = 0; i < kSectionVolume; ++i)
      count += cells[i].type != static_cast<uint8_t>(AIR) ? 1 : 0;
    m_sectionNonAir[section] = count;
  }
}

bool Chunk::occupiedSpanY(int &outMinY, int &outMaxY) const
{
#ifndef NDEBUG
  validateOccupancyMetadata();
#endif
  int first = 0;
  int last = kOccupancySections - 1;
  while (first <= last && m_sectionNonAir[first] == 0)
    ++first;
  if (first > last)
    return false; // no non-air voxel anywhere
  while (m_sectionNonAir[last] == 0)
    --last;
  outMinY = first * kOccupancySectionSize;
  outMaxY = last * kOccupancySectionSize + (kOccupancySectionSize - 1);
  return true;
}

void Chunk::refineOccupiedSpanY(int &occMinY, int &occMaxY) const
{
  if (!m_storage)
    return;
  const auto layerHasVoxel = [this](int y) -> bool
  {
    const Voxel *layer =
        m_storage->voxels.data() + static_cast<size_t>(y) * CHUNK_SIZE * CHUNK_SIZE;
    for (int i = 0; i < CHUNK_SIZE * CHUNK_SIZE; ++i)
      if (layer[i].type != static_cast<uint8_t>(AIR))
        return true;
    return false;
  };
  while (occMinY < occMaxY && !layerHasVoxel(occMinY))
    ++occMinY;
  while (occMaxY > occMinY && !layerHasVoxel(occMaxY))
    --occMaxY;
}

uint16_t Chunk::occupiedSectionMask() const
{
  uint16_t mask = 0;
  for (int section = 0; section < kOccupancySections; ++section)
    if (m_sectionNonAir[section] != 0)
      mask |= static_cast<uint16_t>(1u << section);
  return mask;
}

void Chunk::validateOccupancyMetadata() const
{
  // A per-section count can never exceed the section's voxel count, and a
  // storageless chunk must be empty (issue #115 review).
  constexpr uint16_t kSectionVolume =
      static_cast<uint16_t>(kOccupancySectionSize) * CHUNK_SIZE * CHUNK_SIZE;
  for (const uint16_t count : m_sectionNonAir)
    assert(count <= kSectionVolume && "occupancy count above section volume");
  if (!m_storage)
  {
    for (const uint16_t count : m_sectionNonAir)
      assert(count == 0 && "storageless chunk must have empty occupancy");
  }
}

void Chunk::generateTerrain(TerrainGenerator &generator)
{
  MemoryPublication memoryPublication{*this};
  if (state.load() != ChunkState::UNLOADED)
    return;

  // Ensure we use integer coordinates aligned with world grid
  int genX = static_cast<int>(std::round(position.x));
  int genZ = static_cast<int>(std::round(position.z));

  // Generate directly into this pooled chunk's reusable storage: no
  // temporary ChunkData and no CHUNK_VOLUME / border-shell copies after
  // generation. Storage and border blocks MUST have been prepared on the
  // main thread prior to dispatch (issue #103: the compact border block is
  // borrowed by prepareVoxelStorageForGeneration()).
  assert(m_storage != nullptr && "VoxelStorage must be prepared before generateTerrain");
  assert(m_borders != nullptr && "Borders must be prepared before generateTerrain");
  if (!m_storage || !m_borders)
    return;

  ChunkGenerationTarget target{
      .voxels = std::span<Voxel, CHUNK_VOLUME>(m_storage->voxels),
      .borders = m_borders,
      .biomes = biomeTypes,
      .heightMap = heightMap,
      .grassColors = biomeGrassColors,
      .foliageColors = biomeFoliageColors};
  generator.generateChunkInto(genX, genZ, target);

  // Compact occupancy metadata (issue #105): one canonical pass that only
  // reads the freshly written voxels and fills 16 per-section counters —
  // it replaces the former full-volume pass whose sole purpose was
  // rebuilding the removed activeVoxels bitset.
  recountOccupancy();

  state = ChunkState::GENERATED;
  meshNeedsUpdate = true;
  // Full content replacement invalidates any build result stamped with the
  // previous revision (issue #114 review).
  m_meshRevision.fetch_add(1, std::memory_order_relaxed);
  // A full build rebuilds every section (issue #107).
  m_dirtySections.store(kAllSectionMask, std::memory_order_relaxed);
}

// Chunk-wide sky/block light fields for one mesh job (issue #107):
// computed once per build so a section-selective rebuild samples exactly
// the field a whole-chunk build would produce (no seams at 16-block Y
// boundaries). Thread-local scratch like the mesh workspace.
static thread_local std::vector<uint8_t> s_skyLight;
static thread_local std::vector<uint8_t> s_blockLight;

void Chunk::buildMesh(MeshBuildResult &out, uint64_t generation, uint64_t revision)
{
  buildMesh(out, generation, revision, kAllSectionMask);
}

void Chunk::buildMesh(MeshBuildResult &out, uint64_t generation, uint64_t revision,
                      uint16_t sectionMask)
{
  // Mesh timing must exclude pool/publication overhead: the payload lands
  // in the pooled result block, nothing on the Chunk changes ownership.
  out.beginBuild(this, generation, revision, sectionMask);
  out.isLOD = false;
  // Occupied Y span from the per-section metadata (issue #105): no 64 KiB
  // type copy + full-buffer scan per remesh. Section-granular, then refined
  // to byte-exact layer bounds inside the two boundary sections so slice
  // iteration stays as tight as the old full-scan helper produced.
  int occMinY = 0;
  int occMaxY = CHUNK_HEIGHT - 1;
  {
    // The prepass is real work and is measured as such (issue #115 review),
    // including for empty chunks whose build ends with the early return.
    telemetry::StageSample occupancySample(telemetry::Occupancy);
    if (!occupiedSpanY(occMinY, occMaxY))
    {
      // Empty occupancy: the result stays empty; publishMeshResult commits
      // the (empty) mesh on the main thread.
      return;
    }
    refineOccupiedSpanY(occMinY, occMaxY);
  }
  if (out.sectionsBuilt == 0)
    return;

  telemetry::MeshSample meshSample(telemetry::Skylight);
  // Lighting stays chunk-wide for every job (issue #107 lighting
  // contract): each rebuilt section samples the same light field a whole
  // build would produce, so section boundaries cannot introduce seams.
  computeLightField(meshSample);
  meshSample.next(telemetry::FacesGreedyAO);

  // One greedy pass per dirty section (issue #107). Empty sections are
  // skipped and leave an empty payload behind (which also clears their GPU
  // slot content on upload). Batching all dirty sections in this single
  // worker job keeps initial generation free of tiny-task overhead.
  for (int section = 0; section < kOccupancySections; ++section)
  {
    if (((out.sectionsBuilt >> section) & 1u) == 0)
      continue;
    if (m_sectionNonAir[section] == 0)
      continue; // empty section: no mesh work, empty payload
    const int ownerMinY =
        std::max(section * kOccupancySectionSize, occMinY);
    const int ownerMaxY =
        std::min(section * kOccupancySectionSize + kOccupancySectionSize - 1,
                 occMaxY);
    buildSectionGreedy(out, section, ownerMinY, ownerMaxY, meshSample);
  }
}

void Chunk::buildMeshRanged(MeshBuildResult &out, uint64_t generation, uint64_t revision,
                            int minY, int maxY)
{
  // Probe/test entry (issues #105/#115/#106): full-quality build of the
  // sections the [minY, maxY] range touches. Layers outside the occupied
  // span hold no voxels and classify to no faces, so clipping the section
  // owner ranges to the occupied span (inside buildMesh) keeps this
  // byte-identical to the historical span-clipped behavior.
  minY = std::clamp(minY, 0, CHUNK_HEIGHT - 1);
  maxY = std::clamp(maxY, 0, CHUNK_HEIGHT - 1);
  uint16_t mask = 0;
  for (int s = minY / kOccupancySectionSize; s <= maxY / kOccupancySectionSize; ++s)
    mask |= static_cast<uint16_t>(1u << s);
  buildMesh(out, generation, revision, mask);
}

void Chunk::computeLightField(telemetry::MeshSample &meshSample)
{
  auto &workspace = s_meshWorkspace;
  auto &skyLight = s_skyLight;
  auto &blockLight = s_blockLight;
  skyLight.assign(static_cast<size_t>(CHUNK_VOLUME), 0);
  blockLight.assign(static_cast<size_t>(CHUNK_VOLUME), 0);
  {
    auto idxOf = [](int x, int y, int z) -> size_t {
      return static_cast<size_t>(x + CHUNK_SIZE * (y + CHUNK_HEIGHT * z));
    };
    auto isAirLike = [&](int x, int y, int z) -> bool {
      if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_SIZE)
        return true;
      const auto t = static_cast<TextureType>(getVoxel(x, y, z).type);
      return blockTransmitsSkyLight(t);
    };

    // Sky light: per column, cast down until solid (open sky = 15)
    for (int z = 0; z < CHUNK_SIZE; ++z)
    {
      for (int x = 0; x < CHUNK_SIZE; ++x)
      {
        uint8_t light = 15;
        for (int y = CHUNK_HEIGHT - 1; y >= 0; --y)
        {
          if (!isAirLike(x, y, z))
          {
            light = 0;
            continue;
          }
          skyLight[idxOf(x, y, z)] = light;
          // Full sky light continues down open air; solid roof zeros it above
          if (light > 0 && y > 0 && !isAirLike(x, y - 1, z))
            ; // keep full until blocked
          else if (light > 0)
            light = static_cast<uint8_t>(light > 0 ? light : 0);
        }
      }
    }

    // Horizontal sky flood into caves/overhangs (Minecraft-style −1 per step).
    // Vertical cast alone leaves cavern mouths pitch-black one block in.
    {
      auto &skyQ = workspace.skyQ;
      skyQ.clear();
      for (int z = 0; z < CHUNK_SIZE; ++z)
        for (int y = 0; y < CHUNK_HEIGHT; ++y)
          for (int x = 0; x < CHUNK_SIZE; ++x)
          {
            if (skyLight[idxOf(x, y, z)] > 0 && isAirLike(x, y, z))
              skyQ.emplace_back(x, y, z);
          }
      size_t skyHead = 0;
      const int skyDirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
      while (skyHead < skyQ.size())
      {
        const glm::ivec3 p = skyQ[skyHead++];
        const uint8_t cur = skyLight[idxOf(p.x, p.y, p.z)];
        if (cur <= 1)
          continue;
        const uint8_t next = static_cast<uint8_t>(cur - 1);
        for (auto &d : skyDirs)
        {
          const int nx = p.x + d[0], ny = p.y + d[1], nz = p.z + d[2];
          if (nx < 0 || nx >= CHUNK_SIZE || ny < 0 || ny >= CHUNK_HEIGHT || nz < 0 || nz >= CHUNK_SIZE)
            continue;
          const size_t i = idxOf(nx, ny, nz);
          if (skyLight[i] < next)
          {
            skyLight[i] = next;
            // Propagate through air; also write into solids so face sampling sees light
            if (isAirLike(nx, ny, nz))
              skyQ.emplace_back(nx, ny, nz);
          }
        }
      }
    }

    meshSample.next(telemetry::Blocklight);
    // Seed block light from emissive solids into neighboring air
    auto &queue = workspace.blockQ;
    queue.clear();
    for (int z = 0; z < CHUNK_SIZE; ++z)
      for (int y = 0; y < CHUNK_HEIGHT; ++y)
        for (int x = 0; x < CHUNK_SIZE; ++x)
        {
          const uint8_t em = lighting::blockLightEmission(getVoxel(x, y, z).type);
          if (em == 0)
            continue;
          // Light lives in air cells around the emitter
          const int dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
          for (auto &d : dirs)
          {
            const int nx = x + d[0], ny = y + d[1], nz = z + d[2];
            if (nx < 0 || nx >= CHUNK_SIZE || ny < 0 || ny >= CHUNK_HEIGHT || nz < 0 || nz >= CHUNK_SIZE)
              continue;
            if (!isAirLike(nx, ny, nz) && static_cast<TextureType>(getVoxel(nx, ny, nz).type) != AIR)
            {
              // still light the solid face later via adjacent air — also seed the solid cell
            }
            const size_t i = idxOf(nx, ny, nz);
            if (blockLight[i] < em)
            {
              blockLight[i] = em;
              queue.emplace_back(nx, ny, nz);
            }
          }
          // Also seed emitter cell for face sampling
          const size_t ei = idxOf(x, y, z);
          if (blockLight[ei] < em)
          {
            blockLight[ei] = em;
            queue.emplace_back(x, y, z);
          }
        }

    // Propagate block light (coarse BFS, attenuation 1 per step)
    size_t head = 0;
    const int dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    while (head < queue.size())
    {
      const glm::ivec3 p = queue[head++];
      const uint8_t cur = blockLight[idxOf(p.x, p.y, p.z)];
      if (cur <= 1)
        continue;
      const uint8_t next = static_cast<uint8_t>(cur - 1);
      for (auto &d : dirs)
      {
        const int nx = p.x + d[0], ny = p.y + d[1], nz = p.z + d[2];
        if (nx < 0 || nx >= CHUNK_SIZE || ny < 0 || ny >= CHUNK_HEIGHT || nz < 0 || nz >= CHUNK_SIZE)
          continue;
        // Propagate through air-like; allow into solids so faces pick up light
        const size_t i = idxOf(nx, ny, nz);
        if (blockLight[i] < next)
        {
          blockLight[i] = next;
          if (isAirLike(nx, ny, nz))
            queue.emplace_back(nx, ny, nz);
        }
      }
    }
  }
}

// Greedy meshing of ONE vertical section (issue #107): every greedy face
// whose owning voxel lies in [ownerMinY, ownerMaxY]. Faces, AO and light
// sample the full one-voxel chunk/border context exactly like a whole-chunk
// build, so section boundaries are visually identical to the unsplit mesh;
// only quads that would cross a section boundary are split into one quad
// per section (each section owns the faces of its own voxels). Greedy
// rectangles never merge across sections because the mask plane is clipped
// to the section's occupied Y span and the slice walk repeats per section.
bool Chunk::hasUnuploadedFullMesh() const
{
  return m_pendingResult != nullptr && !m_pendingResult->isLOD;
}

void Chunk::buildSectionGreedy(MeshBuildResult &out, int section, int ownerMinY,
                               int ownerMaxY, telemetry::MeshSample &meshSample)
{
  // The mesher body is written against the historical member names; bind
  // them to this section's payload (issue #104/#107). Indices are
  // section-local (base 0); the upload rebase them onto the section's GPU
  // vertex slot.
  auto &vertices = out.sections[section].opaqueVertices;
  auto &indices = out.sections[section].opaqueIndices;
  auto &waterVertices = out.sections[section].waterVertices;
  auto &waterIndices = out.sections[section].waterIndices;
  auto &skyLight = s_skyLight;
  auto &blockLight = s_blockLight;

  auto &workspace = s_meshWorkspace;
  uint32_t indexCounter = 0;
  uint32_t waterIndexCounter = 0;

  // New helper for greedy meshing that checks local voxels and the precomputed
  // neighbor shell
  auto getVoxelDataForMeshing = [&](int lx, int ly, int lz) -> TextureType
  {
    if (static_cast<uint32_t>(lx) < CHUNK_SIZE && static_cast<uint32_t>(ly) < CHUNK_HEIGHT &&
        static_cast<uint32_t>(lz) < CHUNK_SIZE)
    {
      return static_cast<TextureType>(getVoxel(lx, ly, lz).type);
    }
    // Check the neighbor shell for out-of-bounds coordinates relevant to
    // meshing.
    // Layout-independent border sampling (issue #103): the mesher does not
    // know the physical border representation.
    return sampleForMeshing(lx, ly, lz);
  };

  const int dims[] = {CHUNK_SIZE, CHUNK_HEIGHT, CHUNK_SIZE};

  // Slice range needs neighbors one cell outside solids for face detection.
  // For a section build the range is [ownerMinY-1, ownerMaxY]: the slice
  // below contributes only -q faces (owned by the section's first voxel
  // layer) and the last slice only +q faces - the owner check inside the
  // classification suppresses the faces owned by the adjacent sections.
  const int ySliceMin = std::max(-1, ownerMinY - 1);
  const int ySliceMax = std::min(CHUNK_HEIGHT - 1, ownerMaxY); // x[d] runs to dims[d]-1 inclusive via < dims

  // Iterate over dimensions (X, Y, Z)
  for (int d = 0; d < 3; ++d)
  {
    int u = (d + 1) % 3; // First axis in the plane of the face
    int v = (d + 2) % 3; // Second axis in the plane of the face

    glm::ivec3 x = {0, 0, 0}; // Current voxel coordinate during slice iteration
    glm::ivec3 q = {0, 0,
                    0}; // Normal direction for the face (points from x to x+q)
    q[d] = 1;

    // Ensure mask is large enough for the current slice
    if (workspace.mask.size() < static_cast<size_t>(dims[u] * dims[v]))
    {
      workspace.mask.resize(dims[u] * dims[v]);
      workspace.faceKeyLo.resize(dims[u] * dims[v]);
      workspace.faceKeyHi.resize(dims[u] * dims[v]);
    }

    // Slice range along d; clamp Y when d==1.
    int dStart = -1;
    int dEnd = dims[d]; // exclusive upper for x[d] < dEnd
    if (d == 1)
    {
      dStart = ySliceMin;
      dEnd = ySliceMax + 1;
    }

    // Iterate over each slice of the chunk along dimension 'd'
    // x[d] ranges from -1 (representing boundary before chunk) to dims[d]-1
    // (last voxel layer) A face exists between slice x[d] and slice x[d]+1
    for (x[d] = dStart; x[d] < dEnd; ++x[d])
    {
      // Empty vertical slabs cannot produce in-chunk faces (issue #105):
      // a Y face between slices s and s+1 needs a voxel in section(s) or
      // section(s+1), and there are no vertical neighbor borders.
      if (d == 1 &&
          m_sectionNonAir[std::max(x[d], 0) / kOccupancySectionSize] == 0 &&
          m_sectionNonAir[std::min(x[d] + 1, CHUNK_HEIGHT - 1) / kOccupancySectionSize] == 0)
        continue;
      meshSample.data.maskCells += dims[u] * dims[v];
      // Bound Y when it is a plane axis: owners must stay inside the
      // section's Y range (issue #107), which the caller already clipped to
      // the occupied span. The clamps are slice-invariant.
      int uStart = 0, uEnd = dims[u];
      int vStart = 0, vEnd = dims[v];
      if (u == 1)
      {
        uStart = std::max(0, ownerMinY);
        uEnd = std::min(dims[u], ownerMaxY + 1);
      }
      if (v == 1)
      {
        vStart = std::max(0, ownerMinY);
        vEnd = std::min(dims[v], ownerMaxY + 1);
      }

      // Reset the consumed-mask rect for this slice. Clearing only the
      // built rect is enough: phase 2 seeds and probes stay inside it (the
      // #106 proof - cells outside the rect own no faces), and sectioned
      // builds re-walk the slice axis once per section, so a full-plane
      // clear would multiply the fixed cost by the section count.
      for (int row = uStart; row < uEnd; ++row)
        std::fill(workspace.mask.begin() + row * dims[v] + vStart,
                  workspace.mask.begin() + row * dims[v] + vEnd, 0);

      // Phase 1 (issue #106): materialize the face inputs of every cell of
      // the slice exactly once - the two voxel types straddling the face
      // plane plus the biome owner color of each side. The key plane covers
      // the same clamped rect the seed loop walks; greedy expansion stays
      // inside it because cells beyond the refined occupied Y span hold
      // only AIR voxels (and border samples never own faces), so no
      // in-chunk face can exist there and the old rule-based probes always
      // broke on the first such cell.
      uint64_t *faceKeyLo = workspace.faceKeyLo.data();
      uint64_t *faceKeyHi = workspace.faceKeyHi.data();
      // Chunk-ownership of the two sampled voxels is slice-invariant: the
      // plane axes always iterate inside the chunk, so only the slice axis
      // decides whether a sample is a local voxel or a border/shell read.
      // Border/shell voxels are used solely for occlusion - the neighboring
      // chunk renders its own faces. Owner colors are only ever read for
      // in-chunk owners, so the guarded side stays 0 at the two outermost
      // slices. Classification is deliberately NOT section-gated (PR #117
      // review): a cell pair at a section seam must classify to the same
      // face - same owner, same side - in every section pass that visits
      // the pair, exactly like the whole-chunk build; the owner check below
      // then decides which section EMITS the face. Gating classification
      // instead made the section above a seam classify a spurious second
      // face whenever the first match belonged to the section below.
      const bool type1ChunkSide = (x[d] >= 0);
      const bool type2ChunkSide = (x[d] + 1 < dims[d]);
      const uint8_t airType = static_cast<uint8_t>(AIR);
      auto faceOwnerColor = [&](TextureType t, const glm::ivec3 &ownerCoord) -> uint32_t
      {
        switch (faceColorKindTable()[static_cast<size_t>(t)])
        {
        case kColorGrass:
          return biomeGrassColors[ownerCoord[2] * CHUNK_SIZE + ownerCoord[0]];
        case kColorFoliage:
          return biomeFoliageColors[ownerCoord[2] * CHUNK_SIZE + ownerCoord[0]];
        case kColorWater:
          return WATER_COLOR;
        default:
          return 0;
        }
      };
      for (x[u] = uStart; x[u] < uEnd; ++x[u])
      {
        for (x[v] = vStart; x[v] < vEnd; ++x[v])
        {
          const glm::ivec3 xq = x + q;
          const TextureType type1 = getVoxelDataForMeshing(x[0], x[1], x[2]);
          const TextureType type2 = getVoxelDataForMeshing(xq[0], xq[1], xq[2]);
          const size_t cell = static_cast<size_t>(x[u]) * dims[v] + x[v];
          const uint8_t t1 = static_cast<uint8_t>(type1);
          const uint8_t t2 = static_cast<uint8_t>(type2);

          // Inert pairs: identical types, or two different non-air opaque
          // types. No face can be classified on them from either side and
          // no merge probe can accept them (the backing check fails), so
          // the owner colors and the classification are skipped.
          if (t1 == t2 ||
              (t1 != airType && t2 != airType &&
               !TextureManager::isTransparent(type1) &&
               !TextureManager::isTransparent(type2)))
          {
            faceKeyLo[cell] = makeFaceKeyLo(t1, t2, 0);
            faceKeyHi[cell] = 0;
            continue;
          }

          // Owner colors of both sides: each one is read by merge probes
          // of the matching origin side, regardless of which face (if any)
          // this cell itself classifies to - a -q face of T may merge
          // across a cell whose own visible face is the opposite face of a
          // different transparent block, and that probe compares the T-side
          // owner color of the candidate.
          const uint32_t color1 =
              type1ChunkSide ? faceOwnerColor(type1, x) : 0;
          const uint32_t color2 =
              type2ChunkSide ? faceOwnerColor(type2, xq) : 0;

          // Classify the visible face once (slice-invariant ownership
          // sides) and store it beside the colors.
          bool hasFace = false;
          bool negSide = false;
          if (type1ChunkSide && type1 != AIR &&
              (type2 == AIR ||
               (TextureManager::isTransparent(type2) && type1 != type2)))
          {
            // Face belongs to the voxel at x, pointing towards x+q
            hasFace = true;
          }
          else if (type2ChunkSide && type2 != AIR &&
                   (type1 == AIR || (TextureManager::isTransparent(type1) &&
                                     type1 != type2)))
          {
            // Face belongs to the voxel at x+q, pointing towards x
            hasFace = true;
            negSide = true;
          }
          if (hasFace && d == 1)
          {
            // Section-local emission of the globally classified face
            // (PR #117 review): the owner voxel decides which section
            // emits, so a face at a seam is emitted by exactly one section
            // and every other pass just sees "no face for me".
            const int ownerY = negSide ? x[d] + 1 : x[d];
            if (ownerY < ownerMinY || ownerY > ownerMaxY)
              hasFace = false;
          }
          faceKeyLo[cell] = makeFaceKeyLo(t1, t2, color1,
                                          (hasFace ? kFaceKeyHasFace : 0) |
                                              (negSide ? kFaceKeyNegSide : 0));
          faceKeyHi[cell] = color2;
        }
      }

      // Phase 2 (issue #106): greedy rectangle merge over the key planes -
      // the seed walk reads the precomputed classification and the
      // width/height probes evaluate the exact merge rules of the old
      // mesher on the materialized pair, with no voxel/shell sampling and
      // no biome array lookups left in the hot loops.
      // Iterate over the plane (u, v)
      for (x[u] = uStart; x[u] < uEnd; ++x[u])
      {
        for (x[v] = vStart; x[v] < vEnd; ++x[v])
        {

          if (workspace.mask[x[u] * dims[v] + x[v]])
          {
            continue; // Already processed this part of the slice
          }

          const size_t cell = static_cast<size_t>(x[u]) * dims[v] + x[v];
          const uint64_t keyLo = faceKeyLo[cell];
          if ((keyLo & kFaceKeyHasFace) == 0)
          {
            continue; // No visible face here, or types are the same opaque.
          }

          const bool ownerAtX = (keyLo & kFaceKeyNegSide) == 0;
          const uint8_t originType = static_cast<uint8_t>(
              ownerAtX ? (keyLo & 0xFF) : ((keyLo >> 8) & 0xFF));
          const TextureType quad_type = static_cast<TextureType>(originType);
          const glm::ivec3 quad_normal_dir = ownerAtX ? q : -q;
          const glm::ivec3 quad_origin_voxel_coord = ownerAtX ? x : (x + q);
          const uint32_t originColor =
              ownerAtX ? static_cast<uint32_t>(keyLo >> 16)
                       : static_cast<uint32_t>(faceKeyHi[cell]);

          // Calculate width (w) of the quad along dimension u
          int w;
          for (w = 1; x[u] + w < uEnd; ++w)
          {
            const size_t probe = static_cast<size_t>(x[u] + w) * dims[v] + x[v];
            if (workspace.mask[probe])
              break;
            const uint64_t key = faceKeyLo[probe];
            if (ownerAtX)
            {
              const uint8_t ownerType = static_cast<uint8_t>(key & 0xFF);
              const uint8_t backingType = static_cast<uint8_t>((key >> 8) & 0xFF);
              if (ownerType != originType ||
                  !(backingType == airType ||
                    (TextureManager::isTransparent(static_cast<TextureType>(backingType)) &&
                     ownerType != backingType)) ||
                  static_cast<uint32_t>(key >> 16) != originColor)
                break; // Adjacent cell is not the same mergeable face
            }
            else
            {
              const uint8_t ownerType = static_cast<uint8_t>((key >> 8) & 0xFF);
              const uint8_t backingType = static_cast<uint8_t>(key & 0xFF);
              if (ownerType != originType ||
                  !(backingType == airType ||
                    (TextureManager::isTransparent(static_cast<TextureType>(backingType)) &&
                     ownerType != backingType)) ||
                  static_cast<uint32_t>(faceKeyHi[probe]) != originColor)
                break; // Adjacent cell is not the same mergeable face
            }
          }

          // Calculate height (h) of the quad along dimension v
          int h;
          bool h_break = false;
          for (h = 1; x[v] + h < vEnd; ++h)
          {
            for (int k = 0; k < w;
                 ++k)
            { // Check all cells in the current row of width w
              const size_t probe =
                  static_cast<size_t>(x[u] + k) * dims[v] + (x[v] + h);
              if (workspace.mask[probe])
              {
                h_break = true;
                break;
              }
              const uint64_t key = faceKeyLo[probe];
              if (ownerAtX)
              {
                const uint8_t ownerType = static_cast<uint8_t>(key & 0xFF);
                const uint8_t backingType = static_cast<uint8_t>((key >> 8) & 0xFF);
                if (ownerType != originType ||
                    !(backingType == airType ||
                      (TextureManager::isTransparent(static_cast<TextureType>(backingType)) &&
                       ownerType != backingType)) ||
                    static_cast<uint32_t>(key >> 16) != originColor)
                {
                  h_break = true;
                  break;
                }
              }
              else
              {
                const uint8_t ownerType = static_cast<uint8_t>((key >> 8) & 0xFF);
                const uint8_t backingType = static_cast<uint8_t>(key & 0xFF);
                if (ownerType != originType ||
                    !(backingType == airType ||
                      (TextureManager::isTransparent(static_cast<TextureType>(backingType)) &&
                       ownerType != backingType)) ||
                    static_cast<uint32_t>(faceKeyHi[probe]) != originColor)
                {
                  h_break = true;
                  break;
                }
              }
            }
            if (h_break)
              break;
          }

          // Add quad to mesh
          glm::vec3
              s_coord_float; // Min corner of the quad in local chunk grid space
          s_coord_float[d] = static_cast<float>(
              x[d] + 1.0f); // Corrected: Face is always at x[d]+1
          s_coord_float[u] = static_cast<float>(x[u]);
          s_coord_float[v] = static_cast<float>(x[v]);

          glm::vec3 quad_width_vec = {0, 0, 0};
          quad_width_vec[u] = static_cast<float>(w);
          glm::vec3 quad_height_vec = {0, 0, 0};
          quad_height_vec[v] = static_cast<float>(h);

          glm::vec3 v0_local = s_coord_float;
          glm::vec3 v1_local = s_coord_float + quad_width_vec;
          glm::vec3 v2_local = s_coord_float + quad_width_vec + quad_height_vec;
          glm::vec3 v3_local = s_coord_float + quad_height_vec;

          // Texture coordinates for tiling
          // The greedy mesher uses u=(d+1)%3, v=(d+2)%3.
          // We need consistent texture orientation regardless of face:
          //   d=0 (±X faces): u=Y, v=Z → swap so tex_u=Z(h), tex_v=Y(w)
          //   d=1 (±Y faces): u=Z, v=X → swap so tex_u=X(h), tex_v=Z(w)
          //   d=2 (±Z faces): u=X, v=Y → already correct
          glm::vec2 tc[4];
          float tex_w = static_cast<float>(w);
          float tex_h = static_cast<float>(h);

          bool swapUV = (d == 0 || d == 1);
          float tc_u = swapUV ? tex_h : tex_w;
          float tc_v = swapUV ? tex_w : tex_h;

          if (swapUV)
          {
            // After swap: vertex 0→(0,0), 1→(0,tc_v), 2→(tc_u,tc_v), 3→(tc_u,0)
            tc[0] = {0.0f, 0.0f};
            tc[1] = {0.0f, tc_v};
            tc[2] = {tc_u, tc_v};
            tc[3] = {tc_u, 0.0f};
          }
          else
          {
            tc[0] = {0.0f, 0.0f};
            tc[1] = {tc_u, 0.0f};
            tc[2] = {tc_u, tc_v};
            tc[3] = {0.0f, tc_v};
          }

          // Biome tint: grass, all leaf types, water
          const bool needsBiomeColoring =
              (quad_type == GRASS_TOP || quad_type == GRASS_SIDE ||
               blockIsFoliage(quad_type) || quad_type == WATER);

          const TextureType faceTex = blockFaceTexture(quad_type, quad_normal_dir.y);
          const float texture_idx_val = static_cast<float>(faceTex);

          uint32_t vert_indices[4];
          glm::vec3 quad_vertices_world[4] = {
              this->position + v0_local, this->position + v1_local,
              this->position + v2_local, this->position + v3_local};

          int normalIdx = 0;
          if (quad_normal_dir.x > 0)
            normalIdx = 0;
          else if (quad_normal_dir.x < 0)
            normalIdx = 1;
          else if (quad_normal_dir.y > 0)
            normalIdx = 2;
          else if (quad_normal_dir.y < 0)
            normalIdx = 3;
          else if (quad_normal_dir.z > 0)
            normalIdx = 4;
          else if (quad_normal_dir.z < 0)
            normalIdx = 5;

          uint32_t packedData = (normalIdx & 0x7) |
                                ((static_cast<uint32_t>(texture_idx_val) & 0xFF) << 3) |
                                (needsBiomeColoring ? (1 << 11) : 0);

          // Look up precomputed biome color from per-column arrays
          uint32_t packedColor = 0;
          if (needsBiomeColoring)
          {
            // Decided once per cell in the classification pass (issue #106):
            // the same owner color the merge compared.
            packedColor = originColor;
          }

          auto calculateAO = [&](const glm::vec3 &localPos, int cornerIdx) -> uint32_t
          {
            int pd = (int)std::round(localPos[d]);
            int pu = (int)std::round(localPos[u]);
            int pv = (int)std::round(localPos[v]);

            int layerD = (quad_normal_dir[d] > 0) ? pd : pd - 1;

            auto isSolid = [&](int du, int dv)
            {
              return !TextureManager::isTransparent(getVoxelDataForMeshing(
                  (d == 0 ? layerD : (u == 0 ? pu + du : pv + du)),
                  (d == 1 ? layerD : (u == 1 ? pu + du : pv + du)),
                  (d == 2 ? layerD : (u == 2 ? pu + du : pv + du))));
            };

            bool q1 = isSolid(0, 0);
            bool q2 = isSolid(-1, 0);
            bool q3 = isSolid(-1, -1);
            bool q4 = isSolid(0, -1);

            bool s1, s2, c;
            if (cornerIdx == 0)
            {
              s1 = q2;
              s2 = q4;
              c = q3;
            }
            else if (cornerIdx == 1)
            {
              s1 = q1;
              s2 = q3;
              c = q4;
            }
            else if (cornerIdx == 2)
            {
              s1 = q2;
              s2 = q4;
              c = q1;
            }
            else
            {
              s1 = q1;
              s2 = q3;
              c = q2;
            }

            if (s1 && s2)
              return 0;
            return 3 - (s1 + s2 + c);
          };

          // Determine which mesh buffer this quad goes to
          bool isWater = (quad_type == WATER);
          auto &targetVertices = isWater ? waterVertices : vertices;
          auto &targetIndices = isWater ? waterIndices : indices;
          auto &targetIndexCounter = isWater ? waterIndexCounter : indexCounter;

          // Sample light from air cell in front of the face (Minecraft-style)
          uint8_t faceSky = 15;
          uint8_t faceBlock = 0;
          {
            glm::ivec3 solid = quad_origin_voxel_coord;
            // Face sits between solid and air along normal
            glm::ivec3 airCell = solid;
            if (quad_normal_dir.x > 0)
              airCell.x += 1;
            else if (quad_normal_dir.x < 0)
              ; // solid already on + side; air is solid
            else if (quad_normal_dir.y > 0)
              airCell.y += 1;
            else if (quad_normal_dir.y < 0)
              ;
            else if (quad_normal_dir.z > 0)
              airCell.z += 1;

            // Prefer the open side of the face
            glm::ivec3 sample = solid;
            sample.x += static_cast<int>(quad_normal_dir.x);
            sample.y += static_cast<int>(quad_normal_dir.y);
            sample.z += static_cast<int>(quad_normal_dir.z);
            if (sample.x >= 0 && sample.x < CHUNK_SIZE && sample.y >= 0 && sample.y < CHUNK_HEIGHT &&
                sample.z >= 0 && sample.z < CHUNK_SIZE)
            {
              const size_t li = static_cast<size_t>(sample.x + CHUNK_SIZE * (sample.y + CHUNK_HEIGHT * sample.z));
              faceSky = skyLight[li];
              faceBlock = blockLight[li];
            }
            else
            {
              faceSky = 12;
              faceBlock = 0;
            }
            // Emissive solid itself glows
            if (solid.x >= 0 && solid.x < CHUNK_SIZE && solid.y >= 0 && solid.y < CHUNK_HEIGHT &&
                solid.z >= 0 && solid.z < CHUNK_SIZE)
            {
              faceBlock = std::max(faceBlock, lighting::blockLightEmission(getVoxel(solid.x, solid.y, solid.z).type));
            }
          }
          const uint32_t lightBits = lighting::packLightBits(faceSky, faceBlock);

          for (int i = 0; i < 4; ++i)
          {
            Vertex vert;
            vert.position = quad_vertices_world[i];

            glm::vec3 localPos = vert.position - this->position;
            uint32_t ao = calculateAO(localPos, i);
            ++meshSample.data.aoVertices;

            vert.packedData = packedData | (ao << 12) | lightBits;
            vert.texCoord = tc[i];
            vert.packedBiomeColor = packedColor;

            // I: Direct push for both water and opaque — greedy quads never share vertices
            targetVertices.push_back(vert);
            vert_indices[i] = targetIndexCounter++;
          }

          // Winding order based on normal direction along the main axis 'd'
          if (quad_normal_dir[d] > 0)
          {
            targetIndices.push_back(vert_indices[0]);
            targetIndices.push_back(vert_indices[1]);
            targetIndices.push_back(vert_indices[2]);
            targetIndices.push_back(vert_indices[0]);
            targetIndices.push_back(vert_indices[2]);
            targetIndices.push_back(vert_indices[3]);
          }
          else
          {
            targetIndices.push_back(vert_indices[0]);
            targetIndices.push_back(vert_indices[2]);
            targetIndices.push_back(vert_indices[1]);
            targetIndices.push_back(vert_indices[0]);
            targetIndices.push_back(vert_indices[3]);
            targetIndices.push_back(vert_indices[2]);
          }

          // Mark processed cells in the mask
          for (int iw = 0; iw < w; ++iw)
          {
            for (int ih = 0; ih < h; ++ih)
            {
              workspace.mask[(x[u] + iw) * dims[v] + (x[v] + ih)] = 1;
            }
          }
        }
      }
    }
  }

  // Accumulate across the per-section builds of one job (issue #107).
  meshSample.data.opaqueVertices += vertices.size();
  meshSample.data.opaqueIndices += indices.size();
  meshSample.data.waterVertices += waterVertices.size();
  meshSample.data.waterIndices += waterIndices.size();
}

bool Chunk::generateMesh()
{
  MeshBuildResult *result = nullptr;
  try
  {
    result = m_resultPool->acquire();
    const uint64_t generation = m_meshGeneration;
    const uint64_t revision = m_meshRevision.load(std::memory_order_relaxed);
    buildMesh(*result, generation, revision);
    m_resultPool->finishBuild(result);
  }
  catch (const std::bad_alloc &)
  {
    // Allocation failure: give the block back and retry later. Programming
    // errors must propagate, so only bad_alloc is handled here.
    if (result)
      result->homePool->release(result);
    return false;
  }
  // On rejection (superseded identity) the block is already back in its
  // pool and the chunk state is untouched.
  return publishMeshResult(result);
}

// K: Simplified LOD mesh — one top-face quad per non-empty XZ column.
// Max 256 opaque + 256 water quads vs thousands for a full greedy mesh.
void Chunk::buildLODMesh(MeshBuildResult &out, uint64_t generation, uint64_t revision)
{
  out.beginBuild(this, generation, revision);
  out.isLOD = true;
  // No column holds a voxel above the last occupied section (issue #105):
  // start every column scan there instead of at CHUNK_HEIGHT - 1.
  int occMinY = 0;
  int occMaxY = CHUNK_HEIGHT - 1;
  if (!occupiedSpanY(occMinY, occMaxY))
    return; // empty chunk: the result stays empty
  buildLODMeshRanged(out, occMaxY);
}

void Chunk::buildLODMeshRanged(MeshBuildResult &out, int scanTopY)
{
  // Production callers pass the top of the last occupied section; the probe
  // test passes CHUNK_HEIGHT - 1 (issue #115 review). Anything else would
  // silently skip occupied columns or read out of range.
  assert(scanTopY >= 0 && scanTopY < CHUNK_HEIGHT);
  telemetry::MeshSample meshSample(telemetry::Lod);
  auto &vertices = out.opaqueVertices;
  auto &indices = out.opaqueIndices;
  auto &waterVertices = out.waterVertices;
  auto &waterIndices = out.waterIndices;

  uint32_t indexCounter = 0;
  uint32_t waterIndexCounter = 0;

  for (int cx = 0; cx < CHUNK_SIZE; ++cx)
  {
    for (int cz = 0; cz < CHUNK_SIZE; ++cz)
    {
      // Find topmost non-AIR voxel in this column
      int topY = -1;
      TextureType topType = AIR;
      for (int cy = scanTopY; cy >= 0; --cy)
      {
        TextureType t = static_cast<TextureType>(getVoxel(cx, cy, cz).type);
        if (t != AIR)
        {
          topY = cy;
          topType = t;
          break;
        }
      }
      if (topY < 0)
        continue; // Empty column

      bool isWater = (topType == WATER);

      // Remap block type to its top-face texture
      const TextureType texType = blockTopFace(topType);

      const bool needsBiomeColoring = (topType == GRASS_TOP || topType == GRASS_SIDE ||
                                       blockIsFoliage(topType) || topType == WATER);
      int colIdx = cz * CHUNK_SIZE + cx;
      uint32_t packedColor = 0;
      if (needsBiomeColoring)
      {
        if (topType == GRASS_TOP || topType == GRASS_SIDE)
          packedColor = biomeGrassColors[colIdx];
        else if (blockIsFoliage(topType))
          packedColor = biomeFoliageColors[colIdx];
        else
          packedColor = WATER_COLOR;
      }

      // normalIdx=2 (+Y), ao=3 (no occlusion — skip expensive AO for LOD)
      // Full sky light for distant LOD columns (block light 0)
      uint32_t packedData = (2u & 0x7u) |
                            ((static_cast<uint32_t>(texType) & 0xFFu) << 3) |
                            (needsBiomeColoring ? (1u << 11) : 0u) |
                            (3u << 12) |
                            lighting::packLightBits(15, 0);

      // Top face vertices in world space; axis mapping: d=1(Y), u=2(Z), v=0(X)
      float fy = float(topY + 1);
      float fx = float(cx);
      float fz = float(cz);

      Vertex v0, v1, v2, v3;
      v0.position = this->position + glm::vec3(fx, fy, fz);
      v1.position = this->position + glm::vec3(fx, fy, fz + 1.f);
      v2.position = this->position + glm::vec3(fx + 1.f, fy, fz + 1.f);
      v3.position = this->position + glm::vec3(fx + 1.f, fy, fz);

      // swapUV=true (d=1), w=1, h=1 -> tc_u=1, tc_v=1
      v0.texCoord = {0.f, 0.f};
      v1.texCoord = {0.f, 1.f};
      v2.texCoord = {1.f, 1.f};
      v3.texCoord = {1.f, 0.f};

      v0.packedData = v1.packedData = v2.packedData = v3.packedData = packedData;
      v0.packedBiomeColor = v1.packedBiomeColor =
          v2.packedBiomeColor = v3.packedBiomeColor = packedColor;

      auto &tVerts = isWater ? waterVertices : vertices;
      auto &tIndices = isWater ? waterIndices : indices;
      auto &cnt = isWater ? waterIndexCounter : indexCounter;

      uint32_t base = cnt;
      tVerts.push_back(v0);
      tVerts.push_back(v1);
      tVerts.push_back(v2);
      tVerts.push_back(v3);
      cnt += 4;

      // Winding for +Y normal (quad_normal_dir[d] > 0)
      tIndices.push_back(base + 0);
      tIndices.push_back(base + 1);
      tIndices.push_back(base + 2);
      tIndices.push_back(base + 0);
      tIndices.push_back(base + 2);
      tIndices.push_back(base + 3);
    }
  }

  meshSample.data.opaqueVertices = vertices.size();
  meshSample.data.opaqueIndices = indices.size();
  meshSample.data.waterVertices = waterVertices.size();
  meshSample.data.waterIndices = waterIndices.size();
}

bool Chunk::generateLODMesh()
{
  MeshBuildResult *result = nullptr;
  try
  {
    result = m_resultPool->acquire();
    const uint64_t generation = m_meshGeneration;
    const uint64_t revision = m_meshRevision.load(std::memory_order_relaxed);
    buildLODMesh(*result, generation, revision);
    m_resultPool->finishBuild(result);
  }
  catch (const std::bad_alloc &)
  {
    if (result)
      result->homePool->release(result);
    return false;
  }
  return publishMeshResult(result);
}

bool Chunk::publishMeshResult(MeshBuildResult *result)
{
  if (!result)
    return false;
  // Publish-time identity validation (issue #104/#114): a result built by
  // another chunk, for a retired incarnation (generation), or from older
  // voxel/border content (revision) must never land here. The block goes
  // straight back to its pool and NO chunk state is touched - a stale
  // publish cannot mark the chunk MESHED or clobber a newer pending mesh.
  // The built sections are re-armed so the superseded geometry is rebuilt
  // (issue #107: a rejected section build must not leave its mask empty).
  if (result->owner != this ||
      result->generation != m_meshGeneration ||
      result->revision != m_meshRevision.load(std::memory_order_relaxed))
  {
    m_dirtySections.fetch_or(result->sectionsBuilt, std::memory_order_relaxed);
    result->homePool->release(result);
    return false;
  }
  // Repeated remesh without upload: the newest complete build wins and the
  // previous one returns to the pool.
  if (m_pendingResult && m_pendingResult != result)
    m_pendingResult->homePool->release(m_pendingResult);
  m_pendingResult = result;
  // Single state commit point: the mesh becomes official here, on the main
  // thread, only after validation succeeded.
  m_isLODMesh = result->isLOD;
  state = ChunkState::MESHED;
  meshNeedsUpdate.store(true);
  return true;
}

void Chunk::releasePendingMeshResult()
{
  if (!m_pendingResult)
    return;
  MeshBuildResult *result = m_pendingResult;
  m_pendingResult = nullptr;
  result->homePool->release(result);
}

void Chunk::releaseGPU()
{
  if (m_allocator == VK_NULL_HANDLE)
    return;
  destroyBuffer(m_allocator, vertexBuffer);
  destroyBuffer(m_allocator, indexBuffer);
  destroyBuffer(m_allocator, waterVertexBuffer);
  destroyBuffer(m_allocator, waterIndexBuffer);
  m_allocator = VK_NULL_HANDLE;
  opaqueIndexCount = 0;
  waterIndexCount = 0;
  // Section slots lived inside the destroyed buffers (issue #107).
  m_sectionGpu.fill({});
  m_sectionGpuWater.fill({});
  m_vertexUsedBytes = 0;
  m_indexUsedBytes = 0;
  m_waterVertexUsedBytes = 0;
  m_waterIndexUsedBytes = 0;
}

namespace
{
inline uint32_t alignUpBytes(uint32_t value, uint32_t alignment)
{
  return (value + alignment - 1) / alignment * alignment;
}
// Slot reservation for a (re)allocated section: live payload + 25% headroom
// so bursts of edits in one section mostly re-stage in place instead of
// appending new slots. Vertex reservations stay multiples of sizeof(Vertex)
// so the section's index base is exact.
inline uint32_t vertexSlotCapacity(uint32_t payloadBytes)
{
  return std::max<uint32_t>(alignUpBytes(payloadBytes + payloadBytes / 4, sizeof(Vertex)),
                            4 * sizeof(Vertex));
}
inline uint32_t indexSlotCapacity(uint32_t payloadBytes)
{
  return std::max<uint32_t>(alignUpBytes(payloadBytes + payloadBytes / 4, sizeof(uint32_t)),
                            6 * sizeof(uint32_t));
}
} // namespace

// Shared sectioned-upload core (issue #107): places the build result's
// section payloads into the chunk-level vertex/index buffer pairs. Each
// section owns a slot (offset + reservation, indices stored rebased onto
// the section's vertex base); a payload that fits its slot is re-staged in
// place, an outgrown payload gets a fresh slot APPENDED at the end of the
// used region - existing slots never move, so stored indices stay valid
// without a rewrite. When capacity runs out the buffer is re-created and
// its used region copied to offset 0 (plain bytes; offsets are unchanged).
//
// staging/cmd/retire carry the async frame path; imm carries the
// bootstrap/test path. Returns false only when the async staging ring is
// full: the result stays attached, already-recorded copies are idempotent,
// and the upload retries next frame.
bool Chunk::uploadSectionSlots(MeshBuildResult &result, VmaAllocator allocator,
                               StagingRing *staging, VkCommandBuffer cmd,
                               GpuResourceRetire *retire, ImmediateCommands *imm)
{
  // Three passes keep a staging failure free of GPU-state mutation:
  //   1. plan every masked section (in place vs appended slot) and reserve +
  //      fill the staging slices - the vertex bases of appended slots are
  //      deterministic here, so rebased indices are written once;
  //   2. create/grow the buffers (the whole old buffer is copied to offset 0
  //      of the new one, so every existing slot keeps its offset);
  //   3. record the copies (or upload directly on the bootstrap path) and
  //      commit the slot bookkeeping.
  uint64_t stagedVertexBytes = 0;
  uint64_t stagedIndexBytes = 0;
  std::vector<uint32_t> rebased;    // bootstrap-path index rebase scratch
  std::vector<uint32_t> zeroScratch; // bootstrap-path zero fill scratch

  struct SectionPlan
  {
    bool active{false};  // masked and holds content
    bool clear{false};   // masked but emptied: clear the slot content
    bool inPlace{false};
    uint32_t vBytes{0};
    uint32_t iBytes{0};
    uint32_t vDst{0};
    uint32_t iDst{0};
    uint32_t vBase{0};
    uint32_t newVSlot{0};
    uint32_t newISlot{0};
    // Index-stream bytes that must read as zero inside the drawn range
    // [0, indexUsed): the slot's unused slack, and (for appended slots) the
    // abandoned previous slot. Keeps the single chunk-level drawIndexed
    // correct without ever compacting the buffer.
    uint32_t zeroIOff{0};
    uint32_t zeroIBytes{0};
    uint32_t clearIOff{0};
    uint32_t clearIBytes{0};
    VkDeviceSize stageVOff{0};
    void *stageVPtr{nullptr};
    VkDeviceSize stageIOff{0};
    void *stageIPtr{nullptr};
    VkDeviceSize stageZeroOff{0};
    void *stageZeroPtr{nullptr};
  };

  auto uploadStream = [&](auto payloadSel, AllocatedBuffer &vertexBuf,
                          AllocatedBuffer &indexBuf,
                          std::array<SectionGpuSlot, kOccupancySections> &slots,
                          uint32_t &vertexUsed, uint32_t &indexUsed,
                          VkBufferUsageFlags vertexUsage, VkBufferUsageFlags indexUsage,
                          telemetry::Gauge vertexGauge, telemetry::Gauge indexGauge) -> bool
  {
    SectionPlan plans[kOccupancySections]{};
    const bool freshStream = (vertexBuf.buffer == VK_NULL_HANDLE);
    // A stream without live slots (fresh, or a whole-buffer LOD mesh that a
    // sectioned upload replaces) is laid out from scratch with EXACT slot
    // sizes - never-edited chunks must carry no reservation waste (issue
    // #107 review). Only edit-driven appends on an existing layout buy
    // headroom.
    const bool exactSizing = freshStream || vertexUsed == 0;
    // Cursor for appended slots; appended slots never move existing ones.
    uint32_t curV = vertexUsed;
    uint32_t curI = indexUsed;

    // Pass 1: plan + reserve staging before touching any GPU state.
    uint32_t appendCapV = 0;
    uint32_t appendCapI = 0;
    for (int s = 0; s < kOccupancySections; ++s)
    {
      SectionPlan &p = plans[s];
      if (((result.sectionsBuilt >> s) & 1u) == 0)
        continue;
      const auto [verts, idxs] = payloadSel(s);
      p.vBytes = static_cast<uint32_t>(verts->size() * sizeof(Vertex));
      p.iBytes = static_cast<uint32_t>(idxs->size() * sizeof(uint32_t));
      if (p.vBytes == 0 && p.iBytes == 0)
      {
        p.clear = true;
        continue;
      }
      p.active = true;
      if (!freshStream && p.vBytes <= slots[s].vertexSlotBytes &&
          p.iBytes <= slots[s].indexSlotBytes)
      {
        p.inPlace = true;
        p.vDst = slots[s].vertexOffset;
        p.iDst = slots[s].indexOffset;
        p.vBase = slots[s].vertexBase;
      }
      else
      {
        p.newVSlot = exactSizing ? alignUpBytes(p.vBytes, sizeof(Vertex))
                                 : vertexSlotCapacity(p.vBytes);
        p.newISlot = exactSizing ? alignUpBytes(p.iBytes, sizeof(uint32_t))
                                 : indexSlotCapacity(p.iBytes);
        if (slots[s].indexSlotBytes != 0)
        {
          // The replaced slot is abandoned: its byte range must read as
          // zero inside the drawn range.
          p.clearIOff = slots[s].indexOffset;
          p.clearIBytes = slots[s].indexSlotBytes;
        }
        p.vDst = curV;
        p.vBase = curV / sizeof(Vertex);
        curV += p.newVSlot;
        p.iDst = curI;
        curI += p.newISlot;
        appendCapV += p.newVSlot;
        appendCapI += p.newISlot;
      }
      // The slot slack sits inside the drawn range: zero it.
      p.zeroIOff = p.iDst + p.iBytes;
      p.zeroIBytes = (p.inPlace ? slots[s].indexSlotBytes : p.newISlot) - p.iBytes;
      if (staging)
      {
        if (p.vBytes != 0)
        {
          if (!staging->alloc(p.vBytes, p.stageVOff, p.stageVPtr))
            return false;
          std::memcpy(p.stageVPtr, verts->data(), p.vBytes);
        }
        if (p.iBytes != 0)
        {
          if (!staging->alloc(p.iBytes, p.stageIOff, p.stageIPtr))
            return false;
          uint32_t *out = static_cast<uint32_t *>(p.stageIPtr);
          for (size_t k = 0; k < idxs->size(); ++k)
            out[k] = (*idxs)[k] + p.vBase;
        }
        const VkDeviceSize zeroTotal =
            static_cast<VkDeviceSize>(p.zeroIBytes + p.clearIBytes);
        if (zeroTotal != 0)
        {
          if (!staging->alloc(zeroTotal, p.stageZeroOff, p.stageZeroPtr))
            return false;
          std::memset(p.stageZeroPtr, 0, static_cast<size_t>(zeroTotal));
        }
      }
    }

    // Pass 2: buffer creation/growth (offset-preserving).
    if (freshStream)
    {
      if (appendCapV == 0 && appendCapI == 0)
        return true; // stream carries no masked content
      vertexBuf = createBuffer(allocator, std::max<VkDeviceSize>(appendCapV, 1),
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                               VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
      trackMeshBuffer(vertexBuf, vertexGauge);
      indexBuf = createBuffer(allocator, std::max<VkDeviceSize>(appendCapI, 1),
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
      trackMeshBuffer(indexBuf, indexGauge);
      vertexUsed = 0;
      indexUsed = 0;
    }
    else
    {
      auto grow = [&](AllocatedBuffer &buf, uint32_t liveBytes, uint32_t needed,
                      VkBufferUsageFlags usage, telemetry::Gauge gauge)
      {
        if (static_cast<VkDeviceSize>(needed) <= buf.size)
          return;
        const VkDeviceSize newCapacity =
            std::max<VkDeviceSize>(needed, buf.size * 2);
        AllocatedBuffer nb = createBuffer(allocator, newCapacity,
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
                                          VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
        trackMeshBuffer(nb, gauge);
        // Copy only the live region [0, liveBytes): every slot keeps its
        // offset, and bytes beyond it were never live.
        const VkDeviceSize bytes = liveBytes;
        if (bytes != 0 && staging)
        {
          VkBufferCopy copy{};
          copy.srcOffset = 0;
          copy.dstOffset = 0;
          copy.size = bytes;
          vkCmdCopyBuffer(cmd, buf.buffer, nb.buffer, 1, &copy);
          retire->retireBuffer(buf);
        }
        else
        {
          if (bytes != 0)
          {
            imm->submitAndWait([&](VkCommandBuffer c)
            {
              VkBufferCopy copy{};
              copy.srcOffset = 0;
              copy.dstOffset = 0;
              copy.size = bytes;
              vkCmdCopyBuffer(c, buf.buffer, nb.buffer, 1, &copy);
            });
          }
          destroyBuffer(allocator, buf);
        }
        buf = nb;
      };
      grow(vertexBuf, vertexUsed, curV, vertexUsage, vertexGauge);
      grow(indexBuf, indexUsed, curI, indexUsage, indexGauge);
    }

    // Pass 3: record/upload and commit slot bookkeeping.
    for (int s = 0; s < kOccupancySections; ++s)
    {
      SectionPlan &p = plans[s];
      SectionGpuSlot &slot = slots[s];
      if (!p.active && !p.clear)
        continue;
      if (p.clear)
      {
        // Emptied section: zero its whole index slot (it stays reserved).
        if (slot.indexSlotBytes != 0)
        {
          if (staging)
          {
            VkBufferCopy copy{};
            copy.srcOffset = p.stageZeroOff;
            copy.dstOffset = slot.indexOffset;
            copy.size = slot.indexSlotBytes;
            vkCmdCopyBuffer(cmd, staging->buffer(), indexBuf.buffer, 1, &copy);
          }
          else
          {
            zeroScratch.assign(slot.indexSlotBytes / sizeof(uint32_t), 0);
            uploadBuffer(allocator, *imm, indexBuf, zeroScratch.data(),
                         slot.indexSlotBytes, slot.indexOffset);
          }
        }
        slot.vertexUsedBytes = 0;
        slot.indexUsedBytes = 0;
        slot.indexCount = 0;
        continue;
      }
      if (staging)
      {
        if (p.vBytes != 0)
        {
          VkBufferCopy copy{};
          copy.srcOffset = p.stageVOff;
          copy.dstOffset = p.vDst;
          copy.size = p.vBytes;
          vkCmdCopyBuffer(cmd, staging->buffer(), vertexBuf.buffer, 1, &copy);
          stagedVertexBytes += p.vBytes;
        }
        if (p.iBytes != 0)
        {
          VkBufferCopy copy{};
          copy.srcOffset = p.stageIOff;
          copy.dstOffset = p.iDst;
          copy.size = p.iBytes;
          vkCmdCopyBuffer(cmd, staging->buffer(), indexBuf.buffer, 1, &copy);
          stagedIndexBytes += p.iBytes;
        }
        VkDeviceSize zeroDone = 0;
        auto recordZero = [&](VkDeviceSize dstOff, VkDeviceSize bytes)
        {
          if (bytes == 0)
            return;
          VkBufferCopy copy{};
          copy.srcOffset = p.stageZeroOff + zeroDone;
          copy.dstOffset = dstOff;
          copy.size = bytes;
          vkCmdCopyBuffer(cmd, staging->buffer(), indexBuf.buffer, 1, &copy);
          zeroDone += bytes;
        };
        recordZero(p.zeroIOff, p.zeroIBytes);
        recordZero(p.clearIOff, p.clearIBytes);
        stagedIndexBytes += zeroDone;
      }
      else
      {
        const auto [verts, idxs] = payloadSel(s);
        if (p.vBytes != 0)
          uploadBuffer(allocator, *imm, vertexBuf, verts->data(), p.vBytes, p.vDst);
        if (p.iBytes != 0)
        {
          rebased.resize(idxs->size());
          for (size_t k = 0; k < idxs->size(); ++k)
            rebased[k] = (*idxs)[k] + p.vBase;
          uploadBuffer(allocator, *imm, indexBuf, rebased.data(), p.iBytes, p.iDst);
        }
        if (p.zeroIBytes != 0)
        {
          zeroScratch.assign(p.zeroIBytes / sizeof(uint32_t), 0);
          uploadBuffer(allocator, *imm, indexBuf, zeroScratch.data(), p.zeroIBytes, p.zeroIOff);
        }
        if (p.clearIBytes != 0)
        {
          zeroScratch.assign(p.clearIBytes / sizeof(uint32_t), 0);
          uploadBuffer(allocator, *imm, indexBuf, zeroScratch.data(), p.clearIBytes, p.clearIOff);
        }
      }
      if (!p.inPlace)
      {
        slot.vertexOffset = p.vDst;
        slot.vertexSlotBytes = p.newVSlot;
        slot.vertexBase = p.vBase;
        slot.indexOffset = p.iDst;
        slot.indexSlotBytes = p.newISlot;
      }
      slot.vertexUsedBytes = p.vBytes;
      slot.indexUsedBytes = p.iBytes;
      slot.indexCount = p.iBytes / sizeof(uint32_t);
    }
    // Commit the packed extent (covers live slots + zeroed gaps). When the
    // layout was rebuilt from scratch (no live slots before), the cursor
    // started at 0; otherwise it only grew.
    vertexUsed = exactSizing ? curV : std::max(vertexUsed, curV);
    indexUsed = exactSizing ? curI : std::max(indexUsed, curI);
    return true;
  };

  auto opaquePayload = [&result](int s)
  {
    return std::make_pair(&result.sections[s].opaqueVertices,
                          &result.sections[s].opaqueIndices);
  };
  auto waterPayload = [&result](int s)
  {
    return std::make_pair(&result.sections[s].waterVertices,
                          &result.sections[s].waterIndices);
  };

  if (!uploadStream(opaquePayload, vertexBuffer, indexBuffer, m_sectionGpu,
                    m_vertexUsedBytes, m_indexUsedBytes,
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    telemetry::GpuOpaqueVertex, telemetry::GpuOpaqueIndex))
    return false;
  if (!uploadStream(waterPayload, waterVertexBuffer, waterIndexBuffer, m_sectionGpuWater,
                    m_waterVertexUsedBytes, m_waterIndexUsedBytes,
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    telemetry::GpuWaterVertex, telemetry::GpuWaterIndex))
    return false;

  // The single chunk-level draw covers the whole packed index region; its
  // gaps are zeroed (degenerate triangles), so the drawn count derives from
  // the used byte extent rather than the per-section counts.
  opaqueIndexCount = m_indexUsedBytes / sizeof(uint32_t);
  waterIndexCount = m_waterIndexUsedBytes / sizeof(uint32_t);

  telemetry::registry().add(telemetry::UploadVertexBytes, stagedVertexBytes);
  telemetry::registry().add(telemetry::UploadIndexBytes, stagedIndexBytes);
  return true;
}
void Chunk::uploadToGPU(VmaAllocator allocator, ImmediateCommands &imm)
{
  MemoryPublication memoryPublication{*this};
  if (allocator == VK_NULL_HANDLE)
    throw std::runtime_error("Chunk::uploadToGPU: null allocator");

  // Bootstrap path: immediate destroy is OK (caller waited idle or nothing draws yet).
  if (m_allocator != VK_NULL_HANDLE)
    releaseGPU();
  m_allocator = allocator;

  // The CPU payload lives in the attached build result (issue #104).
  MeshBuildResult *result = m_pendingResult;
  if (result && result->isLOD)
  {
    // Whole-chunk LOD mesh: one buffer pair, no slot layout (issue #107).
    auto &vertices = result->opaqueVertices;
    auto &indices = result->opaqueIndices;
    auto &waterVertices = result->waterVertices;
    auto &waterIndices = result->waterIndices;

    opaqueIndexCount = static_cast<uint32_t>(indices.size());
    if (!vertices.empty() && opaqueIndexCount > 0)
    {
      const VkDeviceSize vSize = vertices.size() * sizeof(Vertex);
      const VkDeviceSize iSize = indices.size() * sizeof(uint32_t);
      vertexBuffer = createBuffer(allocator, vSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
      trackMeshBuffer(vertexBuffer, telemetry::GpuOpaqueVertex);
      indexBuffer = createBuffer(allocator, iSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
      trackMeshBuffer(indexBuffer, telemetry::GpuOpaqueIndex);
      uploadBuffer(allocator, imm, vertexBuffer, vertices.data(), vSize);
      uploadBuffer(allocator, imm, indexBuffer, indices.data(), iSize);
    }

    waterIndexCount = static_cast<uint32_t>(waterIndices.size());
    if (!waterVertices.empty() && waterIndexCount > 0)
    {
      const VkDeviceSize vSize = waterVertices.size() * sizeof(Vertex);
      const VkDeviceSize iSize = waterIndices.size() * sizeof(uint32_t);
      waterVertexBuffer = createBuffer(allocator, vSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
      trackMeshBuffer(waterVertexBuffer, telemetry::GpuWaterVertex);
      waterIndexBuffer = createBuffer(allocator, iSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
      trackMeshBuffer(waterIndexBuffer, telemetry::GpuWaterIndex);
      uploadBuffer(allocator, imm, waterVertexBuffer, waterVertices.data(), vSize);
      uploadBuffer(allocator, imm, waterIndexBuffer, waterIndices.data(), iSize);
    }
  }
  else if (result)
  {
    // Full-quality sectioned upload (issue #107): slot per section.
    uploadSectionSlots(*result, allocator, nullptr, VK_NULL_HANDLE, nullptr, &imm);
  }
  else
  {
    opaqueIndexCount = 0;
    waterIndexCount = 0;
  }

  releasePendingMeshResult();

  releaseNeighborBorders();
  meshNeedsUpdate = false;
}

bool Chunk::uploadToGPUAsync(VmaAllocator allocator, StagingRing &staging, VkCommandBuffer cmd,
                             GpuResourceRetire &retire)
{
  MemoryPublication memoryPublication{*this};
  if (allocator == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE || !staging.isValid())
    return false;

  m_allocator = allocator;

  // The CPU payload lives in the attached build result (issue #104). When
  // staging is full the result stays attached - the completed mesh is
  // neither lost nor copied - and the upload retries next frame.
  MeshBuildResult *result = m_pendingResult;
  if (result && result->isLOD)
  {
    // Whole-chunk LOD mesh: one buffer pair, no slot layout (issue #107).
    const uint32_t newOpaqueCount = static_cast<uint32_t>(result->opaqueIndices.size());
    const uint32_t newWaterCount = static_cast<uint32_t>(result->waterIndices.size());

    AllocatedBuffer newV{}, newI{}, newWV{}, newWI{};
    std::vector<VkBufferCopy> copies;
    copies.reserve(4);
    struct CopyJob
    {
      VkBuffer dst;
      VkDeviceSize srcOffset;
      VkDeviceSize size;
    };
    std::vector<CopyJob> jobs;
    jobs.reserve(4);

    auto stageInto = [&](const void *data, VkDeviceSize size, AllocatedBuffer &dstBuf,
                         VkBufferUsageFlags usage, telemetry::Gauge kind) -> bool {
      if (!data || size == 0)
        return true;
      dstBuf = createBuffer(allocator, size,
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
                            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
      trackMeshBuffer(dstBuf, kind);
      VkDeviceSize off = 0;
      void *ptr = nullptr;
      if (!staging.alloc(size, off, ptr))
      {
        destroyBuffer(allocator, dstBuf);
        return false;
      }
      std::memcpy(ptr, data, static_cast<size_t>(size));
      jobs.push_back({dstBuf.buffer, off, size});
      return true;
    };

    const bool needOpaque = !result->opaqueVertices.empty() && newOpaqueCount > 0;
    const bool needWater = !result->waterVertices.empty() && newWaterCount > 0;

    if (needOpaque)
    {
      const VkDeviceSize vSize = result->opaqueVertices.size() * sizeof(Vertex);
      const VkDeviceSize iSize = result->opaqueIndices.size() * sizeof(uint32_t);
      if (!stageInto(result->opaqueVertices.data(), vSize, newV, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, telemetry::GpuOpaqueVertex) ||
          !stageInto(result->opaqueIndices.data(), iSize, newI, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, telemetry::GpuOpaqueIndex))
      {
        destroyBuffer(allocator, newV);
        destroyBuffer(allocator, newI);
        telemetry::registry().add(telemetry::UploadDeferred);
        return false;
      }
    }

    if (needWater)
    {
      const VkDeviceSize vSize = result->waterVertices.size() * sizeof(Vertex);
      const VkDeviceSize iSize = result->waterIndices.size() * sizeof(uint32_t);
      if (!stageInto(result->waterVertices.data(), vSize, newWV, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, telemetry::GpuWaterVertex) ||
          !stageInto(result->waterIndices.data(), iSize, newWI, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, telemetry::GpuWaterIndex))
      {
        destroyBuffer(allocator, newV);
        destroyBuffer(allocator, newI);
        destroyBuffer(allocator, newWV);
        destroyBuffer(allocator, newWI);
        telemetry::registry().add(telemetry::UploadDeferred);
        return false;
      }
    }

    telemetry::registry().add(telemetry::UploadChunks);
    telemetry::registry().add(telemetry::UploadVertexBytes, newV.size + newWV.size);
    telemetry::registry().add(telemetry::UploadIndexBytes, newI.size + newWI.size);
    for (const auto &j : jobs)
    {
      VkBufferCopy copy{};
      copy.srcOffset = j.srcOffset;
      copy.dstOffset = 0;
      copy.size = j.size;
      vkCmdCopyBuffer(cmd, staging.buffer(), j.dst, 1, &copy);
    }

    // Retire previous GPU meshes (still referenced by in-flight frames).
    if (vertexBuffer.buffer != VK_NULL_HANDLE)
      retire.retireBuffer(vertexBuffer);
    if (indexBuffer.buffer != VK_NULL_HANDLE)
      retire.retireBuffer(indexBuffer);
    if (waterVertexBuffer.buffer != VK_NULL_HANDLE)
      retire.retireBuffer(waterVertexBuffer);
    if (waterIndexBuffer.buffer != VK_NULL_HANDLE)
      retire.retireBuffer(waterIndexBuffer);

    vertexBuffer = newV;
    indexBuffer = newI;
    waterVertexBuffer = newWV;
    waterIndexBuffer = newWI;
    opaqueIndexCount = needOpaque ? newOpaqueCount : 0;
    waterIndexCount = needWater ? newWaterCount : 0;
    // Whole-buffer mesh: no valid section slots (issue #107).
    m_sectionGpu.fill({});
    m_sectionGpuWater.fill({});
    m_vertexUsedBytes = 0;
    m_indexUsedBytes = 0;
    m_waterVertexUsedBytes = 0;
    m_waterIndexUsedBytes = 0;
  }
  else if (result)
  {
    // Full-quality sectioned upload (issue #107): only the built sections'
    // slots are re-staged; a full staging ring defers the whole upload.
    if (!uploadSectionSlots(*result, allocator, &staging, cmd, &retire, nullptr))
    {
      telemetry::registry().add(telemetry::UploadDeferred);
      return false;
    }
    telemetry::registry().add(telemetry::UploadChunks);
  }
  else
  {
    opaqueIndexCount = 0;
    waterIndexCount = 0;
  }

  releasePendingMeshResult();

  releaseNeighborBorders();
  meshNeedsUpdate = false;
  return true;
}

void Chunk::releaseGPUDeferred(GpuResourceRetire &retire)
{
  if (vertexBuffer.buffer != VK_NULL_HANDLE)
    retire.retireBuffer(vertexBuffer);
  if (indexBuffer.buffer != VK_NULL_HANDLE)
    retire.retireBuffer(indexBuffer);
  if (waterVertexBuffer.buffer != VK_NULL_HANDLE)
    retire.retireBuffer(waterVertexBuffer);
  if (waterIndexBuffer.buffer != VK_NULL_HANDLE)
    retire.retireBuffer(waterIndexBuffer);
  vertexBuffer = {};
  indexBuffer = {};
  waterVertexBuffer = {};
  waterIndexBuffer = {};
  m_allocator = VK_NULL_HANDLE;
  opaqueIndexCount = 0;
  waterIndexCount = 0;
  // Section slots lived inside the retired buffers (issue #107).
  m_sectionGpu.fill({});
  m_sectionGpuWater.fill({});
  m_vertexUsedBytes = 0;
  m_indexUsedBytes = 0;
  m_waterVertexUsedBytes = 0;
  m_waterIndexUsedBytes = 0;
}

uint32_t Chunk::draw(VkCommandBuffer cmd)
{
  if (opaqueIndexCount == 0 || vertexBuffer.buffer == VK_NULL_HANDLE)
    return 0;

  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer.buffer, &offset);
  vkCmdBindIndexBuffer(cmd, indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  telemetry::registry().add(telemetry::OpaqueDraws);
  vkCmdDrawIndexed(cmd, opaqueIndexCount, 1, 0, 0, 0);
  return opaqueIndexCount;
}

uint32_t Chunk::drawWater(VkCommandBuffer cmd)
{
  if (waterIndexCount == 0 || waterVertexBuffer.buffer == VK_NULL_HANDLE)
    return 0;

  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &waterVertexBuffer.buffer, &offset);
  vkCmdBindIndexBuffer(cmd, waterIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  telemetry::registry().add(telemetry::WaterDraws);
  vkCmdDrawIndexed(cmd, waterIndexCount, 1, 0, 0, 0);
  return waterIndexCount;
}

void Chunk::drawShadow(VkCommandBuffer cmd, unsigned cascade) const
{
  if (opaqueIndexCount == 0 || meshNeedsUpdate.load() || vertexBuffer.buffer == VK_NULL_HANDLE)
    return;

  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer.buffer, &offset);
  vkCmdBindIndexBuffer(cmd, indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
  telemetry::registry().add(static_cast<telemetry::Event>(telemetry::Shadow0 + std::min(cascade, 2u)));
  vkCmdDrawIndexed(cmd, opaqueIndexCount, 1, 0, 0, 0);
}

void Chunk::releaseNeighborBorders()
{
  if (!m_borders)
    return;
  MemoryPublication memoryPublication{*this};
  // True release (issue #103): border blocks live in a pool, so the block is
  // reusable by other chunks instead of ~17 KiB staying attached here for
  // the remainder of the pool lifetime. A later remesh re-borrows and
  // rebuilds from neighbors (AIR where neighbors are missing).
  ChunkNeighborBorders *b = m_borders;
  m_borders = nullptr;
  m_borderPool->release(b);
  publishCpuTelemetry();
}

void Chunk::rebuildBordersFromNeighbors(const Chunk *west, const Chunk *east,
                                      const Chunk *south, const Chunk *north)
{
  MemoryPublication memoryPublication{*this};
  if (!m_borders)
  {
    try
    {
      m_borders = m_borderPool->acquire();
    }
    catch (const std::bad_alloc &)
    {
      return; // leave borders missing: mesher samples AIR (documented default)
    }
    publishCpuTelemetry();
  }
  ChunkNeighborBorders &b = *m_borders;
  b.resetToAir(); // faces and corners start as AIR for this rebuild

  // West face  (local x = -1):  neighbor's x = CHUNK_SIZE-1
  if (west)
    for (uint32_t y = 0; y < CHUNK_HEIGHT; ++y)
      for (uint32_t z = 0; z < CHUNK_SIZE; ++z)
        b.west[y * CHUNK_SIZE + z] = west->getVoxel(CHUNK_SIZE - 1, y, z).type;

  // East face  (local x = CHUNK_SIZE): neighbor's x = 0
  if (east)
    for (uint32_t y = 0; y < CHUNK_HEIGHT; ++y)
      for (uint32_t z = 0; z < CHUNK_SIZE; ++z)
        b.east[y * CHUNK_SIZE + z] = east->getVoxel(0, y, z).type;

  // South face (local z = -1):  neighbor's z = CHUNK_SIZE-1
  if (south)
    for (uint32_t y = 0; y < CHUNK_HEIGHT; ++y)
      for (uint32_t x = 0; x < CHUNK_SIZE; ++x)
        b.south[y * CHUNK_SIZE + x] = south->getVoxel(x, y, CHUNK_SIZE - 1).type;

  // North face (local z = CHUNK_SIZE): neighbor's z = 0
  if (north)
    for (uint32_t y = 0; y < CHUNK_HEIGHT; ++y)
      for (uint32_t x = 0; x < CHUNK_SIZE; ++x)
        b.north[y * CHUNK_SIZE + x] = north->getVoxel(x, y, 0).type;

  // Corner columns stay AIR exactly as in the previous rebuild path, which
  // never populated the diagonal shell columns either.

  // Border content changed: invalidate in-flight results stamped with the
  // previous revision (issue #114 review).
  m_meshRevision.fetch_add(1, std::memory_order_relaxed);
}

void Chunk::reset(const glm::vec3 &newPosition, ResetMode mode)
{
  MemoryPublication memoryPublication{*this};
  // Drop GPU meshes — next upload recreates VMA buffers.
  releaseGPU();
  opaqueIndexCount = 0;
  waterIndexCount = 0;

  // Reset identity
  position = newPosition;
  visible = false;
  state.store(ChunkState::UNLOADED);
  meshNeedsUpdate.store(true);
  m_isLODMesh = false;
  m_inTransit.store(false);
  m_activeIndex = SIZE_MAX;
  m_dirtySections.store(0, std::memory_order_relaxed);

  // A recycled chunk is a new mesh identity (issue #104/#114): any attached
  // build result goes back to the pool, and in-flight results for the old
  // incarnation are rejected at publish time via both counters.
  releasePendingMeshResult();
  ++m_meshGeneration;
  m_meshRevision.fetch_add(1, std::memory_order_relaxed);

  // Full reset returns voxel storage to the pool for retirement.
  // ForGeneration retains dirty storage until generateTerrain() overwrites it.
  if (mode == ResetMode::Full)
  {
    releaseVoxelStorageOnRetire();
  }

  // Empty occupancy metadata for the recycled incarnation; generation
  // recounts it (issue #105).
  m_sectionNonAir.fill(0);

  // Return transient neighbor-border storage to the BorderPool.
  // No border capacity remains attached to this Chunk.
  releaseNeighborBorders();

  // Reset biome colors and per-column generation state
  biomeGrassColors.fill(0);
  biomeFoliageColors.fill(0);
  biomeTypes.fill(static_cast<BiomeType>(0));
  heightMap.fill(0);
}


void Chunk::publishCpuTelemetry()
{
  if (!telemetry::registry().enabled) return;
  // Slots 3..10 (cpu.opaque/water.* mesh bytes) are always zero here since
  // issue #104: mesh build buffers live in MeshResultPool blocks, not on
  // chunks. The engine publishes them (and CpuMeshCapacity) from
  // MeshResultPoolStats.
  const std::array<uint64_t, 13> current{
    m_storage ? sizeof(VoxelStorage) : 0,
    m_borders ? sizeof(ChunkNeighborBorders) : 0,
    m_borders ? sizeof(ChunkNeighborBorders) : 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    sizeof(biomeGrassColors)+sizeof(biomeFoliageColors)+sizeof(biomeTypes)+sizeof(heightMap), sizeof(m_sectionNonAir)
  };
  telemetry::registry().replaceCpu(m_cpuTelemetry, current);
  m_cpuTelemetry = current;
}
