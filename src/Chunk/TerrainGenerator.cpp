#include <Chunk/TerrainGenerator.hpp>
#include <Chunk/ChunkBorders.hpp>
#include <Chunk/StreamHelpers.hpp>
#include <Block/BlockTraits.hpp>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <mutex>

// =============================================
// STATIC MEMBER INITIALIZATION
// =============================================

std::array<BiomeConfig, BIOME_COUNT> TerrainGenerator::s_biomeConfigs;
static std::once_flag s_biomeConfigsOnceFlag;

void TerrainGenerator::initBiomeConfigs()
{
  std::call_once(s_biomeConfigsOnceFlag, []()
                 {
    // Distinct biomes without neon chroma (moderate grass/foliage tints).
    // Fields: surface, subsurface, underwater, subsurfaceDepth,
    //         treeDensity, bushDensity, rockDensity, fallenLogDensity,
    //         grassColor, foliageColor, hasSnow, hasCacti
    s_biomeConfigs[BIOME_FROZEN_OCEAN] = {
        TextureType::SNOW, TextureType::GRAVEL, TextureType::GRAVEL, 3,
        0.0f, 0.0f, 0.0f, 0.0f,
        glm::vec3(0.50f, 0.68f, 0.62f), glm::vec3(0.38f, 0.55f, 0.52f),
        true, false, 0.0f};

    s_biomeConfigs[BIOME_SNOWY_TUNDRA] = {
        TextureType::SNOW, TextureType::DIRT, TextureType::GRAVEL, 3,
        0.02f, 0.005f, 0.02f, 0.0f,
        glm::vec3(0.48f, 0.66f, 0.50f), glm::vec3(0.36f, 0.52f, 0.42f),
        true, false, 0.0f};

    s_biomeConfigs[BIOME_SNOWY_TAIGA] = {
        TextureType::SNOW, TextureType::DIRT, TextureType::GRAVEL, 3,
        0.15f, 0.03f, 0.015f, 0.01f,
        glm::vec3(0.35f, 0.55f, 0.45f), glm::vec3(0.28f, 0.48f, 0.40f),
        true, false, 0.5f};

    s_biomeConfigs[BIOME_ICE_SPIKES] = {
        TextureType::SNOW, TextureType::PACKED_ICE, TextureType::GRAVEL, 5,
        0.0f, 0.0f, 0.005f, 0.0f,
        glm::vec3(0.55f, 0.70f, 0.72f), glm::vec3(0.42f, 0.58f, 0.62f),
        true, false, 2.0f};

    s_biomeConfigs[BIOME_OCEAN] = {
        TextureType::GRAVEL, TextureType::GRAVEL, TextureType::SAND, 4,
        0.0f, 0.0f, 0.0f, 0.0f,
        glm::vec3(0.35f, 0.58f, 0.48f), glm::vec3(0.28f, 0.50f, 0.42f),
        false, false, 0.0f};

    s_biomeConfigs[BIOME_BEACH] = {
        TextureType::SAND, TextureType::SAND, TextureType::SAND, 5,
        0.0f, 0.0f, 0.0f, 0.0f,
        glm::vec3(0.62f, 0.72f, 0.42f), glm::vec3(0.50f, 0.62f, 0.32f),
        false, false, 0.0f};

    s_biomeConfigs[BIOME_PLAINS] = {
        TextureType::GRASS_TOP, TextureType::DIRT, TextureType::SAND, 3,
        0.01f, 0.02f, 0.004f, 0.0f,
        glm::vec3(0.48f, 0.78f, 0.32f), glm::vec3(0.38f, 0.68f, 0.26f),
        false, false, 0.0f};

    s_biomeConfigs[BIOME_FOREST] = {
        TextureType::GRASS_TOP, TextureType::DIRT, TextureType::GRAVEL, 3,
        0.25f, 0.05f, 0.008f, 0.01f,
        glm::vec3(0.30f, 0.65f, 0.24f), glm::vec3(0.24f, 0.55f, 0.20f),
        false, false, 0.0f};

    s_biomeConfigs[BIOME_BIRCH_FOREST] = {
        TextureType::GRASS_TOP, TextureType::DIRT, TextureType::GRAVEL, 3,
        0.22f, 0.05f, 0.008f, 0.01f,
        glm::vec3(0.48f, 0.76f, 0.36f), glm::vec3(0.42f, 0.68f, 0.32f),
        false, false, 0.0f};

    s_biomeConfigs[BIOME_DARK_FOREST] = {
        TextureType::GRASS_TOP, TextureType::DIRT, TextureType::DIRT, 4,
        0.4f, 0.06f, 0.01f, 0.02f,
        glm::vec3(0.22f, 0.48f, 0.22f), glm::vec3(0.18f, 0.40f, 0.18f),
        false, false, 0.0f};

    s_biomeConfigs[BIOME_SWAMP] = {
        TextureType::MUD, TextureType::DIRT, TextureType::CLAY, 4,
        0.12f, 0.06f, 0.01f, 0.015f,
        glm::vec3(0.38f, 0.50f, 0.28f), glm::vec3(0.32f, 0.44f, 0.24f),
        false, false, 1.0f};

    s_biomeConfigs[BIOME_RIVER] = {
        TextureType::SAND, TextureType::CLAY, TextureType::GRAVEL, 2,
        0.0f, 0.0f, 0.0f, 0.0f,
        glm::vec3(0.38f, 0.62f, 0.36f), glm::vec3(0.32f, 0.54f, 0.30f),
        false, false, 0.0f};

    s_biomeConfigs[BIOME_DESERT] = {
        TextureType::SAND, TextureType::SANDSTONE, TextureType::SAND, 8,
        0.0f, 0.0f, 0.006f, 0.0f,
        glm::vec3(0.78f, 0.70f, 0.38f), glm::vec3(0.65f, 0.58f, 0.28f),
        false, true, 0.0f};

    s_biomeConfigs[BIOME_SAVANNA] = {
        TextureType::GRASS_TOP, TextureType::COARSE_DIRT, TextureType::SAND, 3,
        0.08f, 0.015f, 0.008f, 0.004f,
        glm::vec3(0.72f, 0.70f, 0.32f), glm::vec3(0.60f, 0.58f, 0.26f),
        false, false, 1.5f};

    s_biomeConfigs[BIOME_JUNGLE] = {
        TextureType::GRASS_TOP, TextureType::DIRT, TextureType::DIRT, 3,
        0.45f, 0.08f, 0.005f, 0.01f,
        glm::vec3(0.22f, 0.72f, 0.22f), glm::vec3(0.16f, 0.62f, 0.18f),
        false, false, 3.0f};

    s_biomeConfigs[BIOME_BADLANDS] = {
        TextureType::RED_SAND, TextureType::TERRACOTTA, TextureType::RED_SAND, 8,
        0.0f, 0.0f, 0.01f, 0.0f,
        glm::vec3(0.78f, 0.48f, 0.28f), glm::vec3(0.68f, 0.40f, 0.22f),
        false, true, 5.0f};

    s_biomeConfigs[BIOME_MOUNTAINS] = {
        TextureType::STONE, TextureType::STONE, TextureType::GRAVEL, 2,
        0.08f, 0.01f, 0.03f, 0.004f,
        glm::vec3(0.40f, 0.62f, 0.34f), glm::vec3(0.34f, 0.52f, 0.28f),
        false, false, 8.0f};

    s_biomeConfigs[BIOME_SNOWY_MOUNTAINS] = {
        TextureType::SNOW, TextureType::STONE, TextureType::GRAVEL, 2,
        0.02f, 0.005f, 0.03f, 0.0f,
        glm::vec3(0.50f, 0.66f, 0.55f), glm::vec3(0.40f, 0.55f, 0.48f),
        true, false, 8.0f};

    // Phase 3 configs initially reuse existing blocks. Dedicated blocks and
    // placers are added incrementally in 3B/3C without changing biome ordinals.
    s_biomeConfigs[BIOME_FLOWER_MEADOW] = {
        TextureType::GRASS_TOP, TextureType::DIRT, TextureType::SAND, 3,
        0.005f, 0.10f, 0.002f, 0.0f,
        glm::vec3(0.56f, 0.82f, 0.34f), glm::vec3(0.46f, 0.72f, 0.30f),
        false, false, 0.0f};

    s_biomeConfigs[BIOME_CHERRY_GROVE] = {
        TextureType::GRASS_TOP, TextureType::DIRT, TextureType::GRAVEL, 3,
        0.18f, 0.06f, 0.004f, 0.008f,
        glm::vec3(0.50f, 0.74f, 0.38f), glm::vec3(0.86f, 0.48f, 0.62f),
        false, false, 0.5f};

    s_biomeConfigs[BIOME_AUTUMN_FOREST] = {
        TextureType::GRASS_TOP, TextureType::DIRT, TextureType::GRAVEL, 4,
        0.30f, 0.06f, 0.01f, 0.02f,
        glm::vec3(0.48f, 0.62f, 0.26f), glm::vec3(0.78f, 0.36f, 0.18f),
        false, false, 0.5f};

    s_biomeConfigs[BIOME_REDWOOD_FOREST] = {
        TextureType::PODZOL, TextureType::DIRT, TextureType::GRAVEL, 4,
        0.16f, 0.04f, 0.02f, 0.015f,
        glm::vec3(0.30f, 0.48f, 0.24f), glm::vec3(0.20f, 0.42f, 0.22f),
        false, false, 1.0f};

    s_biomeConfigs[BIOME_MANGROVE_SWAMP] = {
        TextureType::MUD, TextureType::CLAY, TextureType::CLAY, 5,
        0.18f, 0.08f, 0.004f, 0.01f,
        glm::vec3(0.32f, 0.46f, 0.24f), glm::vec3(0.24f, 0.40f, 0.20f),
        false, false, 1.0f};

    s_biomeConfigs[BIOME_BAMBOO_JUNGLE] = {
        TextureType::GRASS_TOP, TextureType::DIRT, TextureType::DIRT, 3,
        0.30f, 0.08f, 0.004f, 0.006f,
        glm::vec3(0.30f, 0.78f, 0.24f), glm::vec3(0.20f, 0.66f, 0.18f),
        false, false, 2.5f};

    s_biomeConfigs[BIOME_MOOR] = {
        TextureType::COARSE_DIRT, TextureType::DIRT, TextureType::GRAVEL, 3,
        0.015f, 0.015f, 0.04f, 0.004f,
        glm::vec3(0.46f, 0.48f, 0.26f), glm::vec3(0.34f, 0.40f, 0.24f),
        false, false, 1.5f};

    s_biomeConfigs[BIOME_GLACIER] = {
        TextureType::PACKED_ICE, TextureType::BLUE_ICE, TextureType::PACKED_ICE, 6,
        0.0f, 0.0f, 0.002f, 0.0f,
        glm::vec3(0.58f, 0.74f, 0.78f), glm::vec3(0.48f, 0.66f, 0.72f),
        true, false, 1.0f};

    s_biomeConfigs[BIOME_FROZEN_RIVER] = {
        TextureType::GRAVEL, TextureType::CLAY, TextureType::GRAVEL, 2,
        0.0f, 0.0f, 0.0f, 0.0f,
        glm::vec3(0.56f, 0.70f, 0.64f), glm::vec3(0.44f, 0.60f, 0.56f),
        true, false, 0.0f};

    s_biomeConfigs[BIOME_VOLCANIC] = {
        TextureType::BASALT, TextureType::BLACKSTONE, TextureType::GRAVEL, 5,
        0.0f, 0.0f, 0.05f, 0.0f,
        glm::vec3(0.32f, 0.28f, 0.25f), glm::vec3(0.38f, 0.24f, 0.18f),
        false, false, 6.0f};

    s_biomeConfigs[BIOME_OASIS] = {
        TextureType::GRASS_TOP, TextureType::DIRT, TextureType::SAND, 4,
        0.12f, 0.08f, 0.002f, 0.002f,
        glm::vec3(0.40f, 0.76f, 0.30f), glm::vec3(0.28f, 0.66f, 0.24f),
        false, false, 0.0f};

    s_biomeConfigs[BIOME_MUSHROOM_FIELDS] = {
        TextureType::MOSS_BLOCK, TextureType::DIRT, TextureType::CLAY, 4,
        0.04f, 0.10f, 0.006f, 0.01f,
        glm::vec3(0.52f, 0.42f, 0.58f), glm::vec3(0.64f, 0.36f, 0.58f),
        false, false, 1.0f};

    s_biomeConfigs[BIOME_CORAL_REEF] = {
        TextureType::SAND, TextureType::SAND, TextureType::GRAVEL, 5,
        0.0f, 0.0f, 0.0f, 0.0f,
        glm::vec3(0.30f, 0.68f, 0.54f), glm::vec3(0.24f, 0.60f, 0.48f),
        false, false, 0.0f};
    for (int i = 0; i < BIOME_COUNT; ++i)
    {
      auto &c = s_biomeConfigs[i];
      if (c.treeDensity > 0.05f) c.decoration = worldgen::Decoration::Woodland;
      if (c.hasSnow) { c.palette = worldgen::Palette::Cold; c.decoration = worldgen::Decoration::Conifer; }
      switch (static_cast<BiomeType>(i))
      {
      case BIOME_DESERT: case BIOME_BADLANDS: case BIOME_SAVANNA:
        c.palette = worldgen::Palette::Arid; c.decoration = worldgen::Decoration::Desert;
        c.relief = i == BIOME_BADLANDS ? worldgen::Relief::Canyon : worldgen::Relief::Plateau; break;
      case BIOME_SWAMP: case BIOME_MANGROVE_SWAMP: case BIOME_OASIS:
        c.palette = worldgen::Palette::Wet; c.decoration = worldgen::Decoration::Wetland; c.relief = worldgen::Relief::Basin; break;
      case BIOME_JUNGLE: case BIOME_BAMBOO_JUNGLE:
        c.palette = worldgen::Palette::Wet; c.decoration = worldgen::Decoration::Tropical; break;
      case BIOME_MOUNTAINS: case BIOME_SNOWY_MOUNTAINS: case BIOME_GLACIER:
        c.palette = worldgen::Palette::Alpine; c.decoration = worldgen::Decoration::Alpine; c.relief = worldgen::Relief::Massif; break;
      case BIOME_VOLCANIC:
        c.palette = worldgen::Palette::Volcanic; c.decoration = worldgen::Decoration::Alpine; c.relief = worldgen::Relief::Cliffs; break;
      case BIOME_MUSHROOM_FIELDS:
        c.palette = worldgen::Palette::Fungal; c.decoration = worldgen::Decoration::Fungal; break;
      case BIOME_OCEAN: case BIOME_FROZEN_OCEAN: case BIOME_CORAL_REEF:
      case BIOME_RIVER: case BIOME_FROZEN_RIVER: case BIOME_BEACH:
        c.palette = worldgen::Palette::Marine; c.decoration = worldgen::Decoration::Marine; c.relief = worldgen::Relief::Basin; break;
      default: break;
      }
    }
  });
}

const BiomeConfig &TerrainGenerator::getBiomeConfig(BiomeType biome)
{
  // std::call_once is the sole synchronization for initialization: repeated
  // calls are cheap and everything written inside the once-block is visible
  // to every caller once it returns. A separate initialized flag here would
  // be read and written outside that mechanism and would race.
  initBiomeConfigs();
  assert(static_cast<std::size_t>(biome) < BIOME_COUNT);
  return s_biomeConfigs[static_cast<std::size_t>(biome)];
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
  // Continental noise - Large scale continent shapes, domain-warped for
  // organic, jagged coastlines (gradient warp in the source domain).
  auto continentalBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto continentalFractal = FastNoise::New<FastNoise::FractalFBm>();
  continentalFractal->SetSource(continentalBase);
  continentalFractal->SetOctaveCount(5);
  continentalFractal->SetLacunarity(2.0f);
  continentalFractal->SetGain(0.38f);
  continentalFractal->SetWeightedStrength(0.0f);

  auto continentalWarp = FastNoise::New<FastNoise::DomainWarpGradient>();
  continentalWarp->SetSource(continentalFractal);
  continentalWarp->SetWarpAmplitude(0.15f);
  continentalWarp->SetWarpFrequency(0.5f);

  // Set frequency directly on the warp node
  auto continentalScale = FastNoise::New<FastNoise::DomainScale>();
  continentalScale->SetSource(continentalWarp);
  continentalScale->SetScale(0.00045f);
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
  erosionScale->SetScale(0.0008f);
  m_erosionNoise = erosionScale;

  // Peaks and Valleys noise - Local height variation, warped so hills and
  // mountain ranges fold instead of blobbing.
  auto pvBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto pvFractal = FastNoise::New<FastNoise::FractalFBm>();
  pvFractal->SetSource(pvBase);
  pvFractal->SetOctaveCount(6);
  pvFractal->SetLacunarity(2.0f);
  pvFractal->SetGain(0.5f);

  auto pvWarp = FastNoise::New<FastNoise::DomainWarpGradient>();
  pvWarp->SetSource(pvFractal);
  pvWarp->SetWarpAmplitude(0.4f);
  pvWarp->SetWarpFrequency(0.5f);

  auto pvScale = FastNoise::New<FastNoise::DomainScale>();
  pvScale->SetSource(pvWarp);
  pvScale->SetScale(0.0045f);
  m_peaksValleysNoise = pvScale;

  // Ridge noise - Sharp mountain peaks, warped into curved ranges
  auto ridgeBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto ridgeFractal = FastNoise::New<FastNoise::FractalRidged>();
  ridgeFractal->SetSource(ridgeBase);
  ridgeFractal->SetOctaveCount(4);
  ridgeFractal->SetLacunarity(2.0f);
  ridgeFractal->SetGain(0.5f);

  auto ridgeWarp = FastNoise::New<FastNoise::DomainWarpGradient>();
  ridgeWarp->SetSource(ridgeFractal);
  ridgeWarp->SetWarpAmplitude(0.3f);
  ridgeWarp->SetWarpFrequency(0.5f);

  auto ridgeScale = FastNoise::New<FastNoise::DomainScale>();
  ridgeScale->SetSource(ridgeWarp);
  ridgeScale->SetScale(0.002f);
  m_ridgeNoise = ridgeScale;

  auto surface3DBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto surface3DFractal = FastNoise::New<FastNoise::FractalFBm>();
  surface3DFractal->SetSource(surface3DBase);
  surface3DFractal->SetOctaveCount(2);
  surface3DFractal->SetLacunarity(2.0f);
  surface3DFractal->SetGain(0.5f);
  auto surface3DScale = FastNoise::New<FastNoise::DomainScale>();
  surface3DScale->SetSource(surface3DFractal);
  surface3DScale->SetScale(0.015f);
  m_surface3DNoise = surface3DScale;
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
  tempScale->SetScale(0.0007f); // Reduced from 0.0015 → ~2× larger temperature zones
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
  humidScale->SetScale(0.0009f); // Reduced from 0.002 → ~2× larger humidity zones
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
  weirdScale->SetScale(0.0015f); // Reduced from 0.003 → larger weirdness patches
  m_weirdnessNoise = weirdScale;

  auto riverBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto riverFractal = FastNoise::New<FastNoise::FractalFBm>();
  riverFractal->SetSource(riverBase);
  riverFractal->SetOctaveCount(2);
  auto riverScale = FastNoise::New<FastNoise::DomainScale>();
  riverScale->SetSource(riverFractal);
  riverScale->SetScale(0.003f);
  m_riverNoise = riverScale;
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

  // Spaghetti caves: the same ridged graph is sampled with two independent
  // seeds and the narrow fields are ANDed during voxel generation.
  auto spaghettiBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto spaghettiFractal = FastNoise::New<FastNoise::FractalRidged>();
  spaghettiFractal->SetSource(spaghettiBase);
  spaghettiFractal->SetOctaveCount(1);
  spaghettiFractal->SetLacunarity(2.0f);
  spaghettiFractal->SetGain(0.5f);

  auto spaghettiScale = FastNoise::New<FastNoise::DomainScale>();
  spaghettiScale->SetSource(spaghettiFractal);
  spaghettiScale->SetScale(0.028f);
  m_spaghettiNoise = spaghettiScale;

  // Low-frequency field for broad rooms in the deep underground.
  auto cavernBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto cavernFractal = FastNoise::New<FastNoise::FractalFBm>();
  cavernFractal->SetSource(cavernBase);
  cavernFractal->SetOctaveCount(1);
  cavernFractal->SetLacunarity(2.0f);
  cavernFractal->SetGain(0.5f);

  auto cavernScale = FastNoise::New<FastNoise::DomainScale>();
  cavernScale->SetSource(cavernFractal);
  cavernScale->SetScale(0.008f);
  m_cavernNoise = cavernScale;

  // Low-frequency 2D regions select dry caves, water aquifers, and their
  // stable local water table. Sampling in world space avoids fluid seams.
  auto aquiferBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto aquiferFractal = FastNoise::New<FastNoise::FractalFBm>();
  aquiferFractal->SetSource(aquiferBase);
  aquiferFractal->SetOctaveCount(2);
  aquiferFractal->SetLacunarity(2.0f);
  aquiferFractal->SetGain(0.5f);
  auto aquiferScale = FastNoise::New<FastNoise::DomainScale>();
  aquiferScale->SetSource(aquiferFractal);
  aquiferScale->SetScale(0.006f);
  m_aquiferNoise = aquiferScale;
}

void TerrainGenerator::setupVegetationNoise()
{
  // Local tree placement noise — high frequency, determines per-column tree candidacy
  auto treeBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto treeFractal = FastNoise::New<FastNoise::FractalFBm>();
  treeFractal->SetSource(treeBase);
  treeFractal->SetOctaveCount(2);
  treeFractal->SetLacunarity(2.0f);
  treeFractal->SetGain(0.5f);

  auto treeScale = FastNoise::New<FastNoise::DomainScale>();
  treeScale->SetSource(treeFractal);
  treeScale->SetScale(0.05f);
  m_treeNoise = treeScale;

  // Forest density noise — low frequency, creates large-scale forest patches and clearings.
  // Positive values => forested area; negative or near-zero => open/clearing.
  // Scale ~0.004 gives forest clusters roughly 100-300 blocks wide.
  auto forestBase = FastNoise::New<FastNoise::OpenSimplex2>();
  auto forestFractal = FastNoise::New<FastNoise::FractalFBm>();
  forestFractal->SetSource(forestBase);
  forestFractal->SetOctaveCount(3);
  forestFractal->SetLacunarity(2.0f);
  forestFractal->SetGain(0.5f);

  auto forestScale = FastNoise::New<FastNoise::DomainScale>();
  forestScale->SetSource(forestFractal);
  forestScale->SetScale(0.004f);
  m_forestDensityNoise = forestScale;
}

void TerrainGenerator::setupOres()
{
  using D = OreDistribution;
  // type, deep variant, Y range, vein size/count, seed, distribution,
  // mountain-only, badlands bonus.
  m_ores.push_back({COAL_ORE, DEEPSLATE_COAL_ORE, 32, 192, 17, 18, 10000,
                    D::High, false, 0});
  m_ores.push_back({IRON_ORE, DEEPSLATE_IRON_ORE, 4, 144, 10, 20, 11000,
                    D::Mid, false, 0});
  m_ores.push_back({COPPER_ORE, DEEPSLATE_COPPER_ORE, 20, 112, 11, 15, 12000,
                    D::Mid, false, 0});
  m_ores.push_back({GOLD_ORE, DEEPSLATE_GOLD_ORE, 4, 48, 9, 4, 13000,
                    D::Deep, false, 8});
  m_ores.push_back({LAPIS_ORE, DEEPSLATE_LAPIS_ORE, 4, 56, 7, 3, 14000,
                    D::Mid, false, 0});
  m_ores.push_back({REDSTONE_ORE, DEEPSLATE_REDSTONE_ORE, 4, 32, 8, 8, 15000,
                    D::Deep, false, 0});
  m_ores.push_back({DIAMOND_ORE, DEEPSLATE_DIAMOND_ORE, 4, 24, 8, 3, 16000,
                    D::Deep, false, 0});
  m_ores.push_back({EMERALD_ORE, DEEPSLATE_EMERALD_ORE, 4, 112, 4, 5, 17000,
                    D::Uniform, true, 0});
}

// =============================================
// THREAD-LOCAL STORAGE FOR OPTIMIZATION
// =============================================

// Height clamping
static constexpr int HEIGHT_CEILING_MARGIN = 16; // Reserved space above max terrain

// Extended sampling window: core chunk sits at [6,21] inside a 28x28 grid.
// The 6-wide halo serves two purposes:
//  - thermal erosion is deterministic for cells [2,25] (their four neighbors
//    [1,26] are all processed by the erosion loop, and give-decisions only
//    read sampled values [0,27]) — covers core, border strips, and the
//    vegetation candidate ring;
//  - vegetation canopies reaching up to MAX_TREE_RADIUS=4 into this chunk
//    from neighbor chunks can be evaluated from identical data.
static constexpr int EXT_CORE_OFFSET = 2 * worldgen::maxFeatureRadius + 4;
static constexpr int EXT_SIZE = CHUNK_SIZE + 2 * EXT_CORE_OFFSET;
static constexpr int UNDERGROUND_NOISE_MAX_Y = 96;
static constexpr int LARGE_CAVERN_MAX_Y = 48;
static constexpr int UNDERGROUND_COARSE_STEP = 2;
static constexpr int UNDERGROUND_COARSE_BORDER = 2;
static constexpr int UNDERGROUND_COARSE_XZ =
    (CHUNK_SIZE + 2 * UNDERGROUND_COARSE_BORDER) /
        UNDERGROUND_COARSE_STEP +
    1;
static constexpr int UNDERGROUND_COARSE_Y =
    UNDERGROUND_NOISE_MAX_Y / UNDERGROUND_COARSE_STEP + 1;
static constexpr int UNDERGROUND_COARSE_VOLUME =
    UNDERGROUND_COARSE_XZ * UNDERGROUND_COARSE_Y *
    UNDERGROUND_COARSE_XZ;

struct GenBuffers
{
  // Base noise buffers (28x28 to support deterministic erosion over chunk boundaries)
  std::array<float, EXT_SIZE * EXT_SIZE> continental;
  std::array<float, EXT_SIZE * EXT_SIZE> erosion;
  std::array<float, EXT_SIZE * EXT_SIZE> peaksValleys;
  std::array<float, EXT_SIZE * EXT_SIZE> ridge;
  std::array<float, EXT_SIZE * EXT_SIZE> temperature;
  std::array<float, EXT_SIZE * EXT_SIZE> humidity;
  std::array<float, EXT_SIZE * EXT_SIZE> weirdness;
  std::array<float, EXT_SIZE * EXT_SIZE> river;
  std::array<float, EXT_SIZE * EXT_SIZE> aquifer;

  // Extended heightmap for erosion (28x28)
  std::array<float, EXT_SIZE * EXT_SIZE> extendedHeightMap;

  // Per-column derived data over the extended window (heights are post-erosion,
  // pre-cave). Biome colors feed the 5x5 smoothing blur for the core chunk.
  std::array<BiomeType, EXT_SIZE * EXT_SIZE> extBiomes;
  std::array<BiomeType, EXT_SIZE * EXT_SIZE> extSurfaceBiomes;
  std::array<int, EXT_SIZE * EXT_SIZE> extHeights;
  std::array<uint32_t, EXT_SIZE * EXT_SIZE> extGrassColors;
  std::array<uint32_t, EXT_SIZE * EXT_SIZE> extFoliageColors;

  // 3D noise is generated separately for the 16x16 core and one-column border strips.
  std::array<float, CHUNK_VOLUME> cave;
  std::array<float, CHUNK_VOLUME> ravine;
  // The additional underground fields use an aligned half-resolution grid.
  // A two-block halo makes the same samples usable by core and border voxels.
  std::array<float, UNDERGROUND_COARSE_VOLUME> spaghettiA;
  std::array<float, UNDERGROUND_COARSE_VOLUME> spaghettiB;
  std::array<float, UNDERGROUND_COARSE_VOLUME> cavern;
  std::array<float, CHUNK_VOLUME> surface3D;

  // Border generation buffers (per-strip: CHUNK_SIZE columns)
  // 3D noise buffers for cave/ravine per strip (CHUNK_SIZE x CHUNK_HEIGHT x 1)
  std::array<float, CHUNK_SIZE * CHUNK_HEIGHT> borderCave;
  std::array<float, CHUNK_SIZE * CHUNK_HEIGHT> borderRavine;
  std::array<float, CHUNK_SIZE * CHUNK_HEIGHT> borderSurface3D;

  // Reusable buffers for vegetation generation (extended window, covers the
  // cross-chunk candidate ring)
  std::array<float, EXT_SIZE * EXT_SIZE> treeNoiseResults;
  std::array<float, EXT_SIZE * EXT_SIZE> forestDensityResults;

  // Reusable buffer for erosion calculation
  std::array<float, EXT_SIZE * EXT_SIZE> erosionTempMap;

  // Persistent generator to avoid expensive setup every chunk
  std::unique_ptr<TerrainGenerator> generator;
};

static thread_local GenBuffers s_genBuffers;

// Dedicated scratch for biome region queries that runs on threads which do
// not provide their own scratch. Kept separate from GenBuffers so chunk
// generation workers never retain region-query capacity.
static thread_local TerrainGenerator::BiomeRegionScratch s_biomeRegionScratch;

void TerrainGenerator::BiomeRegionScratch::trimOversizedCapacity()
{
  // Releases per-field capacity above the scratch's retention bound (the
  // owner-set retainedPointsCap, clamped to the dense-path absolute). The
  // default bound keeps only tiled-path capacity; owners of a single
  // dedicated scratch may retain dense capacity for churn-free reuse.
  // getBiomeRegion() calls this on every exit through an unconditional
  // guard - after successful AND cancelled/failed attempts. Call it
  // manually only when mutating a scratch outside getBiomeRegion().
  const size_t keep = std::min(retainedPointsCap, kMaxDenseDomainPoints);
  for (std::vector<float> *field : {&temperature, &humidity, &weirdness, &river,
                                    &continental, &erosion, &peaksValleys,
                                    &ridge, &height, &erosionTemp})
  {
    if (field->capacity() > keep)
      std::vector<float>().swap(*field);
  }
}

namespace
{

/// Max absolute height delta to the 4 von Neumann neighbors on the extended
/// height grid. Used for rock exposure on steep slopes.
inline float columnSlopeAt(const int *heights, int size, int idx)
{
  const float h = static_cast<float>(heights[idx]);
  float s = std::abs(h - static_cast<float>(heights[idx - 1]));
  s = std::max(s, std::abs(h - static_cast<float>(heights[idx + 1])));
  s = std::max(s, std::abs(h - static_cast<float>(heights[idx - size])));
  s = std::max(s, std::abs(h - static_cast<float>(heights[idx + size])));
  return s;
}

inline bool shouldCarveCave(int worldY, int terrainHeight, float cave,
                           float ravine, float spaghettiA, float spaghettiB,
                           float cavern, bool hasUndergroundNoise)
{
  const float heightRatio =
      std::clamp(static_cast<float>(worldY - TerrainGenerator::SEA_LEVEL) /
                     64.0f,
                 0.0f, 1.0f);
  const float cheeseThreshold = 0.62f + heightRatio * 0.35f;
  const float ravineThreshold = 0.81f + heightRatio * 0.15f;
  if (cave > cheeseThreshold || ravine > ravineThreshold)
    return true;

  const int cover = terrainHeight - worldY;
  if (!hasUndergroundNoise || cover < 7)
    return false;

  const float depthFactor =
      1.0f - std::clamp(static_cast<float>(worldY) /
                            static_cast<float>(UNDERGROUND_NOISE_MAX_Y),
                        0.0f, 1.0f);
  const float spaghettiThreshold = 0.76f - depthFactor * 0.06f;
  if (spaghettiA > spaghettiThreshold && spaghettiB > spaghettiThreshold)
    return true;

  if (worldY < LARGE_CAVERN_MAX_Y && cover >= 10)
  {
    const float ceilingFade =
        std::clamp(static_cast<float>(LARGE_CAVERN_MAX_Y - worldY) / 16.0f,
                   0.0f, 1.0f);
    const float cavernThreshold = std::lerp(0.74f, 0.60f, ceilingFade);
    if (cavern > cavernThreshold)
      return true;
  }

  return false;
}

inline float sampleCoarseUnderground(const float *values, int localX,
                                     int worldY, int localZ)
{
  const int x = std::clamp(
      (localX + UNDERGROUND_COARSE_BORDER) / UNDERGROUND_COARSE_STEP, 0,
      UNDERGROUND_COARSE_XZ - 1);
  const int y = std::clamp(worldY / UNDERGROUND_COARSE_STEP, 0,
                           UNDERGROUND_COARSE_Y - 1);
  const int z = std::clamp(
      (localZ + UNDERGROUND_COARSE_BORDER) / UNDERGROUND_COARSE_STEP, 0,
      UNDERGROUND_COARSE_XZ - 1);
  return values[z * UNDERGROUND_COARSE_Y * UNDERGROUND_COARSE_XZ +
                y * UNDERGROUND_COARSE_XZ + x];
}

inline bool isMountainBiome(BiomeType biome)
{
  return biome == BIOME_MOUNTAINS || biome == BIOME_SNOWY_MOUNTAINS ||
         biome == BIOME_GLACIER;
}

inline TextureType caveFillAt(int worldY, int terrainHeight, BiomeType biome,
                              float aquifer)
{
  // A global deep lava table creates lakes without ever overlapping water.
  if (worldY <= 11)
    return TextureType::LAVA;

  // Volcanic columns may contain isolated, deeper-than-surface lava pockets.
  if (biome == BIOME_VOLCANIC && worldY <= 24 && aquifer < -0.58f)
    return TextureType::LAVA;

  const int cover = terrainHeight - worldY;
  const int localWaterLevel =
      std::clamp(20 + static_cast<int>((aquifer + 1.0f) * 8.0f), 20, 36);
  if (aquifer > 0.10f && cover >= 10 && worldY <= localWaterLevel)
    return TextureType::WATER;
  return TextureType::AIR;
}

} // namespace

TerrainGenerator &TerrainGenerator::getThreadLocal(int seed)
{
  if (!s_genBuffers.generator || s_genBuffers.generator->getSeed() != seed)
  {
    s_genBuffers.generator = std::make_unique<TerrainGenerator>(seed);
  }
  return *s_genBuffers.generator;
}

// =============================================
// CHUNK GENERATION
// =============================================

ChunkData TerrainGenerator::generateChunk(int chunkX, int chunkZ)
{
  // Owning convenience wrapper (tests/tools): allocate fresh storage and
  // generate into it. The streaming hot path uses generateChunkInto() with
  // caller-owned reusable storage instead.
  ChunkData chunkData;
  chunkData.voxels.resize(CHUNK_VOLUME);
  chunkData.borderVoxels.assign(kBorderVoxelCount, static_cast<uint8_t>(AIR));

  ChunkNeighborBorders compactBorders{};
  ChunkGenerationTarget target{
      .voxels = std::span<Voxel, CHUNK_VOLUME>(chunkData.voxels),
      .borders = &compactBorders,
      .biomes = chunkData.biomes,
      .heightMap = chunkData.heightMap,
      .grassColors = chunkData.grassColors,
      .foliageColors = chunkData.foliageColors};
  generateChunkInto(chunkX, chunkZ, target);

  // Expand the compact border output into the owning dense layout expected
  // by the test/tool API. Faces plus corner columns are the only cells the
  // generator ever writes; everything else stays AIR.
  auto expandFace = [&chunkData](const std::array<uint8_t, CHUNK_HEIGHT * CHUNK_SIZE> &face,
                                 int borderX, bool faceIsX)
  {
    for (int y = 0; y < static_cast<int>(CHUNK_HEIGHT); ++y)
      for (int t = 0; t < static_cast<int>(CHUNK_SIZE); ++t)
      {
        const uint8_t type = face[static_cast<size_t>(y) * CHUNK_SIZE + t];
        if (type == static_cast<uint8_t>(AIR))
          continue;
        const int lx = faceIsX ? borderX : t;
        const int lz = faceIsX ? t : borderX;
        chunkData.borderVoxels[(y + 1) * 18 * 18 + (lz + 1) * 18 + (lx + 1)] = type;
      }
  };
  expandFace(compactBorders.west, -1, true);
  expandFace(compactBorders.east, CHUNK_SIZE, true);
  expandFace(compactBorders.south, -1, false);
  expandFace(compactBorders.north, CHUNK_SIZE, false);
  const std::pair<int, int> corners[4] = {{-1, -1}, {CHUNK_SIZE, -1}, {-1, CHUNK_SIZE}, {CHUNK_SIZE, CHUNK_SIZE}};
  const std::array<uint8_t, CHUNK_HEIGHT> *cornerCols[4] = {
      &compactBorders.cornerSW, &compactBorders.cornerSE,
      &compactBorders.cornerNW, &compactBorders.cornerNE};
  for (size_t c = 0; c < 4; ++c)
    for (int y = 0; y < static_cast<int>(CHUNK_HEIGHT); ++y)
    {
      const uint8_t type = (*cornerCols[c])[static_cast<size_t>(y)];
      if (type == static_cast<uint8_t>(AIR))
        continue;
      chunkData.borderVoxels[(y + 1) * 18 * 18 + (corners[c].second + 1) * 18 + (corners[c].first + 1)] = type;
    }
  return chunkData;
}

void TerrainGenerator::generateChunkInto(int chunkX, int chunkZ,
                                         const ChunkGenerationTarget &target)
{
  using Clock = std::chrono::steady_clock;
  const auto totalStart =
      m_activeProfile ? Clock::now() : Clock::time_point{};

  // Reset semantics: the destination storage is reused across pooled
  // chunks, so fully re-initialize every field before generating. Nothing
  // from a previously generated chunk may leak into this one.
  std::fill(target.voxels.begin(), target.voxels.end(),
            Voxel{TextureType::AIR});
  target.borders->resetToAir(); // zero would be BEDROCK, not AIR
  std::fill(target.biomes.begin(), target.biomes.end(),
            static_cast<BiomeType>(0));
  std::fill(target.heightMap.begin(), target.heightMap.end(), 0);
  std::fill(target.grassColors.begin(), target.grassColors.end(), 0u);
  std::fill(target.foliageColors.begin(), target.foliageColors.end(), 0u);

  // Generate the main chunk data
  generateChunkBatch(target, chunkX, chunkZ);

  const auto borderStart = m_activeProfile ? Clock::now() : Clock::time_point{};
  generateChunkBorders(target, chunkX, chunkZ);
  const auto vegetationStart = m_activeProfile ? Clock::now() : Clock::time_point{};
  if (m_activeProfile)
    m_activeProfile->borderMs += std::chrono::duration<double, std::milli>(vegetationStart - borderStart).count();
  generateVegetation(target, chunkX, chunkZ);
  if (m_activeProfile)
  {
    m_activeProfile->vegetationMs +=
        std::chrono::duration<double, std::milli>(Clock::now() - vegetationStart)
            .count();
    m_activeProfile->totalMs +=
        std::chrono::duration<double, std::milli>(Clock::now() - totalStart)
            .count();
    ++m_activeProfile->chunks;
  }
}

ChunkData TerrainGenerator::generateChunkProfiled(
    int chunkX, int chunkZ, TerrainGenerationProfile &profile)
{
  TerrainGenerationProfile *previous = m_activeProfile;
  m_activeProfile = &profile;
  try
  {
    ChunkData result = generateChunk(chunkX, chunkZ);
    m_activeProfile = previous;
    return result;
  }
  catch (...)
  {
    m_activeProfile = previous;
    throw;
  }
}

void TerrainGenerator::sampleTerrainColumnFields(
    const TerrainColumnBuffers &buffers,
    int extStartX, int extStartZ,
    int extWidth, int extHeight,
    float step) const
{
  assert(extWidth > 0);
  assert(extHeight > 0);
  assert(buffers.continental != nullptr);
  assert(buffers.erosion != nullptr);
  assert(buffers.peaksValleys != nullptr);
  assert(buffers.ridge != nullptr);
  assert(buffers.temperature != nullptr);
  assert(buffers.humidity != nullptr);
  assert(buffers.weirdness != nullptr);
  assert(buffers.river != nullptr);

  using Clock = std::chrono::steady_clock;
  const auto noise2DStart =
      m_activeProfile ? Clock::now() : Clock::time_point{};

  m_continentalNoise->GenUniformGrid2D(buffers.continental, extStartX, extStartZ,
                                       extWidth, extHeight, step, m_seed);
  m_erosionNoise->GenUniformGrid2D(buffers.erosion, extStartX, extStartZ,
                                   extWidth, extHeight, step, m_seed + 1000);
  m_peaksValleysNoise->GenUniformGrid2D(buffers.peaksValleys, extStartX, extStartZ,
                                        extWidth, extHeight, step, m_seed + 2000);
  m_ridgeNoise->GenUniformGrid2D(buffers.ridge, extStartX, extStartZ,
                                 extWidth, extHeight, step, m_seed + 3000);
  m_temperatureNoise->GenUniformGrid2D(buffers.temperature, extStartX, extStartZ,
                                       extWidth, extHeight, step, m_seed + 6000);
  m_humidityNoise->GenUniformGrid2D(buffers.humidity, extStartX, extStartZ,
                                    extWidth, extHeight, step, m_seed + 7000);
  m_weirdnessNoise->GenUniformGrid2D(buffers.weirdness, extStartX, extStartZ,
                                     extWidth, extHeight, step, m_seed + 8000);
  m_riverNoise->GenUniformGrid2D(buffers.river, extStartX, extStartZ,
                                 extWidth, extHeight, step, m_seed + 9000);

  if (m_activeProfile)
    m_activeProfile->noise2DMs +=
        std::chrono::duration<double, std::milli>(Clock::now() - noise2DStart)
            .count();
}

void TerrainGenerator::calculateTerrainHeights(
    const TerrainColumnBuffers &buffers,
    int extWidth, int extHeight) const
{
  assert(extWidth > 0);
  assert(extHeight > 0);
  assert(buffers.heightMap != nullptr);
  assert(buffers.continental != nullptr);
  assert(buffers.erosion != nullptr);
  assert(buffers.peaksValleys != nullptr);
  assert(buffers.ridge != nullptr);
  assert(buffers.river != nullptr);
  assert(buffers.weirdness != nullptr);

  const size_t totalPoints = static_cast<size_t>(extWidth) * static_cast<size_t>(extHeight);
  for (size_t i = 0; i < totalPoints; ++i)
  {
    buffers.heightMap[i] = calculateHeightFloat(
        buffers.continental[i], buffers.erosion[i],
        buffers.peaksValleys[i], buffers.ridge[i],
        buffers.river[i], buffers.weirdness[i], buffers.temperature[i], buffers.humidity[i]);
  }
}

BiomeType TerrainGenerator::evaluateBiomeAt(
    const TerrainColumnBuffers &buffers,
    int extIndex) const
{
  assert(buffers.continental != nullptr);
  assert(buffers.temperature != nullptr);
  assert(buffers.humidity != nullptr);
  assert(buffers.weirdness != nullptr);
  assert(buffers.river != nullptr);
  assert(buffers.erosion != nullptr);
  assert(buffers.peaksValleys != nullptr);
  assert(buffers.heightMap != nullptr);

  const float continental = std::clamp(buffers.continental[extIndex], -1.0f, 1.0f);
  const float temperature = std::clamp(buffers.temperature[extIndex], -1.0f, 1.0f);
  const float humidity = std::clamp(buffers.humidity[extIndex], -1.0f, 1.0f);
  const float weirdness = std::clamp(buffers.weirdness[extIndex], -1.0f, 1.0f);
  const float riverVal = buffers.river[extIndex];
  const float erosion = std::clamp(buffers.erosion[extIndex], -1.0f, 1.0f);
  const float pv = std::clamp(buffers.peaksValleys[extIndex], -1.0f, 1.0f);

  const int height = std::clamp(static_cast<int>(std::round(buffers.heightMap[extIndex])), 1, static_cast<int>(CHUNK_HEIGHT - HEIGHT_CEILING_MARGIN));

  return determineBiome(temperature, humidity, weirdness,
                        continental, erosion, pv, riverVal, height);
}

void TerrainGenerator::generateChunkBatch(const ChunkGenerationTarget &chunkData, int chunkX,
                                          int chunkZ)
{
  using Clock = std::chrono::steady_clock;
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
  float *riverResults = s_genBuffers.river.data();
  float *aquiferResults = s_genBuffers.aquifer.data();

  // 3D Noise buffers
  float *caveResults = s_genBuffers.cave.data();
  float *ravineResults = s_genBuffers.ravine.data();
  float *spaghettiAResults = s_genBuffers.spaghettiA.data();
  float *spaghettiBResults = s_genBuffers.spaghettiB.data();
  float *cavernResults = s_genBuffers.cavern.data();
  float *surface3DResults = s_genBuffers.surface3D.data();

  float worldXf = static_cast<float>(chunkX) + NOISE_OFFSET;
  float worldZf = static_cast<float>(chunkZ) + NOISE_OFFSET;

  // Generate terrain noise for the 28x28 extended area.
  float extendedWorldXf = worldXf - static_cast<float>(EXT_CORE_OFFSET);
  float extendedWorldZf = worldZf - static_cast<float>(EXT_CORE_OFFSET);
  const int EXTENDED_SIZE = EXT_SIZE;

  TerrainColumnBuffers buffers{
      .continental = continentalResults,
      .erosion = erosionResults,
      .peaksValleys = peaksValleysResults,
      .ridge = ridgeResults,
      .temperature = temperatureResults,
      .humidity = humidityResults,
      .weirdness = weirdnessResults,
      .river = riverResults,
      .heightMap = s_genBuffers.extendedHeightMap.data(),
      .erosionTemp = s_genBuffers.erosionTempMap.data()};

  sampleTerrainColumnFields(
      buffers,
      static_cast<int>(extendedWorldXf),
      static_cast<int>(extendedWorldZf),
      EXTENDED_SIZE, EXTENDED_SIZE, 1.0f);
  calculateTerrainHeights(buffers, EXTENDED_SIZE, EXTENDED_SIZE);
  applyCanonicalErosion(buffers.heightMap, EXTENDED_SIZE, EXTENDED_SIZE, buffers.erosionTemp);

  const auto aquiferStart =
      m_activeProfile ? Clock::now() : Clock::time_point{};
  m_aquiferNoise->GenUniformGrid2D(aquiferResults, extendedWorldXf,
                                   extendedWorldZf, EXTENDED_SIZE,
                                   EXTENDED_SIZE, 1.0f, m_seed + 10000);
  if (m_activeProfile)
    m_activeProfile->aquiferNoiseMs +=
        std::chrono::duration<double, std::milli>(Clock::now() - aquiferStart)
            .count();

  float *extHeightMap = s_genBuffers.extendedHeightMap.data();

  // Pass 1a: biomes, heights and raw biome colors over the whole extended
  // window (must precede 3D noise so we can bound cave sampling to
  // surface+margin). Border strips and the vegetation ring reuse this data.
  BiomeType *extBiomes = s_genBuffers.extBiomes.data();
  int *extHeights = s_genBuffers.extHeights.data();
  uint32_t *extGrass = s_genBuffers.extGrassColors.data();
  uint32_t *extFoliage = s_genBuffers.extFoliageColors.data();
  for (int extZ = 0; extZ < EXTENDED_SIZE; ++extZ)
  {
    for (int extX = 0; extX < EXTENDED_SIZE; ++extX)
    {
      int extIndex = extZ * EXTENDED_SIZE + extX;

      float weirdness = std::clamp(weirdnessResults[extIndex], -1.0f, 1.0f);
      float erosion = std::clamp(erosionResults[extIndex], -1.0f, 1.0f);

      BiomeType biome = evaluateBiomeAt(buffers, extIndex);
      int height = std::clamp(static_cast<int>(std::round(extHeightMap[extIndex])), 1, static_cast<int>(CHUNK_HEIGHT - HEIGHT_CEILING_MARGIN));

      // Only the water channel has a discrete final floor. Regional
      // terraces and pond transitions are already in the shared height field.
      if (biome == BIOME_RIVER || biome == BIOME_FROZEN_RIVER)
        height = SEA_LEVEL - 2;

      extBiomes[extIndex] = biome;
      extHeights[extIndex] = height;

      const BiomeConfig &cfg = getBiomeConfig(biome);
      extGrass[extIndex] = packColor(cfg.grassColor);
      extFoliage[extIndex] = packColor(cfg.foliageColor);
    }
  }

  // A compact stochastic transition band changes materials, not canonical
  // biome identity. The triangular offset kernel concentrates on the owner
  // and blends both sides of boundaries without a visible rectangular patch.
  for (int z = 4; z < EXT_SIZE - 4; ++z)
    for (int x = 4; x < EXT_SIZE - 4; ++x)
    {
      const int i = z * EXT_SIZE + x;
      const uint32_t hash = treeHash(chunkX + x - EXT_CORE_OFFSET, chunkZ + z - EXT_CORE_OFFSET, m_seed + 35000);
      const int dx = static_cast<int>(hash % 5u) + static_cast<int>((hash >> 8) % 5u) - 4;
      const int dz = static_cast<int>((hash >> 16) % 5u) + static_cast<int>((hash >> 24) % 5u) - 4;
      const auto other = extBiomes[(z + dz) * EXT_SIZE + x + dx];
      // Shore and snow policies stay with their own column.
      s_genBuffers.extSurfaceBiomes[i] = extHeights[i] > SEA_LEVEL + 2 &&
          getBiomeConfig(other).palette != worldgen::Palette::Marine ? other : extBiomes[i];
    }

  // Pass 1b: extract the 16x16 core. Biome grass/foliage colors are smoothed
  // with a 5x5 box blur over the extended window (deterministic across chunk
  // borders — the halo comes from the same shared noise).
  int maxSurfaceHeight = 1;
  for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ)
  {
    for (int localX = 0; localX < CHUNK_SIZE; ++localX)
    {
      int extZ = localZ + EXT_CORE_OFFSET;
      int extX = localX + EXT_CORE_OFFSET;
      int extIndex = extZ * EXTENDED_SIZE + extX;
      int localIndex = localZ * CHUNK_SIZE + localX;

      chunkData.biomes[localIndex] = extBiomes[extIndex];
      chunkData.heightMap[localIndex] = extHeights[extIndex];
      maxSurfaceHeight = std::max(maxSurfaceHeight, extHeights[extIndex]);

      uint32_t rSumG = 0, gSumG = 0, bSumG = 0;
      uint32_t rSumF = 0, gSumF = 0, bSumF = 0;
      for (int dz = -2; dz <= 2; ++dz)
      {
        for (int dx = -2; dx <= 2; ++dx)
        {
          int nIdx = (extZ + dz) * EXTENDED_SIZE + (extX + dx);
          uint32_t g = extGrass[nIdx];
          uint32_t f = extFoliage[nIdx];
          rSumG += g & 0xFFu;
          gSumG += (g >> 8) & 0xFFu;
          bSumG += (g >> 16) & 0xFFu;
          rSumF += f & 0xFFu;
          gSumF += (f >> 8) & 0xFFu;
          bSumF += (f >> 16) & 0xFFu;
        }
      }
      constexpr uint32_t N = 25;
      chunkData.grassColors[localIndex] =
          (rSumG / N) | ((gSumG / N) << 8) | ((bSumG / N) << 16) | (255u << 24);
      chunkData.foliageColors[localIndex] =
          (rSumF / N) | ((gSumF / N) << 8) | ((bSumF / N) << 16) | (255u << 24);
    }
  }

  // 3D cave/surface noise only up to surface+margin (not full 256).
  // Chunk-wide maxSurfaceHeight drives both noise and fill ends (see computeChunkYFillBounds).
  const ChunkYFillBounds yBounds =
	  computeChunkYFillBounds(maxSurfaceHeight, SEA_LEVEL, 16, CHUNK_HEIGHT);
  const int caveYMin = yBounds.yMin;
  const int caveYSize = yBounds.ySize;
  const int yNoiseEnd = yBounds.yNoiseEnd;
  const int yFillEnd = yBounds.yFillEnd;
  const auto base3DStart =
      m_activeProfile ? Clock::now() : Clock::time_point{};
  m_caveNoise->GenUniformGrid3D(caveResults, worldXf, static_cast<float>(caveYMin), worldZf,
                                CHUNK_SIZE, caveYSize, CHUNK_SIZE, 1.0f,
                                m_seed + 4000);
  m_ravineNoise->GenUniformGrid3D(ravineResults, worldXf, static_cast<float>(caveYMin), worldZf,
                                  CHUNK_SIZE, caveYSize, CHUNK_SIZE, 1.0f,
                                  m_seed + 5000);
  m_surface3DNoise->GenUniformGrid3D(surface3DResults, worldXf, static_cast<float>(caveYMin), worldZf,
                                     CHUNK_SIZE, caveYSize, CHUNK_SIZE, 1.0f,
                                     m_seed + 6000);
  if (m_activeProfile)
    m_activeProfile->base3DNoiseMs +=
        std::chrono::duration<double, std::milli>(Clock::now() - base3DStart)
            .count();
  const int undergroundNoiseEnd = std::min(yNoiseEnd, UNDERGROUND_NOISE_MAX_Y);
  if (undergroundNoiseEnd > 0)
  {
    // FastNoise multiplies grid indices by frequency. Dividing the aligned
    // start coordinate by the step therefore samples exact world positions
    // -2, 0, 2, ... and guarantees overlap between neighboring chunks.
    const int coarseStartX =
        (chunkX - UNDERGROUND_COARSE_BORDER) / UNDERGROUND_COARSE_STEP;
    const int coarseStartZ =
        (chunkZ - UNDERGROUND_COARSE_BORDER) / UNDERGROUND_COARSE_STEP;
    const auto spaghettiStart =
        m_activeProfile ? Clock::now() : Clock::time_point{};
    m_spaghettiNoise->GenUniformGrid3D(
        spaghettiAResults, coarseStartX, 0, coarseStartZ,
        UNDERGROUND_COARSE_XZ, UNDERGROUND_COARSE_Y, UNDERGROUND_COARSE_XZ,
        static_cast<float>(UNDERGROUND_COARSE_STEP), m_seed + 7000);
    m_spaghettiNoise->GenUniformGrid3D(
        spaghettiBResults, coarseStartX, 0, coarseStartZ,
        UNDERGROUND_COARSE_XZ, UNDERGROUND_COARSE_Y, UNDERGROUND_COARSE_XZ,
        static_cast<float>(UNDERGROUND_COARSE_STEP), m_seed + 8000);
    if (m_activeProfile)
      m_activeProfile->spaghettiNoiseMs +=
          std::chrono::duration<double, std::milli>(Clock::now() -
                                                    spaghettiStart)
              .count();
    const auto cavernStart =
        m_activeProfile ? Clock::now() : Clock::time_point{};
    m_cavernNoise->GenUniformGrid3D(
        cavernResults, coarseStartX, 0, coarseStartZ,
        UNDERGROUND_COARSE_XZ, UNDERGROUND_COARSE_Y, UNDERGROUND_COARSE_XZ,
        static_cast<float>(UNDERGROUND_COARSE_STEP), m_seed + 9000);
    if (m_activeProfile)
      m_activeProfile->cavernNoiseMs +=
          std::chrono::duration<double, std::milli>(Clock::now() - cavernStart)
              .count();
  }

  // Pass 2: Generate voxel columns
  const auto voxelFillStart =
      m_activeProfile ? Clock::now() : Clock::time_point{};
  for (int localZ = 0; localZ < CHUNK_SIZE; ++localZ)
  {
    for (int localX = 0; localX < CHUNK_SIZE; ++localX)
    {
      int colIndex = getColumnIndex(localX, localZ);
      int terrainHeight = chunkData.heightMap[colIndex];
      BiomeType biome = chunkData.biomes[colIndex];
      int extIndex = (localZ + EXT_CORE_OFFSET) * EXTENDED_SIZE + (localX + EXT_CORE_OFFSET);
      float temperature = std::clamp(temperatureResults[extIndex], -1.0f, 1.0f);

      // Max absolute height delta to the 4 neighbors (rock exposure on cliffs)
      float columnSlope = columnSlopeAt(extHeights, EXTENDED_SIZE, extIndex);

      // 3D surface perturbation amplitude (overhangs/cliffs) for this biome
      const float perturbAmp = getBiomeConfig(biome).surfacePerturbAmp;

      int actualMaxHeight = 0;
      for (int y = caveYMin; y < yFillEnd; ++y)
      {
        int voxelIndex = getVoxelIndex(localX, y, localZ);
        const bool inNoiseBand = (y >= caveYMin && y < yNoiseEnd);

        float caveVal = 0.f;
        float ravineVal = 0.f;
        float spaghettiAVal = 0.f;
        float spaghettiBVal = 0.f;
        float cavernVal = 0.f;
        float surface3DVal = 0.f;
        if (inNoiseBand)
        {
          int localY = y - caveYMin;
          int noiseIndex = (localZ * caveYSize * CHUNK_SIZE) + (localY * CHUNK_SIZE) + localX;
          caveVal = caveResults[noiseIndex];
          ravineVal = ravineResults[noiseIndex];
          surface3DVal = surface3DResults[noiseIndex];
        }
        const bool hasUndergroundNoise =
            y >= caveYMin && y < undergroundNoiseEnd;
        if (hasUndergroundNoise)
        {
          spaghettiAVal =
              sampleCoarseUnderground(spaghettiAResults, localX, y, localZ);
          spaghettiBVal =
              sampleCoarseUnderground(spaghettiBResults, localX, y, localZ);
          cavernVal =
              sampleCoarseUnderground(cavernResults, localX, y, localZ);
        }

        // 3D density: base density is distance below the heightmap surface
        float density = static_cast<float>(terrainHeight - y);

        // Perturb the surface near the heightmap boundary with a per-biome
        // amplitude (mountains: cliffs/overhangs; badlands: hoodoos; flat
        // biomes use 0 and stay pure heightmap).
        if (inNoiseBand && perturbAmp > 0.0f && density > -8.0f && density < 12.0f) {
           // Smooth blend: full effect at surface, fades to zero at edges
           float distToSurface = std::abs(density);
           float blend = std::max(0.0f, 1.0f - distToSurface / 12.0f);
           density += surface3DVal * perturbAmp * blend;
        }

        TextureType type;
        if (density >= 0.0f) {
            // Pass density so overhangs can have grass/dirt/stone correctly
            type = getVoxelTypeAt(chunkX + localX, y, chunkZ + localZ, terrainHeight, biome, temperature, columnSlope, density, s_genBuffers.extSurfaceBiomes[extIndex]);

            if (inNoiseBand && type != TextureType::BEDROCK && type != TextureType::WATER)
            {
              if (shouldCarveCave(y, terrainHeight, caveVal, ravineVal,
                                  spaghettiAVal, spaghettiBVal, cavernVal,
                                  hasUndergroundNoise))
              {
                type = caveFillAt(y, terrainHeight, biome,
                                  aquiferResults[extIndex]);
              }
            }
        } else {
            type = getVoxelTypeAt(chunkX + localX, y, chunkZ + localZ, terrainHeight, biome, temperature, columnSlope, density, s_genBuffers.extSurfaceBiomes[extIndex]);
        }

        if (type != TextureType::AIR && type != TextureType::WATER &&
            type != TextureType::LAVA) {
            actualMaxHeight = std::max(actualMaxHeight, y);
        }
        chunkData.voxels[voxelIndex].type = type;
      }
      chunkData.heightMap[colIndex] = actualMaxHeight;
    }
  }
  if (m_activeProfile)
    m_activeProfile->voxelFillMs +=
        std::chrono::duration<double, std::milli>(Clock::now() -
                                                  voxelFillStart)
            .count();

  // Ore generation pass (depth- and biome-aware random-walk blobs).
  const auto oreStart =
      m_activeProfile ? Clock::now() : Clock::time_point{};
  for (const auto &ore : m_ores)
  {
    int minY = std::max(0, ore.minHeight);
    int maxY = std::min(CHUNK_HEIGHT, ore.maxHeight);
    if (minY >= maxY)
      continue;

    const int candidates = ore.clustersPerChunk + ore.badlandsBonusClusters;
    for (int i = 0; i < candidates; ++i) {
      uint32_t h1 = treeHash(chunkX, chunkZ, m_seed + ore.seedOffset + i);
      uint32_t h2 = treeHash(chunkX, chunkZ,
                             m_seed + ore.seedOffset + 50000 + i * 31);
      int startX = h1 % CHUNK_SIZE;
      int startZ = (h1 >> 8) % CHUNK_SIZE;
      const BiomeType startBiome =
          chunkData.biomes[getColumnIndex(startX, startZ)];
      if (ore.mountainOnly && !isMountainBiome(startBiome))
        continue;
      if (i >= ore.clustersPerChunk && startBiome != BIOME_BADLANDS)
        continue;

      const float u1 = static_cast<float>((h1 >> 16) & 0xffffu) / 65535.0f;
      const float u2 = static_cast<float>(h2 & 0xffffu) / 65535.0f;
      float normalized = u1;
      switch (ore.distribution)
      {
      case OreDistribution::High:
        normalized = 1.0f - u1 * u2;
        break;
      case OreDistribution::Mid:
        normalized = (u1 + u2) * 0.5f;
        break;
      case OreDistribution::Deep:
        normalized = u1 * u2;
        break;
      case OreDistribution::Uniform:
        break;
      }
      int startY =
          minY + std::min(maxY - minY - 1,
                          static_cast<int>(normalized * (maxY - minY)));

      int x = startX, y = startY, z = startZ;
      for (int step = 0; step < ore.clusterSize; ++step) {
        if (x >= 0 && x < CHUNK_SIZE && y >= minY && y < maxY &&
            z >= 0 && z < CHUNK_SIZE) {
          int voxelIndex = getVoxelIndex(x, y, z);
          const auto host = static_cast<TextureType>(chunkData.voxels[voxelIndex].type);
          const BiomeType targetBiome =
              chunkData.biomes[getColumnIndex(x, z)];
          if (blockIsOreHost(host) &&
              (!ore.mountainOnly || isMountainBiome(targetBiome))) {
            chunkData.voxels[voxelIndex].type =
                (host == DEEPSLATE || host == TUFF)
                    ? ore.deepType
                    : ore.type;
          }
        }
        
        uint32_t stepHash = treeHash(chunkX * CHUNK_SIZE + x, chunkZ * CHUNK_SIZE + z, m_seed + step * 7919 + y * 1337);
        int dir = stepHash % 6;
        if (dir == 0) x++;
        else if (dir == 1) x--;
        else if (dir == 2 && y + 1 < maxY) y++;
        else if (dir == 3 && y - 1 >= minY) y--;
        else if (dir == 4) z++;
        else if (dir == 5) z--;
      }
    }
  }
  if (m_activeProfile)
    m_activeProfile->oreMs +=
        std::chrono::duration<double, std::milli>(Clock::now() - oreStart)
            .count();

  // Deterministic cube-based cave decoration. Candidates depend only on
  // world coordinates and seed, so generation order cannot affect results.
  const auto decorationStart =
      m_activeProfile ? Clock::now() : Clock::time_point{};
  for (int z = 0; z < CHUNK_SIZE; ++z)
  {
    for (int x = 0; x < CHUNK_SIZE; ++x)
    {
      const int column = getColumnIndex(x, z);
      const BiomeType biome = chunkData.biomes[column];
      const int maxY = std::min(80, chunkData.heightMap[column] - 7);
      const int worldX = chunkX + x;
      const int worldZ = chunkZ + z;
      for (int y = BEDROCK_LEVEL + 2; y < maxY; ++y)
      {
        const int index = getVoxelIndex(x, y, z);
        const TextureType current =
            static_cast<TextureType>(chunkData.voxels[index].type);

        if (current == AIR)
        {
          const TextureType above = static_cast<TextureType>(
              chunkData.voxels[getVoxelIndex(x, y + 1, z)].type);
          const TextureType below = static_cast<TextureType>(
              chunkData.voxels[getVoxelIndex(x, y - 1, z)].type);
          const uint32_t h =
              treeHash(worldX, worldZ, m_seed + 24000 + y * 977);

          if (blockIsOreHost(above) && (h & 63u) == 0u)
          {
            const int length = 1 + static_cast<int>((h >> 8) % 3u);
            for (int d = 0; d < length && y - d > BEDROCK_LEVEL; ++d)
            {
              const int dripIndex = getVoxelIndex(x, y - d, z);
              if (chunkData.voxels[dripIndex].type != AIR)
                break;
              chunkData.voxels[dripIndex].type = DRIPSTONE_BLOCK;
            }
          }
          else if (blockIsOreHost(below) && ((h >> 6) & 63u) == 0u)
          {
            const int length = 1 + static_cast<int>((h >> 14) % 3u);
            for (int d = 0; d < length && y + d < maxY; ++d)
            {
              const int dripIndex = getVoxelIndex(x, y + d, z);
              if (chunkData.voxels[dripIndex].type != AIR)
                break;
              chunkData.voxels[dripIndex].type = DRIPSTONE_BLOCK;
            }
          }
          else if (biome == BIOME_VOLCANIC && blockIsOreHost(below) &&
                   ((h >> 12) & 15u) == 0u)
          {
            chunkData.voxels[getVoxelIndex(x, y - 1, z)].type = MAGMA;
          }
        }
        else if (current == WATER)
        {
          const TextureType below = static_cast<TextureType>(
              chunkData.voxels[getVoxelIndex(x, y - 1, z)].type);
          const uint32_t h =
              treeHash(worldX, worldZ, m_seed + 25000 + y * 991);
          if (blockIsOreHost(below) && (h & 15u) == 0u)
            chunkData.voxels[getVoxelIndex(x, y - 1, z)].type = MOSS_BLOCK;
        }
      }
    }
  }
  if (m_activeProfile)
    m_activeProfile->decorationMs +=
        std::chrono::duration<double, std::milli>(Clock::now() -
                                                  decorationStart)
            .count();
}

// =============================================
// CONTINENTAL CONSTANTS
// =============================================
// Continental smoothstep thresholds: define where each terrain band begins/ends
// Each pair (lo, hi) is a transition zone in the 0..1 continental factor range.
static constexpr float CONT_OCEAN_LO = 0.25f;  // Ocean -> Beach transition start

namespace
{

enum class TerrainBand : uint8_t
{
  Ocean,
  Coast,
  Flatlands,
  Hills,
  Mountains
};

enum class ClimateBand : size_t
{
  Cold,
  Temperate,
  Hot,
  Count
};

enum class MoistureBand : size_t
{
  Arid,
  Moderate,
  Humid,
  Wet,
  Count
};

constexpr size_t CLIMATE_COUNT = static_cast<size_t>(ClimateBand::Count);
constexpr size_t MOISTURE_COUNT = static_cast<size_t>(MoistureBand::Count);
using ClimateBiomeMatrix =
    std::array<std::array<BiomeType, MOISTURE_COUNT>, CLIMATE_COUNT>;

constexpr ClimateBiomeMatrix FLATLAND_BIOMES = {{
    {{BIOME_SNOWY_TUNDRA, BIOME_SNOWY_TUNDRA, BIOME_SNOWY_TUNDRA,
      BIOME_SNOWY_TUNDRA}},
    {{BIOME_PLAINS, BIOME_PLAINS, BIOME_PLAINS, BIOME_SWAMP}},
    {{BIOME_DESERT, BIOME_SAVANNA, BIOME_SAVANNA, BIOME_SAVANNA}},
}};

constexpr ClimateBiomeMatrix HILL_BIOMES = {{
    {{BIOME_SNOWY_TAIGA, BIOME_SNOWY_TAIGA, BIOME_SNOWY_TAIGA,
      BIOME_SNOWY_TAIGA}},
    {{BIOME_FOREST, BIOME_FOREST, BIOME_DARK_FOREST, BIOME_DARK_FOREST}},
    {{BIOME_BADLANDS, BIOME_SAVANNA, BIOME_JUNGLE, BIOME_JUNGLE}},
}};

ClimateBand climateBandFor(float temperature)
{
  if (temperature < 0.30f)
    return ClimateBand::Cold;
  if (temperature > 0.70f)
    return ClimateBand::Hot;
  return ClimateBand::Temperate;
}

MoistureBand moistureBandFor(float humidity)
{
  if (humidity < 0.30f)
    return MoistureBand::Arid;
  if (humidity < 0.60f)
    return MoistureBand::Moderate;
  if (humidity < 0.65f)
    return MoistureBand::Humid;
  return MoistureBand::Wet;
}

BiomeType matrixBiome(const ClimateBiomeMatrix &matrix, ClimateBand climate,
                      MoistureBand moisture)
{
  return matrix[static_cast<size_t>(climate)][static_cast<size_t>(moisture)];
}

} // namespace

// =============================================
// BIOME DETERMINATION
// =============================================

BiomeType TerrainGenerator::determineBiome(float temperature, float humidity,
                                           float weirdness, float continental,
                                           float erosion, float pv, float riverVal, int height) const
{
  const float temp = std::clamp((temperature + 1.0f) * 0.5f, 0.0f, 1.0f);
  const float humid = std::clamp((humidity + 1.0f) * 0.5f, 0.0f, 1.0f);
  const float weird = std::clamp((weirdness + 1.0f) * 0.5f, 0.0f, 1.0f);
  const float contFactor = std::clamp((continental + 1.0f) * 0.5f, 0.0f, 1.0f);
  const float erosionFactor = std::clamp((erosion + 1.0f) * 0.5f, 0.0f, 1.0f);
  const float reliefFactor = std::abs(std::clamp(pv, -1.0f, 1.0f));

  const auto regionalRelief = worldgen::reliefWeights(erosion, weirdness);
  // Sub-sea columns on fully blended land are inland ponds, not ocean; only
  // the shelf, where the ocean->land blend is still active, may classify as
  // ocean or coast. Same blend endpoints as calculateHeightFloat().
  const bool shelf = continental < -0.18f;
  const TerrainBand terrainBand = height < SEA_LEVEL - 2 && shelf
      ? TerrainBand::Ocean : height <= SEA_LEVEL + 2 && shelf && contFactor < 0.53f
      ? TerrainBand::Coast : (height > 150 || (regionalRelief[2] > 0.42f && height > 96)) ? TerrainBand::Mountains
      : (erosionFactor > 0.39f && erosionFactor < 0.72f && height > SEA_LEVEL + 9)
      ? TerrainBand::Hills : TerrainBand::Flatlands;
  const ClimateBand climateBand = climateBandFor(temp);
  const MoistureBand moistureBand = moistureBandFor(humid);

  // Weirdness remains the primary rare-variant driver. Erosion and local
  // relief only perturb it slightly so the multi-noise refactor preserves the
  // established biome distribution and pinned feature locations.
  const float variantScore = std::clamp(
      weird + (0.5f - erosionFactor) * 0.06f +
          (reliefFactor - 0.5f) * 0.04f,
      0.0f, 1.0f);

  // Rivers only form in carved valleys that reach the water band. The width
  // formula is shared with calculateHeightFloat().
  const float dynamicWidth = worldgen::riverWidth(weirdness);
  const float waterWidth = dynamicWidth * 0.4f;

  if (std::abs(riverVal) < waterWidth && height <= SEA_LEVEL + 1)
  {
    if (terrainBand == TerrainBand::Ocean)
      return climateBand == ClimateBand::Cold ? BIOME_FROZEN_OCEAN : BIOME_OCEAN;
    return temp < 0.32f ? BIOME_FROZEN_RIVER : BIOME_RIVER;
  }

  switch (terrainBand)
  {
  case TerrainBand::Ocean:
    if (climateBand == ClimateBand::Cold)
      return BIOME_FROZEN_OCEAN;
    if (climateBand == ClimateBand::Hot && contFactor > CONT_OCEAN_LO &&
        humid > 0.55f && variantScore > 0.58f)
      return BIOME_CORAL_REEF;
    return BIOME_OCEAN;
  case TerrainBand::Coast:
    if (climateBand == ClimateBand::Cold)
    {
      if (erosionFactor < 0.35f && reliefFactor > 0.45f)
        return BIOME_GLACIER;
      return BIOME_SNOWY_TUNDRA;
    }
    return BIOME_BEACH;
  case TerrainBand::Flatlands:
    // Preserve the established ice-spike pin exactly; other Phase 3 rare
    // variants use the multi-noise score below.
    if (climateBand == ClimateBand::Cold && weird > 0.80f)
      return BIOME_ICE_SPIKES;
    if (climateBand == ClimateBand::Temperate)
    {
      if (moistureBand == MoistureBand::Wet)
      {
        if (weird > 0.84f)
          return BIOME_MUSHROOM_FIELDS;
        if (temp > 0.52f && erosionFactor < 0.55f)
          return BIOME_MANGROVE_SWAMP;
        return BIOME_SWAMP;
      }
      if (moistureBand == MoistureBand::Arid)
      {
        if (erosionFactor > 0.55f || variantScore > 0.55f)
          return BIOME_MOOR;
        return BIOME_PLAINS;
      }
      if (variantScore > 0.62f)
        return BIOME_FLOWER_MEADOW;
      return BIOME_PLAINS;
    }
    if (climateBand == ClimateBand::Hot && humid < 0.37f)
    {
      if (weird > 0.72f && erosionFactor < 0.55f)
        return BIOME_OASIS;
      return BIOME_DESERT;
    }
    return matrixBiome(FLATLAND_BIOMES, climateBand, moistureBand);
  case TerrainBand::Hills:
    if (climateBand == ClimateBand::Cold)
    {
      if (erosionFactor < 0.40f && reliefFactor > 0.45f)
        return BIOME_GLACIER;
      return BIOME_SNOWY_TAIGA;
    }
    if (climateBand == ClimateBand::Temperate)
    {
      if (moistureBand == MoistureBand::Wet ||
          moistureBand == MoistureBand::Humid)
      {
        if (weird > 0.84f)
          return BIOME_MUSHROOM_FIELDS;
        if (erosionFactor < 0.48f)
          return BIOME_REDWOOD_FOREST;
        return BIOME_DARK_FOREST;
      }
      if (moistureBand == MoistureBand::Arid)
        return weird > 0.50f ? BIOME_AUTUMN_FOREST : BIOME_FOREST;
      if (weird > 0.72f)
        return BIOME_CHERRY_GROVE;
      if (variantScore > 0.60f)
        return BIOME_BIRCH_FOREST;
      if (erosionFactor > 0.70f)
        return BIOME_AUTUMN_FOREST;
      return BIOME_FOREST;
    }
    if (climateBand == ClimateBand::Hot)
    {
      if (humid < 0.44f)
        return BIOME_BADLANDS;
      if (humid > 0.50f)
        return weird > 0.52f ? BIOME_BAMBOO_JUNGLE : BIOME_JUNGLE;
      return BIOME_SAVANNA;
    }
    return matrixBiome(HILL_BIOMES, climateBand, moistureBand);
  case TerrainBand::Mountains:
    if (temp < 0.30f && erosionFactor < 0.40f)
      return BIOME_GLACIER;
    if (temp < 0.40f)
      return BIOME_SNOWY_MOUNTAINS;
    if (climateBand == ClimateBand::Hot && weird > 0.68f &&
        erosionFactor < 0.55f)
      return BIOME_VOLCANIC;
    return BIOME_MOUNTAINS;
  }

  // Defensive fallback for compilers that do not prove the enum exhaustive.
  return BIOME_MOUNTAINS;
}

TerrainGenerator::TerrainSample TerrainGenerator::getTerrainSample(int worldX, int worldZ) const
{
  constexpr int HALO = 2;
  constexpr int EXT_W = 1 + 2 * HALO;
  constexpr int EXT_H = 1 + 2 * HALO;
  constexpr int EXT_COUNT = EXT_W * EXT_H;

  std::array<float, EXT_COUNT> cont;
  std::array<float, EXT_COUNT> erosion;
  std::array<float, EXT_COUNT> pv;
  std::array<float, EXT_COUNT> ridge;
  std::array<float, EXT_COUNT> temp;
  std::array<float, EXT_COUNT> humid;
  std::array<float, EXT_COUNT> weird;
  std::array<float, EXT_COUNT> river;
  std::array<float, EXT_COUNT> heightMap;
  std::array<float, EXT_COUNT> erosionTemp;

  TerrainColumnBuffers buffers{
      .continental = cont.data(),
      .erosion = erosion.data(),
      .peaksValleys = pv.data(),
      .ridge = ridge.data(),
      .temperature = temp.data(),
      .humidity = humid.data(),
      .weirdness = weird.data(),
      .river = river.data(),
      .heightMap = heightMap.data(),
      .erosionTemp = erosionTemp.data()};

  const int extStartX = worldX + static_cast<int>(NOISE_OFFSET) - HALO;
  const int extStartZ = worldZ + static_cast<int>(NOISE_OFFSET) - HALO;

  sampleTerrainColumnFields(buffers, extStartX, extStartZ, EXT_W, EXT_H, 1.0f);
  calculateTerrainHeights(buffers, EXT_W, EXT_H);
  applyCanonicalErosion(buffers.heightMap, EXT_W, EXT_H, buffers.erosionTemp);

  constexpr int centerIndex = HALO * EXT_W + HALO;
  const auto biome = evaluateBiomeAt(buffers, centerIndex);
  const int height = biome == BIOME_RIVER || biome == BIOME_FROZEN_RIVER ? SEA_LEVEL - 2 :
      std::clamp(static_cast<int>(std::round(heightMap[centerIndex])), 1, CHUNK_HEIGHT - HEIGHT_CEILING_MARGIN);
  return {height, biome, cont[centerIndex], erosion[centerIndex], weird[centerIndex],
          temp[centerIndex], humid[centerIndex], river[centerIndex],
          worldgen::reliefWeights(erosion[centerIndex], weird[centerIndex])};
}

BiomeType TerrainGenerator::evaluateBiomeColumn(int worldX, int worldZ) const
{
  return getTerrainSample(worldX, worldZ).biome;
}

BiomeType TerrainGenerator::getBiomeAt(int worldX, int worldZ) const
{
  return evaluateBiomeColumn(worldX, worldZ);
}

bool TerrainGenerator::getBiomeRegion(float centerX, float centerZ, float step,
                                      int width, int height,
                                      std::vector<BiomeType> &outBiomes,
                                      const BiomeCancelCheck &shouldCancel,
                                      BiomeRegionStats *outStats) const
{
  return getBiomeRegion(makeBiomeRegionGrid(centerX, centerZ, step, width, height),
                        outBiomes, nullptr, shouldCancel, outStats);
}

bool TerrainGenerator::getBiomeRegion(const BiomeRegionGrid &grid,
                                      std::vector<BiomeType> &outBiomes,
                                      BiomeRegionScratch *scratch,
                                      const BiomeCancelCheck &shouldCancel,
                                      BiomeRegionStats *outStats) const
{
  return getBiomeRegionTile(grid, {0, 0, grid.width, grid.height},
                            outBiomes, scratch, shouldCancel, outStats);
}

std::vector<TerrainGenerator::BiomeRegionTile>
TerrainGenerator::buildBiomeRegionPlan(const BiomeRegionGrid &grid)
{
  if (!grid.valid())
    return {};
  // Bounded canonical domains with a two-column erosion halo. Clamping in
  // float before conversion also handles extremely small positive steps.
  const float maxSpan = std::sqrt(static_cast<float>(kMaxTileDensePoints));
  const float dim = std::floor((maxSpan - 5.0f) / grid.step) + 1.0f;
  const int tileDim = static_cast<int>(std::clamp(dim, 1.0f, 32.0f));
  std::vector<BiomeRegionTile> plan;
  for (int z = 0; z < grid.height;)
  {
    const int height = std::min(tileDim, grid.height - z);
    for (int x = 0; x < grid.width;)
    {
      const int width = std::min(tileDim, grid.width - x);
      plan.push_back({x, z, width, height});
      x += width;
    }
    z += height;
  }
  return plan;
}

bool TerrainGenerator::getBiomeRegionTile(const BiomeRegionGrid &grid,
                                      const BiomeRegionTile &tile,
                                      std::vector<BiomeType> &outBiomes,
                                      BiomeRegionScratch *scratch,
                                      const BiomeCancelCheck &shouldCancel,
                                      BiomeRegionStats *outStats) const
{
  outBiomes.clear();

  // Scratch owned by the caller when provided; otherwise a dedicated
  // thread-local so region capacity never lands in the shared chunk
  // generation buffers. Resolved before any early return so the retention
  // policy below holds on absolutely every exit.
  BiomeRegionScratch &bufs = scratch ? *scratch : s_biomeRegionScratch;

  // Enforce the scratch owner's retention policy on every exit - success,
  // cancellation, invalid request, or an exception propagating out.
  // Generic scratches retain at most the tiled bound; dedicated owners may
  // retain dense capacity up to kMaxDenseDomainPoints.
  struct ScratchTrimGuard
  {
    BiomeRegionScratch &target;
    ~ScratchTrimGuard() { target.trimOversizedCapacity(); }
  } trimGuard{bufs};

  if (!grid.valid() || tile.x < 0 || tile.z < 0 ||
      tile.width <= 0 || tile.height <= 0 ||
      tile.x > grid.width - tile.width || tile.z > grid.height - tile.height)
    return false;

  BiomeRegionStats localStats;
  BiomeRegionStats &stats = outStats ? *outStats : localStats;
  stats = {};

  const auto checkCancelled = [&shouldCancel]() {
    return static_cast<bool>(shouldCancel) && shouldCancel();
  };

  // Canonical voxel columns are clamped to a sane domain: extreme grid
  // parameters (e.g. enormous step or center) would otherwise overflow the
  // int math downstream (span differences, the + NOISE_OFFSET noise-domain
  // translation). One billion blocks is far beyond any reachable world.
  constexpr int64_t kColumnLimit = 1000000000;
  const auto canonicalColumn = [&grid, &tile, kColumnLimit](int xi, int zi) {
    const glm::ivec2 col = grid.columnAt(tile.x + xi, tile.z + zi);
    return glm::ivec2(
        static_cast<int>(std::clamp<int64_t>(col.x, -kColumnLimit, kColumnLimit)),
        static_cast<int>(std::clamp<int64_t>(col.y, -kColumnLimit, kColumnLimit)));
  };

  // Track the largest dense domain sampled in one shot for the stats.
  const auto updatePeak = [&stats](int64_t points) {
    if (static_cast<int64_t>(stats.peakDensePoints) < points)
    {
      stats.peakDensePoints = static_cast<size_t>(points);
      stats.peakScratchBytes =
          stats.peakDensePoints * kBiomeRegionScratchFields * sizeof(float);
    }
  };

  // Grow all scratch fields to hold `points` floats.
  const auto ensureScratch = [&](int64_t points) {
    const size_t n = static_cast<size_t>(points);
    if (bufs.temperature.size() < n)
    {
      bufs.temperature.resize(n);
      bufs.humidity.resize(n);
      bufs.weirdness.resize(n);
      bufs.river.resize(n);
      bufs.continental.resize(n);
      bufs.erosion.resize(n);
      bufs.peaksValleys.resize(n);
      bufs.ridge.resize(n);
      bufs.height.resize(n);
      bufs.erosionTemp.resize(n);
    }
  };

  // Fill output pixels [x0..x1]x[z0..z1] from an already-sampled dense
  // block window at canonical step = 1.0f.
  const auto fillOutputFromDense = [&](const TerrainColumnBuffers &buffers,
                                       int denseStartX, int denseStartZ, int denseW,
                                       int x0, int x1, int z0, int z1) {
    for (int zi = z0; zi <= z1; ++zi)
    {
      for (int xi = x0; xi <= x1; ++xi)
      {
        const glm::ivec2 col = canonicalColumn(xi, zi);
        const int denseX = col.x - denseStartX;
        const int denseZ = col.y - denseStartZ;
        outBiomes[static_cast<size_t>(zi) * static_cast<size_t>(tile.width) +
                  static_cast<size_t>(xi)] =
            evaluateBiomeAt(buffers, denseZ * denseW + denseX);
      }
    }
  };

  const glm::ivec2 minCol = canonicalColumn(0, 0);
  const glm::ivec2 maxCol = canonicalColumn(tile.width - 1, tile.height - 1);
  const int64_t minWorldX = std::min<int64_t>(minCol.x, maxCol.x);
  const int64_t maxWorldX = std::max<int64_t>(minCol.x, maxCol.x);
  const int64_t minWorldZ = std::min<int64_t>(minCol.y, maxCol.y);
  const int64_t maxWorldZ = std::max<int64_t>(minCol.y, maxCol.y);

  // Spans in int64_t: extreme grid parameters must not overflow.
  const int64_t spanX = maxWorldX - minWorldX + 1;
  const int64_t spanZ = maxWorldZ - minWorldZ + 1;

  constexpr int HALO = 2;
  const int64_t denseWidth = spanX + 2 * HALO;
  const int64_t denseHeight = spanZ + 2 * HALO;
  const int64_t totalDensePoints = denseWidth * denseHeight;

  const size_t outputCount = static_cast<size_t>(tile.width) * static_cast<size_t>(tile.height);

  if (totalDensePoints <= static_cast<int64_t>(kMaxDenseDomainPoints))
  {
    if (checkCancelled())
      return false;

    outBiomes.resize(outputCount);
    ensureScratch(totalDensePoints);

    TerrainColumnBuffers buffers{
        .continental = bufs.continental.data(),
        .erosion = bufs.erosion.data(),
        .peaksValleys = bufs.peaksValleys.data(),
        .ridge = bufs.ridge.data(),
        .temperature = bufs.temperature.data(),
        .humidity = bufs.humidity.data(),
        .weirdness = bufs.weirdness.data(),
        .river = bufs.river.data(),
        .heightMap = bufs.height.data(),
        .erosionTemp = bufs.erosionTemp.data()};

    const int denseStartX = static_cast<int>(minWorldX) - HALO;
    const int denseStartZ = static_cast<int>(minWorldZ) - HALO;

    sampleTerrainColumnFields(buffers,
                              denseStartX + static_cast<int>(NOISE_OFFSET),
                              denseStartZ + static_cast<int>(NOISE_OFFSET),
                              static_cast<int>(denseWidth),
                              static_cast<int>(denseHeight), 1.0f);
    calculateTerrainHeights(buffers, static_cast<int>(denseWidth),
                            static_cast<int>(denseHeight));
    applyCanonicalErosion(buffers.heightMap, static_cast<int>(denseWidth),
                          static_cast<int>(denseHeight), buffers.erosionTemp);

    fillOutputFromDense(buffers, denseStartX, denseStartZ,
                        static_cast<int>(denseWidth),
                        0, tile.width - 1, 0, tile.height - 1);
    ++stats.denseTiles;
    updatePeak(totalDensePoints);

    // Late cancellation: never return a fully-sampled but rejected result.
    if (checkCancelled())
    {
      outBiomes.clear();
      return false;
    }
    return true;
  }

  // Bounded tiled path for large regions / zoom-outs. The tile dimension is
  // derived from grid.step so each tile's haloed dense domain always fits in
  // kMaxTileDensePoints even at the smallest zoom (largest step), keeping
  // every pixel on the vectorized dense path. The point-query fallback below
  // is a safety net, not the normal path for any zoom.
  constexpr int kMaxTileDim = 32;
  const float maxTileSpan = std::sqrt(static_cast<float>(kMaxTileDensePoints));
  const float tileDimF =
      std::floor((maxTileSpan - 1.0f - static_cast<float>(2 * HALO)) / grid.step) + 1.0f;
  const int tileDim = static_cast<int>(std::clamp(tileDimF, 1.0f, static_cast<float>(kMaxTileDim)));

  const int tilesX = (tile.width + tileDim - 1) / tileDim;
  const int tilesZ = (tile.height + tileDim - 1) / tileDim;

  outBiomes.resize(outputCount);

  // Tiles are processed sequentially in fixed order: the result depends only
  // on the grid and the seed, never on scheduling. TerrainGenerator does not
  // spawn threads and does not decide CPU policy; the caller chooses when and
  // on which worker the call runs. The Engine can schedule independent
  // rectangles from buildBiomeRegionPlan() as TaskPriority::Low jobs.
  for (int tz = 0; tz < tilesZ; ++tz)
  {
    for (int tx = 0; tx < tilesX; ++tx)
    {
      // Cancellation checkpoint: polled before every tile so a long
      // zoomed-out map build can be abandoned promptly.
      if (checkCancelled())
      {
        outBiomes.clear();
        return false;
      }

      const int x0 = tx * tileDim;
      const int z0 = tz * tileDim;
      const int x1 = std::min(x0 + tileDim - 1, tile.width - 1);
      const int z1 = std::min(z0 + tileDim - 1, tile.height - 1);

      const glm::ivec2 tMinCol = canonicalColumn(x0, z0);
      const glm::ivec2 tMaxCol = canonicalColumn(x1, z1);
      const int64_t tMinX = std::min<int64_t>(tMinCol.x, tMaxCol.x);
      const int64_t tMaxX = std::max<int64_t>(tMinCol.x, tMaxCol.x);
      const int64_t tMinZ = std::min<int64_t>(tMinCol.y, tMaxCol.y);
      const int64_t tMaxZ = std::max<int64_t>(tMinCol.y, tMaxCol.y);

      const int64_t tDenseW = (tMaxX - tMinX + 1) + 2 * HALO;
      const int64_t tDenseH = (tMaxZ - tMinZ + 1) + 2 * HALO;
      const int64_t tDensePoints = tDenseW * tDenseH;

      if (tDensePoints <= static_cast<int64_t>(kMaxTileDensePoints)) // guaranteed by tileDim; kept as a guard
      {
        ensureScratch(tDensePoints);

        TerrainColumnBuffers buffers{
            .continental = bufs.continental.data(),
            .erosion = bufs.erosion.data(),
            .peaksValleys = bufs.peaksValleys.data(),
            .ridge = bufs.ridge.data(),
            .temperature = bufs.temperature.data(),
            .humidity = bufs.humidity.data(),
            .weirdness = bufs.weirdness.data(),
            .river = bufs.river.data(),
            .heightMap = bufs.height.data(),
            .erosionTemp = bufs.erosionTemp.data()};

        const int tDenseStartX = static_cast<int>(tMinX) - HALO;
        const int tDenseStartZ = static_cast<int>(tMinZ) - HALO;

        sampleTerrainColumnFields(buffers,
                                  tDenseStartX + static_cast<int>(NOISE_OFFSET),
                                  tDenseStartZ + static_cast<int>(NOISE_OFFSET),
                                  static_cast<int>(tDenseW),
                                  static_cast<int>(tDenseH), 1.0f);
        calculateTerrainHeights(buffers, static_cast<int>(tDenseW),
                                static_cast<int>(tDenseH));
        applyCanonicalErosion(buffers.heightMap, static_cast<int>(tDenseW),
                              static_cast<int>(tDenseH), buffers.erosionTemp);

        fillOutputFromDense(buffers, tDenseStartX, tDenseStartZ,
                            static_cast<int>(tDenseW), x0, x1, z0, z1);
        ++stats.denseTiles;
        updatePeak(tDensePoints);
      }
      else
      {
        // Safety net only: evaluate point canonical columns directly.
        for (int zi = z0; zi <= z1; ++zi)
        {
          for (int xi = x0; xi <= x1; ++xi)
          {
            const glm::ivec2 col = canonicalColumn(xi, zi);
            outBiomes[static_cast<size_t>(zi) * static_cast<size_t>(tile.width) +
                      static_cast<size_t>(xi)] = evaluateBiomeColumn(col.x, col.y);
          }
        }
        stats.fallbackPixels += static_cast<uint64_t>(x1 - x0 + 1) *
                                static_cast<uint64_t>(z1 - z0 + 1);
      }
    }
  }

  return true;
}

// =============================================
// HEIGHT CALCULATION
// =============================================

// Rock exposure: post-erosion neighbor height delta above which vegetated
// surfaces turn to bare stone/gravel (cliff faces).
static constexpr float ROCKY_SLOPE_THRESHOLD = 2.0f;

// Height clamping
// static constexpr int   HEIGHT_CEILING_MARGIN =  32; // Moved up

float TerrainGenerator::calculateHeightFloat(float continental, float erosion,
    float peaksValleys, float ridge, float riverVal, float weirdness,
    float temperature, float humidity) const
{
  using worldgen::blend;
  const float land = blend(-0.50f, -0.18f, continental);
  const float inland = blend(-0.18f, 0.12f, continental);
  const float pv = std::clamp(peaksValleys, -1.f, 1.f);
  const float ridged = std::clamp(ridge, 0.f, 1.f);
  const auto weights = worldgen::reliefWeights(erosion, weirdness);
  const std::array<float, 6> profiles = {
      9.f + pv * 11.f,
      32.f + 27.f * blend(-0.2f, 0.12f, pv) + pv * 4.f,
      48.f + ridged * ridged * 142.f + pv * 24.f,
      48.f + pv * 9.f - 42.f * (1.f - blend(0.03f, 0.23f, std::abs(pv))),
      14.f + 62.f * blend(-0.08f, 0.12f, pv),
      4.f + 9.f * pv * pv};
  float relief = 0.f;
  for (size_t i = 0; i < weights.size(); ++i) relief += profiles[i] * weights[i];
  // Climate signatures are continuous; the discrete biome never feeds back
  // into its own height and cannot introduce a seam in canonical queries.
  const float arid = blend(0.15f, 0.55f, temperature) * (1.f - blend(-0.5f, -0.05f, humidity));
  const float wet = blend(0.25f, 0.65f, humidity) * (1.f - blend(0.35f, 0.75f, std::abs(erosion)));
  const float dune = 4.f + 7.f * std::pow(0.5f + 0.5f * std::sin(pv * 16.f + weirdness * 5.f), 2.f);
  relief = std::lerp(relief, relief * 0.75f + dune, arid);
  relief = std::lerp(relief, 2.f + pv * 2.f, wet * (1.f - weights[2]));
  const float terrace = std::floor(relief / 7.f) * 7.f +
      blend(0.45f, 0.95f, relief / 7.f - std::floor(relief / 7.f)) * 7.f;
  relief = std::lerp(relief, terrace, arid * 0.85f);
  const float oasis = arid * blend(0.44f, 0.70f, weirdness) * (1.f - blend(-0.1f, 0.1f, erosion));
  // The weight saturates before peaking, so the lerp target must sit well
  // below sea level: the core keeps a real water floor, not a shallow dish.
  relief = std::lerp(relief, -8.f, std::min(1.f, oasis * 1.35f));
  const float volcanic = blend(0.40f, 0.65f, temperature) * blend(0.36f, 0.65f, weirdness)
      * (1.f - blend(0.08f, 0.35f, erosion));
  const float cone = 48.f + 80.f * blend(0.30f, 0.72f, weirdness)
      - 42.f * blend(0.72f, 0.86f, weirdness);
  relief = std::lerp(relief, cone, volcanic);
  const float ocean = -28.f + pv * 6.f + 14.f * blend(-0.7f, -0.45f, continental);
  float height = std::lerp(ocean, relief * (0.25f + 0.75f * inland), land);
  const float valley = 1.f - blend(0.f, worldgen::riverWidth(weirdness) * 2.5f, std::abs(riverVal));
  const float channel = 1.f - blend(worldgen::riverWidth(weirdness) * 0.25f,
                                    worldgen::riverWidth(weirdness), std::abs(riverVal));
  if (height > -2.f)
    height = std::lerp(height - valley * std::min(24.f, height + 2.f), -2.f, channel);
  return std::clamp(SEA_LEVEL + height, 8.f, 220.f);
}

int TerrainGenerator::calculateHeight(float continental, float erosion,
                                      float peaksValleys, float ridge, float riverVal, float weirdness) const
{
  float heightFloat = calculateHeightFloat(continental, erosion, peaksValleys, ridge, riverVal, weirdness);
  return std::clamp(static_cast<int>(std::round(heightFloat)), 1, CHUNK_HEIGHT - HEIGHT_CEILING_MARGIN);
}

void TerrainGenerator::applyErosion(float *heightMap, int size) const
{
  applyCanonicalErosion(heightMap, size, size, nullptr);
}

void TerrainGenerator::applyCanonicalErosion(float *heightMap, int width, int height,
                                            float *tempBuffer) const
{
  if (width < 2 || height < 2)
    return;

  assert(heightMap != nullptr);

  using Clock = std::chrono::steady_clock;
  const auto erosionStart =
      m_activeProfile ? Clock::now() : Clock::time_point{};

  // 1. Single Thermal Erosion pass (smooths overly steep slopes deterministically)
  // Cells with at least 2 cells of halo from the border are fully deterministic
  // across sampling windows because all four neighbors are processed and their
  // give-decisions only read sampled values within the window.
  const float talusAngle = 0.6f;  // Max allowed height diff between adjacent cells
  const float thermalRate = 0.5f; // Fraction of material to move
  const int totalCount = width * height;

  float *tempMap = tempBuffer;
  if (!tempMap)
  {
    if (totalCount <= EXT_SIZE * EXT_SIZE)
    {
      tempMap = s_genBuffers.erosionTempMap.data();
    }
    else
    {
      // Rare fallback for large-domain erosion without a caller-provided
      // temp buffer; kept thread-local and separate from GenBuffers.
      static thread_local std::vector<float> fallbackTemp;
      if (fallbackTemp.size() < static_cast<size_t>(totalCount))
        fallbackTemp.resize(static_cast<size_t>(totalCount));
      tempMap = fallbackTemp.data();
    }
  }
  std::copy(heightMap, heightMap + totalCount, tempMap);

  for (int z = 1; z < height - 1; ++z)
  {
    for (int x = 1; x < width - 1; ++x)
    {
      int idx = z * width + x;
      float h = tempMap[idx];

      float maxDiff = 0.0f;
      int maxDiffIdx = -1;

      // Check 4 neighbors
      const int neighbors[4] = {idx - 1, idx + 1, idx - width, idx + width};
      for (int i = 0; i < 4; ++i)
      {
        float diff = h - tempMap[neighbors[i]];
        if (diff > maxDiff)
        {
          maxDiff = diff;
          maxDiffIdx = neighbors[i];
        }
      }

      if (maxDiff > talusAngle && maxDiffIdx != -1)
      {
        float moveAmount = (maxDiff - talusAngle) * thermalRate;
        heightMap[idx] -= moveAmount;
        heightMap[maxDiffIdx] += moveAmount;
      }
    }
  }
  if (m_activeProfile)
    m_activeProfile->erosionMs +=
        std::chrono::duration<double, std::milli>(Clock::now() - erosionStart)
            .count();
}
  // Hydraulic Erosion pass removed because particle simulation causes
  // boundary mismatches across asynchronously generated chunks.


// =============================================
// VEGETATION GENERATION
// =============================================

namespace
{

/// Species helpers mirroring the placeTree switch — used for bushes / fallen logs.
inline TextureType leafTypeForBiome(BiomeType biome, uint32_t h)
{
  switch (biome)
  {
  case BIOME_SNOWY_TAIGA:
  case BIOME_REDWOOD_FOREST:
    return TextureType::SPRUCE_LEAVES;
  case BIOME_MOUNTAINS:
    return ((h % 10) < 7) ? TextureType::SPRUCE_LEAVES : TextureType::OAK_LEAVES;
  case BIOME_BIRCH_FOREST:
    return ((h % 5) < 4) ? TextureType::BIRCH_LEAVES : TextureType::OAK_LEAVES;
  case BIOME_CHERRY_GROVE:
    return TextureType::CHERRY_LEAVES;
  case BIOME_DARK_FOREST:
  case BIOME_AUTUMN_FOREST:
  {
    const uint32_t r = h % 20;
    if (r < 12)
      return TextureType::DARK_OAK_LEAVES;
    if (r < 17)
      return TextureType::SPRUCE_LEAVES;
    return TextureType::BIRCH_LEAVES;
  }
  case BIOME_JUNGLE:
  case BIOME_BAMBOO_JUNGLE:
    return TextureType::JUNGLE_LEAVES;
  case BIOME_MANGROVE_SWAMP:
    return TextureType::MANGROVE_LEAVES;
  case BIOME_SAVANNA:
  case BIOME_OASIS:
    return TextureType::ACACIA_LEAVES;
  case BIOME_MOOR:
    return (h & 1u) == 0u ? TextureType::SPRUCE_LEAVES
                          : TextureType::OAK_LEAVES;
  case BIOME_MUSHROOM_FIELDS:
    return (h & 1u) == 0u ? TextureType::RED_MUSHROOM_BLOCK
                          : TextureType::BROWN_MUSHROOM_BLOCK;
  default:
    return TextureType::OAK_LEAVES;
  }
}

inline TextureType logTypeForBiome(BiomeType biome, uint32_t h)
{
  switch (biome)
  {
  case BIOME_SNOWY_TAIGA:
  case BIOME_REDWOOD_FOREST:
    return TextureType::SPRUCE_LOG;
  case BIOME_MOUNTAINS:
    return ((h % 10) < 7) ? TextureType::SPRUCE_LOG : TextureType::OAK_LOG;
  case BIOME_BIRCH_FOREST:
    return ((h % 5) < 4) ? TextureType::BIRCH_LOG : TextureType::OAK_LOG;
  case BIOME_CHERRY_GROVE:
    return TextureType::CHERRY_LOG;
  case BIOME_DARK_FOREST:
  case BIOME_AUTUMN_FOREST:
  {
    const uint32_t r = h % 20;
    if (r < 12)
      return TextureType::DARK_OAK_LOG;
    if (r < 17)
      return TextureType::SPRUCE_LOG;
    return TextureType::BIRCH_LOG;
  }
  case BIOME_JUNGLE:
    return TextureType::JUNGLE_LOG;
  case BIOME_MANGROVE_SWAMP:
    return TextureType::MANGROVE_LOG;
  case BIOME_BAMBOO_JUNGLE:
    return TextureType::BAMBOO_BLOCK;
  case BIOME_SAVANNA:
  case BIOME_OASIS:
    return TextureType::ACACIA_LOG;
  case BIOME_MOOR:
    return (h & 1u) == 0u ? TextureType::SPRUCE_LOG : TextureType::OAK_LOG;
  case BIOME_MUSHROOM_FIELDS:
    return TextureType::MUSHROOM_STEM;
  default:
    return TextureType::OAK_LOG;
  }
}

} // namespace

void TerrainGenerator::generateVegetation(const ChunkGenerationTarget &data, int chunkX, int chunkZ)
{
  const auto extIndex = [](int x, int z) { return (z + EXT_CORE_OFFSET) * EXT_SIZE + x + EXT_CORE_OFFSET; };
  auto &b = s_genBuffers;
  const float startX = static_cast<float>(chunkX - EXT_CORE_OFFSET) + NOISE_OFFSET;
  const float startZ = static_cast<float>(chunkZ - EXT_CORE_OFFSET) + NOISE_OFFSET;
  m_treeNoise->GenUniformGrid2D(b.treeNoiseResults.data(), startX, startZ, EXT_SIZE, EXT_SIZE, 1.f, m_seed + 10000);
  m_forestDensityNoise->GenUniformGrid2D(b.forestDensityResults.data(), startX, startZ, EXT_SIZE, EXT_SIZE, 1.f, m_seed + 11000);

  // Pure, cached support query. Never consult partially decorated neighbor
  // voxels to decide whether a candidate exists. Surface cave cover is <7,
  // so the deep, half-resolution cave fields do not participate here.
  std::array<int, EXT_SIZE * EXT_SIZE> supports;
  supports.fill(-2);
  auto support = [&](int x, int z) -> int {
    const int i = extIndex(x, z);
    if (supports[i] != -2) return supports[i];
    const int h = b.extHeights[i];
    const auto biome = b.extBiomes[i];
    const float amp = getBiomeConfig(biome).surfacePerturbAmp;
    const float wx = static_cast<float>(chunkX + x) + NOISE_OFFSET;
    const float wz = static_cast<float>(chunkZ + z) + NOISE_OFFSET;
    for (int y = std::min(CHUNK_HEIGHT - 1, h + static_cast<int>(std::ceil(amp))); y >= h - 5; --y)
    {
      float density = static_cast<float>(h - y);
      if (amp > 0.f && density > -8.f && density < 12.f)
        density += m_surface3DNoise->GenSingle3D(wx, static_cast<float>(y), wz, m_seed + 6000)
            * amp * std::max(0.f, 1.f - std::abs(density) / 12.f);
      if (density < 0.f) continue;
      const float cave = m_caveNoise->GenSingle3D(wx, static_cast<float>(y), wz, m_seed + 4000);
      const float ravine = m_ravineNoise->GenSingle3D(wx, static_cast<float>(y), wz, m_seed + 5000);
      if (shouldCarveCave(y, h, cave, ravine, 0.f, 0.f, 0.f, false)) continue;
      return supports[i] = y;
    }
    return supports[i] = -1; // cave mouth: no floating surface decoration
  };
  auto setIf = [&](int x, int y, int z, TextureType expected, TextureType type) {
    if (featureVoxel(data, x, y, z) == expected) setVoxelSafe(data, x, y, z, type);
  };
  const auto unit = [](uint32_t h) { return static_cast<float>(h & 65535u) / 65536.f; };

  struct Candidate { int x, z, y; BiomeType biome; uint32_t hash; bool mature; };
  // At most 10x10 cells intersect the expanded 18x18 output. No retained
  // heap allocation per worker, and bounded independently of world position.
  std::array<Candidate, 144> candidates{};
  size_t count = 0;
  constexpr int radius = worldgen::maxFeatureRadius;
  constexpr int cell = worldgen::candidateCellSize;
  for (int cz = worldgen::cellAt(chunkZ - 1 - radius); cz <= worldgen::cellAt(chunkZ + CHUNK_SIZE + radius); ++cz)
    for (int cx = worldgen::cellAt(chunkX - 1 - radius); cx <= worldgen::cellAt(chunkX + CHUNK_SIZE + radius); ++cx)
    {
      const uint32_t hash = treeHash(cx, cz, m_seed + 31000);
      const int x = cx * cell + static_cast<int>((hash >> 16) % cell) - chunkX;
      const int z = cz * cell + static_cast<int>((hash >> 20) % cell) - chunkZ;
      const int i = extIndex(x, z);
      const auto biome = b.extBiomes[i];
      const auto &config = getBiomeConfig(biome);
      const float grove = worldgen::blend(-0.5f, 0.5f, b.forestDensityResults[i]);
      const float humidity = std::clamp((b.humidity[i] + 1.f) * 0.5f, 0.f, 1.f);
      const float probability = std::min(0.95f, config.treeDensity * 3.2f * (0.12f + grove)
          * (0.7f + humidity * 0.6f));
      if (unit(hash) >= probability) continue;
      const bool mature = (hash >> 24) % 9u == 0u &&
          biome != BIOME_BAMBOO_JUNGLE && biome != BIOME_OASIS && biome != BIOME_MUSHROOM_FIELDS;
      auto bounds = mature ? worldgen::matureTree : worldgen::smallTree;
      if (biome == BIOME_REDWOOD_FOREST) bounds.height = 33;
      const int y = support(x, z);
      if (y <= SEA_LEVEL || y + bounds.height >= CHUNK_HEIGHT || y > 195) continue;
      if (columnSlopeAt(b.extHeights.data(), EXT_SIZE, i) > bounds.maxSlope) continue;
      const auto ground = getVoxelTypeAt(chunkX + x, y, chunkZ + z, y, biome, b.temperature[i], 0.f, 0.f);
      if (!blockIsPlantableSurface(ground)) continue;
      // Broad trunks and exposed roots must be seated, not suspended on a ledge.
      bool seated = true;
      for (int dz = -1; dz <= 1 && seated; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
          if (std::abs(support(x + dx, z + dz) - y) > 1) { seated = false; break; }
      if (!seated) continue;
      assert(count < candidates.size());
      candidates[count++] = {x, z, y + 1, biome, hash, mature};
    }
  // A total world-coordinate order gives identical overlap winners in every
  // chunk and its shell; mature trees take precedence over small specimens.
  std::sort(candidates.begin(), candidates.begin() + count, [](const Candidate &a, const Candidate &c) {
    if (a.mature != c.mature) return !a.mature;
    if (a.hash != c.hash) return a.hash < c.hash;
    if (a.z != c.z) return a.z < c.z;
    return a.x < c.x;
  });
  for (size_t n = 0; n < count; ++n)
  {
    const auto &c = candidates[n];
    if (c.mature) placeMatureTree(data, c.x, c.z, c.y, c.biome, c.hash);
    else placeTree(data, c.x, c.z, c.y, c.biome, chunkX + c.x, chunkZ + c.z);
  }

  // Ground features have a radius of three. Their decisions, including all
  // support heights, use absolute coordinates and precede fine decoration.
  for (int z = -4; z <= CHUNK_SIZE + 3; ++z)
    for (int x = -4; x <= CHUNK_SIZE + 3; ++x)
    {
      const int i = extIndex(x, z);
      const auto biome = b.extBiomes[i];
      const auto &config = getBiomeConfig(biome);
      const uint32_t hash = treeHash(chunkX + x, chunkZ + z, m_seed + 32000);
      const int y = support(x, z);
      if (y < 0) continue;
      if (biome == BIOME_CORAL_REEF && y < SEA_LEVEL - 2 && unit(hash) < 0.035f)
      {
        const auto coral = static_cast<TextureType>(TUBE_CORAL_BLOCK + (hash >> 16) % 5u);
        for (int dz = -1; dz <= 1; ++dz)
          for (int dx = -1; dx <= 1; ++dx)
          {
            if (std::abs(dx) + std::abs(dz) > 1) continue;
            const int base = support(x + dx, z + dz);
            if (base < 0 || std::abs(base - y) > 1) continue;
            for (int k = 1; k <= 1 + static_cast<int>((hash >> 20) % 3u); ++k)
              setIf(x + dx, base + k, z + dz, WATER, coral);
          }
      }
      if (y <= SEA_LEVEL || y > 210) continue;
      const float slope = columnSlopeAt(b.extHeights.data(), EXT_SIZE, i);
      if (slope > worldgen::groundProp.maxSlope) continue;
      if (biome == BIOME_ICE_SPIKES && unit(hash) < 0.035f)
        placeIceSpike(data, x, z, y + 1, chunkX + x, chunkZ + z);
      if (config.hasCacti && unit(hash ^ 0x735au) < 0.016f)
      {
        // Candidate/height tests are pure; never inspect a partially built neighbor.
        for (int k = 1; k <= 2 + static_cast<int>((hash >> 18) % 4u); ++k)
          setIf(x, y + k, z, AIR, CACTUS);
      }
      if (unit(hash ^ 0x8793u) < config.rockDensity)
      {
        TextureType rock = config.palette == worldgen::Palette::Wet || config.palette == worldgen::Palette::Fungal
            ? MOSSY_COBBLESTONE : config.palette == worldgen::Palette::Volcanic ? BASALT
            : config.palette == worldgen::Palette::Arid ? SANDSTONE : COBBLESTONE;
        const int r = 1 + static_cast<int>((hash >> 16) & 1u);
        for (int dz = -r; dz <= r; ++dz)
          for (int dx = -r; dx <= r; ++dx)
          {
            if (dx * dx + dz * dz > r * r + 1) continue;
            const int base = support(x + dx, z + dz);
            if (base < 0 || std::abs(base - y) > 1) continue;
            // Buried lower half; leave existing trees intact.
            for (int k = 0; k <= (dx == 0 && dz == 0 ? 2 : 1); ++k)
            {
              const auto old = featureVoxel(data, x + dx, base + k, z + dz);
              if (old == AIR || (k == 0 && !blockIsLeaves(old)))
                setVoxelSafe(data, x + dx, base + k, z + dz, rock);
            }
          }
      }
      if (unit(hash ^ 0x343fu) < config.fallenLogDensity)
      {
        const int dx = (hash & 0x10000u) ? 1 : 0;
        const int dz = 1 - dx;
        const int length = 2 + static_cast<int>((hash >> 18) % 3u);
        bool flat = true;
        for (int k = 0; k < length; ++k)
          if (support(x + dx * k, z + dz * k) != y) flat = false;
        if (flat)
          for (int k = 0; k < length; ++k)
            setIf(x + dx * k, y + 1, z + dz * k, AIR, logTypeForBiome(biome, hash));
      }
      if (unit(hash ^ 0xaba5u) < config.bushDensity * 0.5f)
      {
        const auto leaf = leafTypeForBiome(biome, hash);
        for (int dz = -1; dz <= 1; ++dz)
          for (int dx = -1; dx <= 1; ++dx)
            if (std::abs(dx) + std::abs(dz) <= 1 && support(x + dx, z + dz) == y)
              setIf(x + dx, y + 1, z + dz, AIR, leaf);
      }
    }

  for (int z = -1; z <= CHUNK_SIZE; ++z)
    for (int x = -1; x <= CHUNK_SIZE; ++x)
    {
      const int i = extIndex(x, z);
      const auto biome = b.extBiomes[i];
      const auto &config = getBiomeConfig(biome);
      const int y = support(x, z);
      if (y < 0) continue;
      const uint32_t hash = treeHash(chunkX + x, chunkZ + z, m_seed + 33000);
      const float roll = unit(hash);
      if (y < SEA_LEVEL && featureVoxel(data, x, y + 1, z) == WATER)
      {
        if (biome == BIOME_OCEAN && roll < 0.018f && y < SEA_LEVEL - 4)
        {
          const int h = std::min(SEA_LEVEL - y - 1, 2 + static_cast<int>((hash >> 16) % 5u));
          for (int k = 1; k <= h; ++k) setIf(x, y + k, z, WATER, k == h ? KELP_TOP : KELP);
        }
        else if (roll < 0.15f && biome != BIOME_FROZEN_OCEAN && biome != BIOME_FROZEN_RIVER)
          setIf(x, y + 1, z, WATER, SEAGRASS);
        if (y >= SEA_LEVEL - 4 && config.decoration == worldgen::Decoration::Wetland && roll < 0.2f &&
            featureVoxel(data, x, SEA_LEVEL, z) == WATER)
          setIf(x, SEA_LEVEL + 1, z, AIR, LILY_PAD);
        continue;
      }
      const auto ground = featureVoxel(data, x, y, z);
      if (y <= SEA_LEVEL || y > 180 || !blockIsPlantableSurface(ground) || ground == SNOW ||
          ground == PACKED_ICE || ground == BLUE_ICE || columnSlopeAt(b.extHeights.data(), EXT_SIZE, i) > 2.f)
        continue;
      const float patch = worldgen::blend(-0.65f, 0.55f, b.treeNoiseResults[i]);
      const bool arid = config.decoration == worldgen::Decoration::Desert;
      if (roll < (arid ? 0.035f : 0.08f + 0.30f * patch))
      {
        TextureType plant = arid ? DRY_SHRUB : SHORT_GRASS;
        if (!arid && (config.decoration == worldgen::Decoration::Woodland || config.palette == worldgen::Palette::Wet))
          plant = FERN;
        if (!arid && ((hash >> 16) % (biome == BIOME_FLOWER_MEADOW ? 2u : 9u)) == 0u)
          plant = WILDFLOWER;
        setIf(x, y + 1, z, AIR, plant);
      }
    }
}

// vegetation placers extracted to TerrainVegetation.cpp

// =============================================
// VOXEL TYPE DETERMINATION
// =============================================

namespace
{

TextureType deepStoneAt(int worldY, uint32_t cellHash)
{
  if (worldY < 16)
    return TextureType::DEEPSLATE;
  if (worldY < 24)
  {
    const uint32_t r = cellHash % 5;
    if (r < 2)
      return TextureType::TUFF;
    if (r < 4)
      return TextureType::DEEPSLATE;
    return TextureType::STONE;
  }
  const uint32_t r = cellHash % 100;
  if (r < 2)
    return TextureType::GRANITE;
  if (r < 4)
    return TextureType::DIORITE;
  if (r < 6)
    return TextureType::ANDESITE;
  return TextureType::STONE;
}

TextureType badlandsBandAt(int worldY, uint32_t colHash)
{
  static constexpr TextureType kBands[] = {
      TextureType::TERRACOTTA,        TextureType::ORANGE_TERRACOTTA, TextureType::YELLOW_TERRACOTTA,
      TextureType::RED_TERRACOTTA,    TextureType::BROWN_TERRACOTTA,  TextureType::WHITE_TERRACOTTA,
      TextureType::RED_SANDSTONE,     TextureType::TERRACOTTA,
  };
  const int idx = static_cast<int>((static_cast<uint32_t>(worldY) + (colHash & 7u)) % 8u);
  return kBands[idx];
}

/// Biome surface patches: rare alternates, otherwise BiomeConfig::surfaceBlock.
TextureType surfaceBlockForBiome(BiomeType biome, uint32_t colHash, const BiomeConfig &config,
                                 int worldY)
{
  if (config.palette == worldgen::Palette::Alpine && config.surfaceBlock == STONE)
    return (colHash % 7u == 0u) ? DIORITE : (colHash % 3u == 0u) ? ANDESITE : STONE;
  if (config.relief == worldgen::Relief::Cliffs && config.palette == worldgen::Palette::Volcanic && colHash % 3u == 0u)
    return BLACKSTONE;
  switch (biome)
  {
  case BIOME_SNOWY_TAIGA:
  case BIOME_REDWOOD_FOREST:
    if ((colHash % 7u) == 0u)
      return TextureType::PODZOL;
    break;
  case BIOME_SAVANNA:
    if ((colHash % 5u) == 0u)
      return TextureType::COARSE_DIRT;
    break;
  case BIOME_SWAMP:
  case BIOME_MANGROVE_SWAMP:
    if ((colHash % 5u) == 0u)
      return TextureType::MOSS_BLOCK;
    if ((colHash % 4u) == 0u)
      return TextureType::CLAY;
    break; // config.surfaceBlock is MUD
  case BIOME_JUNGLE:
  case BIOME_BAMBOO_JUNGLE:
    if ((colHash % 11u) == 0u)
      return TextureType::MOSS_BLOCK;
    break;
  case BIOME_AUTUMN_FOREST:
    if ((colHash % 6u) == 0u)
      return TextureType::PODZOL;
    if ((colHash % 5u) == 0u)
      return TextureType::COARSE_DIRT;
    break;
  case BIOME_MOOR:
    if ((colHash % 4u) == 0u)
      return TextureType::GRAVEL;
    break;
  case BIOME_GLACIER:
    if ((colHash % 9u) == 0u)
      return TextureType::BLUE_ICE;
    break;
  case BIOME_VOLCANIC:
    if ((colHash % 19u) == 0u)
      return TextureType::MAGMA;
    if ((colHash % 5u) == 0u)
      return TextureType::BLACKSTONE;
    break;
  case BIOME_OASIS:
    if ((colHash % 7u) == 0u)
      return TextureType::MOSS_BLOCK;
    break;
  case BIOME_BADLANDS:
    if ((colHash % 4u) == 0u)
      return badlandsBandAt(worldY, colHash);
    break; // config.surfaceBlock is RED_SAND
  case BIOME_RIVER:
  case BIOME_FROZEN_RIVER:
    if ((colHash % 3u) == 0u)
      return TextureType::CLAY;
    if (((colHash >> 3) % 3u) == 0u)
      return TextureType::GRAVEL; // gravel banks
    break;
  default:
    break;
  }
  return config.surfaceBlock;
}

TextureType underwaterSurfaceBlockForBiome(BiomeType biome,
                                           uint32_t patchHash,
                                           const BiomeConfig &config)
{
  // Four-by-four world cells produce readable material patches instead of
  // per-column salt-and-pepper noise. The hash is world-coordinate based, so
  // a patch remains identical across chunk boundaries.
  switch (biome)
  {
  case BIOME_OCEAN:
    if (patchHash % 13u == 0u)
      return TextureType::CLAY;
    if (patchHash % 5u == 0u)
      return TextureType::GRAVEL;
    return TextureType::SAND;
  case BIOME_FROZEN_OCEAN:
    if (patchHash % 11u == 0u)
      return TextureType::CLAY;
    if (patchHash % 4u == 0u)
      return TextureType::SAND;
    return TextureType::GRAVEL;
  case BIOME_CORAL_REEF:
    if (patchHash % 9u == 0u)
      return TextureType::CLAY;
    if (patchHash % 3u == 0u)
      return TextureType::SAND;
    return TextureType::GRAVEL;
  case BIOME_SWAMP:
  case BIOME_MANGROVE_SWAMP:
    return (patchHash % 3u == 0u) ? TextureType::CLAY : TextureType::MUD;
  default:
    return config.underwaterBlock;
  }
}

} // namespace

TextureType TerrainGenerator::getVoxelTypeAt(int worldX, int worldY, int worldZ, int terrainHeight,
                                             BiomeType biome, float temperature, float slope, float density, BiomeType surfaceBiome) const
{
  if (worldY <= BEDROCK_LEVEL)
    return TextureType::BEDROCK;

  if (density == -1.0f)
    density = static_cast<float>(terrainHeight - worldY);

  const uint32_t colHash =
      (static_cast<uint32_t>(worldX) * 374761393u + static_cast<uint32_t>(worldZ) * 668265263u) ^
      static_cast<uint32_t>(m_seed);
  const uint32_t cellHash = colHash ^ (static_cast<uint32_t>(worldY) * 2654435761u);

  if (density >= 0.0f)
  {
    const BiomeConfig &config = getBiomeConfig(biome);

    if (density < 1.0f)
    {
      if (worldY <= SEA_LEVEL + 2 && config.surfaceBlock != TextureType::STONE)
      {
        const auto floorDiv4 = [](int value) {
          return value >= 0 ? value / 4 : -((-value + 3) / 4);
        };
        const uint32_t patchHash =
            treeHash(floorDiv4(worldX), floorDiv4(worldZ), m_seed + 26000);
        return underwaterSurfaceBlockForBiome(biome, patchHash, config);
      }

      // Global snow line: any land biome caps in snow at altitude (dithered edge).
      const float snowLine = 160.0f + temperature * 10.0f;
      const float dither = static_cast<float>(colHash & 0xFFFF) / 65535.0f;
      if (worldY > snowLine + (dither - 0.5f) * 15.0f)
        return TextureType::SNOW;

      // Rock exposure on steep vegetated slopes (cliffs, scree).
      if (slope > ROCKY_SLOPE_THRESHOLD &&
          (config.surfaceBlock == TextureType::GRASS_TOP ||
           config.surfaceBlock == TextureType::DIRT ||
           config.surfaceBlock == TextureType::MUD ||
           config.surfaceBlock == TextureType::SNOW))
      {
        return ((colHash >> 8) % 4 == 0) ? TextureType::GRAVEL : TextureType::STONE;
      }

      const uint32_t patchHash = treeHash(worldgen::cellAt(worldX), worldgen::cellAt(worldZ), m_seed + 34000);
      const auto materialBiome = surfaceBiome == BIOME_COUNT ? biome : surfaceBiome;
      const auto &materialConfig = getBiomeConfig(materialBiome);
      return surfaceBlockForBiome(materialBiome, patchHash, materialConfig, worldY);
    }

    if (density < static_cast<float>(config.subsurfaceDepth + 1))
    {
      if (worldY <= SEA_LEVEL + 2 && config.subsurfaceBlock != TextureType::STONE)
        return config.underwaterBlock;
      // Bare rock under exposed cliff faces (matches the surface override).
      if (slope > ROCKY_SLOPE_THRESHOLD &&
          (config.surfaceBlock == TextureType::GRASS_TOP ||
           config.surfaceBlock == TextureType::DIRT ||
           config.surfaceBlock == TextureType::MUD ||
           config.surfaceBlock == TextureType::SNOW))
        return TextureType::STONE;
      // Badlands: banded cliffs; other biomes use BiomeConfig::subsurfaceBlock
      // (desert sandstone, etc. already set on the config).
      if (biome == BIOME_BADLANDS)
        return badlandsBandAt(worldY, colHash);
      return config.subsurfaceBlock;
    }

    if (biome == BIOME_BADLANDS && density < static_cast<float>(config.subsurfaceDepth + 12))
      return badlandsBandAt(worldY, colHash);
    return deepStoneAt(worldY, cellHash);
  }

  if (worldY <= SEA_LEVEL)
  {
    if ((biome == BIOME_FROZEN_OCEAN || biome == BIOME_FROZEN_RIVER) &&
        worldY == SEA_LEVEL)
      return TextureType::ICE;
    // Frozen rivers: ice crust on the water surface in cold climates
    // (temperature raw < -0.4 matches the cold band in determineBiome).
    if (biome == BIOME_RIVER && temperature < -0.4f && worldY == SEA_LEVEL)
      return TextureType::ICE;
    return TextureType::WATER;
  }
  return TextureType::AIR;
}

// =============================================
// BORDER GENERATION
// =============================================

void TerrainGenerator::generateChunkBorders(const ChunkGenerationTarget &chunkData, int chunkX,
                                            int chunkZ)
{
  // Compact border output (issue #103): faces plus corner columns replace
  // the dense padded shell. Vertical padding (y = -1 / CHUNK_HEIGHT) is not
  // representable and never was written: border fill bounds start at
  // kBedrock = 0 and stop at CHUNK_HEIGHT.
  ChunkNeighborBorders &borders = *chunkData.borders;
  auto setBorderVoxel = [&borders](int lx, int ly, int lz, TextureType type)
  {
    if (ly < 0 || ly >= static_cast<int>(CHUNK_HEIGHT))
      return;
    const bool westSide = lx < 0;
    const bool eastSide = lx > static_cast<int>(CHUNK_SIZE) - 1;
    const bool southSide = lz < 0;
    const bool northSide = lz > static_cast<int>(CHUNK_SIZE) - 1;
    if (!westSide && !eastSide && !southSide && !northSide)
      return;
    const size_t yi = static_cast<size_t>(ly);
    const auto type8 = static_cast<uint8_t>(type);
    if (westSide || eastSide)
    {
      const size_t t = static_cast<size_t>(lz);
      if (southSide)
      {
        (westSide ? borders.cornerSW : borders.cornerSE)[yi] = type8;
        return;
      }
      if (northSide)
      {
        (westSide ? borders.cornerNW : borders.cornerNE)[yi] = type8;
        return;
      }
      (westSide ? borders.west : borders.east)[yi * CHUNK_SIZE + t] = type8;
      return;
    }
    const size_t t = static_cast<size_t>(lx);
    (southSide ? borders.south : borders.north)[yi * CHUNK_SIZE + t] = type8;
  };

  // Border strips: [localX offset, localZ offset, xSize, zSize]
  // South (lz=-1), North (lz=16), West (lx=-1), East (lx=16), plus 4 corners
  struct BorderStrip
  {
    int lxStart;
    int lzStart;
    int xSize;
    int zSize;
  };
  const BorderStrip strips[8] = {
      {0, -1, CHUNK_SIZE, 1},         // South border
      {0, CHUNK_SIZE, CHUNK_SIZE, 1}, // North border
      {-1, 0, 1, CHUNK_SIZE},         // West border
      {CHUNK_SIZE, 0, 1, CHUNK_SIZE}, // East border
      {-1, -1, 1, 1},                 // SW corner
      {CHUNK_SIZE, -1, 1, 1},         // SE corner
      {-1, CHUNK_SIZE, 1, 1},         // NW corner
      {CHUNK_SIZE, CHUNK_SIZE, 1, 1}  // NE corner
  };

  float *bCave = s_genBuffers.borderCave.data();
  float *bRavine = s_genBuffers.borderRavine.data();
  float *bSurface3D = s_genBuffers.borderSurface3D.data();

  for (int s = 0; s < 8; ++s)
  {
    const auto &strip = strips[s];
    float startX = static_cast<float>(chunkX + strip.lxStart) + NOISE_OFFSET;
    float startZ = static_cast<float>(chunkZ + strip.lzStart) + NOISE_OFFSET;

    int numColumns = strip.xSize * strip.zSize; // CHUNK_SIZE (16) or 1 array Size

    // Reuse the extended-window biomes/heights computed during `generateChunkBatch`.
    // The core chunk is at (x=6..21, z=6..21); border strips read the halo.
    int *extHeights = s_genBuffers.extHeights.data();
    BiomeType *extBiomes = s_genBuffers.extBiomes.data();
    float *temperatureResults = s_genBuffers.temperature.data();
    const int EXTENDED_SIZE = EXT_SIZE;

    // Bound strip 3D noise to max surface height on this strip (reuse ext heights).
    int stripMaxH = 1;
    for (int j = 0; j < numColumns; ++j)
    {
      int lx = strip.lxStart + (strip.xSize > 1 ? j : 0);
      int lz = strip.lzStart + (strip.zSize > 1 ? j : 0);
      int extIndex = (lz + EXT_CORE_OFFSET) * EXTENDED_SIZE + (lx + EXT_CORE_OFFSET);
      stripMaxH = std::max(stripMaxH, extHeights[extIndex]);
    }
    const ChunkYFillBounds borderBounds =
		computeChunkYFillBounds(stripMaxH, SEA_LEVEL, 16, CHUNK_HEIGHT);
    const int borderYMin = borderBounds.yMin;
    const int borderYSize = borderBounds.ySize;
    const int borderNoiseEnd = borderBounds.yNoiseEnd;
    const int borderFillEnd = borderBounds.yFillEnd;

    m_caveNoise->GenUniformGrid3D(bCave, startX, static_cast<float>(borderYMin), startZ,
                                  strip.xSize, borderYSize, strip.zSize, 1.0f, m_seed + 4000);
    m_ravineNoise->GenUniformGrid3D(bRavine, startX, static_cast<float>(borderYMin), startZ,
                                    strip.xSize, borderYSize, strip.zSize, 1.0f, m_seed + 5000);
    m_surface3DNoise->GenUniformGrid3D(bSurface3D, startX, static_cast<float>(borderYMin), startZ,
                                       strip.xSize, borderYSize, strip.zSize, 1.0f, m_seed + 6000);
    const int borderUndergroundEnd =
        std::min(borderNoiseEnd, UNDERGROUND_NOISE_MAX_Y);

    for (int j = 0; j < numColumns; ++j)
    {
      int lx = strip.lxStart + (strip.xSize > 1 ? j : 0);
      int lz = strip.lzStart + (strip.zSize > 1 ? j : 0);

      int extX = lx + EXT_CORE_OFFSET;
      int extZ = lz + EXT_CORE_OFFSET;
      int extIndex = extZ * EXTENDED_SIZE + extX;

      int height = extHeights[extIndex];
      BiomeType biome = extBiomes[extIndex];
      float temperature = std::clamp(temperatureResults[extIndex], -1.0f, 1.0f);
      float columnSlope = columnSlopeAt(extHeights, EXTENDED_SIZE, extIndex);
      const float perturbAmp = getBiomeConfig(biome).surfacePerturbAmp;

      for (int y = borderYMin; y < borderFillEnd; ++y)
      {
        const bool inNoiseBand = (y >= borderYMin && y < borderNoiseEnd);
        int noiseX = (strip.xSize > 1) ? j : 0;
        int noiseZ = (strip.zSize > 1) ? j : 0;
        int localY = y - borderYMin;
        int noiseIdx = noiseZ * (borderYSize * strip.xSize) + localY * strip.xSize + noiseX;
        const bool hasUndergroundNoise =
            y >= borderYMin && y < borderUndergroundEnd;

        float density = static_cast<float>(height - y);
        if (inNoiseBand && perturbAmp > 0.0f && density > -8.0f && density < 12.0f) {
           float distToSurface = std::abs(density);
           float blend = std::max(0.0f, 1.0f - distToSurface / 12.0f);
           density += bSurface3D[noiseIdx] * perturbAmp * blend;
        }

        TextureType type;
        if (density >= 0.0f) {
            type = getVoxelTypeAt(chunkX + lx, y, chunkZ + lz, height, biome, temperature, columnSlope, density, s_genBuffers.extSurfaceBiomes[extIndex]);

            if (inNoiseBand && type != TextureType::AIR && type != TextureType::BEDROCK && type != TextureType::WATER)
            {
              if (shouldCarveCave(
                      y, height, bCave[noiseIdx], bRavine[noiseIdx],
                      hasUndergroundNoise
                          ? sampleCoarseUnderground(
                                s_genBuffers.spaghettiA.data(), lx, y, lz)
                          : 0.0f,
                      hasUndergroundNoise
                          ? sampleCoarseUnderground(
                                s_genBuffers.spaghettiB.data(), lx, y, lz)
                          : 0.0f,
                      hasUndergroundNoise
                          ? sampleCoarseUnderground(
                                s_genBuffers.cavern.data(), lx, y, lz)
                          : 0.0f,
                      hasUndergroundNoise))
              {
                type = caveFillAt(y, height, biome,
                                  s_genBuffers.aquifer[extIndex]);
              }
            }
        } else {
            type = getVoxelTypeAt(chunkX + lx, y, chunkZ + lz, height, biome, temperature, columnSlope, density, s_genBuffers.extSurfaceBiomes[extIndex]);
        }

        if (type != TextureType::AIR)
          setBorderVoxel(lx, y, lz, type);
      }
    }
  }
}

float TerrainGenerator::smoothstep(float edge0, float edge1, float x) const
{
  float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
