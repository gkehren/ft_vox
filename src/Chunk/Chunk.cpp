// Chunk: render-side chunk handle (issue #181 split; issue #206 Phase B).
// The AUTHORITATIVE voxel state lives in WorldChunk (World/WorldChunk.hpp);
// this unit owns construction/moves, the record binding, render-side reset
// and the generation wrapper. Meshing, lighting, editing and GPU publication
// remain partitioned across ChunkEditing.cpp, ChunkLighting.cpp,
// ChunkMeshing.cpp and ChunkGpu.cpp.
#include <Core/WorkloadTelemetry.hpp>
#include "Chunk.hpp"
#include <Chunk/ChunkMeshResult.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <new>
#include <stdexcept>

Chunk::Chunk(const glm::vec3 &position, ChunkState state, VoxelPool *voxelPool,
             BorderPool *borderPool, MeshResultPool *meshPool, ChunkLightPool *lightPool)
    : m_position(position),
      m_voxelPool(voxelPool ? voxelPool : &VoxelPool::defaultPool()),
      m_borderPool(borderPool ? borderPool : &BorderPool::defaultPool()),
      visible(false),
      m_resultPool(meshPool ? meshPool : &MeshResultPool::defaultPool()),
      m_pendingResult(nullptr),
      m_lightStorage(nullptr),
      m_lightPool(lightPool ? lightPool : &ChunkLightPool::defaultPool()),
      opaqueIndexCount(0), waterIndexCount(0),
      meshNeedsUpdate(true)
{
  // The owned record is allocated lazily (recordForWrite): an idle pool
  // handle carries no per-column backing until it is bound or used
  // standalone (issue #206 review — avoids double-counting column memory).
  if (state != ChunkState::UNLOADED)
    recordForWrite()->setState(state);
}

Chunk::Chunk(Chunk &&other) noexcept : Chunk()
{
  moveFrom(std::move(other));
}

Chunk &Chunk::operator=(Chunk &&other) noexcept
{
  if (this != &other)
  {
    // Release the destination's current render resources and authoritative
    // record before the transfer (the owned record's destructor returns its
    // voxel/border backing to the pools it was created with).
    releaseGPU();
    releaseLightStorage();
    releasePendingMeshResult();
    m_ownedWorld.reset();
    moveFrom(std::move(other));
  }
  return *this;
}

void Chunk::moveFrom(Chunk &&other) noexcept
{
  // Single transfer implementation (review M11): the old move ctor/assign
  // pair duplicated ~80 lines and had ALREADY diverged (m_activeIndex was
  // dropped by both; the assign kept the destination's value where the ctor
  // started from SIZE_MAX). Every field now travels exactly one way.
  //
  // The authoritative record (voxel storage, borders, identity, persistent
  // edits, per-column generation state) travels with the handle: standalone
  // chunks move their owned record, a bound chunk moves its binding (bound
  // chunks are never moved in production — the pool holds stable pointers).
  m_ownedWorld = std::move(other.m_ownedWorld);
  m_world = other.m_world;
  m_active = other.m_active;
  m_position = other.m_position;
  // Pool pointers survive the move (pre-split semantics): a moved-from
  // handle recycled through the pool re-creates its record with them.
  m_voxelPool = other.m_voxelPool;
  m_borderPool = other.m_borderPool;
  m_activeIndex = other.m_activeIndex;
  visible = other.visible;
  // A move transfers complete ownership (issue #112 review): the record
  // and its pools travel together; no re-acquisition or voxel copy happens
  // inside this noexcept move.
  m_lightPool = other.m_lightPool;
  m_lightStorage = other.m_lightStorage;
  m_lightHalo = other.m_lightHalo;
  m_resultPool = other.m_resultPool;
  m_arenas = other.m_arenas;
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
  m_localLightCacheWanted.store(other.m_localLightCacheWanted.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);

  // Invariant: the moved-from handle has no record; its render state is
  // zeroed so a recycled chunk starts from a clean GPU state.
  other.m_world = nullptr;
  other.m_active = nullptr;
  other.m_activeIndex = SIZE_MAX;
  other.m_lightStorage = nullptr;
  other.m_lightHalo = nullptr;
  other.m_localLightCacheWanted.store(false, std::memory_order_relaxed);
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
  // A pending build result belongs to the moved-from incarnation: release
  // it rather than transferring (issue #104). Its owner pointer names the
  // source chunk, and the destination identity is carried by the record
  // transfer above - a build still in flight for that same generation
  // remains publishable on the destination.
  other.releasePendingMeshResult();
}

Chunk::~Chunk()
{
  // Release all render resources, then the owned authoritative record (its
  // destructor returns voxel/border backing to the pools and zeroes the
  // memory telemetry it published).
  releasePendingMeshResult();
  releaseLightStorage();
  releaseGPU();
  m_ownedWorld.reset();
}

void Chunk::generateTerrain(TerrainGenerator &generator)
{
  // Standalone / bootstrap wrapper: the record does the authoritative
  // generation; the render flags it used to set inline are published here.
  // The world's async path generates the record directly and publishes the
  // same flags when it drains the generation completion event.
  if (recordForWrite()->generateTerrain(generator))
  {
    meshNeedsUpdate = true;
    m_dirtySections.store(kAllSectionMask, std::memory_order_relaxed);
  }
}

void Chunk::rebuildBordersFromNeighbors(const Chunk *west, const Chunk *east,
                                        const Chunk *south, const Chunk *north)
{
  if (!m_active)
    return;
  m_active->rebuildBordersFromNeighbors(
      west ? west->worldChunk() : nullptr,
      east ? east->worldChunk() : nullptr,
      south ? south->worldChunk() : nullptr,
      north ? north->worldChunk() : nullptr);
}

void Chunk::reset(const glm::vec3 &newPosition, ResetMode mode)
{
  // Drop GPU meshes — next upload recreates the arena ranges.
  releaseGPU();
  opaqueIndexCount = 0;
  waterIndexCount = 0;

  // Reset render identity.
  visible = false;
  meshNeedsUpdate.store(true);
  m_isLODMesh = false;
  m_localLightCacheWanted.store(false, std::memory_order_relaxed);
  m_activeIndex = SIZE_MAX;
  m_dirtySections.store(0, std::memory_order_relaxed);

  // A recycled chunk is a new mesh identity (issue #104/#114): any attached
  // build result goes back to the pool, and in-flight results for the old
  // incarnation are rejected at publish time via both counters.
  releasePendingMeshResult();

  // Return chunk light storage to the ChunkLightPool (issue #128).
  releaseLightStorage();

  // Authoritative reset acts on the active record (owned or bound): reset
  // is an explicit lifecycle operation and bumps the incarnation the world
  // and the deferred-edit queue observe. Pool acquire/release only ever
  // see unbound chunks; the world's own unload path resets records directly.
  m_position = newPosition;
  const WorldChunk::ResetMode recordMode =
      mode == ResetMode::Full ? WorldChunk::ResetMode::Full : WorldChunk::ResetMode::ForGeneration;
  if (m_world)
  {
    // A bound handle's record lives in VoxelWorld::m_chunks keyed by its
    // coordinate: resetting it to a DIFFERENT coordinate would silently
    // desync the map (issue #206 review). The only legitimate bound reset
    // keeps the coordinate (the lifecycle test's recycle-in-place path).
    assert(m_active && m_active->coord() == ::worldToChunkCoord(newPosition) &&
           "reset of a world-bound handle must not change its coordinate");
    // A Full reset clears the persistent-edit map and disarms tracking. On a
    // bound record whose world is open (tracking armed) that would discard
    // accepted save state behind the world's back — the world captures edits
    // on its own unload path, never via a render-handle reset (issue #206
    // review). ForGeneration is always safe (it retains the edit map).
    assert((mode != ResetMode::Full || !m_active->tracksPersistentEdits()) &&
           "Full reset of a world-bound handle while a world is open would discard saved edits");
    m_active->reset(newPosition, recordMode);
  }
  else if (m_ownedWorld)
  {
    m_active = m_ownedWorld.get();
    m_active->reset(newPosition, recordMode);
  }
  else
  {
    // Fresh/moved-from handle: no record yet. It is created lazily on the
    // next standalone use (recordForWrite), so idle pool handles carry no
    // per-column backing.
    m_active = nullptr;
  }
}
