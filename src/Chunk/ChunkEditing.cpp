// Chunk implementation - voxel editing: setVoxel, place/delete and
// edit-driven section dirty marking (issue #181 split; class Chunk, see Chunk.hpp).
// Issue #206 (Epic #204 B): the authoritative write (storage, occupancy,
// persistent-edit capture, revision) lives in WorldChunk::setVoxel; this unit
// keeps the render-side section invalidation that consumes it.
#include "Chunk.hpp"
#include <Renderer/Lighting.hpp>

#include <algorithm>
#include <cassert>
#include <new>

void Chunk::setVoxel(int x, int y, int z, TextureType type)
{
  // Authoritative write first; the previous type feeds the render-side
  // light/section invalidation below (same net effect as the pre-split
  // inline write + markEditDirtySections sequence). recordForWrite lazily
  // creates the owned record for a standalone handle (bound handles use the
  // world record).
  WorldChunk *record = recordForWrite();
  const TextureType previousType = record->setVoxel(x, y, z, type);
  const bool inChunk = static_cast<uint32_t>(x) < CHUNK_SIZE &&
                       static_cast<uint32_t>(y) < CHUNK_HEIGHT &&
                       static_cast<uint32_t>(z) < CHUNK_SIZE;
  const bool onBorder = !inChunk && x >= -1 && x <= CHUNK_SIZE &&
                        z >= -1 && z <= CHUNK_SIZE &&
                        y >= 0 && y < static_cast<int>(CHUNK_HEIGHT);
  if (!inChunk && !onBorder)
    return;
  // Section-local remeshing (issue #107): dirty only the affected vertical
  // sections instead of the whole chunk. Border (mirror) writes dirty
  // exactly the written voxel's y/16 section.
  markEditDirtySections(x, y, z, type, previousType, !inChunk);
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
  if (!hasVoxelStorage())
    return false;

  const glm::vec3 &origin = getPosition();
  int x = static_cast<int>(position.x - origin.x);
  int y = static_cast<int>(position.y - origin.y);
  int z = static_cast<int>(position.z - origin.z);
  if (x < 0)
    x += CHUNK_SIZE;
  if (z < 0)
    z += CHUNK_SIZE;

  if (isVoxelActive(x, y, z))
  {
    setVoxel(x, y, z, AIR);
    meshNeedsUpdate = true;
    setState(ChunkState::GENERATED);
    return true;
  }
  return false;
}

bool Chunk::placeVoxel(const glm::vec3 &position, TextureType type)
{
  const glm::vec3 &origin = getPosition();
  int x = static_cast<int>(position.x - origin.x);
  int y = static_cast<int>(position.y - origin.y);
  int z = static_cast<int>(position.z - origin.z);
  if (x < 0)
    x += CHUNK_SIZE;
  if (z < 0)
    z += CHUNK_SIZE;

  if (!isVoxelActive(x, y, z))
  {
    setVoxel(x, y, z, type);
    meshNeedsUpdate = true;
    setState(ChunkState::GENERATED);
    return true;
  }
  return false;
}
