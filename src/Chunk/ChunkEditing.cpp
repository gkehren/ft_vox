// Chunk implementation - voxel editing: setVoxel, place/delete and
// edit-driven section dirty marking (issue #181 split; class Chunk, see Chunk.hpp).
#include "Chunk.hpp"
#include <Renderer/Lighting.hpp>

#include <algorithm>
#include <cassert>
#include <new>

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
    // Persistent-edit capture (issue #180): only authoritative in-chunk
    // writes that actually change the voxel are recorded; border-shell
    // writes (the else branch below) and no-op rewrites never record.
    if (m_trackPersistentEdits && previousType != type)
      m_persistentEdits[static_cast<uint32_t>(index)] = static_cast<uint8_t>(type);
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

// Section dirtying for one voxel edit (issue #107, PR #117 review). The
// mesh state is one payload per vertical 16^3 section, so an edit re-arms
// only the sections whose quads can change:
//   - the edited section always, plus the section above/below when the
//     voxel sits on a section Y boundary: faces at that plane are owned by
//     both neighbors, and the AO corners of one side sample the other;
//   - for in-chunk edits, a conservative light-dirty Y range. The light
//     FIELD is recomputed chunk-wide by every build (no seams by
//     construction); this range only decides which section meshes refresh
//     their light bits in the same job. Both light channels spread with a
//     6-neighbour BFS at -1 brightness per step, so every mesh whose lit
//     sample can change lies within the BFS radius (<= 15 hops) of a cell
//     whose field value changes:
//       * an occlusion change (blockTransmitsSkyLight flips) reroutes both
//         BFS fields through the edited cell, and additionally flips the
//         skylight column below the edit down to the first blocker - the
//         lateral flood can bypass that blocker by up to the BFS radius,
//         so the range bottom extends 15 cells below it, while the top
//         covers the upward flood above the edit (overhang geometry);
//       * an emitter added/removed/changed reseeds the block-light BFS.
//   - border (mirror) writes change face ownership and AO sampling at the
//     written voxel's y/16 section (cross-chunk light does not exist: the
//     light field samples in-chunk voxels only). The Y-boundary adjacency
//     applies to them too (edge faces' AO corners read the border strip).
void Chunk::markEditDirtySections(int x, int y, int z, TextureType type,
                                  TextureType previousType, bool borderWrite)
{
  if (y < 0 || y >= static_cast<int>(CHUNK_HEIGHT))
    return; // vertical padding has no mesh state
  constexpr int kLightBfsRadius = 15;
  const int section = y / kOccupancySectionSize;
  uint16_t mask = static_cast<uint16_t>(1u << section);
  const int inSection = y % kOccupancySectionSize;
  if (inSection == 0 && section > 0)
    mask |= static_cast<uint16_t>(1u << (section - 1));
  if (inSection == kOccupancySectionSize - 1 && section + 1 < kOccupancySections)
    mask |= static_cast<uint16_t>(1u << (section + 1));

  const bool oldTransmits = blockTransmitsSkyLight(previousType);
  const bool newTransmits = blockTransmitsSkyLight(type);
  if (!borderWrite &&
      (oldTransmits != newTransmits ||
       lighting::blockLightEmission(static_cast<uint8_t>(type)) > 0 ||
       lighting::blockLightEmission(static_cast<uint8_t>(previousType)) > 0))
  {
    int lightMinY = y - kLightBfsRadius;
    if (oldTransmits != newTransmits)
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
      lightMinY = std::min(lightMinY, blocker + 1 - kLightBfsRadius);
    }
    const int lightMinSection =
        std::max(0, lightMinY) / kOccupancySectionSize;
    const int lightMaxSection =
        std::min(kOccupancySections - 1,
                 (y + kLightBfsRadius) / kOccupancySectionSize);
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
