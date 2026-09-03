#include <Chunk/TerrainGenerator.hpp>
#include <Chunk/StreamHelpers.hpp>
#include <Renderer/MinecraftTextures.hpp>
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
  continentalFractal->SetGain(0.45f);
  continentalFractal->SetWeightedStrength(0.0f);

  auto continentalWarp = FastNoise::New<FastNoise::DomainWarpGradient>();
  continentalWarp->SetSource(continentalFractal);
  continentalWarp->SetWarpAmplitude(0.15f);
  continentalWarp->SetWarpFrequency(0.5f);

  // Set frequency directly on the warp node
  auto continentalScale = FastNoise::New<FastNoise::DomainScale>();
  continentalScale->SetSource(continentalWarp);
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
  pvScale->SetScale(0.01f);
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
  ridgeScale->SetScale(0.004f);
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
static constexpr int EXT_SIZE = 28;
static constexpr int EXT_CORE_OFFSET = 6;
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
  using Clock = std::chrono::steady_clock;
  const auto totalStart =
      m_activeProfile ? Clock::now() : Clock::time_point{};
  ChunkData chunkData;
  chunkData.voxels.assign(CHUNK_VOLUME, {TextureType::AIR});
  chunkData.borderVoxels.assign(18 * (CHUNK_HEIGHT + 2) * 18,
                                static_cast<uint8_t>(AIR));

  // Generate the main chunk data
  generateChunkBatch(chunkData, chunkX, chunkZ);

  // Generate vegetation (trees, cacti, etc.)
  const auto vegetationStart =
      m_activeProfile ? Clock::now() : Clock::time_point{};
  generateVegetation(chunkData, chunkX, chunkZ);
  if (m_activeProfile)
    m_activeProfile->vegetationMs +=
        std::chrono::duration<double, std::milli>(Clock::now() -
                                                  vegetationStart)
            .count();

  // Generate border voxels for mesh optimization
  const auto borderStart =
      m_activeProfile ? Clock::now() : Clock::time_point{};
  generateChunkBorders(chunkData, chunkX, chunkZ);
  if (m_activeProfile)
  {
    m_activeProfile->borderMs +=
        std::chrono::duration<double, std::milli>(Clock::now() - borderStart)
            .count();
    m_activeProfile->totalMs +=
        std::chrono::duration<double, std::milli>(Clock::now() - totalStart)
            .count();
    ++m_activeProfile->chunks;
  }

  return chunkData;
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
        buffers.river[i], buffers.weirdness[i]);
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

void TerrainGenerator::generateChunkBatch(ChunkData &chunkData, int chunkX,
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

      // Oasis patches use the strongest weirdness values as a shallow pond
      // core. The outer oasis ring remains dry and hosts palms/ground cover.
      if (biome == BIOME_OASIS)
      {
        const float weirdFactor = (weirdness + 1.0f) * 0.5f;
        if (weirdFactor > 0.82f)
          height = SEA_LEVEL - 2;
      }

      // Keep the navigable river channel at a stable two-block water depth.
      // Valley carving still shapes the banks; this post-erosion override
      // prevents thermal erosion from making the riverbed uneven.
      if (biome == BIOME_RIVER || biome == BIOME_FROZEN_RIVER)
      {
        height = SEA_LEVEL - 2;
      }

      // Erosion-modulated terraces. Badlands always retain a strong mesa
      // profile, while low-erosion cold highlands get broader, softer
      // plateaus. The biome was selected from the unterraced height and
      // thermal erosion has already run, so the resulting treads stay crisp.
      if (biome == BIOME_BADLANDS)
      {
        constexpr float STEP = 7.0f;
        float t = (extHeightMap[extIndex] - static_cast<float>(SEA_LEVEL)) / STEP;
        float tread = std::floor(t);
        float frac = t - tread;
        float terraced = static_cast<float>(SEA_LEVEL) +
                         (tread + smoothstep(0.65f, 1.0f, frac)) * STEP;
        const float erosionFactor = (erosion + 1.0f) * 0.5f;
        const float terraceStrength = 0.65f + 0.35f * (1.0f - erosionFactor);
        const float blended = std::lerp(extHeightMap[extIndex], terraced, terraceStrength);
        height = std::clamp(static_cast<int>(std::round(blended)), 1,
                            static_cast<int>(CHUNK_HEIGHT - HEIGHT_CEILING_MARGIN));
      }
      else if (biome == BIOME_SNOWY_TAIGA || biome == BIOME_SNOWY_MOUNTAINS ||
               biome == BIOME_GLACIER)
      {
        const float erosionFactor = (erosion + 1.0f) * 0.5f;
        const float terraceStrength =
            0.55f * smoothstep(0.60f, 0.15f, erosionFactor);
        if (terraceStrength > 0.0f)
        {
          constexpr float STEP = 10.0f;
          float t = (extHeightMap[extIndex] - static_cast<float>(SEA_LEVEL)) / STEP;
          float tread = std::floor(t);
          float frac = t - tread;
          float terraced = static_cast<float>(SEA_LEVEL) +
                           (tread + smoothstep(0.72f, 1.0f, frac)) * STEP;
          const float blended = std::lerp(extHeightMap[extIndex], terraced, terraceStrength);
          height = std::clamp(static_cast<int>(std::round(blended)), 1,
                              static_cast<int>(CHUNK_HEIGHT - HEIGHT_CEILING_MARGIN));
        }
      }

      extBiomes[extIndex] = biome;
      extHeights[extIndex] = height;

      const BiomeConfig &cfg = getBiomeConfig(biome);
      extGrass[extIndex] = packColor(cfg.grassColor);
      extFoliage[extIndex] = packColor(cfg.foliageColor);
    }
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
            type = getVoxelTypeAt(chunkX + localX, y, chunkZ + localZ, terrainHeight, biome, temperature, columnSlope, density);

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
            type = getVoxelTypeAt(chunkX + localX, y, chunkZ + localZ, terrainHeight, biome, temperature, columnSlope, density);
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
static constexpr float CONT_OCEAN_HI = 0.32f;  // Ocean -> Beach transition end
static constexpr float CONT_BEACH_LO = 0.35f;  // Beach -> Plains transition start
static constexpr float CONT_BEACH_HI = 0.42f;  // Beach -> Plains transition end
static constexpr float CONT_PLAINS_LO = 0.55f; // Plains -> Hills transition start
static constexpr float CONT_PLAINS_HI = 0.65f; // Plains -> Hills transition end
static constexpr float CONT_HILLS_LO = 0.75f;  // Hills -> Mountains transition start
static constexpr float CONT_HILLS_HI = 0.82f;  // Hills -> Mountains transition end

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

TerrainBand terrainBandFor(float continental)
{
  if (continental <= CONT_OCEAN_HI)
    return TerrainBand::Ocean;
  if (continental <= CONT_BEACH_HI)
    return TerrainBand::Coast;
  if (continental <= CONT_PLAINS_HI)
    return TerrainBand::Flatlands;
  if (continental <= CONT_HILLS_HI)
    return TerrainBand::Hills;
  return TerrainBand::Mountains;
}

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

  const TerrainBand terrainBand = terrainBandFor(contFactor);
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
  const float dynamicWidth = 0.05f + 0.06f * weird;
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

BiomeType TerrainGenerator::evaluateBiomeColumn(int worldX, int worldZ) const
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
  return evaluateBiomeAt(buffers, centerIndex);
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

  if (!grid.valid())
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
  const auto canonicalColumn = [&grid, kColumnLimit](int xi, int zi) {
    const glm::ivec2 col = grid.columnAt(xi, zi);
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
        outBiomes[static_cast<size_t>(zi) * static_cast<size_t>(grid.width) +
                  static_cast<size_t>(xi)] =
            evaluateBiomeAt(buffers, denseZ * denseW + denseX);
      }
    }
  };

  const glm::ivec2 minCol = canonicalColumn(0, 0);
  const glm::ivec2 maxCol = canonicalColumn(grid.width - 1, grid.height - 1);
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

  const size_t outputCount = static_cast<size_t>(grid.width) * static_cast<size_t>(grid.height);

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
                        0, grid.width - 1, 0, grid.height - 1);
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
  const int tileDim = std::clamp(static_cast<int>(tileDimF), 1, kMaxTileDim);

  const int tilesX = (grid.width + tileDim - 1) / tileDim;
  const int tilesZ = (grid.height + tileDim - 1) / tileDim;

  outBiomes.resize(outputCount);

  // Tiles are processed sequentially in fixed order: the result depends only
  // on the grid and the seed, never on scheduling. TerrainGenerator does not
  // spawn threads and does not decide CPU policy; the caller chooses when and
  // on which worker the whole call runs (the Engine runs the biome map as a
  // single TaskPriority::Low job).
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
      const int x1 = std::min(x0 + tileDim - 1, grid.width - 1);
      const int z1 = std::min(z0 + tileDim - 1, grid.height - 1);

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
            outBiomes[static_cast<size_t>(zi) * static_cast<size_t>(grid.width) +
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

// Base heights (blocks above/below SEA_LEVEL) and variation amplitudes per terrain band
static constexpr float OCEAN_BASE_HEIGHT = -20.0f;
static constexpr float OCEAN_VARIATION = 8.0f;
static constexpr float BEACH_BASE_HEIGHT = 1.0f;
static constexpr float BEACH_VARIATION = 3.0f;
static constexpr float PLAINS_BASE_HEIGHT = 8.0f;
static constexpr float PLAINS_VARIATION = 12.0f;
static constexpr float HILLS_BASE_HEIGHT = 25.0f;
static constexpr float HILLS_VARIATION = 20.0f;
static constexpr float MOUNTAIN_BASE_HEIGHT = 60.0f;
static constexpr float MOUNTAIN_VARIATION = 50.0f;

// Ridge-driven peak parameters
static constexpr float RIDGE_PEAK_THRESHOLD = 0.2f;   // Ridge value above which peaks form
static constexpr float RIDGE_PEAK_AMPLITUDE = 125.0f; // Max extra height from ridge peaks

// Rock exposure: post-erosion neighbor height delta above which vegetated
// surfaces turn to bare stone/gravel (cliff faces).
static constexpr float ROCKY_SLOPE_THRESHOLD = 2.0f;

// Height clamping
// static constexpr int   HEIGHT_CEILING_MARGIN =  32; // Moved up

float TerrainGenerator::calculateHeightFloat(float continental, float erosion,
                                             float peaksValleys, float ridge, float riverVal, float weirdness) const
{
  continental = std::clamp(continental, -1.0f, 1.0f);
  erosion = std::clamp(erosion, -1.0f, 1.0f);
  peaksValleys = std::clamp(peaksValleys, -1.0f, 1.0f);
  ridge = std::clamp(ridge, -1.0f, 1.0f);
  weirdness = std::clamp(weirdness, -1.0f, 1.0f);

  float continentalFactor = (continental + 1.0f) * 0.5f;
  float erosionFactor = (erosion + 1.0f) * 0.5f;
  float weirdFactor = (weirdness + 1.0f) * 0.5f;

  // Compute blending weights for each terrain band using smoothstep transitions
  float oceanWeight = 1.0f - smoothstep(CONT_OCEAN_LO, CONT_OCEAN_HI, continentalFactor);
  float beachWeight = smoothstep(CONT_OCEAN_LO, CONT_OCEAN_HI, continentalFactor) * (1.0f - smoothstep(CONT_BEACH_LO, CONT_BEACH_HI, continentalFactor));
  float plainsWeight = smoothstep(CONT_BEACH_LO, CONT_BEACH_HI, continentalFactor) * (1.0f - smoothstep(CONT_PLAINS_LO, CONT_PLAINS_HI, continentalFactor));
  float hillsWeight = smoothstep(CONT_PLAINS_LO, CONT_PLAINS_HI, continentalFactor) * (1.0f - smoothstep(CONT_HILLS_LO, CONT_HILLS_HI, continentalFactor));
  float mountainWeight = smoothstep(CONT_HILLS_LO, CONT_HILLS_HI, continentalFactor);

  // Normalize weights to ensure they sum to 1.0
  float totalWeight = oceanWeight + beachWeight + plainsWeight + hillsWeight + mountainWeight;
  if (totalWeight > 0.001f)
  {
    oceanWeight /= totalWeight;
    beachWeight /= totalWeight;
    plainsWeight /= totalWeight;
    hillsWeight /= totalWeight;
    mountainWeight /= totalWeight;
  }

  // Height contribution per terrain band
  float oceanHeight = OCEAN_BASE_HEIGHT + peaksValleys * OCEAN_VARIATION;
  float beachHeight = BEACH_BASE_HEIGHT + peaksValleys * BEACH_VARIATION * erosionFactor;
  float plainsHeight = PLAINS_BASE_HEIGHT + peaksValleys * PLAINS_VARIATION * (0.5f + erosionFactor * 0.5f);
  float hillsHeight = HILLS_BASE_HEIGHT + peaksValleys * HILLS_VARIATION * (0.6f + erosionFactor * 0.4f);

  float mountainHeight = MOUNTAIN_BASE_HEIGHT + peaksValleys * MOUNTAIN_VARIATION;
  if (ridge > RIDGE_PEAK_THRESHOLD) {
    float val = (ridge - RIDGE_PEAK_THRESHOLD) / (1.0f - RIDGE_PEAK_THRESHOLD);
    // Cubic falloff: broad massifs with sharp, narrow summit ridges
    mountainHeight += (val * val * val) * RIDGE_PEAK_AMPLITUDE;
  }

  float heightVariation = oceanWeight * oceanHeight + beachWeight * beachHeight +
                          plainsWeight * plainsHeight + hillsWeight * hillsHeight +
                          mountainWeight * mountainHeight;

  // River Valley carving
  float riverFactor = std::abs(riverVal);

  // Weirdness drives the dynamic river width so the carved valley stays aligned
  // with the RIVER biome band detected in determineBiome (same formula there).
  float dynamicWidth = 0.05f + 0.06f * weirdFactor;
  
  // We allow carving even in the ocean, so the river smoothly enters the sea
  if (riverFactor < dynamicWidth) {
      float valleyDist = riverFactor / dynamicWidth; // 0 at center, 1 at edge
      // Smooth U-shape
      float valleyShape = 1.0f - valleyDist;
      valleyShape = smoothstep(0.0f, 1.0f, valleyShape);
      
      // We subtract up to 32 blocks of height.
      // This will pull mountains down to form passes, and plains below sea level to form rivers.
      float carveAmount = valleyShape * 32.0f;
      float newHeight = heightVariation - carveAmount;

      // Do not cut land below the target two-block riverbed. The exact flat
      // channel is applied after thermal erosion once BIOME_RIVER is known.
      constexpr float RIVERBED_HEIGHT = -2.0f;
      if (heightVariation > RIVERBED_HEIGHT)
        newHeight = std::max(newHeight, RIVERBED_HEIGHT);

      heightVariation = newHeight;
  }

  return static_cast<float>(SEA_LEVEL) + heightVariation;
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

void TerrainGenerator::generateVegetation(ChunkData &chunkData, int chunkX, int chunkZ)
{
  float chunkXf = static_cast<float>(chunkX) + NOISE_OFFSET;
  float chunkZf = static_cast<float>(chunkZ) + NOISE_OFFSET;

  float *treeNoiseResults = s_genBuffers.treeNoiseResults.data();
  float *forestDensityResults = s_genBuffers.forestDensityResults.data();
  const int *extHeights = s_genBuffers.extHeights.data();
  const BiomeType *extBiomes = s_genBuffers.extBiomes.data();
  const float *temperatureResults = s_genBuffers.temperature.data();

  // Vegetation noise over the extended window (covers the cross-chunk ring).
  const float extXf = chunkXf - static_cast<float>(EXT_CORE_OFFSET);
  const float extZf = chunkZf - static_cast<float>(EXT_CORE_OFFSET);
  m_treeNoise->GenUniformGrid2D(treeNoiseResults, extXf, extZf,
                                EXT_SIZE, EXT_SIZE, 1.0f, m_seed + 10000);
  m_forestDensityNoise->GenUniformGrid2D(forestDensityResults, extXf, extZf,
                                         EXT_SIZE, EXT_SIZE, 1.0f, m_seed + 11000);

  auto extIndexAt = [](int lx, int lz) {
    return (lz + EXT_CORE_OFFSET) * EXT_SIZE + (lx + EXT_CORE_OFFSET);
  };
  auto setIfAir = [&](int lx, int y, int lz, TextureType t) {
    if (lx < 0 || lx >= CHUNK_SIZE || lz < 0 || lz >= CHUNK_SIZE || y < 0 || y >= CHUNK_HEIGHT)
      return;
    int idx = getVoxelIndex(lx, y, lz);
    if (chunkData.voxels[idx].type == static_cast<uint8_t>(AIR))
      chunkData.voxels[idx].type = t;
  };
  auto setIfWater = [&](int lx, int y, int lz, TextureType t) {
    if (lx < 0 || lx >= CHUNK_SIZE || lz < 0 || lz >= CHUNK_SIZE ||
        y < 0 || y >= CHUNK_HEIGHT)
      return;
    int idx = getVoxelIndex(lx, y, lz);
    if (chunkData.voxels[idx].type == static_cast<uint8_t>(WATER))
      chunkData.voxels[idx].type = t;
  };
  auto terrainSurfaceIsSolid = [&](int lx, int lz, int terrainHeight,
                                   BiomeType biome) {
    if (lx >= 0 && lx < CHUNK_SIZE && lz >= 0 && lz < CHUNK_SIZE)
    {
      const TextureType type = static_cast<TextureType>(
          chunkData.voxels[getVoxelIndex(lx, terrainHeight, lz)].type);
      return type != AIR && type != WATER && type != LAVA;
    }

    const float worldX =
        static_cast<float>(chunkX + lx) + NOISE_OFFSET;
    const float worldZ =
        static_cast<float>(chunkZ + lz) + NOISE_OFFSET;
    const float cave =
        m_caveNoise->GenSingle3D(worldX, static_cast<float>(terrainHeight),
                                 worldZ, m_seed + 4000);
    const float ravine =
        m_ravineNoise->GenSingle3D(worldX, static_cast<float>(terrainHeight),
                                   worldZ, m_seed + 5000);
    if (shouldCarveCave(terrainHeight, terrainHeight, cave, ravine, 0.0f,
                        0.0f, 0.0f, false))
      return false;

    const float perturbAmp = getBiomeConfig(biome).surfacePerturbAmp;
    if (perturbAmp > 0.0f)
    {
      const float surface =
          m_surface3DNoise->GenSingle3D(
              worldX, static_cast<float>(terrainHeight), worldZ,
              m_seed + 6000);
      if (surface * perturbAmp < 0.0f)
        return false;
    }
    return true;
  };

  // Candidates are evaluated in a ring of MAX_TREE_RADIUS around the core so
  // canopies/features rooted in neighbor chunks get their in-chunk voxels
  // placed identically by every overlapping chunk. All inputs come from the
  // extended window (post-erosion, pre-cave heights/biomes) and world-position
  // hashes -> fully deterministic. Core candidates inspect generated voxels;
  // halo candidates repeat the matching surface cave/perturbation decision.
  for (int localZ = -MAX_TREE_RADIUS; localZ < CHUNK_SIZE + MAX_TREE_RADIUS; ++localZ)
  {
    for (int localX = -MAX_TREE_RADIUS; localX < CHUNK_SIZE + MAX_TREE_RADIUS; ++localX)
    {
      const int extIndex = extIndexAt(localX, localZ);
      const int terrainHeight = extHeights[extIndex];
      const BiomeType biome = extBiomes[extIndex];
      const int worldX = chunkX + localX;
      const int worldZ = chunkZ + localZ;

      // Temperate oceans receive sparse kelp columns. Full cube collision and
      // alpha are acceptable under water; thinner seagrass/lily-pad geometry
      // remains deferred to a future cross-plane/flat-quad renderer.
      if (biome == BIOME_OCEAN)
      {
        const uint32_t kelpHash = treeHash(worldX, worldZ, m_seed + 1300);
        const float kelpRoll =
            static_cast<float>(kelpHash & 0xFFFFu) / 65535.0f;
        if (terrainHeight <= SEA_LEVEL - 4 && kelpRoll < 0.018f)
        {
          const int available = SEA_LEVEL - terrainHeight - 1;
          const int height = std::min(
              available, 2 + static_cast<int>((kelpHash >> 16) % 5u));
          for (int y = 1; y <= height; ++y)
          {
            setIfWater(localX, terrainHeight + y, localZ,
                       y == height ? TextureType::KELP_TOP
                                   : TextureType::KELP);
          }
        }
        continue;
      }

      // Warm shallow oceans receive deterministic coral heads. Candidate
      // evaluation uses the same cross-chunk ring as trees so one-block arms
      // remain complete at chunk boundaries.
      if (biome == BIOME_CORAL_REEF)
      {
        const uint32_t coralHash = treeHash(worldX, worldZ, m_seed + 1200);
        const float coralRoll =
            static_cast<float>(coralHash & 0xFFFFu) / 65535.0f;
        if (terrainHeight < SEA_LEVEL - 1 && coralRoll < 0.035f)
        {
          static constexpr TextureType kCorals[] = {
              TextureType::TUBE_CORAL_BLOCK, TextureType::BRAIN_CORAL_BLOCK,
              TextureType::BUBBLE_CORAL_BLOCK, TextureType::FIRE_CORAL_BLOCK,
              TextureType::HORN_CORAL_BLOCK,
          };
          const TextureType coral =
              kCorals[(coralHash >> 16) % std::size(kCorals)];
          const int height = 1 + static_cast<int>((coralHash >> 20) % 3u);
          for (int y = 1; y <= height; ++y)
            setIfWater(localX, terrainHeight + y, localZ, coral);
          if (((coralHash >> 24) & 1u) != 0u)
          {
            setIfWater(localX + 1, terrainHeight + 1, localZ, coral);
            setIfWater(localX - 1, terrainHeight + 1, localZ, coral);
            setIfWater(localX, terrainHeight + 1, localZ + 1, coral);
            setIfWater(localX, terrainHeight + 1, localZ - 1, coral);
          }
        }
        continue;
      }

      if (terrainHeight <= SEA_LEVEL || terrainHeight > 200)
        continue;

      const BiomeConfig &config = getBiomeConfig(biome);
      const bool wantsVegetation =
          config.treeDensity > 0.0f || config.hasCacti || biome == BIOME_ICE_SPIKES ||
          config.bushDensity > 0.0f || config.rockDensity > 0.0f ||
          config.fallenLogDensity > 0.0f;
      if (!wantsVegetation)
        continue;

      // Forest-cluster factor [0,1]: low values = clearing, high values = dense forest.
      float forestFactor = (forestDensityResults[extIndex] + 1.0f) * 0.5f;
      // Local variation factor [0,1]: finer variation within the forest patch.
      float localFactor = (treeNoiseResults[extIndex] + 1.0f) * 0.5f;

      // Combined probability: biome density modulated by both noise layers.
      float treeProbability = config.treeDensity * forestFactor * localFactor;

      // Surface block at that column: getVoxelTypeAt is pure, so the value
      // matches what the terrain pass computed (same in every chunk).
      const float temperature = std::clamp(temperatureResults[extIndex], -1.0f, 1.0f);
      const float columnSlope = columnSlopeAt(extHeights, EXT_SIZE, extIndex);
      const TextureType surfaceType =
          getVoxelTypeAt(worldX, terrainHeight, worldZ, terrainHeight, biome,
                         temperature, columnSlope, 0.0f);

      if (!blockIsPlantableSurface(surfaceType))
        continue;

      const uint32_t hash = treeHash(worldX, worldZ, m_seed);
      const float roll = static_cast<float>(hash & 0xFFFF) / 65535.0f;

      // Ice spikes: tall packed-ice columns (independent of tree density)
      if (biome == BIOME_ICE_SPIKES)
      {
        if (roll < 0.04f)
          placeIceSpike(chunkData, localX, localZ, terrainHeight + 1, worldX, worldZ);
        continue;
      }

      if (roll < treeProbability)
      {
        if (terrainSurfaceIsSolid(localX, localZ, terrainHeight, biome))
          placeTree(chunkData, localX, localZ, terrainHeight + 1, biome, worldX,
                    worldZ);
        continue;
      }

      if (config.hasCacti && blockIsCactusGround(surfaceType))
      {
        uint32_t cHash = treeHash(worldX, worldZ, m_seed + 1);
        if (static_cast<float>(cHash & 0xFFFF) / 65535.0f < 0.015f)
        {
          placeCactus(chunkData, localX, localZ, terrainHeight + 1, worldX, worldZ);
          continue;
        }
      }

      // ---- Ground cover -------------------------------------------------
      // Bushes (species-tinted leaves, 1-2 high)
      if (config.bushDensity > 0.0f)
      {
        const uint32_t bHash = treeHash(worldX, worldZ, m_seed + 800);
        if (static_cast<float>(bHash & 0xFFFF) / 65535.0f < config.bushDensity)
        {
          const TextureType leaf = leafTypeForBiome(biome, bHash);
          setIfAir(localX, terrainHeight + 1, localZ, leaf);
          if (((bHash >> 16) & 3) == 0)
            setIfAir(localX, terrainHeight + 2, localZ, leaf);
        }
      }

      // Boulder clusters (cobble / mossy cobble in wet biomes)
      if (config.rockDensity > 0.0f)
      {
        const uint32_t rHash = treeHash(worldX, worldZ, m_seed + 700);
        if (static_cast<float>(rHash & 0xFFFF) / 65535.0f < config.rockDensity)
        {
          const bool mossy = biome == BIOME_SWAMP || biome == BIOME_JUNGLE ||
                             biome == BIOME_DARK_FOREST || biome == BIOME_SNOWY_TAIGA ||
                             biome == BIOME_SNOWY_TUNDRA ||
                             biome == BIOME_REDWOOD_FOREST ||
                             biome == BIOME_MANGROVE_SWAMP ||
                             biome == BIOME_BAMBOO_JUNGLE ||
                             biome == BIOME_MUSHROOM_FIELDS;
          const TextureType rockType = mossy ? TextureType::MOSSY_COBBLESTONE
                                             : TextureType::COBBLESTONE;
          static const int kDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
          const int extra = static_cast<int>((rHash >> 16) % 3); // 0-2 neighbors
          setIfAir(localX, terrainHeight + 1, localZ, rockType);
          for (int k = 0; k < extra; ++k)
          {
            const int dir = static_cast<int>((rHash >> (20 + k * 2)) & 3u);
            const int nx = localX + kDirs[dir][0];
            const int nz = localZ + kDirs[dir][1];
            const int nh = extHeights[extIndexAt(nx, nz)];
            if (nh > SEA_LEVEL && nh <= 200)
              setIfAir(nx, nh + 1, nz, rockType);
          }
        }
      }

      // Fallen logs (2-4 long, cardinal direction, follows terrain steps).
      // Reach is 3, so candidates are only evaluated within 3 of the core —
      // their whole length stays inside the extended window.
      if (config.fallenLogDensity > 0.0f &&
          localX >= -3 && localX <= CHUNK_SIZE + 2 &&
          localZ >= -3 && localZ <= CHUNK_SIZE + 2)
      {
        const uint32_t lHash = treeHash(worldX, worldZ, m_seed + 900);
        if (static_cast<float>(lHash & 0xFFFF) / 65535.0f < config.fallenLogDensity)
        {
          const TextureType log = logTypeForBiome(biome, lHash);
          static const int kDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
          const int len = 2 + static_cast<int>((lHash >> 16) % 3); // 2-4
          const int dir = static_cast<int>((lHash >> 20) & 3u);
          for (int k = 0; k < len; ++k)
          {
            const int lx = localX + kDirs[dir][0] * k;
            const int lz = localZ + kDirs[dir][1] * k;
            const int h = extHeights[extIndexAt(lx, lz)];
            if (h > SEA_LEVEL && h <= 200)
              setIfAir(lx, h + 1, lz, log);
          }
        }
      }
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
                                             BiomeType biome, float temperature, float slope, float density) const
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

      return surfaceBlockForBiome(biome, colHash, config, worldY);
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

void TerrainGenerator::generateChunkBorders(ChunkData &chunkData, int chunkX,
                                            int chunkZ)
{
  auto setBorderVoxel = [&](int lx, int ly, int lz, TextureType type)
  {
    if (lx >= -1 && lx <= CHUNK_SIZE && ly >= -1 && ly <= CHUNK_HEIGHT && lz >= -1 && lz <= CHUNK_SIZE)
    {
      chunkData.borderVoxels[(ly + 1) * 18 * 18 + (lz + 1) * 18 + (lx + 1)] = static_cast<uint8_t>(type);
    }
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
            type = getVoxelTypeAt(chunkX + lx, y, chunkZ + lz, height, biome, temperature, columnSlope, density);

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
            type = getVoxelTypeAt(chunkX + lx, y, chunkZ + lz, height, biome, temperature, columnSlope, density);
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
