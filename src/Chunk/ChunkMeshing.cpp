// Chunk implementation - meshing: full/LOD greedy mesh builds and the
// mesh-space voxel sampling helpers (issue #181 split).
#include "Chunk.hpp"
#include "ChunkMeshScratch.hpp"
#include <Chunk/ChunkMeshResult.hpp>
#include <Renderer/Lighting.hpp>
#include <Renderer/MinecraftTextures.hpp>
#include <Engine/WorkloadTelemetry.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <new>

// Packed ABGR water tint colour shared by the full mesh and LOD mesh generators.
static constexpr uint32_t WATER_COLOR = 0xFF'E6804D;

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
  out.lightCacheWantedAtBuild = localLightCacheWanted();
  // Occupied Y span from the per-section metadata (issue #105): no 64 KiB
  // type copy + full-buffer scan per remesh. Section-granular, then refined
  // to byte-exact layer bounds inside the two boundary sections so slice
  // iteration stays as tight as the old full-scan helper produced.
  int occMinY = 0;
  int occMaxY = CHUNK_HEIGHT - 1;
  bool hasOccupancy = false;
  {
    // The prepass is real work and is measured as such (issue #115 review),
    // including for empty chunks whose build ends with the early return.
    telemetry::StageSample occupancySample(telemetry::Occupancy);
    hasOccupancy = occupiedSpanY(occMinY, occMaxY);
    if (hasOccupancy)
    {
      refineOccupiedSpanY(occMinY, occMaxY);
    }
  }

  if (!hasOccupancy)
  {
    // Empty occupancy: if light cache is wanted, compute light field so entities in this
    // chunk (e.g. open air or hollow cave) can sample valid skylight/blocklight.
    if (out.lightCacheWantedAtBuild)
    {
      telemetry::MeshSample meshSample(telemetry::Skylight);
      computeLightField(meshSample);
      populateLightStorage(out);
    }
    else
    {
      out.lightCacheAction = LightCacheAction::Clear;
    }
    return;
  }

  if (out.sectionsBuilt == 0)
  {
    if (out.lightCacheWantedAtBuild)
    {
      telemetry::MeshSample meshSample(telemetry::Skylight);
      computeLightField(meshSample);
      populateLightStorage(out);
    }
    else
    {
      out.lightCacheAction = LightCacheAction::Clear;
    }
    return;
  }

  telemetry::MeshSample meshSample(telemetry::Skylight);
  // Lighting stays chunk-wide for every job (issue #107 lighting
  // contract): each rebuilt section samples the same light field a whole
  // build would produce, so section boundaries cannot introduce seams.
  computeLightField(meshSample);

  if (out.lightCacheWantedAtBuild)
  {
    populateLightStorage(out);
  }
  else
  {
    out.lightCacheAction = LightCacheAction::Clear;
  }

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

// Greedy meshing of ONE vertical section (issue #107): every greedy face
// whose owning voxel lies in [ownerMinY, ownerMaxY]. Faces, AO and light
// sample the full one-voxel chunk/border context exactly like a whole-chunk
// build, so section boundaries are visually identical to the unsplit mesh;
// only quads that would cross a section boundary are split into one quad
// per section (each section owns the faces of its own voxels). Greedy
// rectangles never merge across sections because the mask plane is clipped
// to the section's occupied Y span and the slice walk repeats per section.

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
  auto &skyLight = chunk_detail::s_skyLight;
  auto &blockLightR = chunk_detail::s_blockLightR;
  auto &blockLightG = chunk_detail::s_blockLightG;
  auto &blockLightB = chunk_detail::s_blockLightB;

  auto &workspace = chunk_detail::s_meshWorkspace;
  uint32_t indexCounter = 0;
  uint32_t waterIndexCounter = 0;

  // New helper for greedy meshing that checks local voxels and the precomputed
  // neighbor shell
  auto getBlockGeometryForMeshing = [&](int lx, int ly, int lz) -> TextureType
  {
    const auto type = sampleForMeshing(lx, ly, lz);
    // Geometry view (issue #120 review): details are transparent to cube
    // faces. The fluid itself is NOT part of this view — contained water is
    // emitted by the dedicated binary fluid pass from blockContainsWater,
    // fully independent of BlockShape.
    if (blockIsSmallDetail(type))
      return AIR;
    return type;
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
          const TextureType type1 = getBlockGeometryForMeshing(x[0], x[1], x[2]);
          const TextureType type2 = getBlockGeometryForMeshing(xq[0], xq[1], xq[2]);
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

          auto isMergeableFace = [&](uint8_t ownerType, uint8_t backingType) -> bool
          {
            if (ownerType != originType)
              return false;
            if (backingType == airType)
              return true;
            return TextureManager::isTransparent(static_cast<TextureType>(backingType)) &&
                   ownerType != backingType;
          };

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
              if (!isMergeableFace(ownerType, backingType) ||
                  static_cast<uint32_t>(key >> 16) != originColor)
                break; // Adjacent cell is not the same mergeable face
            }
            else
            {
              const uint8_t ownerType = static_cast<uint8_t>((key >> 8) & 0xFF);
              const uint8_t backingType = static_cast<uint8_t>(key & 0xFF);
              if (!isMergeableFace(ownerType, backingType) ||
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
                if (!isMergeableFace(ownerType, backingType) ||
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
                if (!isMergeableFace(ownerType, backingType) ||
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
          const glm::vec3 quad_vertices_local[4] = {
              v0_local, v1_local, v2_local, v3_local};

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
              return !TextureManager::isTransparent(getBlockGeometryForMeshing(
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

          // Determine which mesh buffer this quad goes to. Water quads are
          // classified and merged here (so the mask stays exact) but their
          // emission is delegated to the dedicated fluid pass below, which
          // owns the whole water volume from blockContainsWater (issue #120).
          const bool isWater = (quad_type == WATER);
          if (!isWater)
          {
          auto &targetVertices = vertices;
          auto &targetIndices = indices;
          auto &targetIndexCounter = indexCounter;

          // Sample light from air cell in front of the face (Minecraft-style)
          uint8_t faceSky = 15;
          uint8_t faceR = 0, faceG = 0, faceB = 0;
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
            // Block light is stored on the halo domain (valid across the
            // ±1 face-sampling shell), so border faces read the neighbor
            // side's real propagated light instead of a zero fallback -
            // no colored-light seams at chunk borders (issue #141).
            const size_t hli = ChunkLightHalo::hidx(sample.x, sample.y, sample.z);
            faceR = blockLightR[hli];
            faceG = blockLightG[hli];
            faceB = blockLightB[hli];
            if (sample.x >= 0 && sample.x < CHUNK_SIZE && sample.y >= 0 && sample.y < CHUNK_HEIGHT &&
                sample.z >= 0 && sample.z < CHUNK_SIZE)
            {
              const size_t li = static_cast<size_t>(sample.x + CHUNK_SIZE * (sample.y + CHUNK_HEIGHT * sample.z));
              faceSky = skyLight[li];
            }
            else
            {
              faceSky = 12;
            }
            // Emissive solid itself glows (its own RGB emission)
            if (solid.x >= 0 && solid.x < CHUNK_SIZE && solid.y >= 0 && solid.y < CHUNK_HEIGHT &&
                solid.z >= 0 && solid.z < CHUNK_SIZE)
            {
              uint8_t emR = 0, emG = 0, emB = 0;
              lighting::unpackBlockLightRGB4(
                  lighting::blockLightEmissionRGB4(getVoxel(solid.x, solid.y, solid.z).type), emR, emG, emB);
              faceR = std::max(faceR, emR);
              faceG = std::max(faceG, emG);
              faceB = std::max(faceB, emB);
            }
          }
          const uint32_t lightBits = lighting::packLightBits(faceSky, faceR, faceG, faceB);

          for (int i = 0; i < 4; ++i)
          {
            Vertex vert;
            const glm::vec3 &localPos = quad_vertices_local[i];
            uint32_t ao = calculateAO(localPos, i);
            ++meshSample.data.aoVertices;

            vert.packedPos = Vertex::packPosition(localPos);
            vert.packedData = packedData | (ao << 12) | lightBits;
            vert.texCoordU = static_cast<uint16_t>(std::lround(tc[i].x));
            vert.texCoordV = static_cast<uint16_t>(std::lround(tc[i].y));
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
          } // !isWater: water quads are emitted by the dedicated fluid pass

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

  // --- Fluid pass (issue #120): the water volume is meshed from
  // blockContainsWater alone — fully independent of BlockShape — so
  // cross-shaped SEAGRASS, cube-shaped KELP and a future waterlogged block
  // hold water exactly like a plain WATER cell. Binary greedy:
  // WATER<->WATER (any water-containing type included) emits nothing;
  // a face exists where a water cell borders a non-water cell whose block
  // geometry is open (AIR or a transparent block). Legacy pair priority is
  // preserved: between two different transparent blocks the -q side sample
  // owns the interface face, so a transparent neighbor at dir < 0 suppresses
  // the fluid face (its own face already covers the interface) while at
  // dir > 0 the fluid face wins. Section seams use the same owner-band
  // gating as the block pass, and chunk borders read the same neighbor
  // shell.
  {
    // Scan 1 (water occupancy): one blockContainsWater sample per cell of
    // the section band, straight from the chunk voxels. Water-free sections
    // - the overwhelming majority inland - stop here and pay nothing else.
    const int yLo = ownerMinY;
    const int yHi = ownerMaxY;
    const int ySize = yHi - yLo + 1;
    const size_t gridSize =
        static_cast<size_t>(CHUNK_SIZE) *
        static_cast<size_t>(ySize) *
        static_cast<size_t>(CHUNK_SIZE);
    if (workspace.waterGrid.size() < gridSize)
      workspace.waterGrid.resize(gridSize);
    size_t waterCount = 0;
    for (int y = yLo; y <= yHi; ++y)
      for (int z = 0; z < CHUNK_SIZE; ++z)
        for (int x = 0; x < CHUNK_SIZE; ++x)
        {
          const size_t gi = (static_cast<size_t>(y - yLo) * CHUNK_SIZE + z) * CHUNK_SIZE + x;
          if (blockContainsWater(getVoxel(x, y, z).getTextureType()))
          {
            workspace.waterGrid[gi] = 1;
            ++waterCount;
          }
          else
          {
            workspace.waterGrid[gi] = 0;
          }
        }

    // Scan 2 (neighbor openness), only for sections that hold water:
    // 0 = closed (opaque block), 1 = open air, 2 = open transparent block.
    if (waterCount != 0)
    {
      if (workspace.openGrid.size() < gridSize)
        workspace.openGrid.resize(gridSize);
      for (int y = yLo; y <= yHi; ++y)
        for (int z = 0; z < CHUNK_SIZE; ++z)
          for (int x = 0; x < CHUNK_SIZE; ++x)
          {
            const size_t gi = (static_cast<size_t>(y - yLo) * CHUNK_SIZE + z) * CHUNK_SIZE + x;
            const TextureType geo = getBlockGeometryForMeshing(x, y, z);
            uint8_t kind = 0;
            if (geo == AIR)
              kind = 1;
            else if (TextureManager::isTransparent(geo))
              kind = 2;
            workspace.openGrid[gi] = kind;
          }

      const auto waterOccupied = [&](int lx, int ly, int lz) -> bool {
        if (ly < yLo || ly > yHi || lx < 0 || lx >= CHUNK_SIZE || lz < 0 || lz >= CHUNK_SIZE)
          return blockContainsWater(sampleForMeshing(lx, ly, lz));
        return workspace.waterGrid[(static_cast<size_t>(ly - yLo) * CHUNK_SIZE + lz) * CHUNK_SIZE + lx] != 0;
      };
    // Three-state neighbor openness (issue #120 review): fluid occupancy
    // first (a water-containing neighbor never exposes a fluid face), then
    // AIR always exposes the fluid face; a transparent block only at
    // dir > 0 (the -q side sample owns the interface face per the legacy
    // pair priority); an opaque block never does.
    const auto fluidSideOpen = [&](int lx, int ly, int lz, int dir) -> bool {
      if (waterOccupied(lx, ly, lz))
        return false;
      if (ly < yLo || ly > yHi || lx < 0 || lx >= CHUNK_SIZE || lz < 0 || lz >= CHUNK_SIZE)
      {
        const TextureType geo = getBlockGeometryForMeshing(lx, ly, lz);
        return geo == AIR || (TextureManager::isTransparent(geo) && dir > 0);
      }
      const uint8_t kind = workspace.openGrid[(static_cast<size_t>(ly - yLo) * CHUNK_SIZE + lz) * CHUNK_SIZE + lx];
      return kind == 1 || (kind == 2 && dir > 0);
    };

      for (int d = 0; d < 3; ++d)
      {
        const int u = (d + 1) % 3;
        const int v = (d + 2) % 3;
        int x[3] = {0, 0, 0};
        // Clamp the plane rect to the section's Y span exactly like the block
        // pass (issue #107): without it, vertical fluid faces spanning several
        // sections would be re-emitted by every section pass.
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
        for (x[d] = 0; x[d] < dims[d]; ++x[d])
        {
          // Section-local emission: the water cell owning the face decides
          // which section emits, exactly like the block pass owner gating.
          if (d == 1 && (x[d] < ownerMinY || x[d] > ownerMaxY))
            continue;
          for (int row = uStart; row < uEnd; ++row)
            std::fill(workspace.mask.begin() + static_cast<size_t>(row) * dims[v] + vStart,
                      workspace.mask.begin() + static_cast<size_t>(row) * dims[v] + vEnd, 0);
          for (int side = 0; side < 2; ++side)
          {
            const int dir = (side == 0) ? -1 : 1;
            const int planeD = x[d] + (dir > 0 ? 1 : 0);

            // 1) face mask for this slice + direction
            for (x[u] = uStart; x[u] < uEnd; ++x[u])
              for (x[v] = vStart; x[v] < vEnd; ++x[v])
              {
                glm::ivec3 n{x[0], x[1], x[2]};
                n[d] += dir;
                const bool face =
                    waterOccupied(x[0], x[1], x[2]) && fluidSideOpen(n[0], n[1], n[2], dir);
                workspace.mask[static_cast<size_t>(x[u]) * dims[v] + x[v]] = face ? 1 : 0;
              }

            // 2) greedy rectangle merge over the mask (expansion stays inside
            // the clamped rect: stale cells outside it belong to other
            // sections and must never extend a fluid quad)
            for (x[u] = uStart; x[u] < uEnd; ++x[u])
              for (x[v] = vStart; x[v] < vEnd; ++x[v])
              {
                if (workspace.mask[static_cast<size_t>(x[u]) * dims[v] + x[v]] == 0)
                  continue;

                int w = 1;
                while (x[u] + w < uEnd &&
                       workspace.mask[static_cast<size_t>(x[u] + w) * dims[v] + x[v]])
                  ++w;
                int h = 1;
                for (; x[v] + h < vEnd; ++h)
                {
                  bool rowFull = true;
                  for (int iw = 0; iw < w; ++iw)
                  {
                    if (workspace.mask[static_cast<size_t>(x[u] + iw) * dims[v] + (x[v] + h)] == 0)
                    {
                      rowFull = false;
                      break;
                    }
                  }
                  if (!rowFull)
                    break;
                }
                for (int iw = 0; iw < w; ++iw)
                  for (int ih = 0; ih < h; ++ih)
                    workspace.mask[static_cast<size_t>(x[u] + iw) * dims[v] + (x[v] + ih)] = 0;

                // 3) emit the quad (same vertex layout as the block pass)
                glm::vec3 sFloat{};
                sFloat[d] = static_cast<float>(planeD);
                sFloat[u] = static_cast<float>(x[u]);
                sFloat[v] = static_cast<float>(x[v]);
                glm::vec3 widthVec{};
                widthVec[u] = static_cast<float>(w);
                glm::vec3 heightVec{};
                heightVec[v] = static_cast<float>(h);
                const glm::vec3 quad_vertices_local[4] = {
                    sFloat, sFloat + widthVec, sFloat + widthVec + heightVec, sFloat + heightVec};

                glm::vec2 tc[4];
                const bool swapUV = (d == 0 || d == 1);
                const float tc_u = swapUV ? static_cast<float>(h) : static_cast<float>(w);
                const float tc_v = swapUV ? static_cast<float>(w) : static_cast<float>(h);
                if (swapUV)
                {
                  tc[0] = {0.f, 0.f};
                  tc[1] = {0.f, tc_v};
                  tc[2] = {tc_u, tc_v};
                  tc[3] = {tc_u, 0.f};
                }
                else
                {
                  tc[0] = {0.f, 0.f};
                  tc[1] = {tc_u, 0.f};
                  tc[2] = {tc_u, tc_v};
                  tc[3] = {0.f, tc_v};
                }

                glm::vec3 normalDir{};
                normalDir[d] = static_cast<float>(dir);
                int normalIdx = 0;
                if (normalDir.x > 0) normalIdx = 0;
                else if (normalDir.x < 0) normalIdx = 1;
                else if (normalDir.y > 0) normalIdx = 2;
                else if (normalDir.y < 0) normalIdx = 3;
                else if (normalDir.z > 0) normalIdx = 4;
                else normalIdx = 5;

                const uint32_t packedData = (normalIdx & 0x7) |
                                            ((static_cast<uint32_t>(WATER) & 0xFF) << 3) |
                                            (1u << 11); // water takes its WATER_COLOR tint

                // Ambient occlusion uses the geometric view: water and details
                // never occlude a fluid corner.
                uint32_t vert_indices[4];
                for (int i = 0; i < 4; ++i)
                {
                  const glm::vec3 &localPos = quad_vertices_local[i];
                  int pd = static_cast<int>(std::round(localPos[d]));
                  int pu = static_cast<int>(std::round(localPos[u]));
                  int pv = static_cast<int>(std::round(localPos[v]));
                  const int layerD = (dir > 0) ? planeD : planeD - 1;
                  auto aoSolid = [&](int du, int dv)
                  {
                    return !TextureManager::isTransparent(getBlockGeometryForMeshing(
                        (d == 0 ? layerD : (u == 0 ? pu + du : pv + du)),
                        (d == 1 ? layerD : (u == 1 ? pu + du : pv + du)),
                        (d == 2 ? layerD : (u == 2 ? pu + du : pv + du))));
                  };
                  const bool q1 = aoSolid(0, 0);
                  const bool q2 = aoSolid(-1, 0);
                  const bool q3 = aoSolid(-1, -1);
                  const bool q4 = aoSolid(0, -1);
                  bool s1, s2, c;
                  if (i == 0) { s1 = q2; s2 = q4; c = q3; }
                  else if (i == 1) { s1 = q1; s2 = q3; c = q4; }
                  else if (i == 2) { s1 = q2; s2 = q4; c = q1; }
                  else { s1 = q1; s2 = q3; c = q2; }
                  const uint32_t ao = (s1 && s2) ? 0u : 3u - static_cast<uint32_t>(s1 + s2 + c);

                  // Light from the open neighbor cell (same policy as the
                  // block pass); block light reads the halo domain so the
                  // ±1 shell crosses borders, sky keeps the daylight
                  // fallback outside the chunk (pre-existing scope).
                  uint8_t faceSky = 12;
                  uint8_t faceR = 0, faceG = 0, faceB = 0;
                  {
                    glm::ivec3 n{x[0], x[1], x[2]};
                    n[d] += dir;
                    const size_t hli = ChunkLightHalo::hidx(n.x, n.y, n.z);
                    faceR = blockLightR[hli];
                    faceG = blockLightG[hli];
                    faceB = blockLightB[hli];
                    if (n.x >= 0 && n.x < CHUNK_SIZE && n.y >= 0 && n.y < CHUNK_HEIGHT &&
                        n.z >= 0 && n.z < CHUNK_SIZE)
                    {
                      const size_t li = static_cast<size_t>(n.x + CHUNK_SIZE * (n.y + CHUNK_HEIGHT * n.z));
                      faceSky = skyLight[li];
                    }
                  }

                  Vertex vert;
                  vert.packedPos = Vertex::packPosition(localPos);
                  vert.packedData = packedData | (ao << 12) |
                                    lighting::packLightBits(faceSky, faceR, faceG, faceB);
                  vert.texCoordU = static_cast<uint16_t>(std::lround(tc[i].x));
                  vert.texCoordV = static_cast<uint16_t>(std::lround(tc[i].y));
                  vert.packedBiomeColor = WATER_COLOR;
                  waterVertices.push_back(vert);
                  vert_indices[i] = waterIndexCounter++;
                }

                if (dir > 0)
                {
                  waterIndices.insert(waterIndices.end(),
                                      {vert_indices[0], vert_indices[1], vert_indices[2],
                                       vert_indices[0], vert_indices[2], vert_indices[3]});
                }
                else
                {
                  waterIndices.insert(waterIndices.end(),
                                      {vert_indices[0], vert_indices[2], vert_indices[1],
                                       vert_indices[0], vert_indices[3], vert_indices[2]});
                }
              }
          }
        }
      }
    }
  }

  // Small plants stay in their owner section and share the opaque alpha-test
  // stream. Reverse triangles render the back side with normal backface culling.
  // Cross/flat vegetation deliberately uses an upward lighting normal
  // (kDetailLightingNormal): it avoids dark alternating planes on
  // double-sided foliage and gives consistent stylized vegetation lighting
  // independent of plane orientation. For lily pads +Y is also the
  // geometrically correct face normal.
  constexpr uint32_t kDetailLightingNormal = 2u; // +Y stylized foliage lighting
  for (int z = 0; z < CHUNK_SIZE; ++z)
    for (int y = ownerMinY; y <= ownerMaxY; ++y)
      for (int x = 0; x < CHUNK_SIZE; ++x)
      {
        const auto type = static_cast<TextureType>(getVoxel(x, y, z).type);
        const auto shape = blockShape(type);
        if (shape == BlockShape::Cube) continue;
        const size_t li = static_cast<size_t>(x + CHUNK_SIZE * (y + CHUNK_HEIGHT * z));
        const bool tint = blockUsesGrassTint(type);
        const size_t hli = ChunkLightHalo::hidx(x, y, z);
        const uint32_t packed = kDetailLightingNormal | (static_cast<uint32_t>(type) << 3)
            | (tint ? 1u << 11 : 0u)
            | (3u << 12) | lighting::packLightBits(skyLight[li], blockLightR[hli],
                                                   blockLightG[hli], blockLightB[hli]);
        auto quad = [&](std::array<glm::vec3, 4> positions) {
          const uint32_t first = static_cast<uint32_t>(vertices.size());
          constexpr glm::vec2 uv[] = {{0.f, 1.f}, {1.f, 1.f}, {1.f, 0.f}, {0.f, 0.f}};
          for (size_t k = 0; k < 4; ++k)
          {
            Vertex v{};
            const glm::vec3 localPos = glm::vec3(x, y, z) + positions[k];
            v.packedPos = Vertex::packPosition(localPos);
            v.texCoordU = static_cast<uint16_t>(uv[k].x);
            v.texCoordV = static_cast<uint16_t>(uv[k].y);
            v.packedData = packed;
            v.packedBiomeColor = tint ? biomeGrassColors[z * CHUNK_SIZE + x] : 0u;
            vertices.push_back(v);
          }
          constexpr uint32_t winding[] = {0, 1, 2, 0, 2, 3, 2, 1, 0, 3, 2, 0};
          for (auto k : winding) indices.push_back(first + k);
        };
        if (shape == BlockShape::Flat)
          quad({glm::vec3(1.f / 16.f, 1.f / 16.f, 1.f / 16.f),
                {15.f / 16.f, 1.f / 16.f, 1.f / 16.f},
                {15.f / 16.f, 1.f / 16.f, 15.f / 16.f},
                {1.f / 16.f, 1.f / 16.f, 15.f / 16.f}});
        else
        {
          quad({glm::vec3(2.f / 16.f, 0.f, 2.f / 16.f),
                {14.f / 16.f, 0.f, 14.f / 16.f},
                {14.f / 16.f, 14.f / 16.f, 14.f / 16.f},
                {2.f / 16.f, 14.f / 16.f, 2.f / 16.f}});
          quad({glm::vec3(14.f / 16.f, 0.f, 2.f / 16.f),
                {2.f / 16.f, 0.f, 14.f / 16.f},
                {2.f / 16.f, 14.f / 16.f, 14.f / 16.f},
                {14.f / 16.f, 14.f / 16.f, 2.f / 16.f}});
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
  out.lightCacheWantedAtBuild = localLightCacheWanted();

  if (out.lightCacheWantedAtBuild)
  {
    telemetry::MeshSample meshSample(telemetry::Skylight);
    computeLightField(meshSample);
    populateLightStorage(out);
  }
  else
  {
    out.lightCacheAction = LightCacheAction::Clear;
  }

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
        // Fluid occupancy is evaluated BEFORE the geometry view (issue #120
        // review): a water-containing detail — cross or a future waterlogged
        // cube — preserves the water column even when its geometry is
        // omitted from the LOD.
        if (blockContainsWater(t))
        {
          topY = cy;
          topType = WATER;
          break;
        }
        if (t != AIR)
        {
          if (blockIsSmallDetail(t))
            continue;
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
                            lighting::packLightBitsRGB4(15, 0);

      // Top face vertices in world space; axis mapping: d=1(Y), u=2(Z), v=0(X)
      float fy = float(topY + 1);
      float fx = float(cx);
      float fz = float(cz);

      Vertex v0, v1, v2, v3;
      v0.packedPos = Vertex::packPosition(glm::vec3(fx, fy, fz));
      v1.packedPos = Vertex::packPosition(glm::vec3(fx, fy, fz + 1.f));
      v2.packedPos = Vertex::packPosition(glm::vec3(fx + 1.f, fy, fz + 1.f));
      v3.packedPos = Vertex::packPosition(glm::vec3(fx + 1.f, fy, fz));

      // swapUV=true (d=1), w=1, h=1 -> tc_u=1, tc_v=1
      v0.texCoordU = 0; v0.texCoordV = 0;
      v1.texCoordU = 0; v1.texCoordV = 1;
      v2.texCoordU = 1; v2.texCoordV = 1;
      v3.texCoordU = 1; v3.texCoordV = 0;

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
