#include <Chunk/TerrainGenerator.hpp>
#include <algorithm>
#include <cmath>
#include <mutex>

// =============================================
// STATIC MEMBER INITIALIZATION
// =============================================

std::array<BiomeConfig, BIOME_COUNT> TerrainGenerator::s_biomeConfigs;
bool TerrainGenerator::s_biomeConfigsInitialized = false;
static std::once_flag s_biomeConfigsOnceFlag;

void TerrainGenerator::initBiomeConfigs()
{
  std::call_once(s_biomeConfigsOnceFlag, []() {
    // BIOME_FROZEN_OCEAN
    s_biomeConfigs[BIOME_FROZEN_OCEAN] = {
        TextureType::SNOW,           // surface
        TextureType::GRAVEL,         // subsurface
        TextureType::GRAVEL,         // underwater
        3,                           // subsurface depth
        0.0f,                        // tree density
        0.0f,                        // vegetation density
        glm::vec3(0.5f, 0.7f, 0.5f), // grass color
        glm::vec3(0.4f, 0.6f, 0.4f), // foliage color
        true,                        // has snow
        false,                       // has cacti
        false                        // has rivers
    };

    // BIOME_SNOWY_TUNDRA
    s_biomeConfigs[BIOME_SNOWY_TUNDRA] = {
        TextureType::SNOW,
        TextureType::DIRT,
        TextureType::GRAVEL,
        3,
        0.02f, // sparse trees
        0.05f,
        glm::vec3(0.5f, 0.7f, 0.5f),
        glm::vec3(0.4f, 0.6f, 0.4f),
        true,
        false,
        true};

    // BIOME_SNOWY_TAIGA
    s_biomeConfigs[BIOME_SNOWY_TAIGA] = {
        TextureType::SNOW,
        TextureType::DIRT,
        TextureType::GRAVEL,
        3,
        0.15f, // medium tree density
        0.1f,
        glm::vec3(0.5f, 0.7f, 0.5f),
        glm::vec3(0.4f, 0.6f, 0.4f),
        true,
        false,
        true};

    // BIOME_ICE_SPIKES
    s_biomeConfigs[BIOME_ICE_SPIKES] = {
        TextureType::SNOW,
        TextureType::SNOW,
        TextureType::GRAVEL,
        5,
        0.0f,
        0.0f,
        glm::vec3(0.5f, 0.7f, 0.5f),
        glm::vec3(0.4f, 0.6f, 0.4f),
        true,
        false,
        false};

    // BIOME_OCEAN
    s_biomeConfigs[BIOME_OCEAN] = {
        TextureType::GRAVEL,
        TextureType::GRAVEL,
        TextureType::SAND,
        4,
        0.0f,
        0.0f,
        glm::vec3(0.4f, 0.65f, 0.4f),
        glm::vec3(0.3f, 0.55f, 0.3f),
        false,
        false,
        false};

    // BIOME_BEACH
    s_biomeConfigs[BIOME_BEACH] = {
        TextureType::SAND,
        TextureType::SAND,
        TextureType::SAND,
        5,
        0.0f,
        0.0f,
        glm::vec3(0.55f, 0.7f, 0.4f),
        glm::vec3(0.45f, 0.6f, 0.3f),
        false,
        false,
        false};

    // BIOME_PLAINS
    s_biomeConfigs[BIOME_PLAINS] = {
        TextureType::GRASS_TOP,
        TextureType::DIRT,
        TextureType::SAND,
        3,
        0.01f, // very sparse trees
        0.3f,  // flowers/grass
        glm::vec3(0.55f, 0.75f, 0.35f),
        glm::vec3(0.4f, 0.65f, 0.25f),
        false,
        false,
        true};

    // BIOME_FOREST
    s_biomeConfigs[BIOME_FOREST] = {
        TextureType::GRASS_TOP,
        TextureType::DIRT,
        TextureType::GRAVEL,
        3,
        0.25f, // dense trees
        0.4f,
        glm::vec3(0.4f, 0.7f, 0.3f),
        glm::vec3(0.3f, 0.6f, 0.2f),
        false,
        false,
        true};

    // BIOME_BIRCH_FOREST
    s_biomeConfigs[BIOME_BIRCH_FOREST] = {
        TextureType::GRASS_TOP,
        TextureType::DIRT,
        TextureType::GRAVEL,
        3,
        0.22f,
        0.35f,
        glm::vec3(0.5f, 0.75f, 0.4f),
        glm::vec3(0.45f, 0.7f, 0.35f),
        false,
        false,
        true};

    // BIOME_DARK_FOREST
    s_biomeConfigs[BIOME_DARK_FOREST] = {
        TextureType::GRASS_TOP,
        TextureType::DIRT,
        TextureType::DIRT,
        4,
        0.4f, // very dense
        0.5f,
        glm::vec3(0.3f, 0.55f, 0.25f),
        glm::vec3(0.25f, 0.45f, 0.2f),
        false,
        false,
        true};

    // BIOME_SWAMP
    s_biomeConfigs[BIOME_SWAMP] = {
        TextureType::GRASS_TOP,
        TextureType::DIRT,
        TextureType::DIRT,
        4,
        0.12f,
        0.6f,
        glm::vec3(0.35f, 0.5f, 0.3f),
        glm::vec3(0.3f, 0.45f, 0.25f),
        false,
        false,
        true};

    // BIOME_RIVER
    s_biomeConfigs[BIOME_RIVER] = {
        TextureType::SAND,
        TextureType::SAND,
        TextureType::GRAVEL,
        2,
        0.0f,
        0.0f,
        glm::vec3(0.4f, 0.65f, 0.35f),
        glm::vec3(0.35f, 0.55f, 0.3f),
        false,
        false,
        false};

    // BIOME_DESERT
    s_biomeConfigs[BIOME_DESERT] = {
        TextureType::SAND,
        TextureType::SAND,
        TextureType::SAND,
        8,
        0.0f,
        0.02f, // cacti
        glm::vec3(0.7f, 0.7f, 0.4f),
        glm::vec3(0.6f, 0.6f, 0.3f),
        false,
        true,
        false};

    // BIOME_SAVANNA
    s_biomeConfigs[BIOME_SAVANNA] = {
        TextureType::GRASS_TOP,
        TextureType::DIRT,
        TextureType::SAND,
        3,
        0.05f, // acacia-like sparse trees
        0.2f,
        glm::vec3(0.7f, 0.75f, 0.35f),
        glm::vec3(0.6f, 0.65f, 0.25f),
        false,
        false,
        false};

    // BIOME_JUNGLE
    s_biomeConfigs[BIOME_JUNGLE] = {
        TextureType::GRASS_TOP,
        TextureType::DIRT,
        TextureType::DIRT,
        3,
        0.45f, // very dense jungle
        0.7f,
        glm::vec3(0.3f, 0.8f, 0.2f),
        glm::vec3(0.25f, 0.7f, 0.15f),
        false,
        false,
        true};

    // BIOME_BADLANDS
    s_biomeConfigs[BIOME_BADLANDS] = {
        TextureType::SAND, // orange terracotta would be better
        TextureType::SAND,
        TextureType::SAND,
        6,
        0.0f,
        0.01f,
        glm::vec3(0.8f, 0.6f, 0.4f),
        glm::vec3(0.7f, 0.5f, 0.3f),
        false,
        true,
        false};

    // BIOME_MOUNTAINS
    s_biomeConfigs[BIOME_MOUNTAINS] = {
        TextureType::GRASS_TOP,
        TextureType::STONE,
        TextureType::GRAVEL,
        2,
        0.08f,
        0.1f,
        glm::vec3(0.45f, 0.65f, 0.35f),
        glm::vec3(0.4f, 0.55f, 0.3f),
        false,
        false,
        true};

    // BIOME_SNOWY_MOUNTAINS
    s_biomeConfigs[BIOME_SNOWY_MOUNTAINS] = {
        TextureType::SNOW,
        TextureType::STONE,
        TextureType::GRAVEL,
        2,
        0.02f,
        0.02f,
        glm::vec3(0.5f, 0.7f, 0.5f),
        glm::vec3(0.4f, 0.6f, 0.4f),
        true,
        false,
        false};

    s_biomeConfigsInitialized = true;
  });
}

const BiomeConfig &TerrainGenerator::getBiomeConfig(BiomeType biome)
{
  if (!s_biomeConfigsInitialized)
  {
    initBiomeConfigs();
  }
  return s_biomeConfigs[biome];
}

// =============================================
// CONSTRUCTOR
// =============================================

TerrainGenerator::TerrainGenerator(int seed) : m_seed(seed)
{
  initBiomeConfigs();
  setupNoiseGenerators();
}

// =============================================
// NOISE SETUP
// =============================================

void TerrainGenerator::setupNoiseGenerators()
{
  setupTerrainNoise();
  setupBiomeNoise();
  setupCaveNoise();
  setupVegetationNoise();
  setupOres();
}

void TerrainGenerator::setupTerrainNoise()
{
  // Continental noise - Large scale continent shapes
  auto continentalBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto continentalFractal = FastNoise::New<FastNoise::FractalFBm>();
  continentalFractal->SetSource(continentalBase);
  continentalFractal->SetOctaveCount(5);
  continentalFractal->SetLacunarity(2.0f);
  continentalFractal->SetGain(0.45f);
  continentalFractal->SetWeightedStrength(0.0f);

  // Set frequency directly on the fractal node
  auto continentalScale = FastNoise::New<FastNoise::DomainScale>();
  continentalScale->SetSource(continentalFractal);
  continentalScale->SetScale(0.002f); 
  m_continentalNoise = continentalScale;

  // Erosion noise - Controls terrain smoothness
  auto erosionBase = FastNoise::New<FastNoise::Perlin>();
  auto erosionFractal = FastNoise::New<FastNoise::FractalFBm>();
  erosionFractal->SetSource(erosionBase);
  erosionFractal->SetOctaveCount(4);
  erosionFractal->SetLacunarity(2.0f);
  erosionFractal->SetGain(0.5f);

  auto erosionScale = FastNoise::New<FastNoise::DomainScale>();
  erosionScale->SetSource(erosionFractal);
  erosionScale->SetScale(0.004f); 
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
  pvScale->SetScale(0.01f); 
  m_peaksValleysNoise = pvScale;

  // Ridge noise - For sharp mountain peaks
  auto ridgeBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto ridgeFractal = FastNoise::New<FastNoise::FractalRidged>();
  ridgeFractal->SetSource(ridgeBase);
  ridgeFractal->SetOctaveCount(3);
  ridgeFractal->SetLacunarity(2.0f);
  ridgeFractal->SetGain(0.5f);

  auto ridgeScale = FastNoise::New<FastNoise::DomainScale>();
  ridgeScale->SetSource(ridgeFractal);
  ridgeScale->SetScale(0.004f); 
  m_ridgeNoise = ridgeScale;
}

void TerrainGenerator::setupBiomeNoise()
{
  // Temperature noise - Varies from cold (north) to hot (south) with local variation
  auto tempBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto tempFractal = FastNoise::New<FastNoise::FractalFBm>();
  tempFractal->SetSource(tempBase);
  tempFractal->SetOctaveCount(4);
  tempFractal->SetLacunarity(2.0f);
  tempFractal->SetGain(0.5f);

  auto tempScale = FastNoise::New<FastNoise::DomainScale>();
  tempScale->SetSource(tempFractal);
  tempScale->SetScale(0.0015f);
  m_temperatureNoise = tempScale;

  // Humidity noise - Controls wet/dry biomes
  auto humidBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto humidFractal = FastNoise::New<FastNoise::FractalFBm>();
  humidFractal->SetSource(humidBase);
  humidFractal->SetOctaveCount(4);
  humidFractal->SetLacunarity(2.0f);
  humidFractal->SetGain(0.5f);

  auto humidScale = FastNoise::New<FastNoise::DomainScale>();
  humidScale->SetSource(humidFractal);
  humidScale->SetScale(0.002f);
  m_humidityNoise = humidScale;

  // Weirdness noise - For rare/unusual biomes
  auto weirdBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto weirdFractal = FastNoise::New<FastNoise::FractalFBm>();
  weirdFractal->SetSource(weirdBase);
  weirdFractal->SetOctaveCount(3);
  weirdFractal->SetLacunarity(2.0f);
  weirdFractal->SetGain(0.5f);

  auto weirdScale = FastNoise::New<FastNoise::DomainScale>();
  weirdScale->SetSource(weirdFractal);
  weirdScale->SetScale(0.003f);
  m_weirdnessNoise = weirdScale;
}

void TerrainGenerator::setupCaveNoise()
{
  // Cave noise - 3D Simplex "cheese" caves
  auto caveBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto caveFractal = FastNoise::New<FastNoise::FractalFBm>();
  caveFractal->SetSource(caveBase);
  caveFractal->SetOctaveCount(2);
  caveFractal->SetLacunarity(2.0f);
  caveFractal->SetGain(0.5f);

  auto caveScale = FastNoise::New<FastNoise::DomainScale>();
  caveScale->SetSource(caveFractal);
  caveScale->SetScale(0.02f);
  m_caveNoise = caveScale;

  // Ravine noise - Vertical faults
  auto ravineBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto ravineFractal = FastNoise::New<FastNoise::FractalRidged>();
  ravineFractal->SetSource(ravineBase);
  ravineFractal->SetOctaveCount(2);
  ravineFractal->SetLacunarity(2.0f);
  ravineFractal->SetGain(0.5f);

  auto ravineScale = FastNoise::New<FastNoise::DomainScale>();
  ravineScale->SetSource(ravineFractal);
  ravineScale->SetScale(0.005f);
  m_ravineNoise = ravineScale;
}

void TerrainGenerator::setupVegetationNoise()
{
  // Tree placement noise
  auto treeBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto treeFractal = FastNoise::New<FastNoise::FractalFBm>();
  treeFractal->SetSource(treeBase);
  treeFractal->SetOctaveCount(2);
  treeFractal->SetLacunarity(2.0f);
  treeFractal->SetGain(0.5f);

  auto treeScale = FastNoise::New<FastNoise::DomainScale>();
  treeScale->SetSource(treeFractal);
  treeScale->SetScale(0.05f); // High frequency for varied tree placement
  m_treeNoise = treeScale;
}

void TerrainGenerator::setupOres()
{
  // Coal: Common, large veins
  m_ores.push_back({TextureType::COAL_ORE, 0, 128, 0.1f, 0.65f, 10000});
  // Iron: Common, medium veins
  m_ores.push_back({TextureType::IRON_ORE, 0, 64, 0.12f, 0.75f, 11000});
  // Copper: Medium rarity
  m_ores.push_back({TextureType::COPPER_ORE, 0, 96, 0.11f, 0.75f, 12000});
  // Gold: Rare, deep
  m_ores.push_back({TextureType::GOLD_ORE, 0, 32, 0.15f, 0.85f, 13000});
  // Lapis: Rare, deep
  m_ores.push_back({TextureType::LAPIS_ORE, 0, 32, 0.15f, 0.85f, 14000});
  // Redstone: Common deep
  m_ores.push_back({TextureType::REDSTONE_ORE, 0, 16, 0.15f, 0.8f, 15000});
  // Diamond: Very rare
  m_ores.push_back({TextureType::DIAMOND_ORE, 1, 16, 0.18f, 0.9f, 16000});
  // Emerald: Very rare
  m_ores.push_back({TextureType::EMERALD_ORE, 4, 32, 0.18f, 0.93f, 17000});

  // Ore noise generator
  m_oreNoise = FastNoise::New<FastNoise::OpenSimplex2>();
}

// =============================================
// THREAD-LOCAL STORAGE FOR OPTIMIZATION
// =============================================

struct GenBuffers
{
  std::array<float, CHUNK_SIZE * CHUNK_SIZE> continental;
  std::array<float, CHUNK_SIZE * CHUNK_SIZE> erosion;
  std::array<float, CHUNK_SIZE * CHUNK_SIZE> peaksValleys;
  std::array<float, CHUNK_SIZE * CHUNK_SIZE> ridge;
  std::array<float, CHUNK_SIZE * CHUNK_SIZE> temperature;
  std::array<float, CHUNK_SIZE * CHUNK_SIZE> humidity;
  std::array<float, CHUNK_SIZE * CHUNK_SIZE> weirdness;
  std::array<float, CHUNK_VOLUME> cave;
  std::array<float, CHUNK_VOLUME> ravine;

  // Reusable buffer for ore generation (avoids per-ore heap allocation)
  std::array<float, CHUNK_VOLUME> oreNoise;

  // Border generation buffers (per-strip: CHUNK_SIZE columns)
  // 2D noise buffers for height/biome determination (7 noise types x CHUNK_SIZE)
  std::array<float, CHUNK_SIZE> borderCont;
  std::array<float, CHUNK_SIZE> borderErosion;
  std::array<float, CHUNK_SIZE> borderPV;
  std::array<float, CHUNK_SIZE> borderRidge;
  std::array<float, CHUNK_SIZE> borderTemp;
  std::array<float, CHUNK_SIZE> borderHumid;
  std::array<float, CHUNK_SIZE> borderWeird;
  // 3D noise buffers for cave/ravine per strip (CHUNK_SIZE x CHUNK_HEIGHT x 1)
  std::array<float, CHUNK_SIZE * CHUNK_HEIGHT> borderCave;
  std::array<float, CHUNK_SIZE * CHUNK_HEIGHT> borderRavine;

  // Persistent generator to avoid expensive setup every chunk
  std::unique_ptr<TerrainGenerator> generator;
};

static thread_local GenBuffers s_genBuffers;

TerrainGenerator& TerrainGenerator::getThreadLocal(int seed)
{
  if (!s_genBuffers.generator || s_genBuffers.generator->getSeed() != seed) {
    s_genBuffers.generator = std::make_unique<TerrainGenerator>(seed);
  }
  return *s_genBuffers.generator;
}

// =============================================
// CHUNK GENERATION
// =============================================

ChunkData TerrainGenerator::generateChunk(int chunkX, int chunkZ)
{
  ChunkData chunkData;
  chunkData.voxels.assign(CHUNK_VOLUME, {TextureType::AIR});
  chunkData.borderVoxels.assign(18 * (CHUNK_HEIGHT + 2) * 18,
                                static_cast<uint8_t>(AIR));

  // Generate the main chunk data
  generateChunkBatch(chunkData, chunkX, chunkZ);

  // Generate vegetation (trees, cacti, etc.)
  generateVegetation(chunkData, chunkX, chunkZ);

  // Generate border voxels for mesh optimization
  generateChunkBorders(chunkData, chunkX, chunkZ);

  return chunkData;
}

void TerrainGenerator::generateChunkBatch(ChunkData &chunkData, int chunkX,
                                          int chunkZ)
{
  constexpr int totalPoints = CHUNK_SIZE * CHUNK_SIZE;
  constexpr int totalVoxels = CHUNK_VOLUME;

  // Use thread-local scratch buffers to avoid heap allocations
  float *continentalResults = s_genBuffers.continental.data();
  float *erosionResults = s_genBuffers.erosion.data();
  float *peaksValleysResults = s_genBuffers.peaksValleys.data();
  float *ridgeResults = s_genBuffers.ridge.data();

  // Biome noise buffers
  float *temperatureResults = s_genBuffers.temperature.data();
  float *humidityResults = s_genBuffers.humidity.data();
  float *weirdnessResults = s_genBuffers.weirdness.data();

  // 3D Noise buffers
  float *caveResults = s_genBuffers.cave.data();
  float *ravineResults = s_genBuffers.ravine.data();

  float worldXf = static_cast<float>(chunkX) + NOISE_OFFSET;
  float worldZf = static_cast<float>(chunkZ) + NOISE_OFFSET;

  // Generate terrain noise
  m_continentalNoise->GenUniformGrid2D(continentalResults, worldXf,
                                       worldZf, CHUNK_SIZE, CHUNK_SIZE, 1.0f,
                                       m_seed);

  m_erosionNoise->GenUniformGrid2D(erosionResults, worldXf, worldZf,
                                   CHUNK_SIZE, CHUNK_SIZE, 1.0f, m_seed + 1000);

  m_peaksValleysNoise->GenUniformGrid2D(peaksValleysResults, worldXf,
                                        worldZf, CHUNK_SIZE, CHUNK_SIZE, 1.0f,
                                        m_seed + 2000);

  m_ridgeNoise->GenUniformGrid2D(ridgeResults, worldXf, worldZf,
                                 CHUNK_SIZE, CHUNK_SIZE, 1.0f, m_seed + 3000);

  // Generate biome noise
  m_temperatureNoise->GenUniformGrid2D(temperatureResults, worldXf, worldZf,
                                       CHUNK_SIZE, CHUNK_SIZE, 1.0f, m_seed + 6000);

  m_humidityNoise->GenUniformGrid2D(humidityResults, worldXf, worldZf,
                                    CHUNK_SIZE, CHUNK_SIZE, 1.0f, m_seed + 7000);

  m_weirdnessNoise->GenUniformGrid2D(weirdnessResults, worldXf, worldZf,
                                     CHUNK_SIZE, CHUNK_SIZE, 1.0f, m_seed + 8000);

  // Generate 3D noise for caves and ravines
  m_caveNoise->GenUniformGrid3D(caveResults, worldXf, 0.0f, worldZf,
                                CHUNK_SIZE, CHUNK_HEIGHT, CHUNK_SIZE, 1.0f,
                                m_seed + 4000);

  m_ravineNoise->GenUniformGrid3D(ravineResults, worldXf, 0.0f, worldZf,
                                  CHUNK_SIZE, CHUNK_HEIGHT, CHUNK_SIZE, 1.0f,
                                  m_seed + 5000);

  // Pass 1: Calculate biomes and heights
  for (int i = 0; i < totalPoints; ++i)
  {
    float continental = std::clamp(continentalResults[i], -1.0f, 1.0f);
    float temperature = std::clamp(temperatureResults[i], -1.0f, 1.0f);
    float humidity = std::clamp(humidityResults[i], -1.0f, 1.0f);
    float weirdness = std::clamp(weirdnessResults[i], -1.0f, 1.0f);

    int height = calculateHeight(continental, erosionResults[i],
                                 peaksValleysResults[i], ridgeResults[i]);

    BiomeType biome = determineBiome(temperature, humidity, weirdness,
                                     continental, height);

    chunkData.biomes[i] = biome;
    chunkData.heightMap[i] = height;
  }

  // Pass 2: Generate voxel columns
  for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ)
  {
    for (int localX = 0; localX < CHUNK_SIZE; ++localX)
    {
      int colIndex = getColumnIndex(localX, localZ);
      int terrainHeight = chunkData.heightMap[colIndex];
      BiomeType biome = chunkData.biomes[colIndex];

      for (int y = 0; y < CHUNK_HEIGHT; ++y)
      {
        int voxelIndex = getVoxelIndex(localX, y, localZ);
        int noiseIndex = (localZ * CHUNK_HEIGHT * CHUNK_SIZE) + (y * CHUNK_SIZE) + localX;

        float caveVal = caveResults[noiseIndex];
        float ravineVal = ravineResults[noiseIndex];

        TextureType type = getVoxelTypeAt(y, terrainHeight, biome);

        // Carve caves
        if (type != TextureType::BEDROCK && type != TextureType::AIR && type != TextureType::WATER)
        {
          float heightRatio = std::clamp(static_cast<float>(y - SEA_LEVEL) / 64.0f, 0.0f, 1.0f);
          float caveThreshold = 0.6f + heightRatio * 0.35f;
          float ravineThreshold = 0.8f + heightRatio * 0.15f;

          if (caveVal > caveThreshold || ravineVal > ravineThreshold)
          {
            type = TextureType::AIR;
          }
        }

        chunkData.voxels[voxelIndex].type = type;
      }
    }
  }

  // Ore generation pass (reuses thread-local buffer to avoid heap allocations)
  float *oreNoiseBuffer = s_genBuffers.oreNoise.data();
  for (const auto &ore : m_ores)
  {
    int minY = std::max(0, ore.minHeight);
    int maxY = std::min(CHUNK_HEIGHT, ore.maxHeight);
    if (minY >= maxY) continue;

    int heightSize = maxY - minY;

    m_oreNoise->GenUniformGrid3D(oreNoiseBuffer, worldXf, static_cast<float>(minY), worldZf,
                                 CHUNK_SIZE, heightSize, CHUNK_SIZE, ore.scale,
                                 m_seed + ore.seedOffset);

    int noiseIdx = 0;
    for (int z = 0; z < CHUNK_SIZE; ++z)
    {
      for (int y = 0; y < heightSize; ++y)
      {
        for (int x = 0; x < CHUNK_SIZE; ++x)
        {
          float noiseVal = oreNoiseBuffer[noiseIdx++];
          int voxelIndex = getVoxelIndex(x, minY + y, z);

          if (chunkData.voxels[voxelIndex].type == TextureType::STONE)
          {
            if (noiseVal > ore.threshold)
            {
              chunkData.voxels[voxelIndex].type = ore.type;
            }
          }
        }
      }
    }
  }
}

// =============================================
// BIOME DETERMINATION
// =============================================

BiomeType TerrainGenerator::determineBiome(float temperature, float humidity,
                                           float weirdness, float continental,
                                           int height) const
{
  float temp = (temperature + 1.0f) * 0.5f;
  float humid = (humidity + 1.0f) * 0.5f;
  float cont = (continental + 1.0f) * 0.5f;
  float weird = (weirdness + 1.0f) * 0.5f;

  if (height < SEA_LEVEL - 5 && cont < 0.45f)
  {
    if (temp < 0.35f) return BIOME_FROZEN_OCEAN;
    return BIOME_OCEAN;
  }

  if (height >= SEA_LEVEL - 3 && height <= SEA_LEVEL + 3 && cont < 0.5f)
  {
    if (temp < 0.35f) return BIOME_SNOWY_TUNDRA;
    return BIOME_BEACH;
  }

  if (height > 130)
  {
    if (temp < 0.4f || height > 170) return BIOME_SNOWY_MOUNTAINS;
    return BIOME_MOUNTAINS;
  }

  if (temp < 0.35f)
  {
    if (humid < 0.3f) return BIOME_SNOWY_TUNDRA;
    return BIOME_SNOWY_TAIGA;
  }

  if (temp > 0.65f)
  {
    if (humid < 0.25f) return BIOME_DESERT;
    if (humid < 0.45f) return (weird > 0.65f) ? BIOME_BADLANDS : BIOME_SAVANNA;
    return BIOME_JUNGLE;
  }

  if (humid < 0.3f) return BIOME_PLAINS;
  if (humid < 0.5f) return BIOME_FOREST;
  if (humid < 0.65f) return (weird > 0.6f) ? BIOME_BIRCH_FOREST : BIOME_FOREST;
  if (humid < 0.8f) return BIOME_DARK_FOREST;
  return BIOME_SWAMP;
}

BiomeType TerrainGenerator::getBiomeAt(int worldX, int worldZ) const
{
  float x = static_cast<float>(worldX) + NOISE_OFFSET;
  float z = static_cast<float>(worldZ) + NOISE_OFFSET;

  float temp = m_temperatureNoise->GenSingle2D(x, z, m_seed + 6000);
  float humid = m_humidityNoise->GenSingle2D(x, z, m_seed + 7000);
  float weird = m_weirdnessNoise->GenSingle2D(x, z, m_seed + 8000);
  float cont = m_continentalNoise->GenSingle2D(x, z, m_seed);
  float erosion = m_erosionNoise->GenSingle2D(x, z, m_seed + 1000);
  float pv = m_peaksValleysNoise->GenSingle2D(x, z, m_seed + 2000);
  float ridge = m_ridgeNoise->GenSingle2D(x, z, m_seed + 3000);

  int height = calculateHeight(cont, erosion, pv, ridge);
  return determineBiome(temp, humid, weird, cont, height);
}

// =============================================
// HEIGHT CALCULATION
// =============================================

// Continental smoothstep thresholds: define where each terrain band begins/ends
// Each pair (lo, hi) is a transition zone in the 0..1 continental factor range.
static constexpr float CONT_OCEAN_LO    = 0.25f; // Ocean -> Beach transition start
static constexpr float CONT_OCEAN_HI    = 0.32f; // Ocean -> Beach transition end
static constexpr float CONT_BEACH_LO    = 0.35f; // Beach -> Plains transition start
static constexpr float CONT_BEACH_HI    = 0.42f; // Beach -> Plains transition end
static constexpr float CONT_PLAINS_LO   = 0.55f; // Plains -> Hills transition start
static constexpr float CONT_PLAINS_HI   = 0.65f; // Plains -> Hills transition end
static constexpr float CONT_HILLS_LO    = 0.75f; // Hills -> Mountains transition start
static constexpr float CONT_HILLS_HI    = 0.85f; // Hills -> Mountains transition end

// Base heights (blocks above/below SEA_LEVEL) and variation amplitudes per terrain band
static constexpr float OCEAN_BASE_HEIGHT     = -20.0f;
static constexpr float OCEAN_VARIATION       =   8.0f;
static constexpr float BEACH_BASE_HEIGHT     =   3.0f;
static constexpr float BEACH_VARIATION       =   3.0f;
static constexpr float PLAINS_BASE_HEIGHT    =  25.0f;
static constexpr float PLAINS_VARIATION      =  25.0f;
static constexpr float HILLS_BASE_HEIGHT     =  55.0f;
static constexpr float HILLS_VARIATION       =  30.0f;
static constexpr float MOUNTAIN_BASE_HEIGHT  =  80.0f;
static constexpr float MOUNTAIN_VARIATION    =  35.0f;

// Ridge-driven peak parameters
static constexpr float RIDGE_PEAK_THRESHOLD  =  0.2f;  // Ridge value above which peaks form
static constexpr float RIDGE_PEAK_EXPONENT   =  2.0f;  // Controls peak sharpness
static constexpr float RIDGE_PEAK_AMPLITUDE  = 95.0f;  // Max extra height from ridge peaks

// Height clamping
static constexpr int   HEIGHT_CEILING_MARGIN =  32;     // Reserved space above max terrain

int TerrainGenerator::calculateHeight(float continental, float erosion,
                                      float peaksValleys, float ridge) const
{
  continental = std::clamp(continental, -1.0f, 1.0f);
  erosion = std::clamp(erosion, -1.0f, 1.0f);
  peaksValleys = std::clamp(peaksValleys, -1.0f, 1.0f);
  ridge = std::clamp(ridge, -1.0f, 1.0f);

  float continentalFactor = (continental + 1.0f) * 0.5f;
  float erosionFactor = (erosion + 1.0f) * 0.5f;

  // Compute blending weights for each terrain band using smoothstep transitions
  float oceanWeight    = 1.0f - smoothstep(CONT_OCEAN_LO, CONT_OCEAN_HI, continentalFactor);
  float beachWeight    = smoothstep(CONT_OCEAN_LO, CONT_OCEAN_HI, continentalFactor) * (1.0f - smoothstep(CONT_BEACH_LO, CONT_BEACH_HI, continentalFactor));
  float plainsWeight   = smoothstep(CONT_BEACH_LO, CONT_BEACH_HI, continentalFactor) * (1.0f - smoothstep(CONT_PLAINS_LO, CONT_PLAINS_HI, continentalFactor));
  float hillsWeight    = smoothstep(CONT_PLAINS_LO, CONT_PLAINS_HI, continentalFactor) * (1.0f - smoothstep(CONT_HILLS_LO, CONT_HILLS_HI, continentalFactor));
  float mountainWeight = smoothstep(CONT_HILLS_LO, CONT_HILLS_HI, continentalFactor);

  // Normalize weights to ensure they sum to 1.0
  float totalWeight = oceanWeight + beachWeight + plainsWeight + hillsWeight + mountainWeight;
  if (totalWeight > 0.001f) {
    oceanWeight /= totalWeight; beachWeight /= totalWeight; plainsWeight /= totalWeight;
    hillsWeight /= totalWeight; mountainWeight /= totalWeight;
  }

  // Height contribution per terrain band
  float oceanHeight    = OCEAN_BASE_HEIGHT    + peaksValleys * OCEAN_VARIATION;
  float beachHeight    = BEACH_BASE_HEIGHT    + peaksValleys * BEACH_VARIATION * erosionFactor;
  float plainsHeight   = PLAINS_BASE_HEIGHT   + peaksValleys * PLAINS_VARIATION * (0.5f + erosionFactor * 0.5f);
  float hillsHeight    = HILLS_BASE_HEIGHT    + peaksValleys * HILLS_VARIATION * (0.6f + erosionFactor * 0.4f);
  float mountainHeight = MOUNTAIN_BASE_HEIGHT + peaksValleys * MOUNTAIN_VARIATION
      + (ridge > RIDGE_PEAK_THRESHOLD
         ? std::pow((ridge - RIDGE_PEAK_THRESHOLD) / (1.0f - RIDGE_PEAK_THRESHOLD), RIDGE_PEAK_EXPONENT) * RIDGE_PEAK_AMPLITUDE
         : 0.0f);

  float heightVariation = oceanWeight * oceanHeight + beachWeight * beachHeight +
                          plainsWeight * plainsHeight + hillsWeight * hillsHeight +
                          mountainWeight * mountainHeight;

  return std::clamp(static_cast<int>(std::round(static_cast<float>(SEA_LEVEL) + heightVariation)), 1, CHUNK_HEIGHT - HEIGHT_CEILING_MARGIN);
}

// =============================================
// VEGETATION GENERATION
// =============================================

void TerrainGenerator::generateVegetation(ChunkData &chunkData, int chunkX, int chunkZ)
{
  std::vector<float> treeNoiseResults(CHUNK_SIZE * CHUNK_SIZE);
  m_treeNoise->GenUniformGrid2D(treeNoiseResults.data(), static_cast<float>(chunkX) + NOISE_OFFSET, static_cast<float>(chunkZ) + NOISE_OFFSET,
                                CHUNK_SIZE, CHUNK_SIZE, 1.0f, m_seed + 10000);

  for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ)
  {
    for (int localX = 0; localX < CHUNK_SIZE; ++localX)
    {
      int colIndex = getColumnIndex(localX, localZ);
      BiomeType biome = chunkData.biomes[colIndex];
      int terrainHeight = chunkData.heightMap[colIndex];

      if (terrainHeight <= SEA_LEVEL || terrainHeight > 200) continue;

      const BiomeConfig &config = getBiomeConfig(biome);
      float treeProbability = config.treeDensity * (treeNoiseResults[colIndex] + 1.0f) * 0.5f;

      int surfaceIndex = getVoxelIndex(localX, terrainHeight, localZ);
      TextureType surfaceType = static_cast<TextureType>(chunkData.voxels[surfaceIndex].type);

      if (surfaceType != TextureType::GRASS_TOP && surfaceType != TextureType::DIRT &&
          surfaceType != TextureType::SAND && surfaceType != TextureType::SNOW) continue;

      uint32_t hash = ((chunkX + localX) * 374761393 + (chunkZ + localZ) * 668265263) ^ m_seed;
      if (static_cast<float>(hash & 0xFFFF) / 65535.0f < treeProbability * 0.3f)
      {
        placeTree(chunkData, localX, localZ, terrainHeight + 1, biome);
      }
      else if (config.hasCacti && biome == BIOME_DESERT)
      {
        uint32_t cHash = ((chunkX + localX) * 198491317 + (chunkZ + localZ) * 781874213) ^ (m_seed + 1);
        if (static_cast<float>(cHash & 0xFFFF) / 65535.0f < 0.015f && surfaceType == TextureType::SAND)
        {
          placeCactus(chunkData, localX, localZ, terrainHeight + 1);
        }
      }
    }
  }
}

void TerrainGenerator::placeTree(ChunkData &chunkData, int localX, int localZ,
                                 int baseY, BiomeType biome)
{
  switch (biome)
  {
  case BIOME_SNOWY_TAIGA:
  case BIOME_ICE_SPIKES: placeSpruceTree(chunkData, localX, localZ, baseY); break;
  case BIOME_BIRCH_FOREST: placeBirchTree(chunkData, localX, localZ, baseY); break;
  case BIOME_JUNGLE: placeJungleTree(chunkData, localX, localZ, baseY); break;
  default: placeOakTree(chunkData, localX, localZ, baseY); break;
  }
}

void TerrainGenerator::placeOakTree(ChunkData &chunkData, int localX, int localZ, int baseY)
{
  int trunkHeight = 4 + (baseY % 3);
  for (int y = 0; y < trunkHeight; ++y) setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::OAK_LOG);
  int leafStart = baseY + trunkHeight - 2;
  int leafEnd = baseY + trunkHeight + 1;
  for (int ly = leafStart; ly <= leafEnd; ++ly) {
    int radius = (ly == leafEnd) ? 1 : 2;
    for (int lx = -radius; lx <= radius; ++lx) {
      for (int lz = -radius; lz <= radius; ++lz) {
        if (std::abs(lx) == radius && std::abs(lz) == radius && (ly == leafStart || ly == leafEnd) && radius > 1) continue;
        if (lx == 0 && lz == 0 && ly < baseY + trunkHeight) continue;
        setVoxelSafe(chunkData, localX + lx, ly, localZ + lz, TextureType::OAK_LEAVES);
      }
    }
  }
}

// TODO: Replace OAK_LOG/OAK_LEAVES with BIRCH_LOG/BIRCH_LEAVES when texture assets are available
void TerrainGenerator::placeBirchTree(ChunkData &chunkData, int localX, int localZ, int baseY)
{
  int trunkHeight = 5 + (baseY % 3);
  for (int y = 0; y < trunkHeight; ++y) setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::OAK_LOG);
  int leafStart = baseY + trunkHeight - 3;
  int leafEnd = baseY + trunkHeight + 1;
  for (int ly = leafStart; ly <= leafEnd; ++ly) {
    int radius = (ly >= baseY + trunkHeight) ? 1 : 2;
    for (int lx = -radius; lx <= radius; ++lx) {
      for (int lz = -radius; lz <= radius; ++lz) {
        if (std::abs(lx) == radius && std::abs(lz) == radius && ly != leafStart + 1) continue;
        if (lx == 0 && lz == 0 && ly < baseY + trunkHeight) continue;
        setVoxelSafe(chunkData, localX + lx, ly, localZ + lz, TextureType::OAK_LEAVES);
      }
    }
  }
}

// TODO: Replace OAK_LOG/OAK_LEAVES with SPRUCE_LOG/SPRUCE_LEAVES when texture assets are available
void TerrainGenerator::placeSpruceTree(ChunkData &chunkData, int localX, int localZ, int baseY)
{
  int trunkHeight = 6 + (baseY % 4);
  for (int y = 0; y < trunkHeight; ++y) setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::OAK_LOG);
  for (int layer = 0; layer < trunkHeight - 1; ++layer) {
    int ly = baseY + trunkHeight - 1 - layer;
    int radius = layer / 2;
    if (layer % 2 == 0 || layer == trunkHeight - 2) {
      for (int lx = -radius; lx <= radius; ++lx) {
        for (int lz = -radius; lz <= radius; ++lz) {
          if (lx == 0 && lz == 0) continue;
          if (std::abs(lx) + std::abs(lz) <= radius + 1) setVoxelSafe(chunkData, localX + lx, ly, localZ + lz, TextureType::OAK_LEAVES);
        }
      }
    }
  }
  setVoxelSafe(chunkData, localX, baseY + trunkHeight, localZ, TextureType::OAK_LEAVES);
}

// TODO: Replace OAK_LOG/OAK_LEAVES with JUNGLE_LOG/JUNGLE_LEAVES when texture assets are available
void TerrainGenerator::placeJungleTree(ChunkData &chunkData, int localX, int localZ, int baseY)
{
  int trunkHeight = 8 + (baseY % 6);
  for (int y = 0; y < trunkHeight; ++y) setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::OAK_LOG);
  int leafStart = baseY + trunkHeight - 3;
  int leafEnd = baseY + trunkHeight + 2;
  for (int ly = leafStart; ly <= leafEnd; ++ly) {
    int radius = 3 - std::abs(ly - (leafStart + 2)) / 2;
    for (int lx = -radius; lx <= radius; ++lx) {
      for (int lz = -radius; lz <= radius; ++lz) {
        if (lx * lx + lz * lz > radius * radius + 1) continue;
        if (lx == 0 && lz == 0 && ly < baseY + trunkHeight) continue;
        setVoxelSafe(chunkData, localX + lx, ly, localZ + lz, TextureType::OAK_LEAVES);
      }
    }
  }
  for (int dx = -2; dx <= 2; dx += 4) {
    for (int dz = -2; dz <= 2; dz += 4) {
      for (int vy = 0; vy < 3; ++vy) setVoxelSafe(chunkData, localX + dx, leafStart - vy, localZ + dz, TextureType::OAK_LEAVES);
    }
  }
}

// TODO: Replace OAK_LOG with CACTUS block type when texture asset is available
void TerrainGenerator::placeCactus(ChunkData &chunkData, int localX, int localZ, int baseY)
{
  int height = 1 + (baseY % 3);
  for (int y = 0; y < height; ++y) {
    bool canPlace = true;
    for (int dx = -1; dx <= 1 && canPlace; ++dx) {
      for (int dz = -1; dz <= 1 && canPlace; ++dz) {
        if (dx == 0 && dz == 0) continue;
        int nx = localX + dx, nz = localZ + dz;
        if (nx >= 0 && nx < CHUNK_SIZE && nz >= 0 && nz < CHUNK_SIZE) {
          int checkIndex = getVoxelIndex(nx, baseY + y, nz);
          if (chunkData.voxels[checkIndex].type != TextureType::AIR && chunkData.voxels[checkIndex].type != TextureType::SAND) canPlace = false;
        }
      }
    }
    if (canPlace) setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::OAK_LOG);
  }
}

// =============================================
// VOXEL TYPE DETERMINATION
// =============================================

TextureType TerrainGenerator::getVoxelTypeAt(int worldY, int terrainHeight,
                                             BiomeType biome) const
{
  if (worldY <= BEDROCK_LEVEL) return TextureType::BEDROCK;
  const BiomeConfig &config = getBiomeConfig(biome);
  if (worldY <= terrainHeight) {
    int depth = terrainHeight - worldY;
    if (depth == 0) {
      if (terrainHeight <= SEA_LEVEL + 2) return config.underwaterBlock;
      if (config.hasSnow && terrainHeight > 120) return TextureType::SNOW;
      return config.surfaceBlock;
    }
    if (depth <= config.subsurfaceDepth) {
      if (terrainHeight <= SEA_LEVEL + 2) return config.underwaterBlock;
      return config.subsurfaceBlock;
    }
    return TextureType::STONE;
  }
  return (worldY <= SEA_LEVEL) ? TextureType::WATER : TextureType::AIR;
}

// =============================================
// BORDER GENERATION
// =============================================

void TerrainGenerator::generateChunkBorders(ChunkData &chunkData, int chunkX,
                                            int chunkZ)
{
  auto setBorderVoxel = [&](int lx, int ly, int lz, TextureType type) {
    if (lx >= -1 && lx <= CHUNK_SIZE && ly >= -1 && ly <= CHUNK_HEIGHT && lz >= -1 && lz <= CHUNK_SIZE) {
      chunkData.borderVoxels[(ly + 1) * 18 * 18 + (lz + 1) * 18 + (lx + 1)] = static_cast<uint8_t>(type);
    }
  };

  // Border strips: [localX offset, localZ offset, xSize, zSize]
  // South (lz=-1), North (lz=16), West (lx=-1), East (lx=16)
  struct BorderStrip { int lxStart; int lzStart; int xSize; int zSize; };
  const BorderStrip strips[4] = {
    {0,         -1,         CHUNK_SIZE, 1},          // South border
    {0,         CHUNK_SIZE, CHUNK_SIZE, 1},          // North border
    {-1,        0,          1,          CHUNK_SIZE}, // West border
    {CHUNK_SIZE,0,          1,          CHUNK_SIZE}, // East border
  };

  float *bCont    = s_genBuffers.borderCont.data();
  float *bErosion = s_genBuffers.borderErosion.data();
  float *bPV      = s_genBuffers.borderPV.data();
  float *bRidge   = s_genBuffers.borderRidge.data();
  float *bTemp    = s_genBuffers.borderTemp.data();
  float *bHumid   = s_genBuffers.borderHumid.data();
  float *bWeird   = s_genBuffers.borderWeird.data();
  float *bCave    = s_genBuffers.borderCave.data();
  float *bRavine  = s_genBuffers.borderRavine.data();

  for (int s = 0; s < 4; ++s) {
    const auto &strip = strips[s];
    float startX = static_cast<float>(chunkX + strip.lxStart) + NOISE_OFFSET;
    float startZ = static_cast<float>(chunkZ + strip.lzStart) + NOISE_OFFSET;

    // Batch 2D noise for all columns in this strip
    m_continentalNoise->GenUniformGrid2D(bCont, startX, startZ, strip.xSize, strip.zSize, 1.0f, m_seed);
    m_erosionNoise->GenUniformGrid2D(bErosion, startX, startZ, strip.xSize, strip.zSize, 1.0f, m_seed + 1000);
    m_peaksValleysNoise->GenUniformGrid2D(bPV, startX, startZ, strip.xSize, strip.zSize, 1.0f, m_seed + 2000);
    m_ridgeNoise->GenUniformGrid2D(bRidge, startX, startZ, strip.xSize, strip.zSize, 1.0f, m_seed + 3000);
    m_temperatureNoise->GenUniformGrid2D(bTemp, startX, startZ, strip.xSize, strip.zSize, 1.0f, m_seed + 6000);
    m_humidityNoise->GenUniformGrid2D(bHumid, startX, startZ, strip.xSize, strip.zSize, 1.0f, m_seed + 7000);
    m_weirdnessNoise->GenUniformGrid2D(bWeird, startX, startZ, strip.xSize, strip.zSize, 1.0f, m_seed + 8000);

    // Batch 3D noise for cave/ravine across the full strip volume
    m_caveNoise->GenUniformGrid3D(bCave, startX, 0.0f, startZ,
                                  strip.xSize, CHUNK_HEIGHT, strip.zSize, 1.0f, m_seed + 4000);
    m_ravineNoise->GenUniformGrid3D(bRavine, startX, 0.0f, startZ,
                                    strip.xSize, CHUNK_HEIGHT, strip.zSize, 1.0f, m_seed + 5000);

    int numColumns = strip.xSize * strip.zSize; // Always CHUNK_SIZE (16)
    for (int j = 0; j < numColumns; ++j) {
      int lx = strip.lxStart + (strip.xSize > 1 ? j : 0);
      int lz = strip.lzStart + (strip.zSize > 1 ? j : 0);

      int height = calculateHeight(bCont[j], bErosion[j], bPV[j], bRidge[j]);
      BiomeType biome = determineBiome(bTemp[j], bHumid[j], bWeird[j], bCont[j], height);

      for (int y = 0; y < CHUNK_HEIGHT; ++y) {
        TextureType type = getVoxelTypeAt(y, height, biome);

        if (type != TextureType::AIR && type != TextureType::BEDROCK && type != TextureType::WATER) {
          // 3D noise index: for a strip of xSize x CHUNK_HEIGHT x zSize,
          // FastNoise layout is z * (CHUNK_HEIGHT * xSize) + y * xSize + x
          int noiseX = (strip.xSize > 1) ? j : 0;
          int noiseZ = (strip.zSize > 1) ? j : 0;
          int noiseIdx = noiseZ * (CHUNK_HEIGHT * strip.xSize) + y * strip.xSize + noiseX;

          float hRatio = std::clamp(static_cast<float>(y - SEA_LEVEL) / 64.0f, 0.0f, 1.0f);
          if (bCave[noiseIdx] > 0.6f + hRatio * 0.35f || bRavine[noiseIdx] > 0.8f + hRatio * 0.15f) {
            type = TextureType::AIR;
          }
        }

        if (type != TextureType::AIR) setBorderVoxel(lx, y, lz, type);
      }
    }
  }
}

float TerrainGenerator::smoothstep(float edge0, float edge1, float x) const
{
  float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

float TerrainGenerator::lerp(float a, float b, float t) const
{
  return a + t * (b - a);
}