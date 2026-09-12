// Chunk: authoritative per-chunk voxel state and lifecycle (issue #181
// split). The class implementation is partitioned by responsibility across
// ChunkEditing.cpp, ChunkLighting.cpp, ChunkMeshing.cpp and ChunkGpu.cpp;
// this unit owns construction/moves, storage lifecycle, occupancy metadata,
// border-shell borrow/release, terrain generation and reset.
#include <Engine/WorkloadTelemetry.hpp>
#include "Chunk.hpp"
#include <Chunk/ChunkMeshResult.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>

Chunk::Chunk(const glm::vec3 &position, ChunkState state, VoxelPool *voxelPool,
             BorderPool *borderPool, MeshResultPool *meshPool, ChunkLightPool *lightPool)
    : position(position), visible(false), state(state),
      opaqueIndexCount(0), waterIndexCount(0),
      m_voxelPool(voxelPool ? voxelPool : &VoxelPool::defaultPool()),
      m_storage(nullptr),
      m_borderPool(borderPool ? borderPool : &BorderPool::defaultPool()),
      m_borders(nullptr),
      m_resultPool(meshPool ? meshPool : &MeshResultPool::defaultPool()),
      m_pendingResult(nullptr),
      m_lightStorage(nullptr),
      m_lightPool(lightPool ? lightPool : &ChunkLightPool::defaultPool()),
      meshNeedsUpdate(true) { publishCpuTelemetry(); }

Chunk::Chunk(Chunk &&other) noexcept
    : position(std::move(other.position)), visible(other.visible),
      state(other.state.load()),
      m_allocator(other.m_allocator),
      m_sectionGpu(other.m_sectionGpu),
      m_sectionGpuWater(other.m_sectionGpuWater),
      m_lodOpaqueVertices(other.m_lodOpaqueVertices),
      m_lodOpaqueIndices(other.m_lodOpaqueIndices),
      m_lodWaterVertices(other.m_lodWaterVertices),
      m_lodWaterIndices(other.m_lodWaterIndices),
      m_cachedOpaqueDrawCount(other.m_cachedOpaqueDrawCount),
      m_cachedWaterDrawCount(other.m_cachedWaterDrawCount),
      m_cachedOpaqueDraws(other.m_cachedOpaqueDraws),
      m_cachedWaterDraws(other.m_cachedWaterDraws),
      opaqueIndexCount(other.opaqueIndexCount), waterIndexCount(other.waterIndexCount),
      meshNeedsUpdate(other.meshNeedsUpdate.load()),
      m_voxelPool(other.m_voxelPool),
      m_storage(other.m_storage),
      m_sectionNonAir(other.m_sectionNonAir),
      m_dirtySections(other.m_dirtySections.load(std::memory_order_relaxed)),
      m_borderPool(other.m_borderPool),
      m_borders(other.m_borders),
      m_lightPool(other.m_lightPool),
      m_lightStorage(other.m_lightStorage),
      m_resultPool(other.m_resultPool),
      m_arenas(other.m_arenas),
      m_meshGeneration(other.m_meshGeneration),
      m_meshRevision(other.m_meshRevision.load(std::memory_order_relaxed)),
      biomeGrassColors(other.biomeGrassColors),
      biomeFoliageColors(other.biomeFoliageColors),
      biomeTypes(other.biomeTypes),
      heightMap(other.heightMap),
      m_isLODMesh(other.m_isLODMesh),
      m_localLightCacheWanted(other.m_localLightCacheWanted.load(std::memory_order_relaxed))
{
  other.m_storage = nullptr;
  other.m_borders = nullptr;
  other.m_lightStorage = nullptr;
  other.m_localLightCacheWanted.store(false, std::memory_order_relaxed);
  // Invariant: no storage => empty occupancy metadata (issue #115 review).
  other.m_sectionNonAir.fill(0);
  other.publishCpuTelemetry();
  publishCpuTelemetry();
  other.m_allocator = VK_NULL_HANDLE;
  // Arena ranges and slot layouts travel with the chunk (issue #109); zero
  // the moved-from side so a recycled chunk starts from a clean GPU state.
  other.m_lodOpaqueVertices = {};
  other.m_lodOpaqueIndices = {};
  other.m_lodWaterVertices = {};
  other.m_lodWaterIndices = {};
  other.m_cachedOpaqueDrawCount = 0;
  other.m_cachedWaterDrawCount = 0;
  other.m_cachedOpaqueDraws.fill({});
  other.m_cachedWaterDraws.fill({});
  other.m_sectionGpu.fill({});
  other.m_sectionGpuWater.fill({});
  other.m_dirtySections.store(0, std::memory_order_relaxed);
  other.opaqueIndexCount = 0;
  other.waterIndexCount = 0;
  other.m_arenas = nullptr;
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
    releaseLightStorage();
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

    // Transfer light storage ownership together with its pool (issue #128).
    m_lightPool = other.m_lightPool;
    m_lightStorage = other.m_lightStorage;
    other.m_lightStorage = nullptr;

    m_resultPool = other.m_resultPool;
    m_arenas = other.m_arenas;
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
    // Arena ranges travel with the chunk that describes them (issue #109).
    m_lodOpaqueVertices = other.m_lodOpaqueVertices;
    m_lodOpaqueIndices = other.m_lodOpaqueIndices;
    m_lodWaterVertices = other.m_lodWaterVertices;
    m_lodWaterIndices = other.m_lodWaterIndices;
    m_cachedOpaqueDrawCount = other.m_cachedOpaqueDrawCount;
    m_cachedWaterDrawCount = other.m_cachedWaterDrawCount;
    m_cachedOpaqueDraws = other.m_cachedOpaqueDraws;
    m_cachedWaterDraws = other.m_cachedWaterDraws;
    m_sectionGpu = other.m_sectionGpu;
    m_sectionGpuWater = other.m_sectionGpuWater;
    m_dirtySections.store(other.m_dirtySections.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
    opaqueIndexCount = other.opaqueIndexCount;
    waterIndexCount = other.waterIndexCount;
    meshNeedsUpdate.store(other.meshNeedsUpdate.load());
    m_isLODMesh = other.m_isLODMesh;
    m_inTransit.store(other.m_inTransit.load());
    m_localLightCacheWanted.store(other.m_localLightCacheWanted.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
    other.m_localLightCacheWanted.store(false, std::memory_order_relaxed);

    other.publishCpuTelemetry();
    publishCpuTelemetry();
    other.m_allocator = VK_NULL_HANDLE;
    other.m_lodOpaqueVertices = {};
    other.m_lodOpaqueIndices = {};
    other.m_lodWaterVertices = {};
    other.m_lodWaterIndices = {};
    other.m_cachedOpaqueDrawCount = 0;
    other.m_cachedWaterDrawCount = 0;
    other.m_cachedOpaqueDraws.fill({});
    other.m_cachedWaterDraws.fill({});
    other.m_sectionGpu.fill({});
    other.m_sectionGpuWater.fill({});
    other.m_dirtySections.store(0, std::memory_order_relaxed);
    other.opaqueIndexCount = 0;
    other.waterIndexCount = 0;
    other.m_arenas = nullptr;
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
  releaseLightStorage();
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
  m_localLightCacheWanted.store(false, std::memory_order_relaxed);
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

  // Persistent-edit bookkeeping (issue #180): the edit map never leaks
  // across recycled chunks - capturing it is the owner's job before the
  // chunk is released. The asymmetry between the two modes is deliberate:
  // ForGeneration keeps tracking armed because ChunkPool::acquire() reuses
  // the same Chunk object for the next generation while a world is open and
  // the manager wants tracking to continue for it, while the Full reset
  // (ChunkPool::release, retirement) disarms tracking entirely.
  m_persistentEdits.clear();
  if (mode == ResetMode::Full)
    m_trackPersistentEdits = false;

  // Empty occupancy metadata for the recycled incarnation; generation
  // recounts it (issue #105).
  m_sectionNonAir.fill(0);

  // Return transient neighbor-border storage to the BorderPool.
  // No border capacity remains attached to this Chunk.
  releaseNeighborBorders();

  // Return chunk light storage to the ChunkLightPool (issue #128).
  releaseLightStorage();

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
