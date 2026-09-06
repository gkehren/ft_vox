#include <Chunk/TerrainGenerator.hpp>
#include <Block/BlockTraits.hpp>
#include <cmath>
#include <algorithm>

// Vegetation placers (trees, cactus, ice spikes) — split out of TerrainGenerator.cpp
// so core noise/batch generation stays scannable.

void TerrainGenerator::placeMatureTree(const ChunkGenerationTarget &data, int x, int z,
                                       int base, BiomeType biome, uint32_t hash)
{
  TextureType log = OAK_LOG, leaf = OAK_LEAVES;
  bool conifer = false;
  switch (biome)
  {
  case BIOME_BIRCH_FOREST: log = BIRCH_LOG; leaf = BIRCH_LEAVES; break;
  case BIOME_CHERRY_GROVE: log = CHERRY_LOG; leaf = CHERRY_LEAVES; break;
  case BIOME_DARK_FOREST: case BIOME_AUTUMN_FOREST: log = DARK_OAK_LOG; leaf = DARK_OAK_LEAVES; break;
  case BIOME_JUNGLE: log = JUNGLE_LOG; leaf = JUNGLE_LEAVES; break;
  case BIOME_SWAMP: case BIOME_MANGROVE_SWAMP: log = MANGROVE_LOG; leaf = MANGROVE_LEAVES; break;
  case BIOME_SAVANNA: log = ACACIA_LOG; leaf = ACACIA_LEAVES; break;
  case BIOME_SNOWY_TAIGA: case BIOME_SNOWY_TUNDRA: case BIOME_MOUNTAINS:
  case BIOME_REDWOOD_FOREST: case BIOME_SNOWY_MOUNTAINS:
    log = SPRUCE_LOG; leaf = SPRUCE_LEAVES; conifer = true; break;
  default: break;
  }
  const int height = (conifer ? 23 : biome == BIOME_JUNGLE ? 19 : 11) + static_cast<int>((hash >> 10) % 8u);
  const bool thick = biome != BIOME_BIRCH_FOREST && biome != BIOME_SAVANNA;
  for (int y = 0; y < height; ++y)
    for (int dz = 0; dz <= (thick ? 1 : 0); ++dz)
      for (int dx = 0; dx <= (thick ? 1 : 0); ++dx)
        setVoxelSafe(data, x + dx, base + y, z + dz, log);

  auto crown = [&](int cx, int cy, int cz, int radius, int vertical) {
    for (int dy = -vertical; dy <= vertical; ++dy)
      for (int dz = -radius; dz <= radius; ++dz)
        for (int dx = -radius; dx <= radius; ++dx)
        {
          if (dx * dx + dz * dz + dy * dy * radius * radius / (vertical * vertical) > radius * radius + 1)
            continue;
          // Lobed crowns with sparse, reproducible edge holes.
          const auto edge = treeHash(dx + cx, dz + cz, static_cast<int>(hash ^ static_cast<uint32_t>(dy)));
          if (dx * dx + dz * dz > radius * radius - 2 && (edge & 7u) == 0u) continue;
          const int px = x + cx + dx, pz = z + cz + dz, py = base + cy + dy;
          const auto old = featureVoxel(data, px, py, pz);
          if (old == AIR || blockIsLeaves(old)) setVoxelSafe(data, px, py, pz, leaf);
        }
  };
  if (conifer)
  {
    for (int depth = 0; depth < height - 7; depth += 2)
      crown(0, height - depth, 0, std::min(6, 1 + depth / 3), 1);
  }
  else
  {
    const bool flat = biome == BIOME_SAVANNA || biome == BIOME_CHERRY_GROVE;
    crown(0, height, 0, 4, flat ? 2 : 3);
    constexpr int directions[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    for (int n = 0; n < 4; ++n)
    {
      const auto &dir = directions[(n + (hash >> 20)) % 4];
      const int reach = 3 + static_cast<int>((hash >> (n * 3)) & 1u);
      const int branchY = height - 5 + n % 3;
      for (int k = 0; k <= reach; ++k)
      {
        const int py = base + branchY + k / 2;
        // Two voxels at a rise keep branches face-connected.
        setVoxelSafe(data, x + dir[0] * k, py, z + dir[1] * k, log);
        if (k > 0) setVoxelSafe(data, x + dir[0] * (k - 1), py, z + dir[1] * (k - 1), log);
      }
      crown(dir[0] * reach, branchY + reach / 2 + 1, dir[1] * reach, 3, flat ? 1 : 2);
    }
  }
  // Buttresses remain within the checked 3x3 support footprint.
  if (thick)
    for (int dz = -1; dz <= 1; ++dz)
      for (int dx = -1; dx <= 1; ++dx)
        if (std::abs(dx) + std::abs(dz) == 1)
          for (int y = -1; y <= 0; ++y)
            setVoxelSafe(data, x + dx, base + y, z + dz, biome == BIOME_MANGROVE_SWAMP ? MANGROVE_ROOTS : log);
}

void TerrainGenerator::placeTree(const ChunkGenerationTarget &chunkData, int localX, int localZ,
                                 int baseY, BiomeType biome, int worldX, int worldZ)
{
  // Mix tree types within biomes for variety
  uint32_t h = treeHash(worldX, worldZ, m_seed + 200);
  switch (biome)
  {
  case BIOME_SNOWY_TAIGA:
    placeSpruceTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  case BIOME_REDWOOD_FOREST:
    placeRedwoodTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  case BIOME_CHERRY_GROVE:
    placeCherryTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  case BIOME_MOUNTAINS:
    // 70% spruce, 30% oak
    if ((h % 10) < 7)
      placeSpruceTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    else
      placeOakTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  case BIOME_BIRCH_FOREST:
    // 80% birch, 20% oak
    if ((h % 5) < 4)
      placeBirchTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    else
      placeOakTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  case BIOME_DARK_FOREST:
  case BIOME_AUTUMN_FOREST:
    // 60% dark oak, 25% spruce, 15% birch — gloomy mixed canopy
    {
      uint32_t r = h % 20;
      if (r < 12)
        placeDarkOakTree(chunkData, localX, localZ, baseY, worldX, worldZ);
      else if (r < 17)
        placeSpruceTree(chunkData, localX, localZ, baseY, worldX, worldZ);
      else
        placeBirchTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    }
    break;
  case BIOME_FOREST:
    // 70% oak, 30% birch
    if ((h % 10) < 7)
      placeOakTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    else
      placeBirchTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  case BIOME_JUNGLE:
    placeJungleTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  case BIOME_MANGROVE_SWAMP:
    placeMangroveTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  case BIOME_BAMBOO_JUNGLE:
    placeBamboo(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  case BIOME_SAVANNA:
    placeAcaciaTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  case BIOME_OASIS:
    placePalmTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  case BIOME_SWAMP:
    // Swamp: oak + occasional mossy floor already from surface mud
    placeOakTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  case BIOME_MOOR:
    if ((h & 1u) == 0u)
      placeSpruceTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    else
      placeOakTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  case BIOME_MUSHROOM_FIELDS:
    placeGiantMushroom(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  default:
    placeOakTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  }
}

void TerrainGenerator::placeOakTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ)
{
  uint32_t h = treeHash(worldX, worldZ, m_seed + 100);

  int trunkHeight = 4 + static_cast<int>(h & 3);       // 4–7 blocks
  bool wideCanopy = ((h >> 2) & 7) == 0;               // ~12%: radius-3 canopy
  int extraTopLayers = static_cast<int>((h >> 5) & 1); // 0 or 1 extra cap layer

  // No margin guard: candidates may root outside the chunk (cross-chunk
  // canopies); setVoxelSafe clips voxels to this chunk.
  for (int y = 0; y < trunkHeight; ++y)
    setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::OAK_LOG);

  int topY = baseY + trunkHeight;
  int crownRadius = wideCanopy ? 3 : 2;
  // Bottom layers: crownRadius wide, corners trimmed
  // Top cap layers: radius 1
  int leafBottom = topY - 2;
  int leafCapBase = topY + 1;
  int leafTop = topY + 1 + extraTopLayers;

  for (int ly = leafBottom; ly <= leafTop; ++ly)
  {
    int radius = (ly >= leafCapBase) ? 1 : crownRadius;
    for (int dx = -radius; dx <= radius; ++dx)
    {
      for (int dz = -radius; dz <= radius; ++dz)
      {
        // Trim corners on wide layers for a rounder silhouette
        if (radius >= 2 && std::abs(dx) == radius && std::abs(dz) == radius)
          continue;
        // Leave trunk column intact
        if (dx == 0 && dz == 0 && ly < topY)
          continue;
        setVoxelSafe(chunkData, localX + dx, ly, localZ + dz, TextureType::OAK_LEAVES);
      }
    }
  }
}

void TerrainGenerator::placeBirchTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ)
{
  uint32_t h = treeHash(worldX, worldZ, m_seed + 150);

  int trunkHeight = 5 + static_cast<int>(h & 3);       // 5–8 blocks (birches are tall and slender)
  int extraTopLayers = static_cast<int>((h >> 2) & 1); // 0 or 1 extra cap layer

  for (int y = 0; y < trunkHeight; ++y)
    setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::BIRCH_LOG);

  int topY = baseY + trunkHeight;
  // Birch crown: slender (radius 2 min, radius 1 cap), no wide variant
  int leafBottom = topY - 3;
  int leafCapBase = topY + 1;
  int leafTop = topY + 1 + extraTopLayers;

  for (int ly = leafBottom; ly <= leafTop; ++ly)
  {
    int radius = (ly >= leafCapBase) ? 1 : 2;
    for (int dx = -radius; dx <= radius; ++dx)
    {
      for (int dz = -radius; dz <= radius; ++dz)
      {
        // Birches have tighter corners — skip all outer diagonal cells on radius-2 layers,
        // except the second layer from bottom which keeps its full square
        if (radius == 2 && std::abs(dx) == 2 && std::abs(dz) == 2 && ly != leafBottom + 1)
          continue;
        if (dx == 0 && dz == 0 && ly < topY)
          continue;
        setVoxelSafe(chunkData, localX + dx, ly, localZ + dz, TextureType::BIRCH_LEAVES);
      }
    }
  }
}

void TerrainGenerator::placeSpruceTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ)
{
  uint32_t h = treeHash(worldX, worldZ, m_seed + 300);

  int trunkHeight = 6 + static_cast<int>(h % 7); // 6–12 blocks
  bool fatVariant = ((h >> 3) % 5) == 0;         // 20%: each layer is one block wider
  bool bareBottom = ((h >> 6) & 3) != 0;         // 75%: lower trunk is exposed (no bottom leaves)
  int maxLayer = bareBottom ? trunkHeight - 3 : trunkHeight - 1;

  for (int y = 0; y < trunkHeight; ++y)
    setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::SPRUCE_LOG);

  // Apex leaf block
  setVoxelSafe(chunkData, localX, baseY + trunkHeight, localZ, TextureType::SPRUCE_LEAVES);

  // Stepped cone: every-other layer, working down from apex
  for (int layer = 0; layer <= maxLayer; ++layer)
  {
    if (layer % 2 != 0)
      continue; // stepped — only even layers get leaves

    int ly = baseY + trunkHeight - 1 - layer;
    int radius = layer / 2;
    if (fatVariant && layer > 0)
      ++radius; // fat variant: bump radius on non-apex layers
    // Keep the widest layers within MAX_TREE_RADIUS so cross-chunk candidates
    // (evaluated in a ring of that width) place complete canopies.
    if (radius > worldgen::smallTree.radius)
      radius = worldgen::smallTree.radius;

    for (int dx = -radius; dx <= radius; ++dx)
    {
      for (int dz = -radius; dz <= radius; ++dz)
      {
        if (dx == 0 && dz == 0)
          continue; // trunk stays as log
        // Use a loose diamond shape at larger radii for a softer silhouette
        if (radius >= 3 && std::abs(dx) + std::abs(dz) > radius + 1)
          continue;
        setVoxelSafe(chunkData, localX + dx, ly, localZ + dz, TextureType::SPRUCE_LEAVES);
      }
    }
  }
}

void TerrainGenerator::placeJungleTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ)
{
  uint32_t h = treeHash(worldX, worldZ, m_seed + 400);

  int trunkHeight = 8 + static_cast<int>(h % 9);         // 8–16 blocks — large variation
  int canopyRadius = 3 + static_cast<int>((h >> 4) & 1); // 3 or 4
  bool hasPropRoots = ((h >> 5) & 3) != 0;               // 75%: extra root-logs close to base

  for (int y = 0; y < trunkHeight; ++y)
    setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::JUNGLE_LOG);

  // Prop roots: 2-block-tall extra logs at the 4 cardinal sides of the base
  if (hasPropRoots)
  {
    const int offsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    int rootHeight = 2 + static_cast<int>((h >> 7) & 1); // 2 or 3
    for (auto &off : offsets)
    {
      for (int ry = 0; ry < rootHeight; ++ry)
        setVoxelSafe(chunkData, localX + off[0], baseY + ry, localZ + off[1], TextureType::JUNGLE_LOG);
    }
  }

  // Spherical-ish canopy centred slightly below apex so it isn't too top-heavy
  int topY = baseY + trunkHeight;
  int canopyCentreY = topY - 1;

  for (int ly = canopyCentreY - canopyRadius + 1; ly <= canopyCentreY + 2; ++ly)
  {
    float dy = static_cast<float>(ly - canopyCentreY);
    // Oblate ellipsoid: compress vertically so the canopy is wide and flat
    float rSq = static_cast<float>(canopyRadius * canopyRadius) - dy * dy * 2.0f;
    if (rSq < 0.0f)
      continue;
    int layerRadius = static_cast<int>(std::sqrt(rSq));

    for (int dx = -layerRadius; dx <= layerRadius; ++dx)
    {
      for (int dz = -layerRadius; dz <= layerRadius; ++dz)
      {
        if (dx * dx + dz * dz > layerRadius * layerRadius + 1)
          continue;
        if (dx == 0 && dz == 0 && ly < topY)
          continue;
        setVoxelSafe(chunkData, localX + dx, ly, localZ + dz, TextureType::JUNGLE_LEAVES);
      }
    }
  }
}

void TerrainGenerator::placeAcaciaTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ)
{
  uint32_t h = treeHash(worldX, worldZ, m_seed + 450);

  int trunkHeight = 4 + static_cast<int>(h & 3); // 4–7, often lean
  int leanX = static_cast<int>((h >> 2) & 1) * (((h >> 3) & 1) ? 1 : -1);
  int leanZ = static_cast<int>((h >> 4) & 1) * (((h >> 5) & 1) ? 1 : -1);
  int canopyRadius = 3 + static_cast<int>((h >> 6) & 1); // 3 or 4 — flat wide top

  int tipX = localX;
  int tipZ = localZ;
  for (int y = 0; y < trunkHeight; ++y)
  {
    // Mild diagonal lean on the upper half
    if (y >= trunkHeight / 2)
    {
      tipX = localX + leanX;
      tipZ = localZ + leanZ;
    }
    setVoxelSafe(chunkData, tipX, baseY + y, tipZ, TextureType::ACACIA_LOG);
  }

  // Flat umbrella canopy at the tip
  int topY = baseY + trunkHeight;
  for (int ly = topY; ly <= topY + 1; ++ly)
  {
    int radius = (ly == topY) ? canopyRadius : canopyRadius - 1;
    for (int dx = -radius; dx <= radius; ++dx)
    {
      for (int dz = -radius; dz <= radius; ++dz)
      {
        if (dx * dx + dz * dz > radius * radius + 1)
          continue;
        if (dx == 0 && dz == 0 && ly == topY)
          continue;
        setVoxelSafe(chunkData, tipX + dx, ly, tipZ + dz, TextureType::ACACIA_LEAVES);
      }
    }
  }
}

void TerrainGenerator::placeDarkOakTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ)
{
  uint32_t h = treeHash(worldX, worldZ, m_seed + 480);

  // Thick 2×2 trunk for most dark oaks. Hash-only decision (no position
  // condition) so every chunk evaluating this candidate agrees; setVoxelSafe
  // clips the 2x2 trunk at chunk edges.
  bool thick = ((h & 3) != 0);
  int trunkHeight = 5 + static_cast<int>((h >> 2) & 3); // 5–8
  int canopyRadius = thick ? 4 : 3;

  for (int y = 0; y < trunkHeight; ++y)
  {
    setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::DARK_OAK_LOG);
    if (thick)
    {
      setVoxelSafe(chunkData, localX + 1, baseY + y, localZ, TextureType::DARK_OAK_LOG);
      setVoxelSafe(chunkData, localX, baseY + y, localZ + 1, TextureType::DARK_OAK_LOG);
      setVoxelSafe(chunkData, localX + 1, baseY + y, localZ + 1, TextureType::DARK_OAK_LOG);
    }
  }

  int topY = baseY + trunkHeight;
  const float centerX = thick ? static_cast<float>(localX) + 0.5f : static_cast<float>(localX);
  const float centerZ = thick ? static_cast<float>(localZ) + 0.5f : static_cast<float>(localZ);

  for (int ly = topY - 1; ly <= topY + 2; ++ly)
  {
    int radius = (ly >= topY + 1) ? canopyRadius - 1 : canopyRadius;
    for (int dx = -radius; dx <= radius; ++dx)
    {
      for (int dz = -radius; dz <= radius; ++dz)
      {
        const float fx = static_cast<float>(localX + dx) - centerX;
        const float fz = static_cast<float>(localZ + dz) - centerZ;
        if (fx * fx + fz * fz > static_cast<float>(radius * radius) + 0.5f)
          continue;
        // Keep trunk columns as log below the crown top
        if (ly < topY)
        {
          const bool onTrunk = (dx == 0 && dz == 0) ||
                               (thick && ((dx == 0 || dx == 1) && (dz == 0 || dz == 1)));
          if (onTrunk)
            continue;
        }
        setVoxelSafe(chunkData, localX + dx, ly, localZ + dz, TextureType::DARK_OAK_LEAVES);
      }
    }
  }
}

void TerrainGenerator::placeCherryTree(const ChunkGenerationTarget &chunkData, int localX, int localZ,
                                       int baseY, int worldX, int worldZ)
{
  const uint32_t h = treeHash(worldX, worldZ, m_seed + 520);
  const int trunkHeight = 5 + static_cast<int>(h % 4); // 5-8
  for (int y = 0; y < trunkHeight; ++y)
    setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::CHERRY_LOG);

  const int topY = baseY + trunkHeight;
  for (int layer = -2; layer <= 2; ++layer)
  {
    const int radius = layer <= 0 ? 4 : 3 - layer;
    for (int dx = -radius; dx <= radius; ++dx)
    {
      for (int dz = -radius; dz <= radius; ++dz)
      {
        if (dx * dx + dz * dz > radius * radius + 2)
          continue;
        if (((treeHash(worldX + dx, worldZ + dz, m_seed + 521 + layer) >> 3) & 15u) == 0u)
          continue;
        if (dx == 0 && dz == 0 && layer < 0)
          continue;
        setVoxelSafe(chunkData, localX + dx, topY + layer, localZ + dz,
                     TextureType::CHERRY_LEAVES);
      }
    }
  }
}

void TerrainGenerator::placeRedwoodTree(const ChunkGenerationTarget &chunkData, int localX, int localZ,
                                        int baseY, int worldX, int worldZ)
{
  const uint32_t h = treeHash(worldX, worldZ, m_seed + 540);
  const int trunkHeight = 16 + static_cast<int>(h % 15); // 16-30

  for (int y = 0; y < trunkHeight; ++y)
  {
    setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::SPRUCE_LOG);
    setVoxelSafe(chunkData, localX + 1, baseY + y, localZ, TextureType::SPRUCE_LOG);
    setVoxelSafe(chunkData, localX, baseY + y, localZ + 1, TextureType::SPRUCE_LOG);
    setVoxelSafe(chunkData, localX + 1, baseY + y, localZ + 1, TextureType::SPRUCE_LOG);
  }

  const int topY = baseY + trunkHeight;
  for (int depth = 0; depth <= 12; depth += 2)
  {
    const int ly = topY - depth;
    const int radius = std::min(worldgen::smallTree.radius, 1 + depth / 3);
    for (int dx = 1 - radius; dx <= radius; ++dx)
    {
      for (int dz = 1 - radius; dz <= radius; ++dz)
      {
        const int distance = std::abs(dx) + std::abs(dz);
        if (distance > radius + 2)
          continue;
        const bool trunkCell = (dx == 0 || dx == 1) && (dz == 0 || dz == 1);
        if (trunkCell && ly < topY)
          continue;
        setVoxelSafe(chunkData, localX + dx, ly, localZ + dz,
                     TextureType::SPRUCE_LEAVES);
      }
    }
  }
  setVoxelSafe(chunkData, localX, topY + 1, localZ, TextureType::SPRUCE_LEAVES);
  setVoxelSafe(chunkData, localX + 1, topY + 1, localZ, TextureType::SPRUCE_LEAVES);
}

void TerrainGenerator::placePalmTree(const ChunkGenerationTarget &chunkData, int localX, int localZ,
                                     int baseY, int worldX, int worldZ)
{
  const uint32_t h = treeHash(worldX, worldZ, m_seed + 560);
  const int trunkHeight = 7 + static_cast<int>(h % 5); // 7-11
  static constexpr int kDirections[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  const int *direction = kDirections[(h >> 8) & 3u];
  int tipX = localX;
  int tipZ = localZ;

  for (int y = 0; y < trunkHeight; ++y)
  {
    if (y >= (trunkHeight * 2) / 3)
    {
      tipX = localX + direction[0];
      tipZ = localZ + direction[1];
    }
    setVoxelSafe(chunkData, tipX, baseY + y, tipZ, TextureType::JUNGLE_LOG);
  }

  const int topY = baseY + trunkHeight;
  setVoxelSafe(chunkData, tipX, topY, tipZ, TextureType::JUNGLE_LEAVES);
  for (const auto &arm : kDirections)
  {
    for (int step = 1; step <= 3; ++step)
    {
      const int drop = step == 3 ? 1 : 0;
      setVoxelSafe(chunkData, tipX + arm[0] * step, topY - drop,
                   tipZ + arm[1] * step, TextureType::JUNGLE_LEAVES);
    }
  }
}

void TerrainGenerator::placeMangroveTree(const ChunkGenerationTarget &chunkData, int localX, int localZ,
                                         int baseY, int worldX, int worldZ)
{
  const uint32_t h = treeHash(worldX, worldZ, m_seed + 580);
  const int trunkHeight = 6 + static_cast<int>(h % 5); // 6-10
  static constexpr int kRoots[8][2] = {
      {1, 0}, {-1, 0}, {0, 1}, {0, -1},
      {1, 1}, {-1, 1}, {1, -1}, {-1, -1},
  };

  for (int y = 0; y < trunkHeight; ++y)
    setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::MANGROVE_LOG);

  for (size_t i = 0; i < std::size(kRoots); ++i)
  {
    if (((h >> (i % 16)) & 1u) == 0u)
      continue;
    const int dx = kRoots[i][0];
    const int dz = kRoots[i][1];
    setVoxelSafe(chunkData, localX + dx, baseY, localZ + dz,
                 TextureType::MANGROVE_ROOTS);
    if (i < 4)
      setVoxelSafe(chunkData, localX + dx * 2, baseY - 1, localZ + dz * 2,
                   TextureType::MANGROVE_ROOTS);
  }

  const int topY = baseY + trunkHeight;
  for (int ly = topY - 2; ly <= topY + 2; ++ly)
  {
    const int radius = ly == topY + 2 ? 2 : 4;
    for (int dx = -radius; dx <= radius; ++dx)
    {
      for (int dz = -radius; dz <= radius; ++dz)
      {
        if (dx * dx + dz * dz > radius * radius + 1)
          continue;
        if (dx == 0 && dz == 0 && ly < topY)
          continue;
        setVoxelSafe(chunkData, localX + dx, ly, localZ + dz,
                     TextureType::MANGROVE_LEAVES);
      }
    }
  }
}

void TerrainGenerator::placeBamboo(const ChunkGenerationTarget &chunkData, int localX, int localZ,
                                   int baseY, int worldX, int worldZ)
{
  const uint32_t h = treeHash(worldX, worldZ, m_seed + 600);
  const int height = 6 + static_cast<int>(h % 9); // 6-14
  for (int y = 0; y < height; ++y)
  {
    const TextureType type = y + 1 == height ? TextureType::BAMBOO_BLOCK
                                             : TextureType::BAMBOO_STALK;
    setVoxelSafe(chunkData, localX, baseY + y, localZ, type);
  }
}

void TerrainGenerator::placeGiantMushroom(const ChunkGenerationTarget &chunkData, int localX,
                                          int localZ, int baseY, int worldX,
                                          int worldZ)
{
  const uint32_t h = treeHash(worldX, worldZ, m_seed + 620);
  const int stemHeight = 5 + static_cast<int>(h % 5); // 5-9
  const bool red = ((h >> 8) & 1u) == 0u;
  const TextureType cap = red ? TextureType::RED_MUSHROOM_BLOCK
                              : TextureType::BROWN_MUSHROOM_BLOCK;

  for (int y = 0; y < stemHeight; ++y)
    setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::MUSHROOM_STEM);

  const int topY = baseY + stemHeight;
  const int radius = red ? 3 : 4;
  for (int dx = -radius; dx <= radius; ++dx)
  {
    for (int dz = -radius; dz <= radius; ++dz)
    {
      if (dx * dx + dz * dz > radius * radius + 1)
        continue;
      const int edge = std::max(std::abs(dx), std::abs(dz));
      const int ly = topY - (edge == radius ? 1 : 0);
      setVoxelSafe(chunkData, localX + dx, ly, localZ + dz, cap);
    }
  }
  setVoxelSafe(chunkData, localX, topY + 1, localZ, cap);
}

void TerrainGenerator::placeCactus(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ)
{
  uint32_t h = treeHash(worldX, worldZ, m_seed + 500);
  int height = 1 + static_cast<int>(h & 3); // 1–4 blocks

  for (int y = 0; y < height; ++y)
  {
    bool canPlace = true;
    for (int dx = -1; dx <= 1 && canPlace; ++dx)
    {
      for (int dz = -1; dz <= 1 && canPlace; ++dz)
      {
        if (dx == 0 && dz == 0)
          continue;
        int nx = localX + dx, nz = localZ + dz;
        if (nx >= 0 && nx < CHUNK_SIZE && nz >= 0 && nz < CHUNK_SIZE)
        {
          int checkIndex = getVoxelIndex(nx, baseY + y, nz);
          auto nt = static_cast<TextureType>(chunkData.voxels[checkIndex].type);
          if (nt != TextureType::AIR && nt != TextureType::SAND && nt != TextureType::RED_SAND)
            canPlace = false;
        }
      }
    }
    if (canPlace)
      setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::CACTUS);
  }
}

void TerrainGenerator::placeIceSpike(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ)
{
  uint32_t h = treeHash(worldX, worldZ, m_seed + 600);
  int height = 8 + static_cast<int>(h % 18); // 8–25 tall
  bool thick = ((h >> 4) & 7) == 0;          // rare thicker base
  bool blueCore = ((h >> 8) & 15) == 0;      // rare blue ice core

  for (int y = 0; y < height; ++y)
  {
    // Taper: full column lower third, then shrink
    float t = static_cast<float>(y) / static_cast<float>(std::max(1, height - 1));
    int radius = 0;
    if (thick && t < 0.35f)
      radius = 1;
    else if (t > 0.85f)
      radius = 0;

    TextureType iceType = TextureType::PACKED_ICE;
    if (blueCore && y < height / 3)
      iceType = TextureType::BLUE_ICE;

    for (int dx = -radius; dx <= radius; ++dx)
    {
      for (int dz = -radius; dz <= radius; ++dz)
      {
        if (radius > 0 && std::abs(dx) + std::abs(dz) > radius + 1)
          continue;
        setVoxelSafe(chunkData, localX + dx, baseY + y, localZ + dz, iceType);
      }
    }
  }
}
