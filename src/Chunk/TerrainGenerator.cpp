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
  setupRiverNoise();
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
    tempScale->SetScale(0.0015f); // Increased from 0.0005f
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
    humidScale->SetScale(0.002f); // Increased from 0.0006f
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
    weirdScale->SetScale(0.003f); // Increased from 0.001f
    m_weirdnessNoise = weirdScale;
  }
void TerrainGenerator::setupRiverNoise()
{
  // River path noise - Uses ridged noise for natural river paths
  auto riverBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto riverFractal = FastNoise::New<FastNoise::FractalRidged>();
  riverFractal->SetSource(riverBase);
  riverFractal->SetOctaveCount(2);
  riverFractal->SetLacunarity(2.0f);
  riverFractal->SetGain(0.5f);

  auto riverScale = FastNoise::New<FastNoise::DomainScale>();
  riverScale->SetSource(riverFractal);
  riverScale->SetScale(0.003f); // Creates winding river patterns
  m_riverNoise = riverScale;

  // River mask - Determines where rivers can form
  auto maskBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto maskFractal = FastNoise::New<FastNoise::FractalFBm>();
  maskFractal->SetSource(maskBase);
  maskFractal->SetOctaveCount(2);
  maskFractal->SetLacunarity(2.0f);
  maskFractal->SetGain(0.5f);

  auto maskScale = FastNoise::New<FastNoise::DomainScale>();
  maskScale->SetSource(maskFractal);
  maskScale->SetScale(0.001f);
  m_riverMaskNoise = maskScale;
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

  // General vegetation noise
  auto vegBase = FastNoise::New<FastNoise::OpenSimplex2>();
  m_vegetationNoise = vegBase;
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
  std::array<float, CHUNK_SIZE * CHUNK_SIZE> river;
  std::array<float, CHUNK_SIZE * CHUNK_SIZE> riverMask;
  std::array<float, CHUNK_VOLUME> cave;
  std::array<float, CHUNK_VOLUME> ravine;
};

static thread_local GenBuffers s_genBuffers;

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

  // River noise buffers
  float *riverResults = s_genBuffers.river.data();
  float *riverMaskResults = s_genBuffers.riverMask.data();

  // 3D Noise buffers
  float *caveResults = s_genBuffers.cave.data();
  float *ravineResults = s_genBuffers.ravine.data();

  // Generate terrain noise
  m_continentalNoise->GenUniformGrid2D(continentalResults, chunkX,
                                       chunkZ, CHUNK_SIZE, CHUNK_SIZE, 1.0f,
                                       m_seed);

  m_erosionNoise->GenUniformGrid2D(erosionResults, chunkX, chunkZ,
                                   CHUNK_SIZE, CHUNK_SIZE, 1.0f, m_seed + 1000);

  m_peaksValleysNoise->GenUniformGrid2D(peaksValleysResults, chunkX,
                                        chunkZ, CHUNK_SIZE, CHUNK_SIZE, 1.0f,
                                        m_seed + 2000);

  m_ridgeNoise->GenUniformGrid2D(ridgeResults, chunkX, chunkZ,
                                 CHUNK_SIZE, CHUNK_SIZE, 1.0f, m_seed + 3000);

  // Generate biome noise
  m_temperatureNoise->GenUniformGrid2D(temperatureResults, chunkX, chunkZ,
                                       CHUNK_SIZE, CHUNK_SIZE, 1.0f, m_seed + 6000);

  m_humidityNoise->GenUniformGrid2D(humidityResults, chunkX, chunkZ,
                                    CHUNK_SIZE, CHUNK_SIZE, 1.0f, m_seed + 7000);

  m_weirdnessNoise->GenUniformGrid2D(weirdnessResults, chunkX, chunkZ,
                                     CHUNK_SIZE, CHUNK_SIZE, 1.0f, m_seed + 8000);

  // Generate river noise
  m_riverNoise->GenUniformGrid2D(riverResults, chunkX, chunkZ,
                                 CHUNK_SIZE, CHUNK_SIZE, 1.0f, m_seed + 9000);

  m_riverMaskNoise->GenUniformGrid2D(riverMaskResults, chunkX, chunkZ,
                                     CHUNK_SIZE, CHUNK_SIZE, 1.0f, m_seed + 9500);

  // Generate 3D noise for caves and ravines
  m_caveNoise->GenUniformGrid3D(caveResults, chunkX, 0, chunkZ,
                                CHUNK_SIZE, CHUNK_HEIGHT, CHUNK_SIZE, 1.0f,
                                m_seed + 4000);

  m_ravineNoise->GenUniformGrid3D(ravineResults, chunkX, 0, chunkZ,
                                  CHUNK_SIZE, CHUNK_HEIGHT, CHUNK_SIZE, 1.0f,
                                  m_seed + 5000);

  // First pass: Calculate biomes and heights
  for (int i = 0; i < totalPoints; ++i)
  {
    // Normalize noise values
    float continental = std::clamp(continentalResults[i], -1.0f, 1.0f);
    float temperature = std::clamp(temperatureResults[i], -1.0f, 1.0f);
    float humidity = std::clamp(humidityResults[i], -1.0f, 1.0f);
    float weirdness = std::clamp(weirdnessResults[i], -1.0f, 1.0f);

    // Calculate preliminary height for biome determination
    int prelimHeight = calculateHeight(continental, erosionResults[i],
                                       peaksValleysResults[i], ridgeResults[i],
                                       BIOME_PLAINS);

    // Determine biome
    BiomeType biome = determineBiome(temperature, humidity, weirdness,
                                     continental, prelimHeight);

    // Check for river
    bool hasRiver = isRiver(riverResults[i], riverMaskResults[i], prelimHeight);
    if (hasRiver && getBiomeConfig(biome).hasRivers)
    {
      biome = BIOME_RIVER;
    }

    chunkData.biomes[i] = biome;

    // Recalculate height with biome-specific adjustments
    chunkData.heightMap[i] = calculateHeight(continental, erosionResults[i],
                                             peaksValleysResults[i], ridgeResults[i],
                                             biome);
  }

  // Second pass: Generate voxel columns
  for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ)
  {
    for (int localX = 0; localX < CHUNK_SIZE; ++localX)
    {
      int colIndex = getColumnIndex(localX, localZ);
      int terrainHeight = chunkData.heightMap[colIndex];
      BiomeType biome = chunkData.biomes[colIndex];
      bool isRiverBiome = (biome == BIOME_RIVER);

      // Fill column
      for (int y = 0; y < CHUNK_HEIGHT; ++y)
      {
        int voxelIndex = getVoxelIndex(localX, y, localZ);
        
        // FastNoise2 GenUniformGrid3D output is in [z][y][x] order
        int noiseIndex = (localZ * CHUNK_HEIGHT * CHUNK_SIZE) + (y * CHUNK_SIZE) + localX;

        float caveVal = caveResults[noiseIndex];
        float ravineVal = ravineResults[noiseIndex];

        // Determine basic terrain type
        TextureType type = getVoxelTypeAt(y, terrainHeight, biome, isRiverBiome);

        // Carve caves and ravines (not in bedrock or air)
        if (type != TextureType::BEDROCK && type != TextureType::AIR &&
            type != TextureType::WATER)
        {
          float heightRatio = std::clamp(static_cast<float>(y - SEA_LEVEL) / 64.0f,
                                         0.0f, 1.0f);
          float caveThreshold = 0.6f + heightRatio * 0.35f;
          float ravineThreshold = 0.8f + heightRatio * 0.15f;

          if (caveVal > caveThreshold)
          {
            type = TextureType::AIR;
          }
          else if (ravineVal > ravineThreshold)
          {
            type = TextureType::AIR;
          }
        }

        chunkData.voxels[voxelIndex].type = type;
      }
    }
  }

  // Ore generation pass
  for (const auto &ore : m_ores)
  {
    int minY = std::max(0, ore.minHeight);
    int maxY = std::min(CHUNK_HEIGHT, ore.maxHeight);
    if (minY >= maxY)
      continue;

    int heightSize = maxY - minY;
    std::vector<float> oreNoise(CHUNK_SIZE * heightSize * CHUNK_SIZE);

    m_oreNoise->GenUniformGrid3D(oreNoise.data(), chunkX, minY, chunkZ,
                                 CHUNK_SIZE, heightSize, CHUNK_SIZE, ore.scale,
                                 m_seed + ore.seedOffset);

    int noiseIdx = 0;
    for (int z = 0; z < CHUNK_SIZE; ++z)
    {
      for (int y = 0; y < heightSize; ++y)
      {
        for (int x = 0; x < CHUNK_SIZE; ++x)
        {
          float noiseVal = oreNoise[noiseIdx++];
          int worldY = minY + y;
          int voxelIndex = getVoxelIndex(x, worldY, z);

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
  // Normalize to 0-1 range
  float temp = (temperature + 1.0f) * 0.5f;
  float humid = (humidity + 1.0f) * 0.5f;
  float cont = (continental + 1.0f) * 0.5f;
  float weird = (weirdness + 1.0f) * 0.5f;

  // Check for ocean first (below sea level with low continental)
  if (height < SEA_LEVEL - 5 && cont < 0.45f)
  {
    if (temp < 0.35f)
    {
      return BIOME_FROZEN_OCEAN;
    }
    return BIOME_OCEAN;
  }

  // Beach areas (near sea level with low continental)
  if (height >= SEA_LEVEL - 3 && height <= SEA_LEVEL + 3 && cont < 0.5f)
  {
    if (temp < 0.35f)
    {
      return BIOME_SNOWY_TUNDRA; // Snowy beach
    }
    return BIOME_BEACH;
  }

  // High elevation = mountains
  if (height > 130)
  {
    if (temp < 0.4f || height > 170)
    {
      return BIOME_SNOWY_MOUNTAINS;
    }
    return BIOME_MOUNTAINS;
  }

  // Cold biomes (temp < 0.35)
  if (temp < 0.35f)
  {
    if (humid < 0.3f)
    {
      return BIOME_SNOWY_TUNDRA;
    }
    else if (humid < 0.6f)
    {
      return BIOME_SNOWY_TAIGA;
    }
    else
    {
      if (weird > 0.7f)
      {
        return BIOME_ICE_SPIKES;
      }
      return BIOME_SNOWY_TAIGA;
    }
  }

  // Hot biomes (temp > 0.65)
  if (temp > 0.65f)
  {
    if (humid < 0.25f)
    {
      return BIOME_DESERT;
    }
    else if (humid < 0.45f)
    {
      if (weird > 0.65f)
      {
        return BIOME_BADLANDS;
      }
      return BIOME_SAVANNA;
    }
    else
    {
      return BIOME_JUNGLE;
    }
  }

  // Temperate biomes
  if (humid < 0.3f)
  {
    return BIOME_PLAINS;
  }
  else if (humid < 0.5f)
  {
    return BIOME_FOREST;
  }
  else if (humid < 0.65f)
  {
    if (weird > 0.6f)
    {
      return BIOME_BIRCH_FOREST;
    }
    return BIOME_FOREST;
  }
  else if (humid < 0.8f)
  {
    return BIOME_DARK_FOREST;
  }
  else
  {
    return BIOME_SWAMP;
  }
}

BiomeType TerrainGenerator::getBiomeAt(int worldX, int worldZ) const
{
  std::vector<float> temp(1), humid(1), weird(1), cont(1), erosion(1), pv(1), ridge(1);

  m_temperatureNoise->GenUniformGrid2D(temp.data(), worldX, worldZ, 1, 1, 1.0f, m_seed + 6000);
  m_humidityNoise->GenUniformGrid2D(humid.data(), worldX, worldZ, 1, 1, 1.0f, m_seed + 7000);
  m_weirdnessNoise->GenUniformGrid2D(weird.data(), worldX, worldZ, 1, 1, 1.0f, m_seed + 8000);
  m_continentalNoise->GenUniformGrid2D(cont.data(), worldX, worldZ, 1, 1, 1.0f, m_seed);
  m_erosionNoise->GenUniformGrid2D(erosion.data(), worldX, worldZ, 1, 1, 1.0f, m_seed + 1000);
  m_peaksValleysNoise->GenUniformGrid2D(pv.data(), worldX, worldZ, 1, 1, 1.0f, m_seed + 2000);
  m_ridgeNoise->GenUniformGrid2D(ridge.data(), worldX, worldZ, 1, 1, 1.0f, m_seed + 3000);

  int height = calculateHeight(cont[0], erosion[0], pv[0], ridge[0], BIOME_PLAINS);
  return determineBiome(temp[0], humid[0], weird[0], cont[0], height);
}

// =============================================
// HEIGHT CALCULATION
// =============================================

int TerrainGenerator::calculateHeight(float continental, float erosion,
                                      float peaksValleys, float ridge,
                                      BiomeType biome) const
{
  // Clamp noise values
  continental = std::clamp(continental, -1.0f, 1.0f);
  erosion = std::clamp(erosion, -1.0f, 1.0f);
  peaksValleys = std::clamp(peaksValleys, -1.0f, 1.0f);
  ridge = std::clamp(ridge, -1.0f, 1.0f);

  // Normalize to [0, 1]
  float continentalFactor = (continental + 1.0f) * 0.5f;
  float erosionFactor = (erosion + 1.0f) * 0.5f;

  float baseHeight = static_cast<float>(SEA_LEVEL);

  // Calculate terrain weights
  float oceanWeight = 1.0f - smoothstep(0.25f, 0.32f, continentalFactor);
  float beachWeight = smoothstep(0.25f, 0.32f, continentalFactor) *
                      (1.0f - smoothstep(0.35f, 0.42f, continentalFactor));
  float plainsWeight = smoothstep(0.35f, 0.42f, continentalFactor) *
                       (1.0f - smoothstep(0.55f, 0.65f, continentalFactor));
  float hillsWeight = smoothstep(0.55f, 0.65f, continentalFactor) *
                      (1.0f - smoothstep(0.75f, 0.85f, continentalFactor));
  float mountainWeight = smoothstep(0.75f, 0.85f, continentalFactor);

  // Normalize weights
  float totalWeight = oceanWeight + beachWeight + plainsWeight + hillsWeight + mountainWeight;
  if (totalWeight > 0.001f)
  {
    oceanWeight /= totalWeight;
    beachWeight /= totalWeight;
    plainsWeight /= totalWeight;
    hillsWeight /= totalWeight;
    mountainWeight /= totalWeight;
  }

  // Height contributions
  float oceanHeight = -20.0f + peaksValleys * 8.0f;
  float beachHeight = 3.0f + peaksValleys * 3.0f * erosionFactor;
  float plainsBase = 25.0f;
  float plainsVariation = peaksValleys * 25.0f * (0.5f + erosionFactor * 0.5f);
  float plainsHeight = plainsBase + plainsVariation;

  float hillsBase = 55.0f;
  float hillsVariation = peaksValleys * 30.0f * (0.6f + erosionFactor * 0.4f);
  float hillsHeight = hillsBase + hillsVariation;

  float mountainBase = 80.0f;
  float peakContribution = 0.0f;
  if (ridge > 0.2f)
  {
    float peakFactor = (ridge - 0.2f) / 0.8f;
    peakFactor = peakFactor * peakFactor;
    peakContribution = peakFactor * 95.0f;
  }
  float mountainVariation = peaksValleys * 35.0f;
  float mountainHeight = mountainBase + mountainVariation + peakContribution;

  // Blend terrain types
  float heightVariation = oceanWeight * oceanHeight + beachWeight * beachHeight +
                          plainsWeight * plainsHeight + hillsWeight * hillsHeight +
                          mountainWeight * mountainHeight;

  // Final height calculation (Biome-specific logic removed to prevent seams)
  float finalHeight = baseHeight + heightVariation;
  
  return std::clamp(static_cast<int>(std::round(finalHeight)), 1, CHUNK_HEIGHT - 32);
}

// =============================================
// RIVER GENERATION
// =============================================

bool TerrainGenerator::isRiver(float riverNoise, float riverMask, int height) const
{
  // Rivers only form above sea level but not too high
  if (height < SEA_LEVEL + 2 || height > 120)
  {
    return false;
  }

  // River mask prevents rivers in certain areas
  if (riverMask < -0.2f)
  {
    return false;
  }

  // Ridged noise creates river paths - values close to 0 are rivers
  // (ridged noise has peaks at 1 and valleys at 0)
  float riverWidth = 0.15f; // Controls river width
  return std::abs(riverNoise) < riverWidth;
}

void TerrainGenerator::carveRiver(ChunkData &chunkData, int localX, int localZ,
                                  int terrainHeight)
{
  // Carve river bed
  int riverBottom = SEA_LEVEL - 3;
  int riverSurface = SEA_LEVEL - 1;

  for (int y = riverBottom; y <= terrainHeight; ++y)
  {
    int voxelIndex = getVoxelIndex(localX, y, localZ);
    if (y <= riverSurface)
    {
      chunkData.voxels[voxelIndex].type = TextureType::WATER;
    }
    else
    {
      chunkData.voxels[voxelIndex].type = TextureType::AIR;
    }
  }

  // River bed
  for (int y = riverBottom - 2; y < riverBottom; ++y)
  {
    if (y > 0)
    {
      int voxelIndex = getVoxelIndex(localX, y, localZ);
      if (chunkData.voxels[voxelIndex].type != TextureType::BEDROCK)
      {
        chunkData.voxels[voxelIndex].type = TextureType::GRAVEL;
      }
    }
  }
}

// =============================================
// VEGETATION GENERATION
// =============================================

void TerrainGenerator::generateVegetation(ChunkData &chunkData, int chunkX, int chunkZ)
{
  // Use deterministic RNG based on chunk coordinates
  std::mt19937 rng(m_seed ^ (chunkX * 73856093) ^ (chunkZ * 19349663));
  std::uniform_real_distribution<float> dist(0.0f, 1.0f);

  // Generate tree noise for this chunk
  std::vector<float> treeNoiseResults(CHUNK_SIZE * CHUNK_SIZE);
  m_treeNoise->GenUniformGrid2D(treeNoiseResults.data(), chunkX, chunkZ,
                                CHUNK_SIZE, CHUNK_SIZE, 1.0f, m_seed + 10000);

  for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ)
  {
    for (int localX = 0; localX < CHUNK_SIZE; ++localX)
    {
      int colIndex = getColumnIndex(localX, localZ);
      BiomeType biome = chunkData.biomes[colIndex];
      int terrainHeight = chunkData.heightMap[colIndex];

      // Skip water and very high areas
      if (terrainHeight <= SEA_LEVEL || terrainHeight > 200)
      {
        continue;
      }

      const BiomeConfig &config = getBiomeConfig(biome);
      float treeNoise = (treeNoiseResults[colIndex] + 1.0f) * 0.5f;

      // Check for surface block (must be solid ground)
      int surfaceY = terrainHeight;
      int surfaceIndex = getVoxelIndex(localX, surfaceY, localZ);
      TextureType surfaceType = static_cast<TextureType>(chunkData.voxels[surfaceIndex].type);

      // Only place vegetation on valid surfaces
      if (surfaceType != TextureType::GRASS_TOP &&
          surfaceType != TextureType::DIRT &&
          surfaceType != TextureType::SAND &&
          surfaceType != TextureType::SNOW)
      {
        continue;
      }

      // Tree placement
      if (config.treeDensity > 0.0f)
      {
        // Use both noise and RNG for natural distribution
        float treeProbability = config.treeDensity * treeNoise;

        // Minimum spacing check using hash
        int worldX = chunkX + localX;
        int worldZ = chunkZ + localZ;
        uint32_t hash = (worldX * 374761393 + worldZ * 668265263) ^ m_seed;
        float hashValue = static_cast<float>(hash & 0xFFFF) / 65535.0f;

        if (hashValue < treeProbability * 0.3f)
        {
          // Ensure minimum spacing from other trees (simple check)
          bool canPlaceTree = true;
          for (int dx = -2; dx <= 2 && canPlaceTree; ++dx)
          {
            for (int dz = -2; dz <= 2 && canPlaceTree; ++dz)
            {
              if (dx == 0 && dz == 0)
                continue;
              int nx = localX + dx;
              int nz = localZ + dz;
              if (nx >= 0 && nx < CHUNK_SIZE && nz >= 0 && nz < CHUNK_SIZE)
              {
                int nIndex = getColumnIndex(nx, nz);
                int nHeight = chunkData.heightMap[nIndex];
                int checkY = nHeight + 1;
                if (checkY < CHUNK_HEIGHT)
                {
                  int checkIndex = getVoxelIndex(nx, checkY, nz);
                  if (chunkData.voxels[checkIndex].type == TextureType::OAK_LOG)
                  {
                    canPlaceTree = false;
                  }
                }
              }
            }
          }

          if (canPlaceTree)
          {
            placeTree(chunkData, localX, localZ, surfaceY + 1, biome);
          }
        }
      }

      // Cactus placement (desert only)
      if (config.hasCacti && biome == BIOME_DESERT)
      {
        float cactusProbability = 0.015f;
        int worldX = chunkX + localX;
        int worldZ = chunkZ + localZ;
        uint32_t hash = (worldX * 198491317 + worldZ * 781874213) ^ (m_seed + 1);
        float hashValue = static_cast<float>(hash & 0xFFFF) / 65535.0f;

        if (hashValue < cactusProbability && surfaceType == TextureType::SAND)
        {
          placeCactus(chunkData, localX, localZ, surfaceY + 1);
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
  case BIOME_ICE_SPIKES:
    placeSprucetree(chunkData, localX, localZ, baseY);
    break;
  case BIOME_BIRCH_FOREST:
    placeBirchTree(chunkData, localX, localZ, baseY);
    break;
  case BIOME_JUNGLE:
    placeJungleTree(chunkData, localX, localZ, baseY);
    break;
  case BIOME_DARK_FOREST:
  case BIOME_FOREST:
  case BIOME_PLAINS:
  case BIOME_SWAMP:
  case BIOME_MOUNTAINS:
  case BIOME_SAVANNA:
  default:
    placeOakTree(chunkData, localX, localZ, baseY);
    break;
  }
}

void TerrainGenerator::placeOakTree(ChunkData &chunkData, int localX, int localZ, int baseY)
{
  // Standard oak tree: 4-6 blocks tall trunk, 3x3 to 5x5 leaf canopy
  int trunkHeight = 4 + (baseY % 3); // 4-6 blocks

  // Place trunk
  for (int y = 0; y < trunkHeight; ++y)
  {
    setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::OAK_LOG);
  }

  // Place leaves (roughly spherical canopy)
  int leafStart = baseY + trunkHeight - 2;
  int leafEnd = baseY + trunkHeight + 1;

  for (int ly = leafStart; ly <= leafEnd; ++ly)
  {
    int radius = (ly == leafEnd) ? 1 : 2;
    for (int lx = -radius; lx <= radius; ++lx)
    {
      for (int lz = -radius; lz <= radius; ++lz)
      {
        // Skip corners for more natural shape
        if (std::abs(lx) == radius && std::abs(lz) == radius)
        {
          if ((ly == leafStart || ly == leafEnd) && radius > 1)
            continue;
        }

        int wx = localX + lx;
        int wz = localZ + lz;

        // Don't overwrite trunk
        if (lx == 0 && lz == 0 && ly < baseY + trunkHeight)
          continue;

        setVoxelSafe(chunkData, wx, ly, wz, TextureType::OAK_LEAVES);
      }
    }
  }
}

void TerrainGenerator::placeBirchTree(ChunkData &chunkData, int localX, int localZ, int baseY)
{
  // Birch trees are similar to oak but taller and thinner
  int trunkHeight = 5 + (baseY % 3);

  for (int y = 0; y < trunkHeight; ++y)
  {
    setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::OAK_LOG);
  }

  int leafStart = baseY + trunkHeight - 3;
  int leafEnd = baseY + trunkHeight + 1;

  for (int ly = leafStart; ly <= leafEnd; ++ly)
  {
    int radius = (ly >= baseY + trunkHeight) ? 1 : 2;
    for (int lx = -radius; lx <= radius; ++lx)
    {
      for (int lz = -radius; lz <= radius; ++lz)
      {
        if (std::abs(lx) == radius && std::abs(lz) == radius && ly != leafStart + 1)
          continue;

        int wx = localX + lx;
        int wz = localZ + lz;

        if (lx == 0 && lz == 0 && ly < baseY + trunkHeight)
          continue;

        setVoxelSafe(chunkData, wx, ly, wz, TextureType::OAK_LEAVES);
      }
    }
  }
}

void TerrainGenerator::placeSprucetree(ChunkData &chunkData, int localX, int localZ, int baseY)
{
  // Spruce: Tall, conical shape
  int trunkHeight = 6 + (baseY % 4);

  for (int y = 0; y < trunkHeight; ++y)
  {
    setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::OAK_LOG);
  }

  // Conical leaves
  for (int layer = 0; layer < trunkHeight - 1; ++layer)
  {
    int ly = baseY + trunkHeight - 1 - layer;
    int radius = layer / 2;

    if (layer % 2 == 0 || layer == trunkHeight - 2)
    {
      for (int lx = -radius; lx <= radius; ++lx)
      {
        for (int lz = -radius; lz <= radius; ++lz)
        {
          if (lx == 0 && lz == 0)
            continue;

          // Diamond pattern for conical shape
          if (std::abs(lx) + std::abs(lz) <= radius + 1)
          {
            setVoxelSafe(chunkData, localX + lx, ly, localZ + lz, TextureType::OAK_LEAVES);
          }
        }
      }
    }
  }

  // Top leaf
  setVoxelSafe(chunkData, localX, baseY + trunkHeight, localZ, TextureType::OAK_LEAVES);
}

void TerrainGenerator::placeJungleTree(ChunkData &chunkData, int localX, int localZ, int baseY)
{
  // Jungle trees: Very tall with large canopy
  int trunkHeight = 8 + (baseY % 6);

  for (int y = 0; y < trunkHeight; ++y)
  {
    setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::OAK_LOG);
  }

  // Large spherical canopy
  int leafStart = baseY + trunkHeight - 3;
  int leafEnd = baseY + trunkHeight + 2;

  for (int ly = leafStart; ly <= leafEnd; ++ly)
  {
    int distFromCenter = std::abs(ly - (leafStart + 2));
    int radius = 3 - distFromCenter / 2;

    for (int lx = -radius; lx <= radius; ++lx)
    {
      for (int lz = -radius; lz <= radius; ++lz)
      {
        if (lx * lx + lz * lz > radius * radius + 1)
          continue;

        int wx = localX + lx;
        int wz = localZ + lz;

        if (lx == 0 && lz == 0 && ly < baseY + trunkHeight)
          continue;

        setVoxelSafe(chunkData, wx, ly, wz, TextureType::OAK_LEAVES);
      }
    }
  }

  // Hanging vines/extra leaves
  for (int dx = -2; dx <= 2; dx += 4)
  {
    for (int dz = -2; dz <= 2; dz += 4)
    {
      for (int vy = 0; vy < 3; ++vy)
      {
        setVoxelSafe(chunkData, localX + dx, leafStart - vy, localZ + dz, TextureType::OAK_LEAVES);
      }
    }
  }
}

void TerrainGenerator::placeCactus(ChunkData &chunkData, int localX, int localZ, int baseY)
{
  // Cactus: 1-3 blocks tall, using oak log as placeholder
  int height = 1 + (baseY % 3);

  for (int y = 0; y < height; ++y)
  {
    // Check for adjacent blocks (cacti can't touch)
    bool canPlace = true;
    for (int dx = -1; dx <= 1 && canPlace; ++dx)
    {
      for (int dz = -1; dz <= 1 && canPlace; ++dz)
      {
        if (dx == 0 && dz == 0)
          continue;
        int nx = localX + dx;
        int nz = localZ + dz;
        if (nx >= 0 && nx < CHUNK_SIZE && nz >= 0 && nz < CHUNK_SIZE)
        {
          int checkIndex = getVoxelIndex(nx, baseY + y, nz);
          if (chunkData.voxels[checkIndex].type != TextureType::AIR &&
              chunkData.voxels[checkIndex].type != TextureType::SAND)
          {
            canPlace = false;
          }
        }
      }
    }

    if (canPlace)
    {
      // Using OAK_LOG as cactus placeholder (green tint in rendering would be ideal)
      setVoxelSafe(chunkData, localX, baseY + y, localZ, TextureType::OAK_LOG);
    }
  }
}

void TerrainGenerator::placeDeadBush(ChunkData &chunkData, int localX, int localZ, int baseY)
{
  // Dead bush is a single block - using gravel as placeholder
  setVoxelSafe(chunkData, localX, baseY, localZ, TextureType::GRAVEL);
}

// =============================================
// VOXEL TYPE DETERMINATION
// =============================================

TextureType TerrainGenerator::getVoxelTypeAt(int worldY, int terrainHeight,
                                             BiomeType biome, bool isRiver) const
{
  // Bedrock layer
  if (worldY <= BEDROCK_LEVEL)
  {
    return TextureType::BEDROCK;
  }

  const BiomeConfig &config = getBiomeConfig(biome);

  // River handling
  if (isRiver)
  {
    int riverBottom = SEA_LEVEL - 3;
    if (worldY <= riverBottom)
    {
      if (worldY <= riverBottom - 2)
      {
        return TextureType::GRAVEL;
      }
      return TextureType::SAND;
    }
    if (worldY <= SEA_LEVEL - 1 && worldY > terrainHeight)
    {
      return TextureType::WATER;
    }
  }

  // Below terrain height
  if (worldY <= terrainHeight)
  {
    int depthFromSurface = terrainHeight - worldY;

    if (depthFromSurface == 0)
    {
      // Surface block - biome specific
      if (terrainHeight <= SEA_LEVEL + 2)
      {
        return config.underwaterBlock; // Beach/shoreline
      }

      // Snow on top for cold biomes at high elevation
      if (config.hasSnow && terrainHeight > 120)
      {
        return TextureType::SNOW;
      }

      return config.surfaceBlock;
    }
    else if (depthFromSurface <= config.subsurfaceDepth)
    {
      // Subsurface layer
      if (terrainHeight <= SEA_LEVEL + 2)
      {
        return config.underwaterBlock;
      }
      return config.subsurfaceBlock;
    }
    else
    {
      // Deep underground
      return TextureType::STONE;
    }
  }

  // Water between terrain and sea level
  if (worldY <= SEA_LEVEL)
  {
    return TextureType::WATER;
  }

  // Air above terrain
  return TextureType::AIR;
}

// =============================================
// BORDER GENERATION
// =============================================

void TerrainGenerator::generateChunkBorders(ChunkData &chunkData, int chunkX,
                                            int chunkZ)
{
  constexpr int borderSize = 1;

  auto getHeightAndBiomeAt = [this](int worldX, int worldZ) -> std::pair<int, BiomeType>
  {
    std::vector<float> continental(1), erosion(1), pv(1), ridge(1);
    std::vector<float> temp(1), humid(1), weird(1);

    m_continentalNoise->GenUniformGrid2D(continental.data(), worldX, worldZ, 1, 1, 1.0f, m_seed);
    m_erosionNoise->GenUniformGrid2D(erosion.data(), worldX, worldZ, 1, 1, 1.0f, m_seed + 1000);
    m_peaksValleysNoise->GenUniformGrid2D(pv.data(), worldX, worldZ, 1, 1, 1.0f, m_seed + 2000);
    m_ridgeNoise->GenUniformGrid2D(ridge.data(), worldX, worldZ, 1, 1, 1.0f, m_seed + 3000);
    m_temperatureNoise->GenUniformGrid2D(temp.data(), worldX, worldZ, 1, 1, 1.0f, m_seed + 6000);
    m_humidityNoise->GenUniformGrid2D(humid.data(), worldX, worldZ, 1, 1, 1.0f, m_seed + 7000);
    m_weirdnessNoise->GenUniformGrid2D(weird.data(), worldX, worldZ, 1, 1, 1.0f, m_seed + 8000);

    int prelimHeight = calculateHeight(continental[0], erosion[0], pv[0], ridge[0], BIOME_PLAINS);
    BiomeType biome = determineBiome(temp[0], humid[0], weird[0], continental[0], prelimHeight);
    int height = calculateHeight(continental[0], erosion[0], pv[0], ridge[0], biome);

    return {height, biome};
  };

  auto isCaveOrRavine = [this](int worldX, int worldY, int worldZ) -> bool
  {
    float heightRatio = std::clamp(static_cast<float>(worldY - SEA_LEVEL) / 64.0f, 0.0f, 1.0f);
    float caveThreshold = 0.6f + heightRatio * 0.35f;
    float ravineThreshold = 0.8f + heightRatio * 0.15f;

    std::vector<float> cave(1), ravine(1);
    m_caveNoise->GenUniformGrid3D(cave.data(), worldX, worldY, worldZ, 1, 1, 1, 1.0f, m_seed + 4000);
    if (cave[0] > caveThreshold)
      return true;

    m_ravineNoise->GenUniformGrid3D(ravine.data(), worldX, worldY, worldZ, 1, 1, 1, 1.0f, m_seed + 5000);
    if (ravine[0] > ravineThreshold)
      return true;

    return false;
  };

  auto setBorderVoxel = [&](int lx, int ly, int lz, TextureType type) {
    if (lx >= -1 && lx <= CHUNK_SIZE && ly >= -1 && ly <= CHUNK_HEIGHT &&
        lz >= -1 && lz <= CHUNK_SIZE) {
      size_t index = (ly + 1) * 18 * 18 + (lz + 1) * 18 + (lx + 1);
      chunkData.borderVoxels[index] = static_cast<uint8_t>(type);
    }
  };

  // North border (Z = -1)
  for (int x = 0; x < CHUNK_SIZE; ++x)
  {
    int worldX = chunkX + x;
    int worldZ = chunkZ - borderSize;
    auto [height, biome] = getHeightAndBiomeAt(worldX, worldZ);

    for (int y = 0; y < CHUNK_HEIGHT; ++y)
    {
      TextureType voxelType = getVoxelTypeAt(y, height, biome);

      if (voxelType != TextureType::AIR && voxelType != TextureType::BEDROCK)
      {
        if (isCaveOrRavine(worldX, y, worldZ))
        {
          voxelType = TextureType::AIR;
        }
      }

      if (voxelType != TextureType::AIR)
      {
        setBorderVoxel(x, y, -borderSize, voxelType);
      }
    }
  }

  // South border (Z = CHUNK_SIZE)
  for (int x = 0; x < CHUNK_SIZE; ++x)
  {
    int worldX = chunkX + x;
    int worldZ = chunkZ + CHUNK_SIZE;
    auto [height, biome] = getHeightAndBiomeAt(worldX, worldZ);

    for (int y = 0; y < CHUNK_HEIGHT; ++y)
    {
      TextureType voxelType = getVoxelTypeAt(y, height, biome);

      if (voxelType != TextureType::AIR && voxelType != TextureType::BEDROCK)
      {
        if (isCaveOrRavine(worldX, y, worldZ))
        {
          voxelType = TextureType::AIR;
        }
      }

      if (voxelType != TextureType::AIR)
      {
        setBorderVoxel(x, y, CHUNK_SIZE, voxelType);
      }
    }
  }

  // West border (X = -1)
  for (int z = 0; z < CHUNK_SIZE; ++z)
  {
    int worldX = chunkX - borderSize;
    int worldZ = chunkZ + z;
    auto [height, biome] = getHeightAndBiomeAt(worldX, worldZ);

    for (int y = 0; y < CHUNK_HEIGHT; ++y)
    {
      TextureType voxelType = getVoxelTypeAt(y, height, biome);

      if (voxelType != TextureType::AIR && voxelType != TextureType::BEDROCK)
      {
        if (isCaveOrRavine(worldX, y, worldZ))
        {
          voxelType = TextureType::AIR;
        }
      }

      if (voxelType != TextureType::AIR)
      {
        setBorderVoxel(-borderSize, y, z, voxelType);
      }
    }
  }

  // East border (X = CHUNK_SIZE)
  for (int z = 0; z < CHUNK_SIZE; ++z)
  {
    int worldX = chunkX + CHUNK_SIZE;
    int worldZ = chunkZ + z;
    auto [height, biome] = getHeightAndBiomeAt(worldX, worldZ);

    for (int y = 0; y < CHUNK_HEIGHT; ++y)
    {
      TextureType voxelType = getVoxelTypeAt(y, height, biome);

      if (voxelType != TextureType::AIR && voxelType != TextureType::BEDROCK)
      {
        if (isCaveOrRavine(worldX, y, worldZ))
        {
          voxelType = TextureType::AIR;
        }
      }

      if (voxelType != TextureType::AIR)
      {
        setBorderVoxel(CHUNK_SIZE, y, z, voxelType);
      }
    }
  }
}

// =============================================
// UTILITY FUNCTIONS
// =============================================

float TerrainGenerator::smoothstep(float edge0, float edge1, float x) const
{
  float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

float TerrainGenerator::lerp(float a, float b, float t) const
{
  return a + t * (b - a);
}
