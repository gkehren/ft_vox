#pragma once

#include <FastNoise/FastNoise.h>
#include <array>
#include <glm/glm.hpp>
#include <unordered_map>
#include <random>
#include <memory>

#include <utils.hpp>

struct ChunkData
{
  std::vector<Voxel> voxels;

  // Border voxels from neighboring chunks (1-thick shell: 18x(H+2)x18)
  std::vector<uint8_t> borderVoxels;

  // Biome data for the chunk (per column)
  std::array<BiomeType, CHUNK_SIZE * CHUNK_SIZE> biomes;

  // Height map for quick access
  std::array<int, CHUNK_SIZE * CHUNK_SIZE> heightMap;
};

// Biome properties for terrain generation
struct BiomeConfig
{
  TextureType surfaceBlock;
  TextureType subsurfaceBlock;
  TextureType underwaterBlock;
  int subsurfaceDepth;
  float treeDensity;       // 0.0 to 1.0
  float vegetationDensity; // 0.0 to 1.0
  glm::vec3 grassColor;
  glm::vec3 foliageColor;
  bool hasSnow;
  bool hasCacti;
  bool hasRivers;
};

class TerrainGenerator
{
public:
  // Terrain generation constants
  static constexpr int SEA_LEVEL = 64;
  static constexpr int BEDROCK_LEVEL = 5;
  static constexpr int BEACH_START = SEA_LEVEL - 3;
  static constexpr int BEACH_END = SEA_LEVEL + 3;
  static constexpr float NOISE_OFFSET = 1000000.0f;

  explicit TerrainGenerator(int seed = 1337);
  ChunkData generateChunk(int chunkX, int chunkZ);

  // Getter for thread-local generator to avoid redundant node graph setup
  static TerrainGenerator &getThreadLocal(int seed);

  // Getter for seed to enable thread-safe generation
  int getSeed() const { return m_seed; }

  // Get biome at world position (for cross-chunk queries)
  BiomeType getBiomeAt(int worldX, int worldZ) const;

  // Get biome configuration
  static const BiomeConfig &getBiomeConfig(BiomeType biome);

private:
  // =============================================
  // NOISE GENERATORS
  // =============================================

  // Terrain shape noise
  FastNoise::SmartNode<FastNoise::Generator> m_continentalNoise;
  FastNoise::SmartNode<FastNoise::Generator> m_erosionNoise;
  FastNoise::SmartNode<FastNoise::Generator> m_peaksValleysNoise;
  FastNoise::SmartNode<FastNoise::Generator> m_ridgeNoise;

  // Biome noise
  FastNoise::SmartNode<FastNoise::Generator> m_temperatureNoise;
  FastNoise::SmartNode<FastNoise::Generator> m_humidityNoise;
  FastNoise::SmartNode<FastNoise::Generator> m_weirdnessNoise; // For rare biomes

  // Cave and structure noise
  FastNoise::SmartNode<FastNoise::Generator> m_caveNoise;
  FastNoise::SmartNode<FastNoise::Generator> m_ravineNoise;

  // Vegetation noise
  FastNoise::SmartNode<FastNoise::Generator> m_treeNoise;
  FastNoise::SmartNode<FastNoise::Generator> m_vegetationNoise;

  // Ore generation
  struct OreDef
  {
    TextureType type;
    int minHeight;
    int maxHeight;
    float scale;
    float threshold;
    int seedOffset;
  };
  std::vector<OreDef> m_ores;
  FastNoise::SmartNode<FastNoise::Generator> m_oreNoise;

  // Generation parameters
  int m_seed;

  // Biome configurations (static)
  static std::array<BiomeConfig, BIOME_COUNT> s_biomeConfigs;
  static bool s_biomeConfigsInitialized;
  static void initBiomeConfigs();
  // =============================================
  // SETUP METHODS
  // =============================================
  void setupNoiseGenerators();
  void setupTerrainNoise();
  void setupBiomeNoise();
  void setupCaveNoise();
  void setupVegetationNoise();
  void setupOres();

  // =============================================
  // GENERATION METHODS
  // =============================================

  // Core terrain generation
  void generateChunkBatch(ChunkData &chunkData, int chunkX, int chunkZ);
  void generateChunkBorders(ChunkData &chunkData, int chunkX, int chunkZ);

  // Height calculation
  int calculateHeight(float continental, float erosion, float peaksValleys,
                      float ridge) const;

  // Biome determination
  BiomeType determineBiome(float temperature, float humidity, float weirdness,
                           float continental, int height) const;

  // Vegetation generation
  void generateVegetation(ChunkData &chunkData, int chunkX, int chunkZ);
  void placeTree(ChunkData &chunkData, int localX, int localZ, int baseY, BiomeType biome);
  void placeOakTree(ChunkData &chunkData, int localX, int localZ, int baseY);
  void placeBirchTree(ChunkData &chunkData, int localX, int localZ, int baseY);
  void placeSprucetree(ChunkData &chunkData, int localX, int localZ, int baseY);
  void placeJungleTree(ChunkData &chunkData, int localX, int localZ, int baseY);
  void placeCactus(ChunkData &chunkData, int localX, int localZ, int baseY);
  void placeDeadBush(ChunkData &chunkData, int localX, int localZ, int baseY);

  // Voxel type determination
  TextureType getVoxelTypeAt(int worldY, int terrainHeight, BiomeType biome) const;

  // =============================================
  // UTILITY FUNCTIONS
  // =============================================
  float smoothstep(float edge0, float edge1, float x) const;
  float lerp(float a, float b, float t) const;

  inline int getVoxelIndex(int x, int y, int z) const
  {
    return y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x;
  }

  inline int getColumnIndex(int x, int z) const
  {
    return z * CHUNK_SIZE + x;
  }

  // Safe voxel setting (bounds checking)
  inline bool setVoxelSafe(ChunkData &chunkData, int x, int y, int z, TextureType type)
  {
    if (x < 0 || x >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE ||
        y < 0 || y >= CHUNK_HEIGHT)
    {
      return false;
    }
    chunkData.voxels[getVoxelIndex(x, y, z)].type = type;
    return true;
  }
};
