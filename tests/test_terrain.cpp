// Terrain generation: determinism, cross-chunk border consistency, world invariants.
// Also provides an opt-in biome histogram calibration tool:
//   ./test_terrain --histogram [size] [step]   (default 2048 blocks, step 4)
#include <Chunk/TerrainGenerator.hpp>
#include <Renderer/MinecraftTextures.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

static int g_fails = 0;

#define CHECK(cond, msg)                                                       \
	do                                                                         \
	{                                                                          \
		if (!(cond))                                                           \
		{                                                                      \
			std::cerr << "FAIL: " << msg << " (" << __LINE__ << ")\n";         \
			++g_fails;                                                         \
		}                                                                      \
	} while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int borderIndex(int lx, int y, int lz)
{
	return (y + 1) * 18 * 18 + (lz + 1) * 18 + (lx + 1);
}

static int voxelIndex(int x, int y, int z)
{
	return y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x;
}

/// Blocks a chunk may place locally on top of shared terrain: vegetation
/// (trunks/canopies/cacti/spikes/bushes/boulders/fallen logs) and ore veins.
/// These are chunk-seeded and intentionally NOT part of the deterministic
/// border strips, so border vs neighbor-column comparisons must treat them
/// as overwrites.
static bool isChunkLocalFeature(uint8_t raw)
{
	switch (static_cast<TextureType>(raw))
	{
	case OAK_LOG:
	case BIRCH_LOG:
	case SPRUCE_LOG:
	case JUNGLE_LOG:
	case ACACIA_LOG:
	case DARK_OAK_LOG:
	case OAK_LEAVES:
	case BIRCH_LEAVES:
	case SPRUCE_LEAVES:
	case JUNGLE_LEAVES:
	case ACACIA_LEAVES:
	case DARK_OAK_LEAVES:
	case CHERRY_LOG:
	case CHERRY_LEAVES:
	case MANGROVE_LOG:
	case MANGROVE_ROOTS:
	case MANGROVE_LEAVES:
	case BAMBOO_BLOCK:
	case BAMBOO_STALK:
	case RED_MUSHROOM_BLOCK:
	case BROWN_MUSHROOM_BLOCK:
	case MUSHROOM_STEM:
	case TUBE_CORAL_BLOCK:
	case BRAIN_CORAL_BLOCK:
	case BUBBLE_CORAL_BLOCK:
	case FIRE_CORAL_BLOCK:
	case HORN_CORAL_BLOCK:
	case CACTUS:
	case COBBLESTONE:		// boulders (terrain never generates cobble)
	case MOSSY_COBBLESTONE: // mossy boulders
	case PACKED_ICE:		// ice spikes (also terrain in ICE_SPIKES, but that matches anyway)
	case BLUE_ICE:
	case COAL_ORE:
	case COPPER_ORE:
	case DIAMOND_ORE:
	case EMERALD_ORE:
	case GOLD_ORE:
	case IRON_ORE:
	case LAPIS_ORE:
	case REDSTONE_ORE:
		return true;
	default:
		return false;
	}
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

static void expectChunkDataEqual(const ChunkData &a, const ChunkData &b, const char *label)
{
	CHECK(a.voxels.size() == b.voxels.size(), label);
	CHECK(std::memcmp(a.voxels.data(), b.voxels.data(),
					  a.voxels.size() * sizeof(Voxel)) == 0,
		  "voxels differ");
	CHECK(std::memcmp(a.borderVoxels.data(), b.borderVoxels.data(),
					  a.borderVoxels.size() * sizeof(uint8_t)) == 0,
		  "border voxels differ");
	CHECK(std::memcmp(a.biomes.data(), b.biomes.data(),
					  a.biomes.size() * sizeof(BiomeType)) == 0,
		  "biomes differ");
	CHECK(std::memcmp(a.heightMap.data(), b.heightMap.data(),
					  a.heightMap.size() * sizeof(int)) == 0,
		  "heightMap differs");
	CHECK(std::memcmp(a.grassColors.data(), b.grassColors.data(),
					  a.grassColors.size() * sizeof(uint32_t)) == 0,
		  "grassColors differ");
	CHECK(std::memcmp(a.foliageColors.data(), b.foliageColors.data(),
					  a.foliageColors.size() * sizeof(uint32_t)) == 0,
		  "foliageColors differ");
}

static void testDeterminismSameSeed()
{
	constexpr int kSeed = 4242;
	// Two independent generator instances must produce byte-identical chunks.
	TerrainGenerator genA(kSeed);
	TerrainGenerator genB(kSeed);

	const int coords[][2] = {
		{0, 0},
		{160, -80},
		{-320, 4000},
		{1024, 1024},
	};
	for (const auto &c : coords)
	{
		ChunkData a = genA.generateChunk(c[0], c[1]);
		ChunkData b = genB.generateChunk(c[0], c[1]);
		expectChunkDataEqual(a, b, "same-seed determinism");
	}

	// Order independence: regenerating a chunk after others must give the same
	// result (guards against thread-local scratch buffer aliasing).
	ChunkData first = genA.generateChunk(0, 0);
	genA.generateChunk(160, -80);
	genA.generateChunk(-320, 4000);
	ChunkData second = genA.generateChunk(0, 0);
	expectChunkDataEqual(first, second, "generation order independence");

	std::cout << "determinism: same seed -> identical ChunkData on 4 chunks\n";
}

static void testDifferentSeedsDiffer()
{
	TerrainGenerator genA(1);
	TerrainGenerator genB(2);
	ChunkData a = genA.generateChunk(0, 0);
	ChunkData b = genB.generateChunk(0, 0);
	CHECK(std::memcmp(a.voxels.data(), b.voxels.data(),
					  a.voxels.size() * sizeof(Voxel)) != 0,
		  "different seeds should produce different terrain");
}

// ---------------------------------------------------------------------------
// Border consistency (deterministic terrain shells across chunk edges)
// ---------------------------------------------------------------------------

struct EdgeStats
{
	int compared = 0;
	int skips = 0;
};

/// Compare one owner shell edge against the matching neighbor column/row.
/// varyZ: edge runs along Z (E/W faces) when true, along X (S/N faces) otherwise.
static EdgeStats checkEdge(const ChunkData &owner, int shellLx, int shellLz,
						   const ChunkData &nbr, int nx, int nz,
						   bool varyZ, const char *label)
{
	EdgeStats stats;
	int reported = 0;
	for (int y = 0; y < CHUNK_HEIGHT; ++y)
	{
		for (int i = 0; i < CHUNK_SIZE; ++i)
		{
			const int lx = varyZ ? shellLx : i;
			const int lz = varyZ ? i : shellLz;
			const uint8_t expected = owner.borderVoxels[borderIndex(lx, y, lz)];
			const uint8_t actual = varyZ ? nbr.voxels[voxelIndex(nx, y, i)].type
										 : nbr.voxels[voxelIndex(i, y, nz)].type;
			++stats.compared;
			if (expected == actual)
				continue;
			if (isChunkLocalFeature(actual))
			{
				++stats.skips; // neighbor-local vegetation/ore overwrote terrain
				continue;
			}
			if (reported < 5)
			{
				std::cerr << "FAIL: " << label << " mismatch at y=" << y << " i=" << i
						  << " shell=" << static_cast<int>(expected)
						  << " neighbor=" << static_cast<int>(actual) << "\n";
				++reported;
			}
			++g_fails;
		}
	}
	return stats;
}

static void testBorderConsistency()
{
	constexpr int kSeed = 777;
	TerrainGenerator gen(kSeed);

	// 2x2 neighborhood (block-space origins)
	ChunkData A = gen.generateChunk(0, 0);
	ChunkData B = gen.generateChunk(CHUNK_SIZE, 0);	 // East of A
	ChunkData C = gen.generateChunk(0, CHUNK_SIZE);	 // North of A
	ChunkData D = gen.generateChunk(CHUNK_SIZE, CHUNK_SIZE); // NE diagonal

	int totalSkips = 0;
	int totalCompared = 0;
	auto acc = [&](EdgeStats s) {
		totalCompared += s.compared;
		totalSkips += s.skips;
	};

	// A.east (lx=16) vs B column x=0 ; B.west (lx=-1) vs A column x=15
	acc(checkEdge(A, CHUNK_SIZE, 0, B, 0, 0, true, "A.east==B.x0"));
	acc(checkEdge(B, -1, 0, A, CHUNK_SIZE - 1, 0, true, "B.west==A.x15"));
	// A.north (lz=16) vs C row z=0 ; C.south (lz=-1) vs A row z=15
	acc(checkEdge(A, 0, CHUNK_SIZE, C, 0, 0, false, "A.north==C.z0"));
	acc(checkEdge(C, 0, -1, A, 0, CHUNK_SIZE - 1, false, "C.south==A.z15"));
	// B.north vs D row z=0 ; C.east vs D column x=0
	acc(checkEdge(B, 0, CHUNK_SIZE, D, 0, 0, false, "B.north==D.z0"));
	acc(checkEdge(C, CHUNK_SIZE, 0, D, 0, 0, true, "C.east==D.x0"));
	// Corner: A NE corner shell cell (lx=16, lz=16) vs D column (x=0, z=0)
	{
		EdgeStats s;
		for (int y = 0; y < CHUNK_HEIGHT; ++y)
		{
			const uint8_t expected = A.borderVoxels[borderIndex(CHUNK_SIZE, y, CHUNK_SIZE)];
			const uint8_t actual = D.voxels[voxelIndex(0, y, 0)].type;
			++s.compared;
			if (expected == actual)
				continue;
			if (isChunkLocalFeature(actual))
			{
				++s.skips;
				continue;
			}
			CHECK(false, "A.cornerNE==D.(0,0) mismatch");
		}
		acc(s);
	}

	// Vegetation/ore skips must stay rare; everything else matched exactly.
	CHECK(totalSkips < totalCompared / 20,
		  "too many vegetation/ore overwrites on shared edges (<5% expected)");
	std::cout << "borders: " << totalCompared << " cells compared, " << totalSkips
			  << " vegetation/ore skips\n";
}

// ---------------------------------------------------------------------------
// Cross-chunk vegetation: trunks may root in border columns (previously
// impossible — placers refused any trunk within canopyMargin of the edge,
// leaving tree-free bands along every chunk border).
// ---------------------------------------------------------------------------

static bool isLogType(uint8_t raw)
{
	switch (static_cast<TextureType>(raw))
	{
	case OAK_LOG:
	case BIRCH_LOG:
	case SPRUCE_LOG:
	case JUNGLE_LOG:
	case ACACIA_LOG:
	case DARK_OAK_LOG:
		return true;
	default:
		return false;
	}
}

static void testBorderTrunks()
{
	// Seed 7 has dense forest near the origin (Phase 2 terrain); scan 8x8
	// chunks and look for trunks rooted in border columns (x = 0 or 15) —
	// a vertical run of >= 4 logs.
	int trunkColumns = 0;
	TerrainGenerator gen(7);
	for (int cx = 0; cx < 8 && trunkColumns < 4; ++cx)
	{
		for (int cz = 0; cz < 8 && trunkColumns < 4; ++cz)
		{
			ChunkData data = gen.generateChunk(cx * CHUNK_SIZE, cz * CHUNK_SIZE);
			for (int z = 0; z < CHUNK_SIZE && trunkColumns < 4; ++z)
			{
				for (int x : {0, CHUNK_SIZE - 1})
				{
					int run = 0;
					for (int y = 0; y < CHUNK_HEIGHT; ++y)
					{
						run = isLogType(data.voxels[voxelIndex(x, y, z)].type) ? run + 1 : 0;
						if (run >= 4)
						{
							++trunkColumns;
							break;
						}
					}
				}
			}
		}
	}
	CHECK(trunkColumns >= 4, "trees can root in border columns (cross-chunk canopies)");
	std::cout << "cross-chunk vegetation: " << trunkColumns << " border trunk columns found\n";
}

// ---------------------------------------------------------------------------
// Phase 2 rivers: the biome channel has a stable two-block water column.
// ---------------------------------------------------------------------------

static void testFlatRiverChannels()
{
	constexpr int kSeed = 1337;
	constexpr int kRiverbedY = TerrainGenerator::SEA_LEVEL - 2;
	int riverColumns = 0;
	TerrainGenerator gen(kSeed);

	for (int cx = -4; cx < 4; ++cx)
	{
		for (int cz = -4; cz < 4; ++cz)
		{
			ChunkData data = gen.generateChunk(cx * CHUNK_SIZE, cz * CHUNK_SIZE);
			for (int z = 0; z < CHUNK_SIZE; ++z)
			{
				for (int x = 0; x < CHUNK_SIZE; ++x)
				{
					const int column = z * CHUNK_SIZE + x;
					if (data.biomes[column] != BIOME_RIVER &&
						data.biomes[column] != BIOME_FROZEN_RIVER)
						continue;

					++riverColumns;
					CHECK(data.heightMap[column] <= kRiverbedY,
						  "river solid surface does not rise into the two-block channel");

					const auto belowSurface = static_cast<TextureType>(
						data.voxels[voxelIndex(x, TerrainGenerator::SEA_LEVEL - 1, z)].type);
					const auto surface = static_cast<TextureType>(
						data.voxels[voxelIndex(x, TerrainGenerator::SEA_LEVEL, z)].type);
					CHECK(belowSurface == WATER,
						  "river channel contains water below the surface");
					CHECK(surface == WATER || surface == ICE,
						  "river surface is water or cold-climate ice");
				}
			}
		}
	}

	CHECK(riverColumns >= 32, "river regression sample contains enough river columns");
	std::cout << "flat rivers: " << riverColumns
			  << " two-block channel columns validated\n";
}

static void testBiomeRegistry()
{
	for (int i = 0; i < BIOME_COUNT; ++i)
	{
		const auto biome = static_cast<BiomeType>(i);
		const BiomeConfig &config = TerrainGenerator::getBiomeConfig(biome);
		CHECK(biomeTypeString[i] != nullptr && biomeTypeString[i][0] != '\0',
			  "every biome has a display name");
		CHECK(config.surfaceBlock >= BEDROCK && config.surfaceBlock < COUNT,
			  "biome surface block is a valid texture");
		CHECK(config.subsurfaceBlock >= BEDROCK && config.subsurfaceBlock < COUNT,
			  "biome subsurface block is a valid texture");
		CHECK(config.underwaterBlock >= BEDROCK && config.underwaterBlock < COUNT,
			  "biome underwater block is a valid texture");
		CHECK(config.subsurfaceDepth > 0, "biome subsurface depth is positive");
		CHECK(config.treeDensity >= 0.0f && config.treeDensity <= 1.0f,
			  "biome tree density is normalized");
		CHECK(config.bushDensity >= 0.0f && config.bushDensity <= 1.0f,
			  "biome bush density is normalized");
		CHECK(config.rockDensity >= 0.0f && config.rockDensity <= 1.0f,
			  "biome rock density is normalized");
		CHECK(config.fallenLogDensity >= 0.0f && config.fallenLogDensity <= 1.0f,
			  "biome fallen-log density is normalized");
		CHECK(config.surfacePerturbAmp >= 0.0f,
			  "biome surface perturbation is non-negative");
	}
	std::cout << "biome registry: " << BIOME_COUNT
			  << " append-only entries have valid configs\n";
}

static void testPhase3BiomePresence()
{
	std::array<long long, BIOME_COUNT> counts{};
	constexpr int kRegionSize = 512;
	constexpr float kStep = 16.0f;
	constexpr int kSeeds[] = {1337, 4242, 9001};
	std::vector<BiomeType> region;

	for (int seed : kSeeds)
	{
		TerrainGenerator gen(seed);
		gen.getBiomeRegion(0.0f, 0.0f, kStep, kRegionSize, kRegionSize, region);
		for (BiomeType biome : region)
		{
			const int index = static_cast<int>(biome);
			if (index >= 0 && index < BIOME_COUNT)
				++counts[static_cast<size_t>(index)];
		}
	}

	constexpr BiomeType kPhase3Biomes[] = {
		BIOME_FLOWER_MEADOW,  BIOME_CHERRY_GROVE,   BIOME_AUTUMN_FOREST,
		BIOME_REDWOOD_FOREST, BIOME_MANGROVE_SWAMP, BIOME_BAMBOO_JUNGLE,
		BIOME_MOOR,           BIOME_GLACIER,         BIOME_FROZEN_RIVER,
		BIOME_VOLCANIC,       BIOME_OASIS,           BIOME_MUSHROOM_FIELDS,
		BIOME_CORAL_REEF,
	};
	for (BiomeType biome : kPhase3Biomes)
	{
		CHECK(counts[static_cast<size_t>(biome)] > 0,
			  "every Phase 3 biome is reachable across calibration seeds");
	}

	constexpr BiomeType kOrdinaryPhase3Biomes[] = {
		BIOME_FLOWER_MEADOW, BIOME_CHERRY_GROVE, BIOME_AUTUMN_FOREST,
		BIOME_REDWOOD_FOREST, BIOME_MANGROVE_SWAMP, BIOME_BAMBOO_JUNGLE,
		BIOME_MOOR, BIOME_FROZEN_RIVER,
	};
	constexpr long long kTotalSamples =
		static_cast<long long>(kRegionSize) * kRegionSize * std::size(kSeeds);
	constexpr long long kMinimumOrdinarySamples =
		static_cast<long long>(static_cast<double>(kTotalSamples) * 0.0045);
	for (BiomeType biome : kOrdinaryPhase3Biomes)
	{
		CHECK(counts[static_cast<size_t>(biome)] >= kMinimumOrdinarySamples,
			  "ordinary Phase 3 biome has a meaningful calibration sample");
	}

	constexpr BiomeType kBalancedLegacyBiomes[] = {
		BIOME_DESERT, BIOME_BADLANDS, BIOME_JUNGLE,
		BIOME_MOUNTAINS, BIOME_SNOWY_MOUNTAINS,
	};
	for (BiomeType biome : kBalancedLegacyBiomes)
	{
		CHECK(counts[static_cast<size_t>(biome)] >= kMinimumOrdinarySamples,
			  "ordinary legacy biome remains represented after Phase 3 balancing");
	}

	std::cout << "phase 3 biome selection: all 13 additions reachable across "
			  << std::size(kSeeds) << " seeds\n";
}

static void testPhase3FeatureBlocks()
{
	struct FeatureExpectation
	{
		BiomeType biome;
		const char *label;
		std::array<TextureType, 5> blocks;
	};

	constexpr FeatureExpectation kExpectations[] = {
		{BIOME_CHERRY_GROVE, "cherry trees",
		 {CHERRY_LOG, CHERRY_LEAVES, COUNT, COUNT, COUNT}},
		{BIOME_REDWOOD_FOREST, "redwoods",
		 {SPRUCE_LOG, SPRUCE_LEAVES, COUNT, COUNT, COUNT}},
		{BIOME_MANGROVE_SWAMP, "mangroves",
		 {MANGROVE_LOG, MANGROVE_ROOTS, MANGROVE_LEAVES, COUNT, COUNT}},
		{BIOME_BAMBOO_JUNGLE, "bamboo",
		 {BAMBOO_STALK, BAMBOO_BLOCK, COUNT, COUNT, COUNT}},
		{BIOME_OASIS, "palms",
		 {JUNGLE_LOG, JUNGLE_LEAVES, COUNT, COUNT, COUNT}},
		{BIOME_MUSHROOM_FIELDS, "giant mushrooms",
		 {MUSHROOM_STEM, RED_MUSHROOM_BLOCK, BROWN_MUSHROOM_BLOCK, COUNT, COUNT}},
		{BIOME_CORAL_REEF, "coral heads",
		 {TUBE_CORAL_BLOCK, BRAIN_CORAL_BLOCK, BUBBLE_CORAL_BLOCK,
		  FIRE_CORAL_BLOCK, HORN_CORAL_BLOCK}},
		{BIOME_VOLCANIC, "volcanic surface",
		 {BASALT, BLACKSTONE, MAGMA, COUNT, COUNT}},
	};

	constexpr int kRegionSize = 512;
	constexpr int kHalfRegion = kRegionSize / 2;
	constexpr int kStep = CHUNK_SIZE;
	TerrainGenerator gen(1337);
	std::vector<BiomeType> region;
	gen.getBiomeRegion(0.0f, 0.0f, static_cast<float>(kStep),
					   kRegionSize, kRegionSize, region);

	for (const FeatureExpectation &expectation : kExpectations)
	{
		bool found = false;
		int generatedChunks = 0;
		for (int z = 0; z < kRegionSize && !found && generatedChunks < 64; ++z)
		{
			for (int x = 0; x < kRegionSize && !found && generatedChunks < 64; ++x)
			{
				if (region[static_cast<size_t>(z) * kRegionSize + x] != expectation.biome)
					continue;

				const int worldX = (x - kHalfRegion) * kStep;
				const int worldZ = (z - kHalfRegion) * kStep;
				ChunkData data = gen.generateChunk(worldX, worldZ);
				++generatedChunks;
				for (const Voxel &voxel : data.voxels)
				{
					const TextureType type = static_cast<TextureType>(voxel.type);
					for (TextureType expected : expectation.blocks)
					{
						if (expected != COUNT && type == expected)
						{
							found = true;
							break;
						}
					}
					if (found)
						break;
				}
			}
		}
		CHECK(found, expectation.label);
	}

	std::cout << "phase 3 features: dedicated trees, bamboo, mushrooms, coral, "
				 "and volcanic blocks generated\n";
}

static void testOasisPonds()
{
	constexpr int kRegionSize = 512;
	constexpr int kHalfRegion = kRegionSize / 2;
	TerrainGenerator gen(1337);
	std::vector<BiomeType> region;
	gen.getBiomeRegion(0.0f, 0.0f, static_cast<float>(CHUNK_SIZE),
					   kRegionSize, kRegionSize, region);

	bool foundPond = false;
	int generatedChunks = 0;
	for (int z = 0; z < kRegionSize && !foundPond && generatedChunks < 64; ++z)
	{
		for (int x = 0; x < kRegionSize && !foundPond && generatedChunks < 64; ++x)
		{
			if (region[static_cast<size_t>(z) * kRegionSize + x] != BIOME_OASIS)
				continue;

			ChunkData data = gen.generateChunk((x - kHalfRegion) * CHUNK_SIZE,
											  (z - kHalfRegion) * CHUNK_SIZE);
			++generatedChunks;
			for (int localZ = 0; localZ < CHUNK_SIZE && !foundPond; ++localZ)
			{
				for (int localX = 0; localX < CHUNK_SIZE; ++localX)
				{
					const int column = localZ * CHUNK_SIZE + localX;
					if (data.biomes[column] != BIOME_OASIS)
						continue;
					const TextureType surface = static_cast<TextureType>(
						data.voxels[voxelIndex(localX, TerrainGenerator::SEA_LEVEL,
											 localZ)]
							.type);
					if (surface == WATER &&
						data.heightMap[column] <= TerrainGenerator::SEA_LEVEL - 2)
					{
						foundPond = true;
						break;
					}
				}
			}
		}
	}

	CHECK(foundPond, "oasis has a shallow water core surrounded by palm terrain");
	std::cout << "oasis: deterministic shallow pond core validated\n";
}

// ---------------------------------------------------------------------------
// World invariants
// ---------------------------------------------------------------------------

static void testWorldInvariants()
{
	constexpr int kSeed = 9001;
	TerrainGenerator gen(kSeed);

	const int coords[][2] = {
		{0, 0},
		{512, -768},
		{-2048, 1024},
	};
	for (const auto &c : coords)
	{
		ChunkData data = gen.generateChunk(c[0], c[1]);

		for (int z = 0; z < CHUNK_SIZE; ++z)
		{
			for (int x = 0; x < CHUNK_SIZE; ++x)
			{
				// Bedrock floor
				for (int y = 0; y <= TerrainGenerator::BEDROCK_LEVEL; ++y)
				{
					CHECK(data.voxels[voxelIndex(x, y, z)].type == BEDROCK,
						  "bedrock floor missing");
				}

				const int col = z * CHUNK_SIZE + x;
				CHECK(data.heightMap[col] >= 1 && data.heightMap[col] < CHUNK_HEIGHT,
					  "heightMap within [1, CHUNK_HEIGHT)");
				CHECK(static_cast<int>(data.biomes[col]) >= 0 &&
						  static_cast<int>(data.biomes[col]) < BIOME_COUNT,
					  "biome id in range");
				CHECK((data.grassColors[col] >> 24) == 255u &&
						  (data.foliageColors[col] >> 24) == 255u,
					  "packed biome colors have alpha 255");

				// No water above sea level
				for (int y = TerrainGenerator::SEA_LEVEL + 1; y < CHUNK_HEIGHT; ++y)
				{
					CHECK(data.voxels[voxelIndex(x, y, z)].type != WATER,
						  "no water above sea level");
				}
			}
		}
	}
	std::cout << "invariants: bedrock floor, water level, heightMap/biome/color ranges OK\n";
}

// ---------------------------------------------------------------------------
// Biome histogram calibration tool (opt-in, not run under ctest)
// ---------------------------------------------------------------------------

static int runHistogram(int argc, char **argv)
{
	int size = 2048; // world blocks covered along each axis
	int step = 4;	 // blocks per sample
	if (argc > 2)
		size = std::atoi(argv[2]);
	if (argc > 3)
		step = std::atoi(argv[3]);
	const int seed = argc > 4 ? std::atoi(argv[4]) : 1337;
	if (size <= 0 || step <= 0)
	{
		std::cerr << "usage: test_terrain --histogram [size] [step] [seed]\n";
		return 2;
	}

	TerrainGenerator gen(seed);
	std::array<long long, BIOME_COUNT> counts{};
	long long total = 0;

	constexpr int kTile = 256;
	const int tiles = std::max(1, size / (kTile * step));

	const auto t0 = std::chrono::steady_clock::now();
	std::vector<BiomeType> biomes;
	for (int tz = 0; tz < tiles; ++tz)
	{
		for (int tx = 0; tx < tiles; ++tx)
		{
			const float cx = (tx - tiles * 0.5f + 0.5f) * kTile * step;
			const float cz = (tz - tiles * 0.5f + 0.5f) * kTile * step;
			gen.getBiomeRegion(cx, cz, static_cast<float>(step), kTile, kTile, biomes);
			for (BiomeType b : biomes)
			{
				const int idx = static_cast<int>(b);
				if (idx >= 0 && idx < BIOME_COUNT)
					++counts[static_cast<size_t>(idx)];
				++total;
			}
		}
	}
	const auto t1 = std::chrono::steady_clock::now();
	const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

	std::cout << "Biome histogram (seed " << seed << ", " << size << "x" << size
			  << " blocks, step " << step << ", " << total << " samples, " << ms
			  << " ms)\n";
	for (int i = 0; i < BIOME_COUNT; ++i)
	{
		const double pct = total ? 100.0 * static_cast<double>(counts[static_cast<size_t>(i)]) /
									   static_cast<double>(total)
								 : 0.0;
		std::cout << "  " << biomeTypeString[i] << ": " << counts[static_cast<size_t>(i)]
				  << " (" << pct << "%)\n";
	}
	return 0;
}

static int runHeightHistogram(int argc, char **argv)
{
	const int grid = (argc > 2) ? std::max(1, std::atoi(argv[2])) : 16;
	constexpr int kSeed = 1337;
	TerrainGenerator gen(kSeed);
	std::vector<int> heights;
	heights.reserve(static_cast<size_t>(grid) * grid * CHUNK_SIZE * CHUNK_SIZE);

	const int start = -(grid / 2);
	const auto t0 = std::chrono::steady_clock::now();
	for (int cz = 0; cz < grid; ++cz)
	{
		for (int cx = 0; cx < grid; ++cx)
		{
			ChunkData data = gen.generateChunk((start + cx) * CHUNK_SIZE,
											  (start + cz) * CHUNK_SIZE);
			heights.insert(heights.end(), data.heightMap.begin(), data.heightMap.end());
		}
	}
	const auto t1 = std::chrono::steady_clock::now();
	const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

	std::sort(heights.begin(), heights.end());
	auto percentile = [&](double p) {
		const size_t index = static_cast<size_t>(
			p * static_cast<double>(heights.size() - 1));
		return heights[index];
	};

	double sum = 0.0;
	size_t alpineColumns = 0;
	for (int h : heights)
	{
		sum += static_cast<double>(h);
		alpineColumns += h >= 200 ? 1u : 0u;
	}

	const double chunks = static_cast<double>(grid) * grid;
	std::cout << "Height histogram (seed " << kSeed << ", " << grid << "x" << grid
			  << " chunks, " << heights.size() << " columns)\n"
			  << "  min/p05/p50/p95/p99/max: "
			  << heights.front() << '/' << percentile(0.05) << '/'
			  << percentile(0.50) << '/' << percentile(0.95) << '/'
			  << percentile(0.99) << '/' << heights.back() << '\n'
			  << "  mean: " << (sum / static_cast<double>(heights.size()))
			  << ", columns >= 200: "
			  << (100.0 * static_cast<double>(alpineColumns) /
				  static_cast<double>(heights.size()))
			  << "%\n"
			  << "  generation: " << ms << " ms total, " << (ms / chunks)
			  << " ms/chunk\n";
	return 0;
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
	if (argc > 1 && std::strcmp(argv[1], "--histogram") == 0)
		return runHistogram(argc, argv);
	if (argc > 1 && std::strcmp(argv[1], "--height-histogram") == 0)
		return runHeightHistogram(argc, argv);

	testDeterminismSameSeed();
	testDifferentSeedsDiffer();
	testBorderConsistency();
	testBorderTrunks();
	testFlatRiverChannels();
	testBiomeRegistry();
	testPhase3BiomePresence();
	testPhase3FeatureBlocks();
	testOasisPonds();
	testWorldInvariants();

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "PASS: terrain determinism + border consistency + world invariants\n";
	return 0;
}
