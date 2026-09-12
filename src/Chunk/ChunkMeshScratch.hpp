#pragma once

// Internal scratch buffers shared by the Chunk implementation's worker-side
// translation units (ChunkLighting.cpp / ChunkMeshing.cpp, issue #181 split).
// NOT a public Chunk header: it must not be included outside src/Chunk.
// Everything lives in the chunk_detail namespace to keep these internals out
// of the global namespace.
//
// Chunk-wide sky/block light fields for one mesh job (issue #107):
// computed once per build so a section-selective rebuild samples exactly
// the field a whole-chunk build would produce (no seams at 16-block Y
// boundaries). Thread-local scratch like the mesh workspace: reserved once
// per worker, no heap churn afterwards.

#include <vector>
#include <glm/glm.hpp>
#include <utils.hpp>

namespace chunk_detail
{

// The instances MUST stay `inline thread_local`: `static` or an anonymous
// namespace here would give every including TU its own scratch, breaking
// the per-worker reuse the reserves exist for.

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
  // Fluid pass grids (issue #120): water occupancy and neighbor openness,
  // one entry per cell of the section band. Thread-local: sized on demand
  // and reused across sections/builds without per-section allocations.
  std::vector<uint8_t> waterGrid;
  std::vector<uint8_t> openGrid;
  // 0 = closed (opaque block), 1 = open air, 2 = open transparent block.
  // Filled only when the section actually holds water.

  MeshWorkspace()
  {
    mask.reserve(CHUNK_HEIGHT * CHUNK_SIZE);
    faceKeyLo.reserve(CHUNK_HEIGHT * CHUNK_SIZE);
    faceKeyHi.reserve(CHUNK_HEIGHT * CHUNK_SIZE);
    skyQ.reserve(512);
    blockQ.reserve(256);
    waterGrid.reserve(CHUNK_HEIGHT * CHUNK_SIZE);
    openGrid.reserve(CHUNK_HEIGHT * CHUNK_SIZE);
  }
};

inline thread_local MeshWorkspace s_meshWorkspace;

// Chunk-wide sky/block light fields for one mesh job (issue #107):
// computed once per build so a section-selective rebuild samples exactly
// the field a whole-chunk build would produce (no seams at 16-block Y
// boundaries). Thread-local scratch like the mesh workspace.
// Block light is RGB (issue #141): three planar 4-bit channels (64 KiB each,
// transient). Planes keep the BFS inner loop on scalar uint8 compares like
// the historical scalar field; the public RGB4 pack helpers stay the
// interop format (tests, entities #128).
inline thread_local std::vector<uint8_t> s_skyLight;
inline thread_local std::vector<uint8_t> s_blockLightR;
inline thread_local std::vector<uint8_t> s_blockLightG;
inline thread_local std::vector<uint8_t> s_blockLightB;

} // namespace chunk_detail
