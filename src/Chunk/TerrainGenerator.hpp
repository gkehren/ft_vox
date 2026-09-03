#pragma once

#include <FastNoise/FastNoise.h>
#include <array>
#include <functional>
#include <glm/glm.hpp>
#include <span>
#include <unordered_map>
#include <memory>
#include <cstdint>

#include <utils.hpp>
#include <Chunk/BiomeRegionGrid.hpp>

struct ChunkData
{
  std::vector<Voxel> voxels;

  // Border voxels from neighboring chunks (1-thick shell: 18x(H+2)x18)
  std::vector<uint8_t> borderVoxels;

  // Biome data for the chunk (per column)
  std::array<BiomeType, CHUNK_SIZE * CHUNK_SIZE> biomes;

  // Height map for quick access
  std::array<int, CHUNK_SIZE * CHUNK_SIZE> heightMap;

  // Precomputed packed RGBA biome colors per column (for mesh generation)
  std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> grassColors;
  std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> foliageColors;
};

// 1-thick neighbor shell around a chunk (18 x (CHUNK_HEIGHT + 2) x 18).
constexpr size_t kBorderVoxelCount =
    static_cast<size_t>(18) * (CHUNK_HEIGHT + 2) * 18;

// Caller-owned destination storage for one chunk generation. The spans refer
// to memory owned by the caller - pooled Chunk storage at runtime, an owning
// ChunkData in tests/tools. generateChunkInto() fully resets every field
// before generating, so reused storage cannot leak data between chunks.
// View semantics: a const target still allows writing through its spans
// (only the span members themselves are protected).
struct ChunkGenerationTarget
{
  std::span<Voxel> voxels;                // size() == CHUNK_VOLUME
  std::span<uint8_t> borderVoxels;        // size() == kBorderVoxelCount
  std::span<BiomeType, CHUNK_SIZE * CHUNK_SIZE> biomes;
  std::span<int, CHUNK_SIZE * CHUNK_SIZE> heightMap;
  std::span<uint32_t, CHUNK_SIZE * CHUNK_SIZE> grassColors;
  std::span<uint32_t, CHUNK_SIZE * CHUNK_SIZE> foliageColors;
};

struct TerrainGenerationProfile
{
  double noise2DMs{0.0};
  double aquiferNoiseMs{0.0};
  double erosionMs{0.0};
  double base3DNoiseMs{0.0};
  double spaghettiNoiseMs{0.0};
  double cavernNoiseMs{0.0};
  double voxelFillMs{0.0};
  double oreMs{0.0};
  double decorationMs{0.0};
  double vegetationMs{0.0};
  double borderMs{0.0};
  double totalMs{0.0};
  uint64_t chunks{0};
};

// Biome properties for terrain generation
struct BiomeConfig
{
  TextureType surfaceBlock;
  TextureType subsurfaceBlock;
  TextureType underwaterBlock;
  int subsurfaceDepth;
  float treeDensity;      // 0.0 to 1.0
  float bushDensity;      // 0.0 to 1.0 — leaf bushes on the surface
  float rockDensity;      // 0.0 to 1.0 — small boulder clusters
  float fallenLogDensity; // 0.0 to 1.0 — fallen trunks (forests)
  glm::vec3 grassColor;
  glm::vec3 foliageColor;
  bool hasSnow;
  bool hasCacti;
  // Amplitude of the 3D surface perturbation (overhangs/cliffs). 0 = flat
  // heightmap terrain; mountains use ~8, badlands hoodoos ~5.
  float surfacePerturbAmp;
};

class TerrainGenerator
{
public:
  // Terrain generation constants
  static constexpr int SEA_LEVEL = 64;
  static constexpr int BEDROCK_LEVEL = 5;
  // Max horizontal reach of any vegetation feature from its trunk/center column.
  // Vegetation candidates are evaluated in a ring of this width around each
  // chunk so canopies from neighbor chunks are placed deterministically.
  static constexpr int MAX_TREE_RADIUS = 4;
  // Offset added to all noise coordinates to avoid symmetry artifacts near the origin.
  static constexpr float NOISE_OFFSET = 10000.0f;

  explicit TerrainGenerator(int seed = 1337);
  // Owning convenience wrapper (tests/tools): allocates a fresh ChunkData
  // and generates into it. The runtime streaming hot path must use
  // generateChunkInto() instead so pooled storage is reused directly.
  ChunkData generateChunk(int chunkX, int chunkZ);
  // Generates a chunk directly into caller-owned reusable storage. Fully
  // resets all destination state first (voxels, neighbor shell, biomes,
  // height map, colors), so nothing leaks from a previously generated
  // chunk. Equivalent output to generateChunk().
  void generateChunkInto(int chunkX, int chunkZ,
                         const ChunkGenerationTarget &target);
  ChunkData generateChunkProfiled(int chunkX, int chunkZ,
                                  TerrainGenerationProfile &profile);

  // Getter for thread-local generator to avoid redundant node graph setup
  static TerrainGenerator &getThreadLocal(int seed);

  // Getter for seed to enable thread-safe generation
  int getSeed() const { return m_seed; }

  // Get biome at world position (for cross-chunk queries). Equivalent to
  // the biome stored in the generated chunk containing (worldX, worldZ).
  BiomeType getBiomeAt(int worldX, int worldZ) const;

  // Optional early-exit hook polled between region tiles; return true to
  // cancel. Kept as a lightweight std::function so TerrainGenerator stays
  // decoupled from any specific threading primitive. getBiomeRegion() runs
  // its tiles sequentially on the calling thread, so no extra
  // synchronization is required of the callable.
  using BiomeCancelCheck = std::function<bool()>;

  // Domain point-count bounds. No region buffer may exceed these without an
  // explicit fallback path; kMaxDenseDomainPoints bounds the single-pass
  // dense scratch (~10 MiB across all 10 float fields), kMaxTileDensePoints
  // the per-tile scratch (~68 KB per field).
  static constexpr size_t kMaxDenseDomainPoints = 516 * 516;
  static constexpr size_t kMaxTileDensePoints = 132 * 132;
  static constexpr size_t kBiomeRegionScratchFields = 10;

  // Scratch working set for one getBiomeRegion() call. Owned by the caller
  // so heavy capacity is not retained indefinitely inside TerrainGenerator's
  // shared thread-local chunk buffers. Reuse one scratch per producer job
  // (e.g. the biome-map worker) to avoid re-allocating between refreshes.
  struct BiomeRegionScratch
  {
    std::vector<float> temperature;
    std::vector<float> humidity;
    std::vector<float> weirdness;
    std::vector<float> river;
    std::vector<float> continental;
    std::vector<float> erosion;
    std::vector<float> peaksValleys;
    std::vector<float> ridge;
    std::vector<float> height;
    std::vector<float> erosionTemp;

    // Per-field capacity retained across attempts (points). The contract
    // after getBiomeRegion() returns - on success, cancellation, invalid
    // request, or exception - is exactly:
    //
    //   capacity() <= min(retainedPointsCap, kMaxDenseDomainPoints)
    //
    // for every field. The default keeps only tiled-path capacity so a
    // generic scratch stays small no matter who owns it; a producer with a
    // single dedicated scratch (e.g. the biome-map job) may raise it up to
    // kMaxDenseDomainPoints to reuse dense capacity across refreshes
    // without allocation churn - retention then stays bounded to the
    // absolute maximum of kMaxDenseDomainPoints points per field, i.e.
    // a logical payload of ~10 MiB for that one scratch
    // (516^2 points x 10 fields x 4 bytes).
    size_t retainedPointsCap{kMaxTileDensePoints};

    // Releases per-field capacity above retainedPointsCap (clamped to
    // kMaxDenseDomainPoints). getBiomeRegion() runs this automatically on
    // every exit - after successful AND cancelled/failed attempts - so the
    // contract above always holds. Call it manually only when mutating a
    // scratch outside getBiomeRegion().
    void trimOversizedCapacity();
  };

  // Counters describing how a getBiomeRegion() call was executed.
  struct BiomeRegionStats
  {
    uint64_t denseTiles{0};      // tiles processed by the vectorized dense path
    uint64_t fallbackPixels{0};  // pixels evaluated by point-query fallback
    size_t peakDensePoints{0};   // largest dense domain sampled in one shot
    size_t peakScratchBytes{0};  // peakDensePoints * fields * sizeof(float)
  };

  // Batch biome sampling for map visualization. `grid` is the single source
  // of truth for the pixel <-> world <-> voxel-column mapping (see
  // BiomeRegionGrid.hpp). Output is row-major, outBiomes[grid.width * z + x].
  // The canonical block-resolution pipeline (incl. erosion at step = 1.0f) is
  // independent of grid.step: the grid only selects which canonical columns
  // are sampled. Runs sequentially on the calling thread; tiles are sampled
  // in a fixed order so the result is deterministic and the scheduler only
  // decides when the whole call runs, not what it produces. Returns false
  // (and clears outBiomes) when the grid is invalid or `shouldCancel`
  // returned true; partial results are never returned. On every exit —
  // success, cancellation, failure, or exception — the scratch is trimmed
  // to the tiled-path bound, so oversized dense capacity is never retained
  // across attempts. `outStats` is optional. `scratch` may be null, in
  // which case a dedicated internal thread-local scratch is used; pass a
  // caller-owned scratch to control memory retention (see
  // BiomeRegionScratch).
  bool getBiomeRegion(const BiomeRegionGrid &grid,
                      std::vector<BiomeType> &outBiomes,
                      BiomeRegionScratch *scratch = nullptr,
                      const BiomeCancelCheck &shouldCancel = {},
                      BiomeRegionStats *outStats = nullptr) const;

  // Convenience overload building the grid from explicit parameters.
  bool getBiomeRegion(float centerX, float centerZ, float step,
                      int width, int height,
                      std::vector<BiomeType> &outBiomes,
                      const BiomeCancelCheck &shouldCancel = {},
                      BiomeRegionStats *outStats = nullptr) const;

  // Get biome configuration
  static const BiomeConfig &getBiomeConfig(BiomeType biome);

  // Pack a float RGB color into a uint32_t RGBA (alpha = 255)
  static inline uint32_t packColor(const glm::vec3 &c)
  {
    uint32_t r = static_cast<uint32_t>(c.r * 255.0f);
    uint32_t g = static_cast<uint32_t>(c.g * 255.0f);
    uint32_t b = static_cast<uint32_t>(c.b * 255.0f);
    return r | (g << 8) | (b << 16) | (255u << 24);
  }

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
  FastNoise::SmartNode<FastNoise::Generator> m_riverNoise;

  // Cave and structure noise
  FastNoise::SmartNode<FastNoise::Generator> m_caveNoise;
  FastNoise::SmartNode<FastNoise::Generator> m_ravineNoise;
  FastNoise::SmartNode<FastNoise::Generator> m_spaghettiNoise;
  FastNoise::SmartNode<FastNoise::Generator> m_cavernNoise;
  FastNoise::SmartNode<FastNoise::Generator> m_aquiferNoise;
  FastNoise::SmartNode<FastNoise::Generator> m_surface3DNoise;

  // Vegetation noise
  FastNoise::SmartNode<FastNoise::Generator> m_treeNoise;
  FastNoise::SmartNode<FastNoise::Generator> m_forestDensityNoise; // Large-scale forest cluster noise

  // Ore generation
  enum class OreDistribution : uint8_t
  {
    Uniform,
    High,
    Mid,
    Deep
  };

  struct OreDef
  {
    TextureType type;
    TextureType deepType;
    int minHeight;
    int maxHeight;
    int clusterSize;
    int clustersPerChunk;
    int seedOffset;
    OreDistribution distribution;
    bool mountainOnly;
    int badlandsBonusClusters;
  };
  std::vector<OreDef> m_ores;

  // Generation parameters
  int m_seed;
  TerrainGenerationProfile *m_activeProfile{nullptr};

  // Biome configurations (static)
  static std::array<BiomeConfig, BIOME_COUNT> s_biomeConfigs;
  // Lazily fills s_biomeConfigs exactly once; std::call_once is the single
  // synchronization mechanism, so this is safe to call from any thread.
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
  void generateChunkBatch(const ChunkGenerationTarget &chunkData, int chunkX, int chunkZ);
  void generateChunkBorders(const ChunkGenerationTarget &chunkData, int chunkX, int chunkZ);

  // Height calculation (weirdness drives river valley width, matching determineBiome)
  int calculateHeight(float continental, float erosion, float peaksValleys,
                      float ridge, float riverVal, float weirdness) const;
  float calculateHeightFloat(float continental, float erosion, float peaksValleys, float ridge, float riverVal, float weirdness) const;
  void applyErosion(float *heightMap, int size) const;
  void applyCanonicalErosion(float *heightMap, int width, int height, float *tempBuffer = nullptr) const;

  // Shared canonical column pipeline stages
  struct TerrainColumnBuffers
  {
    float *continental{nullptr};
    float *erosion{nullptr};
    float *peaksValleys{nullptr};
    float *ridge{nullptr};
    float *temperature{nullptr};
    float *humidity{nullptr};
    float *weirdness{nullptr};
    float *river{nullptr};
    float *heightMap{nullptr};
    float *erosionTemp{nullptr};
  };

  void sampleTerrainColumnFields(
      const TerrainColumnBuffers &buffers,
      int extStartX, int extStartZ,
      int extWidth, int extHeight,
      float step = 1.0f) const;

  void calculateTerrainHeights(
      const TerrainColumnBuffers &buffers,
      int extWidth, int extHeight) const;

  BiomeType evaluateBiomeAt(
      const TerrainColumnBuffers &buffers,
      int extIndex) const;

  // Canonical column biome evaluation primitive (5x5 halo window at
  // step = 1.0f). Private: external callers must use getBiomeAt().
  BiomeType evaluateBiomeColumn(int worldX, int worldZ) const;

  // Biome determination
  BiomeType determineBiome(float temperature, float humidity, float weirdness,
                           float continental, float erosion, float pv, float riverVal, int height) const;

  // Vegetation generation
  void generateVegetation(const ChunkGenerationTarget &chunkData, int chunkX, int chunkZ);
  void placeTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, BiomeType biome, int worldX, int worldZ);
  void placeOakTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ);
  void placeBirchTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ);
  void placeSpruceTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ);
  void placeJungleTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ);
  void placeAcaciaTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ);
  void placeDarkOakTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ);
  void placeCherryTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ);
  void placeRedwoodTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ);
  void placePalmTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ);
  void placeMangroveTree(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ);
  void placeBamboo(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ);
  void placeGiantMushroom(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ);
  void placeCactus(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ);
  void placeIceSpike(const ChunkGenerationTarget &chunkData, int localX, int localZ, int baseY, int worldX, int worldZ);

  // Per-tree deterministic RNG from world position
  static inline uint32_t treeHash(int worldX, int worldZ, int seed)
  {
    uint32_t h = (static_cast<uint32_t>(worldX) * 374761393u +
                  static_cast<uint32_t>(worldZ) * 668265263u) ^
                 static_cast<uint32_t>(seed);
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    return h;
  }

  // Voxel type determination (slope = max |height delta| to 4 neighbors, for rock exposure)
  TextureType getVoxelTypeAt(int worldX, int worldY, int worldZ, int terrainHeight, BiomeType biome, float temperature, float slope, float density = -1.0f) const;

  // =============================================
  // UTILITY FUNCTIONS
  // =============================================
  float smoothstep(float edge0, float edge1, float x) const;

  inline int getVoxelIndex(int x, int y, int z) const
  {
    return y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x;
  }

  inline int getColumnIndex(int x, int z) const
  {
    return z * CHUNK_SIZE + x;
  }

  // Safe voxel setting (bounds checking)
  inline bool setVoxelSafe(const ChunkGenerationTarget &chunkData, int x, int y, int z, TextureType type)
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
