#include <Chunk/TerrainGenerator.hpp>
#include <Renderer/MinecraftTextures.hpp>
#include <cmath>

// Vegetation placers (trees, cactus, ice spikes) — split out of TerrainGenerator.cpp
// so core noise/batch generation stays scannable.

void TerrainGenerator::placeTree(ChunkData &chunkData, int localX, int localZ,
                                 int baseY, BiomeType biome, int worldX, int worldZ)
{
  // Mix tree types within biomes for variety
  uint32_t h = treeHash(worldX, worldZ, m_seed + 200);
  switch (biome)
  {
  case BIOME_SNOWY_TAIGA:
    placeSpruceTree(chunkData, localX, localZ, baseY, worldX, worldZ);
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
  case BIOME_SAVANNA:
    placeAcaciaTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  case BIOME_SWAMP:
    // Swamp: oak + occasional mossy floor already from surface mud
    placeOakTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  default:
    placeOakTree(chunkData, localX, localZ, baseY, worldX, worldZ);
    break;
  }
}

void TerrainGenerator::placeOakTree(ChunkData &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ)
{
  uint32_t h = treeHash(worldX, worldZ, m_seed + 100);

  int trunkHeight = 4 + static_cast<int>(h & 3);       // 4–7 blocks
  bool wideCanopy = ((h >> 2) & 7) == 0;               // ~12%: radius-3 canopy
  int extraTopLayers = static_cast<int>((h >> 5) & 1); // 0 or 1 extra cap layer
  int canopyMargin = wideCanopy ? 3 : 2;

  if (localX < canopyMargin || localX >= CHUNK_SIZE - canopyMargin ||
      localZ < canopyMargin || localZ >= CHUNK_SIZE - canopyMargin)
  {
    return;
  }

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

void TerrainGenerator::placeBirchTree(ChunkData &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ)
{
  uint32_t h = treeHash(worldX, worldZ, m_seed + 150);

  int trunkHeight = 5 + static_cast<int>(h & 3);       // 5–8 blocks (birches are tall and slender)
  int extraTopLayers = static_cast<int>((h >> 2) & 1); // 0 or 1 extra cap layer

  if (localX < 2 || localX >= CHUNK_SIZE - 2 || localZ < 2 || localZ >= CHUNK_SIZE - 2)
  {
    return;
  }

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

void TerrainGenerator::placeSpruceTree(ChunkData &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ)
{
  uint32_t h = treeHash(worldX, worldZ, m_seed + 300);

  int trunkHeight = 6 + static_cast<int>(h % 7); // 6–12 blocks
  bool fatVariant = ((h >> 3) % 5) == 0;         // 20%: each layer is one block wider
  bool bareBottom = ((h >> 6) & 3) != 0;         // 75%: lower trunk is exposed (no bottom leaves)
  int maxLayer = bareBottom ? trunkHeight - 3 : trunkHeight - 1;
  int canopyMargin = (maxLayer / 2) + (fatVariant ? 1 : 0);

  if (localX < canopyMargin || localX >= CHUNK_SIZE - canopyMargin ||
      localZ < canopyMargin || localZ >= CHUNK_SIZE - canopyMargin)
  {
    return;
  }

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

void TerrainGenerator::placeJungleTree(ChunkData &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ)
{
  uint32_t h = treeHash(worldX, worldZ, m_seed + 400);

  int trunkHeight = 8 + static_cast<int>(h % 9);         // 8–16 blocks — large variation
  int canopyRadius = 3 + static_cast<int>((h >> 4) & 1); // 3 or 4
  bool hasPropRoots = ((h >> 5) & 3) != 0;               // 75%: extra root-logs close to base

  if (localX < canopyRadius || localX >= CHUNK_SIZE - canopyRadius ||
      localZ < canopyRadius || localZ >= CHUNK_SIZE - canopyRadius)
  {
    return;
  }

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

void TerrainGenerator::placeAcaciaTree(ChunkData &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ)
{
  uint32_t h = treeHash(worldX, worldZ, m_seed + 450);

  int trunkHeight = 4 + static_cast<int>(h & 3); // 4–7, often lean
  int leanX = static_cast<int>((h >> 2) & 1) * (((h >> 3) & 1) ? 1 : -1);
  int leanZ = static_cast<int>((h >> 4) & 1) * (((h >> 5) & 1) ? 1 : -1);
  int canopyRadius = 3 + static_cast<int>((h >> 6) & 1); // 3 or 4 — flat wide top

  if (localX < canopyRadius || localX >= CHUNK_SIZE - canopyRadius ||
      localZ < canopyRadius || localZ >= CHUNK_SIZE - canopyRadius)
    return;

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

void TerrainGenerator::placeDarkOakTree(ChunkData &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ)
{
  uint32_t h = treeHash(worldX, worldZ, m_seed + 480);

  // Thick 2×2 trunk when there is room; otherwise single trunk
  bool thick = (localX >= 2 && localX < CHUNK_SIZE - 3 && localZ >= 2 && localZ < CHUNK_SIZE - 3 &&
                ((h & 3) != 0));
  int trunkHeight = 5 + static_cast<int>((h >> 2) & 3); // 5–8
  int canopyRadius = thick ? 4 : 3;

  if (localX < canopyRadius || localX >= CHUNK_SIZE - canopyRadius ||
      localZ < canopyRadius || localZ >= CHUNK_SIZE - canopyRadius)
    return;

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

void TerrainGenerator::placeCactus(ChunkData &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ)
{
  uint32_t h = treeHash(worldX, worldZ, m_seed + 500);
  int height = 1 + static_cast<int>(h & 3); // 1–4 blocks

  if (localX <= 0 || localX >= CHUNK_SIZE - 1 || localZ <= 0 || localZ >= CHUNK_SIZE - 1)
    return;

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

void TerrainGenerator::placeIceSpike(ChunkData &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ)
{
  uint32_t h = treeHash(worldX, worldZ, m_seed + 600);
  int height = 8 + static_cast<int>(h % 18); // 8–25 tall
  bool thick = ((h >> 4) & 7) == 0;          // rare thicker base
  bool blueCore = ((h >> 8) & 15) == 0;      // rare blue ice core

  if (localX < 1 || localX >= CHUNK_SIZE - 1 || localZ < 1 || localZ >= CHUNK_SIZE - 1)
    return;

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

