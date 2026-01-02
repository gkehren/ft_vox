#include <Chunk/TerrainGenerator.hpp>
#include <algorithm>
#include <cmath>

TerrainGenerator::TerrainGenerator(int seed) : m_seed(seed) {
  setupNoiseGenerators();
}

void TerrainGenerator::setupNoiseGenerators() {
  // Continental noise - Large scale continent shapes
  // Controls overall terrain elevation (oceans vs land vs mountains)
  auto continentalBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto continentalFractal = FastNoise::New<FastNoise::FractalFBm>();
  continentalFractal->SetSource(continentalBase);
  continentalFractal->SetOctaveCount(5);
  continentalFractal->SetLacunarity(2.0f);
  continentalFractal->SetGain(0.45f);

  auto continentalScale = FastNoise::New<FastNoise::DomainScale>();
  continentalScale->SetSource(continentalFractal);
  continentalScale->SetScale(
      0.0008f); // Very large scale for smooth continent-sized features
  m_continentalNoise = continentalScale;

  // Erosion noise - Controls terrain smoothness vs. roughness
  // Low values = rough/sharp, High values = smooth/eroded
  auto erosionBase = FastNoise::New<FastNoise::Perlin>();
  auto erosionFractal = FastNoise::New<FastNoise::FractalFBm>();
  erosionFractal->SetSource(erosionBase);
  erosionFractal->SetOctaveCount(4);
  erosionFractal->SetLacunarity(2.0f);
  erosionFractal->SetGain(0.5f);

  auto erosionScale = FastNoise::New<FastNoise::DomainScale>();
  erosionScale->SetSource(erosionFractal);
  erosionScale->SetScale(0.002f); // Large scale for smooth erosion zones
  m_erosionNoise = erosionScale;

  // Peaks and Valleys noise - Local height variation
  auto pvBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto pvFractal = FastNoise::New<FastNoise::FractalFBm>();
  pvFractal->SetSource(pvBase);
  pvFractal->SetOctaveCount(6);
  pvFractal->SetLacunarity(2.0f);
  pvFractal->SetGain(0.5f);

  auto pvScale = FastNoise::New<FastNoise::DomainScale>();
  pvScale->SetSource(pvFractal);
  pvScale->SetScale(0.006f); // Medium scale for terrain details
  m_peaksValleysNoise = pvScale;

  // Ridge noise - For sharp mountain peaks and dramatic formations
  // Low frequency for few, large peaks
  auto ridgeBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto ridgeFractal = FastNoise::New<FastNoise::FractalRidged>();
  ridgeFractal->SetSource(ridgeBase);
  ridgeFractal->SetOctaveCount(3); // Fewer octaves for broader peaks
  ridgeFractal->SetLacunarity(2.0f);
  ridgeFractal->SetGain(0.5f);

  auto ridgeScale = FastNoise::New<FastNoise::DomainScale>();
  ridgeScale->SetSource(ridgeFractal);
  ridgeScale->SetScale(0.002f); // Large scale for few, dramatic peaks
  m_ridgeNoise = ridgeScale;

  // Cave noise - 3D Simplex "cheese" caves
  auto caveBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto caveFractal = FastNoise::New<FastNoise::FractalFBm>();
  caveFractal->SetSource(caveBase);
  caveFractal->SetOctaveCount(2);
  caveFractal->SetLacunarity(2.0f);
  caveFractal->SetGain(0.5f);

  auto caveScale = FastNoise::New<FastNoise::DomainScale>();
  caveScale->SetSource(caveFractal);
  caveScale->SetScale(0.02f); // Medium scale for traversable caves
  m_caveNoise = caveScale;

  // Ravine noise - Vertical faults
  // Using Ridged noise for crack-like structures
  auto ravineBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto ravineFractal = FastNoise::New<FastNoise::FractalRidged>();
  ravineFractal->SetSource(ravineBase);
  ravineFractal->SetOctaveCount(2);
  ravineFractal->SetLacunarity(2.0f);
  ravineFractal->SetGain(0.5f);

  // Anisotropic scaling for tall, narrow ravines
  // Scale X/Z by normal amount, Y by small amount to stretch features
  // vertically Since basic DomainScale is uniform, we rely on the fractal
  // characteristics or we can use a trick: separate 2D noise for path and 1D
  // for depth? For now, using a slightly different scale and high threshold to
  // create linear-ish features.
  auto ravineScale = FastNoise::New<FastNoise::DomainScale>();
  ravineScale->SetSource(ravineFractal);
  ravineScale->SetScale(0.005f);
  m_ravineNoise = ravineScale;

  // Ore Noise - High frequency simplex
  auto oreBase = FastNoise::New<FastNoise::OpenSimplex2>();
  m_oreNoise = oreBase;

  setupOres();
}

void TerrainGenerator::setupOres() {
  // Configure Ore Definitions
  // Type, MinH, MaxH, Scale, Threshold, SeedOffset

  // Coal: Common, large veins, everywhere (Increased threshold 0.6 -> 0.65)
  m_ores.push_back({TextureType::COAL_ORE, 0, 128, 0.1f, 0.65f, 10000});

  // Iron: Common, medium veins, mid-depth (0.7 -> 0.75)
  m_ores.push_back({TextureType::IRON_ORE, 0, 64, 0.12f, 0.75f, 11000});

  // Copper: Medium, mid-high (0.7 -> 0.75)
  m_ores.push_back({TextureType::COPPER_ORE, 0, 96, 0.11f, 0.75f, 12000});

  // Gold: Rare, small veins, deep (0.8 -> 0.85)
  m_ores.push_back({TextureType::GOLD_ORE, 0, 32, 0.15f, 0.85f, 13000});

  // Lapis: Rare, deep (0.8 -> 0.85)
  m_ores.push_back({TextureType::LAPIS_ORE, 0, 32, 0.15f, 0.85f, 14000});

  // Redstone: Common deep, scattered (0.75 -> 0.8)
  m_ores.push_back({TextureType::REDSTONE_ORE, 0, 16, 0.15f, 0.8f, 15000});

  // Diamond: Very rare, tiny veins, very deep (0.85 -> 0.9)
  m_ores.push_back({TextureType::DIAMOND_ORE, 1, 16, 0.18f, 0.9f, 16000});

  // Emerald: Rare, deep (0.9 -> 0.93)
  m_ores.push_back({TextureType::EMERALD_ORE, 4, 32, 0.18f, 0.93f, 17000});
}

ChunkData TerrainGenerator::generateChunk(int chunkX, int chunkZ) {
  ChunkData chunkData;
  chunkData.voxels.fill({TextureType::AIR});

  // Generate the main chunk data using batch noise
  generateChunkBatch(chunkData, chunkX, chunkZ);

  // Generate border voxels for mesh optimization
  generateChunkBorders(chunkData, chunkX, chunkZ);

  return chunkData;
}

void TerrainGenerator::generateChunkBatch(ChunkData &chunkData, int chunkX,
                                          int chunkZ) {
  constexpr int totalPoints = CHUNK_SIZE * CHUNK_SIZE;
  constexpr int totalVoxels = CHUNK_VOLUME;

  // Allocate noise output buffers
  std::vector<float> continentalResults(totalPoints);
  std::vector<float> erosionResults(totalPoints);
  std::vector<float> peaksValleysResults(totalPoints);
  std::vector<float> ridgeResults(totalPoints);
  std::vector<int> heightMap(totalPoints);

  // 3D Noise buffers
  std::vector<float> caveResults(totalVoxels);
  std::vector<float> ravineResults(totalVoxels);

  // Generate noise using GenUniformGrid2D for efficient batch processing
  m_continentalNoise->GenUniformGrid2D(continentalResults.data(), chunkX,
                                       chunkZ, CHUNK_SIZE, CHUNK_SIZE, 1.0f,
                                       m_seed);

  m_erosionNoise->GenUniformGrid2D(erosionResults.data(), chunkX, chunkZ,
                                   CHUNK_SIZE, CHUNK_SIZE, 1.0f, m_seed + 1000);

  m_peaksValleysNoise->GenUniformGrid2D(peaksValleysResults.data(), chunkX,
                                        chunkZ, CHUNK_SIZE, CHUNK_SIZE, 1.0f,
                                        m_seed + 2000);

  m_ridgeNoise->GenUniformGrid2D(ridgeResults.data(), chunkX, chunkZ,
                                 CHUNK_SIZE, CHUNK_SIZE, 1.0f, m_seed + 3000);

  // Generate 3D noise for caves and ravines
  m_caveNoise->GenUniformGrid3D(caveResults.data(), chunkX, 0, chunkZ,
                                CHUNK_SIZE, CHUNK_HEIGHT, CHUNK_SIZE, 1.0f,
                                m_seed + 4000);

  m_ravineNoise->GenUniformGrid3D(ravineResults.data(), chunkX, 0, chunkZ,
                                  CHUNK_SIZE, CHUNK_HEIGHT, CHUNK_SIZE,
                                  1.0f, // Frequency
                                  m_seed + 5000);

  // Calculate height map from noise values
  for (int i = 0; i < totalPoints; ++i) {
    heightMap[i] = calculateHeight(continentalResults[i], erosionResults[i],
                                   peaksValleysResults[i], ridgeResults[i]);
  }

  // Generate voxel columns
  for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ) {
    for (int localX = 0; localX < CHUNK_SIZE; ++localX) {
      int colIndex = localZ * CHUNK_SIZE + localX;
      int terrainHeight = heightMap[colIndex];

      // Fill column
      for (int y = 0; y < CHUNK_HEIGHT; ++y) {
        int voxelIndex = getVoxelIndex(localX, y, localZ);

        // FastNoise 3D index mapping
        // GenUniformGrid3D(..., xSize, ySize, zSize) matches loops:
        // for z < zSize; for y < ySize; for x < xSize
        // Index = z * (ySize * xSize) + y * xSize + x
        // We called it with (SIZE, HEIGHT, SIZE) so:
        // Index = localZ * (HEIGHT * SIZE) + y * SIZE + localX
        int noiseIndex =
            localZ * (CHUNK_HEIGHT * CHUNK_SIZE) + y * CHUNK_SIZE + localX;

        float caveVal = caveResults[noiseIndex];
        float ravineVal = ravineResults[noiseIndex];

        // Determine basic terrain type
        TextureType type = getVoxelTypeAt(y, terrainHeight);

        // Carve caves and ravines
        // Bedrock is immutable
        if (type != TextureType::BEDROCK && type != TextureType::AIR) {
          // Bias: Reduce caves near surface
          // Updated bias: Steeper ramp-up for caves (0.35f factor)
          // Added bias relative to surface for Ravines too
          float heightRatio =
              std::clamp(static_cast<float>(y - SEA_LEVEL) / 64.0f, 0.0f, 1.0f);

          // 0.6 -> 0.95 at peak
          float caveThreshold = 0.6f + heightRatio * 0.35f;

          // 0.8 -> 0.95 at peak for ravines
          float ravineThreshold = 0.8f + heightRatio * 0.15f;

          // Cave Threshold
          if (caveVal > caveThreshold) {
            type = TextureType::AIR;
          }

          // Ravine Threshold
          if (type != TextureType::AIR && ravineVal > ravineThreshold) {
            type = TextureType::AIR;
          }
        }

        chunkData.voxels[voxelIndex].type = type;
      }
    }
  }

  // Ore Generation (Post-processing on STONE blocks)
  // We do this in a separate pass per ore to use batch noise generation
  // effectively Optimization: Only generate noise for the relevant height slab
  for (const auto &ore : m_ores) {
    int minY = std::max(0, ore.minHeight);
    int maxY = std::min(CHUNK_HEIGHT, ore.maxHeight);
    if (minY >= maxY)
      continue;

    int heightSize = maxY - minY;
    std::vector<float> oreNoise(CHUNK_SIZE * heightSize * CHUNK_SIZE);

    // Generate noise for this slab
    // frequency argument is passed as 'frequency' to GenUniformGrid3D
    m_oreNoise->GenUniformGrid3D(oreNoise.data(), chunkX, minY, chunkZ,
                                 CHUNK_SIZE, heightSize, CHUNK_SIZE, ore.scale,
                                 m_seed + ore.seedOffset);

    // Apply ore to voxels
    int noiseIdx = 0;
    // GenUniformGrid3D loop order: Z, Y, X
    for (int z = 0; z < CHUNK_SIZE; ++z) {
      for (int y = 0; y < heightSize; ++y) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
          float noiseVal = oreNoise[noiseIdx++];

          // Map back to chunk coordinates
          int worldY = minY + y;

          // Direct voxel access
          int voxelIndex = getVoxelIndex(x, worldY, z);

          // Only replace STONE with Ore
          if (chunkData.voxels[voxelIndex].type == TextureType::STONE) {
            if (noiseVal > ore.threshold) {
              chunkData.voxels[voxelIndex].type = ore.type;
            }
          }
        }
      }
    }
  }
}

int TerrainGenerator::calculateHeight(float continental, float erosion,
                                      float peaksValleys, float ridge) const {
  // Clamp noise values to [-1, 1] range
  continental = std::clamp(continental, -1.0f, 1.0f);
  erosion = std::clamp(erosion, -1.0f, 1.0f);
  peaksValleys = std::clamp(peaksValleys, -1.0f, 1.0f);
  ridge = std::clamp(ridge, -1.0f, 1.0f);

  // Normalize to [0, 1] range
  float continentalFactor = (continental + 1.0f) * 0.5f;
  float erosionFactor = (erosion + 1.0f) * 0.5f;

  // ============================================
  // TERRAIN HEIGHT CALCULATION
  // Target heights:
  //   Ocean: 35-64 (below sea level)
  //   Beach: 64-70
  //   Plains: 70-120 (gradual variation)
  //   Hills: 90-150 (gentle elevated terrain)
  //   Mountains: 120-240 (dramatic peaks)
  // ============================================

  float baseHeight = static_cast<float>(SEA_LEVEL); // 64

  // Define terrain type weights using smooth transitions
  // Ocean weight: continental < 0.3
  float oceanWeight = 1.0f - smoothstep(0.25f, 0.32f, continentalFactor);

  // Beach/coastal weight: continental 0.3-0.4
  float beachWeight = smoothstep(0.25f, 0.32f, continentalFactor) *
                      (1.0f - smoothstep(0.35f, 0.42f, continentalFactor));

  // Plains weight: continental 0.4-0.6 (wide zone)
  float plainsWeight = smoothstep(0.35f, 0.42f, continentalFactor) *
                       (1.0f - smoothstep(0.55f, 0.65f, continentalFactor));

  // Hills weight: continental 0.6-0.8
  float hillsWeight = smoothstep(0.55f, 0.65f, continentalFactor) *
                      (1.0f - smoothstep(0.75f, 0.85f, continentalFactor));

  // Mountain weight: continental > 0.8
  float mountainWeight = smoothstep(0.75f, 0.85f, continentalFactor);

  // Normalize weights
  float totalWeight =
      oceanWeight + beachWeight + plainsWeight + hillsWeight + mountainWeight;
  if (totalWeight > 0.001f) {
    oceanWeight /= totalWeight;
    beachWeight /= totalWeight;
    plainsWeight /= totalWeight;
    hillsWeight /= totalWeight;
    mountainWeight /= totalWeight;
  }

  // ============================================
  // Calculate height contribution from each terrain type
  // Heights are ABOVE sea level (64)
  // ============================================

  // Ocean: -29 to -6 above sea level (height 35-58)
  float oceanHeight = -20.0f + peaksValleys * 8.0f;

  // Beach/Coastal: 0 to 6 above sea level (height 64-70)
  float beachHeight = 3.0f + peaksValleys * 3.0f * erosionFactor;

  // Plains: 6 to 56 above sea level (height 70-120)
  // GRADUAL variation with smooth rolling terrain
  float plainsBase = 25.0f; // Center around height 89
  float plainsVariation =
      peaksValleys * 25.0f * (0.5f + erosionFactor * 0.5f); // ±25 gradual
  float plainsHeight = plainsBase + plainsVariation;

  // Hills: 26 to 86 above sea level (height 90-150)
  // Gentle elevated terrain, smooth rounded hills
  float hillsBase = 55.0f; // Center around height 119
  float hillsVariation = peaksValleys * 30.0f * (0.6f + erosionFactor * 0.4f);
  float hillsHeight = hillsBase + hillsVariation;

  // Mountains: 56 to 176 above sea level (height 120-240)
  // Dramatic peaks with steep slopes forming mountain ranges
  float mountainBase = 80.0f; // Start at height ~144

  // Ridge creates dramatic peaks - only strong ridge values form peaks
  float peakContribution = 0.0f;
  if (ridge > 0.2f) {
    // Quadratic growth for dramatic peaks
    float peakFactor = (ridge - 0.2f) / 0.8f; // 0 to 1 for ridge 0.2 to 1.0
    peakFactor = peakFactor * peakFactor;     // Squared for steep peaks
    peakContribution = peakFactor * 95.0f; // Up to 95 extra height (total ~240)
  }

  // General mountain terrain with valleys between peaks
  float mountainVariation = peaksValleys * 35.0f;

  // Combine mountain elements
  float mountainHeight = mountainBase + mountainVariation + peakContribution;

  // ============================================
  // Blend all terrain types together
  // ============================================

  float heightVariation =
      oceanWeight * oceanHeight + beachWeight * beachHeight +
      plainsWeight * plainsHeight + hillsWeight * hillsHeight +
      mountainWeight * mountainHeight;

  // Apply final height calculation
  float finalHeight = baseHeight + heightVariation;

  // Clamp to valid range (1 to 250)
  return std::clamp(static_cast<int>(finalHeight), 1, CHUNK_HEIGHT - 6);
}

void TerrainGenerator::generateColumn(std::array<Voxel, CHUNK_VOLUME> &voxels,
                                      int localX, int localZ,
                                      int terrainHeight) {
  for (int y = 0; y < CHUNK_HEIGHT; ++y) {
    int index = getVoxelIndex(localX, y, localZ);
    TextureType type = getVoxelTypeAt(y, terrainHeight);
    voxels[index].type = type;
  }
}

TextureType TerrainGenerator::getVoxelTypeAt(int worldY,
                                             int terrainHeight) const {
  // Bedrock layer at the bottom
  if (worldY <= BEDROCK_LEVEL) {
    return TextureType::BEDROCK;
  }

  // Below terrain height
  if (worldY <= terrainHeight) {
    int depthFromSurface = terrainHeight - worldY;

    if (depthFromSurface == 0) {
      // Surface block
      if (terrainHeight <= SEA_LEVEL + 2) {
        return TextureType::SAND; // Beach/shoreline
      }
      return TextureType::GRASS_TOP;
    } else if (depthFromSurface <= 3) {
      // Subsurface (3 blocks of dirt)
      if (terrainHeight <= SEA_LEVEL + 2) {
        return TextureType::SAND;
      }
      return TextureType::DIRT;
    } else {
      // Deep underground
      return TextureType::STONE;
    }
  }

  // Water between terrain and sea level
  if (worldY <= SEA_LEVEL) {
    return TextureType::WATER;
  }

  // Air above terrain
  return TextureType::AIR;
}

void TerrainGenerator::generateChunkBorders(ChunkData &chunkData, int chunkX,
                                            int chunkZ) {
  constexpr int borderSize = 1;

  // Helper lambda to get height at world position
  // Helper lambda to get height at world position
  auto getHeightAt = [this](int worldX, int worldZ) -> int {
    std::vector<float> continental(1), erosion(1), pv(1), ridge(1);

    m_continentalNoise->GenUniformGrid2D(continental.data(), worldX, worldZ, 1,
                                         1, 1.0f, m_seed);
    m_erosionNoise->GenUniformGrid2D(erosion.data(), worldX, worldZ, 1, 1, 1.0f,
                                     m_seed + 1000);
    m_peaksValleysNoise->GenUniformGrid2D(pv.data(), worldX, worldZ, 1, 1, 1.0f,
                                          m_seed + 2000);
    m_ridgeNoise->GenUniformGrid2D(ridge.data(), worldX, worldZ, 1, 1, 1.0f,
                                   m_seed + 3000);

    return calculateHeight(continental[0], erosion[0], pv[0], ridge[0]);
  };

  // Helper lambda to check if a position is a cave or ravine (Air)
  auto isCaveOrRavine = [this](int worldX, int worldY, int worldZ) -> bool {
    // Bias: Reduce caves near surface
    // Linearly interpolate threshold from 0.6 (deep) to higher values (surface)
    float heightRatio =
        std::clamp(static_cast<float>(worldY - SEA_LEVEL) / 64.0f, 0.0f, 1.0f);

    // Tune: 0.6 + 0.35 * ratio (was 0.3)
    float caveThreshold = 0.6f + heightRatio * 0.35f;
    // Curve: 0.8 + 0.15 * ratio (was 0.8 const)
    float ravineThreshold = 0.8f + heightRatio * 0.15f;

    std::vector<float> cave(1), ravine(1);
    // Note: GenUniformGrid3D takes X start, Y start, Z start
    m_caveNoise->GenUniformGrid3D(cave.data(), worldX, worldY, worldZ, 1, 1, 1,
                                  1.0f, m_seed + 4000);
    if (cave[0] > caveThreshold)
      return true;

    m_ravineNoise->GenUniformGrid3D(ravine.data(), worldX, worldY, worldZ, 1, 1,
                                    1, 1.0f, m_seed + 5000);
    if (ravine[0] > ravineThreshold)
      return true;

    return false;
  };

  // North border (Z = -1)
  for (int x = 0; x < CHUNK_SIZE; ++x) {
    int worldX = chunkX + x;
    int worldZ = chunkZ - borderSize;
    int height = getHeightAt(worldX, worldZ);

    for (int y = 0; y < CHUNK_HEIGHT; ++y) {
      TextureType voxelType = getVoxelTypeAt(y, height);

      // Apply cave carving to border logic
      if (voxelType != TextureType::AIR && voxelType != TextureType::BEDROCK) {
        if (isCaveOrRavine(worldX, y, worldZ)) {
          voxelType = TextureType::AIR;
        }
      }

      if (voxelType != TextureType::AIR) {
        chunkData.borderVoxels[glm::ivec3(x, y, -borderSize)] = voxelType;
      }
    }
  }
  // South border (Z = CHUNK_SIZE)
  for (int x = 0; x < CHUNK_SIZE; ++x) {
    int worldX = chunkX + x;
    int worldZ = chunkZ + CHUNK_SIZE;
    int height = getHeightAt(worldX, worldZ);

    for (int y = 0; y < CHUNK_HEIGHT; ++y) {
      TextureType voxelType = getVoxelTypeAt(y, height);

      if (voxelType != TextureType::AIR && voxelType != TextureType::BEDROCK) {
        if (isCaveOrRavine(worldX, y, worldZ)) {
          voxelType = TextureType::AIR;
        }
      }

      if (voxelType != TextureType::AIR) {
        chunkData.borderVoxels[glm::ivec3(x, y, CHUNK_SIZE)] = voxelType;
      }
    }
  }

  // West border (X = -1)
  for (int z = 0; z < CHUNK_SIZE; ++z) {
    int worldX = chunkX - borderSize;
    int worldZ = chunkZ + z;
    int height = getHeightAt(worldX, worldZ);

    for (int y = 0; y < CHUNK_HEIGHT; ++y) {
      TextureType voxelType = getVoxelTypeAt(y, height);

      if (voxelType != TextureType::AIR && voxelType != TextureType::BEDROCK) {
        if (isCaveOrRavine(worldX, y, worldZ)) {
          voxelType = TextureType::AIR;
        }
      }

      if (voxelType != TextureType::AIR) {
        chunkData.borderVoxels[glm::ivec3(-borderSize, y, z)] = voxelType;
      }
    }
  }

  // East border (X = CHUNK_SIZE)
  for (int z = 0; z < CHUNK_SIZE; ++z) {
    int worldX = chunkX + CHUNK_SIZE;
    int worldZ = chunkZ + z;
    int height = getHeightAt(worldX, worldZ);

    for (int y = 0; y < CHUNK_HEIGHT; ++y) {
      TextureType voxelType = getVoxelTypeAt(y, height);

      if (voxelType != TextureType::AIR && voxelType != TextureType::BEDROCK) {
        if (isCaveOrRavine(worldX, y, worldZ)) {
          voxelType = TextureType::AIR;
        }
      }

      if (voxelType != TextureType::AIR) {
        chunkData.borderVoxels[glm::ivec3(CHUNK_SIZE, y, z)] = voxelType;
      }
    }
  }
}

float TerrainGenerator::smoothstep(float edge0, float edge1, float x) const {
  float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
