#pragma once

#include <FastNoise/FastNoise.h>
#include <array>
#include <glm/glm.hpp>
#include <unordered_map>

#include <utils.hpp>

struct ChunkData {
  std::array<Voxel, CHUNK_VOLUME> voxels;

  // Border voxels from neighboring chunks for mesh optimization
  std::unordered_map<glm::ivec3, TextureType, IVec3Hash> borderVoxels;
};

class TerrainGenerator {
public:
  // Terrain generation constants
  static constexpr int SEA_LEVEL = 64;
  static constexpr int BEDROCK_LEVEL = 5;

  explicit TerrainGenerator(int seed = 1337);
  ChunkData generateChunk(int chunkX, int chunkZ);

  // Getter for seed to enable thread-safe generation
  int getSeed() const { return m_seed; }

private:
  // Noise generators for terrain height
  FastNoise::SmartNode<FastNoise::Generator> m_continentalNoise;
  FastNoise::SmartNode<FastNoise::Generator> m_erosionNoise;
  FastNoise::SmartNode<FastNoise::Generator> m_peaksValleysNoise;
  FastNoise::SmartNode<FastNoise::Generator>
      m_ridgeNoise; // For dramatic mountain peaks
  FastNoise::SmartNode<FastNoise::Generator> m_caveNoise; // For 3D cheese caves
  FastNoise::SmartNode<FastNoise::Generator>
      m_ravineNoise; // For vertical faults/ravines

  // Ore generation
  struct OreDef {
    TextureType type;
    int minHeight;
    int maxHeight;
    float scale;     // Noise frequency
    float threshold; // > threshold = ore
    int seedOffset;
  };
  std::vector<OreDef> m_ores;
  FastNoise::SmartNode<FastNoise::Generator>
      m_oreNoise; // Shared noise generator re-configured per ore? Or just one
                  // high-freq base?
  // Better: One generator, we scale input coordinates or use domain scaling?
  // Actually, keeping one generator and just changing the seed/offset
  // effectively gives unique patterns. We'll use one OpenSimplex2 generator for
  // all ores.

  // Generation parameters
  int m_seed;

  // Setup noise generators
  void setupNoiseGenerators();
  void setupOres();

  // Core terrain generation using batch noise
  void generateChunkBatch(ChunkData &chunkData, int chunkX, int chunkZ);

  // Generate border voxels for mesh optimization
  void generateChunkBorders(ChunkData &chunkData, int chunkX, int chunkZ);

  // Calculate height from noise values
  int calculateHeight(float continental, float erosion, float peaksValleys,
                      float ridge) const;

  // Generate a single column of voxels
  void generateColumn(std::array<Voxel, CHUNK_VOLUME> &voxels, int localX,
                      int localZ, int terrainHeight);

  // Get voxel type at a specific position (for border generation)
  TextureType getVoxelTypeAt(int worldY, int terrainHeight) const;

  // Utility functions
  float smoothstep(float edge0, float edge1, float x) const;

  // Inline utility function for voxel indexing
  inline int getVoxelIndex(int x, int y, int z) const {
    return y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x;
  }
};
