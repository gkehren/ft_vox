// Chunk implementation - lighting: light sampling, the chunk-wide sky/block
// light field, light-cache packing and the neighbor light halo (issue #181 split).
#include "Chunk.hpp"
#include "ChunkMeshScratch.hpp"
#include <Chunk/ChunkMeshResult.hpp>
#include <Renderer/Lighting.hpp>
#include <Engine/WorkloadTelemetry.hpp>

#include <algorithm>
#include <cassert>

void Chunk::releaseLightStorage()
{
  if (m_lightStorage && m_lightPool)
  {
    m_lightPool->release(m_lightStorage);
    m_lightStorage = nullptr;
  }
}

void Chunk::attachLightStorage(ChunkLightStorage *storage)
{
  if (!storage)
    return;
  if (m_lightStorage)
    releaseLightStorage();
  m_lightStorage = storage;
}

uint16_t Chunk::sampleLightRaw(int x, int y, int z) const
{
  if (!m_lightStorage || x < 0 || x >= static_cast<int>(CHUNK_SIZE) ||
      y < 0 || y >= static_cast<int>(CHUNK_HEIGHT) ||
      z < 0 || z >= static_cast<int>(CHUNK_SIZE))
    return 0;
  const size_t vi = static_cast<size_t>(x + CHUNK_SIZE * (y + CHUNK_HEIGHT * z));
  return m_lightStorage->voxels[vi];
}

lighting::LocalVoxelLight Chunk::sampleLight(int x, int y, int z) const
{
  return lighting::unpackLocalVoxelLight(sampleLightRaw(x, y, z));
}

void Chunk::packLightField(ChunkLightStorage &out) const
{
  for (int z = 0; z < CHUNK_SIZE; ++z)
    for (int y = 0; y < CHUNK_HEIGHT; ++y)
      for (int x = 0; x < CHUNK_SIZE; ++x)
      {
        const size_t vi = static_cast<size_t>(x + CHUNK_SIZE * (y + CHUNK_HEIGHT * z));
        const size_t li = ChunkLightHalo::hidx(x, y, z);
        out.voxels[vi] = lighting::packVoxelLight(
            chunk_detail::s_skyLight[vi], chunk_detail::s_blockLightR[li],
            chunk_detail::s_blockLightG[li], chunk_detail::s_blockLightB[li]);
      }
}

void Chunk::populateLightStorage(MeshBuildResult &out)
{
  out.lightPool = m_lightPool;
  if (!out.lightStorage && m_lightPool)
    out.lightStorage = m_lightPool->acquire();
  if (out.lightStorage)
  {
    packLightField(*out.lightStorage);
    out.lightCacheAction = LightCacheAction::Replace;
  }
  else
  {
    out.lightCacheAction = LightCacheAction::Clear;
  }
}

void Chunk::buildLightCache(ChunkLightStorage &out)
{
  // LightCache telemetry family (issue #173 review): a cache-only build
  // never reports into mesh.skylight / mesh.blocklight / mesh.build.
  telemetry::MeshSample meshSample(telemetry::Skylight, telemetry::LightCacheFamily);
  computeLightField(meshSample);
  packLightField(out);
}

// Cross-chunk block-light context (issue #141 review fix): snapshot the
// 15-voxel ring of neighbor voxels around `center` (4 sides + 4 diagonals;
// a 6-neighbour Manhattan BFS can only route light through these). Missing
// or unreadable neighbors (state UNLOADED - stale pool bytes while their
// generation worker runs) contribute AIR: light is only seeded by real
// sources, so a missing neighbor simply contributes no light, and the
// arrival/edit light rules dirty the affected neighbors once its content
// lands. A neighbor in transit for a MESH job is read normally - its voxels
// are stable (edits are deferred) and skipping it would blank the halo
// whenever two neighbors are dispatched in the same batch. Center cells
// stay AIR in the snapshot - the light field reads them from the chunk.
//
// Performance note (issue #141 review round 2, P2): this runs on the main
// thread inside the mesh-dispatch critical section, so both the source
// reads and the halo writes walk contiguous rows (y outermost - the voxel
// layout is y-major) and the emissive test is a table lookup. There is no
// bulk reset: every ring cell is written exactly once below (neighbor
// bytes or AIR), making a resetToAir() memset redundant.
void fillLightHaloFromNeighbors(ChunkLightHalo &halo, const Chunk *center,
                                const Chunk *west, const Chunk *east,
                                const Chunk *south, const Chunk *north,
                                const Chunk *southWest, const Chunk *southEast,
                                const Chunk *northWest, const Chunk *northEast)
{
  (void)center;
  halo.emissives.clear();
  // Copy one rectangle of the ring from `src`: halo columns
  // [hxFrom, hxFrom+cols) x halo rows [hzFrom, hzFrom+rows) map to source
  // coordinates starting at (sxFrom, szFrom), row-major. Loop order is
  // y -> row -> column so both the source reads and the halo writes walk
  // contiguous bytes (both layouts are x-contiguous).
  //
  // Readability is the voxel-backing contract ONLY (state != UNLOADED):
  // in-transit covers mesh jobs too, and a chunk being meshed has stable,
  // fully readable voxels (edits targeting it are deferred). Skipping it
  // would blank the halo whenever two neighbors are dispatched in the same
  // batch - the second snapshot would treat the first as AIR and publish a
  // permanent seam no invalidation would ever catch (issue #141 review
  // round 3).
  auto copyRegion = [&](const Chunk *src, int hxFrom, int hzFrom, int cols, int rows,
                        uint32_t sxFrom, uint32_t szFrom)
  {
    const bool readable = src && src->isVoxelBackingReadable();
    for (uint32_t y = 0; y < CHUNK_HEIGHT; ++y)
    {
      const size_t hy = static_cast<size_t>(y) * ChunkLightHalo::kExtent;
      for (int rr = 0; rr < rows; ++rr)
      {
        size_t h = static_cast<size_t>(hxFrom) + hy +
                   static_cast<size_t>(CHUNK_HEIGHT * ChunkLightHalo::kExtent) *
                       static_cast<size_t>(hzFrom + rr);
        uint32_t sx = sxFrom;
        for (int c = 0; c < cols; ++c, ++sx, ++h)
        {
          const uint8_t t = readable
              ? src->getVoxel(sx, y, static_cast<uint32_t>(szFrom + rr)).type
              : static_cast<uint8_t>(AIR);
          halo.voxels[h] = t;
          if (lighting::kIsBlockLightSourceTable[t])
            halo.emissives.push_back(
                {static_cast<uint32_t>(h), lighting::blockLightEmissionRGB4(t)});
        }
      }
    }
  };
  constexpr int kR = ChunkLightHalo::kRadius;
  // Region table: raw halo column/row starts (hx = x+kR, hz = z+kR), sizes,
  // and the source chunk coordinates the rectangle maps from.
  //   sides: 15-deep slab x a full 16-wide chunk row/column
  //   corners: 15x15 squares (the diagonal chunks' near corner)
  copyRegion(west, 0, kR, kR, CHUNK_SIZE, 1, 0);
  copyRegion(east, kR + CHUNK_SIZE, kR, kR, CHUNK_SIZE, 0, 0);
  copyRegion(south, kR, 0, CHUNK_SIZE, kR, 0, 1);
  copyRegion(north, kR, kR + CHUNK_SIZE, CHUNK_SIZE, kR, 0, 0);
  copyRegion(southWest, 0, 0, kR, kR, 1, 1);
  copyRegion(southEast, kR + CHUNK_SIZE, 0, kR, kR, 0, 1);
  copyRegion(northWest, 0, kR + CHUNK_SIZE, kR, kR, 1, 0);
  copyRegion(northEast, kR + CHUNK_SIZE, kR + CHUNK_SIZE, kR, kR, 0, 0);
}

void Chunk::computeLightField(telemetry::MeshSample &meshSample)
{
  auto &workspace = chunk_detail::s_meshWorkspace;
  auto &skyLight = chunk_detail::s_skyLight;
  auto &blockLightR = chunk_detail::s_blockLightR;
  auto &blockLightG = chunk_detail::s_blockLightG;
  auto &blockLightB = chunk_detail::s_blockLightB;
  skyLight.assign(static_cast<size_t>(CHUNK_VOLUME), 0);
  // Block light lives on the halo domain (center + 15-voxel ring) so it
  // crosses chunk borders (issue #141 review fix); with no halo attached
  // the ring stays zero and the historical in-chunk-only behavior holds.
  // Sky light stays chunk-wide (unchanged scope).
  const ChunkLightHalo *halo = m_lightHalo;
  const int lo = halo ? -ChunkLightHalo::kRadius : 0;
  const int hi = halo ? CHUNK_SIZE + ChunkLightHalo::kRadius
                      : static_cast<int>(CHUNK_SIZE);
  blockLightR.assign(ChunkLightHalo::kVolume, 0);
  blockLightG.assign(ChunkLightHalo::kVolume, 0);
  blockLightB.assign(ChunkLightHalo::kVolume, 0);
  {
    auto idxOf = [](int x, int y, int z) -> size_t {
      return static_cast<size_t>(x + CHUNK_SIZE * (y + CHUNK_HEIGHT * z));
    };
    auto lightIdx = [](int x, int y, int z) -> size_t {
      return ChunkLightHalo::hidx(x, y, z);
    };
    auto isAirLike = [&](int x, int y, int z) -> bool {
      if (x >= 0 && x < CHUNK_SIZE && z >= 0 && z < CHUNK_SIZE &&
          y >= 0 && y < CHUNK_HEIGHT)
        return blockTransmitsSkyLight(static_cast<TextureType>(getVoxel(x, y, z).type));
      if (!halo || y < 0 || y >= CHUNK_HEIGHT)
        return true;
      return blockTransmitsSkyLight(static_cast<TextureType>(halo->voxelAt(x, y, z)));
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
    // Seed block light from emissive solids into neighboring air (issue #141):
    // each source carries its own RGB emission, colored per block type. With
    // a halo attached, ring sources seed too and the domain bounds extend
    // beyond the chunk so light crosses borders (review fix).
    auto &queue = workspace.blockQ;
    queue.clear();
    auto seedCell = [&](int x, int y, int z, uint8_t emR, uint8_t emG, uint8_t emB)
    {
      const size_t i = lightIdx(x, y, z);
      bool improved = false;
      // Overlap policy: per-channel max (see propagation below).
      if (blockLightR[i] < emR) { blockLightR[i] = emR; improved = true; }
      if (blockLightG[i] < emG) { blockLightG[i] = emG; improved = true; }
      if (blockLightB[i] < emB) { blockLightB[i] = emB; improved = true; }
      if (improved)
        queue.emplace_back(x, y, z);
    };
    auto seedSource = [&](int x, int y, int z, uint16_t em)
    {
      if (em == 0)
        return;
      uint8_t emR = 0, emG = 0, emB = 0;
      lighting::unpackBlockLightRGB4(em, emR, emG, emB);
      // Light lives in air cells around the emitter
      const int dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
      for (auto &d : dirs)
      {
        const int nx = x + d[0], ny = y + d[1], nz = z + d[2];
        if (nx < lo || nx >= hi || nz < lo || nz >= hi || ny < 0 || ny >= CHUNK_HEIGHT)
          continue;
        seedCell(nx, ny, nz, emR, emG, emB);
      }
      // Also seed emitter cell for face sampling
      seedCell(x, y, z, emR, emG, emB);
    };
    for (int z = 0; z < CHUNK_SIZE; ++z)
      for (int y = 0; y < CHUNK_HEIGHT; ++y)
        for (int x = 0; x < CHUNK_SIZE; ++x)
          seedSource(x, y, z, lighting::blockLightEmissionRGB4(getVoxel(x, y, z).type));
    if (halo)
    {
      for (const ChunkLightHalo::Emissive &e : halo->emissives)
      {
        const uint32_t rest = e.hidx / ChunkLightHalo::kExtent;
        const int x = static_cast<int>(e.hidx % ChunkLightHalo::kExtent) - ChunkLightHalo::kRadius;
        const int z = static_cast<int>(rest / CHUNK_HEIGHT) - ChunkLightHalo::kRadius;
        const int y = static_cast<int>(rest % CHUNK_HEIGHT);
        seedSource(x, y, z, e.emRGB4);
      }
    }

    // Propagate block light (coarse BFS, attenuation 1 per channel per step)
    // over [lo, hi) x [0, CHUNK_HEIGHT) x [lo, hi). Overlapping sources
    // combine by per-channel max (issue #141) — the plane-wise max matches
    // lighting::maxBlockLightRGB4. Max is commutative/associative, so the
    // settled field is independent of traversal order and equals the
    // per-source attenuation fixed point.
    size_t head = 0;
    const int dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    while (head < queue.size())
    {
      const glm::ivec3 p = queue[head++];
      const size_t pi = lightIdx(p.x, p.y, p.z);
      const uint8_t curR = blockLightR[pi];
      const uint8_t curG = blockLightG[pi];
      const uint8_t curB = blockLightB[pi];
      if ((curR | curG | curB) <= 1u)
        continue;
      const uint8_t nextR = static_cast<uint8_t>(curR > 1u ? curR - 1u : 0u);
      const uint8_t nextG = static_cast<uint8_t>(curG > 1u ? curG - 1u : 0u);
      const uint8_t nextB = static_cast<uint8_t>(curB > 1u ? curB - 1u : 0u);
      for (auto &d : dirs)
      {
        const int nx = p.x + d[0], ny = p.y + d[1], nz = p.z + d[2];
        if (nx < lo || nx >= hi || nz < lo || nz >= hi || ny < 0 || ny >= CHUNK_HEIGHT)
          continue;
        // Propagate through air-like; allow into solids so faces pick up light
        const size_t i = lightIdx(nx, ny, nz);
        bool improved = false;
        if (blockLightR[i] < nextR) { blockLightR[i] = nextR; improved = true; }
        if (blockLightG[i] < nextG) { blockLightG[i] = nextG; improved = true; }
        if (blockLightB[i] < nextB) { blockLightB[i] = nextB; improved = true; }
        if (improved && isAirLike(nx, ny, nz))
          queue.emplace_back(nx, ny, nz);
      }
    }
  }
}
