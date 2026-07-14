// Phase D streaming helpers + terrain occupancy — drives shipped free functions.
#include <Chunk/StreamHelpers.hpp>
#include <Chunk/TerrainGenerator.hpp>

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

static int g_fails = 0;

#define CHECK(cond, msg)                                                       \
	do                                                                         \
	{                                                                          \
		if (!(cond))                                                           \
		{                                                                      \
			std::cerr << "FAIL: " << msg << "\n";                              \
			++g_fails;                                                         \
		}                                                                      \
	} while (0)

static void testLoadPriorityNearestFirst()
{
	std::vector<LoadCandidate> c = {
		{{5, 0, 0}, 100.f},
		{{1, 0, 0}, 4.f},
		{{3, 0, 0}, 50.f},
	};
	sortLoadCandidatesNearestFirst(c);
	CHECK(c[0].pos.x == 1, "nearest candidate first after sort");
	CHECK(c[1].pos.x == 3, "mid distance second");
	CHECK(c[2].pos.x == 5, "far candidate last");
}

static void testPruneFarLoads()
{
	std::vector<LoadCandidate> c = {
		{{0, 0, 0}, 0.f},
		{{20, 0, 0}, 0.f}, // world center ~ 20*16+8 = 328
	};
	const glm::vec3 cam(8.f, 0.f, 8.f); // near chunk (0,0)
	const float maxDistSq = 64.f * 64.f; // 64 blocks
	pruneLoadCandidatesByDistance(c, cam, maxDistSq);
	CHECK(c.size() == 1, "far load pruned");
	CHECK(c[0].pos.x == 0, "near load kept");
}

static void testCaveYRangeBounds()
{
	int yMin = -1, ySize = -1;
	computeCaveYRange(80, yMin, ySize, 16);
	CHECK(yMin == 0, "cave yMin is 0");
	CHECK(ySize == 80 + 16 + 1, "cave ySize = surface+margin+1");
	CHECK(ySize < CHUNK_HEIGHT, "cave range smaller than full height for mid surface");

	computeCaveYRange(250, yMin, ySize, 16);
	CHECK(yMin + ySize <= CHUNK_HEIGHT, "clamped to CHUNK_HEIGHT");
}

static void testRemainingBudget()
{
	CHECK(remainingCountBudget(10, 1.0, 4.0) == 10, "under budget keeps count");
	CHECK(remainingCountBudget(10, 4.0, 4.0) == 0, "at budget returns 0");
	CHECK(remainingCountBudget(10, 5.0, 4.0) == 0, "over budget returns 0");
	CHECK(remainingCountBudget(10, 99.0, 0.0) == 10, "maxStreamMs=0 disables cap");
}

static void testOccupancyY()
{
	std::vector<uint8_t> types(CHUNK_VOLUME, static_cast<uint8_t>(AIR));
	// Place a solid layer at y=40 only
	for (int z = 0; z < CHUNK_SIZE; ++z)
		for (int x = 0; x < CHUNK_SIZE; ++x)
			types[40 * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x] = static_cast<uint8_t>(STONE);

	int minY = -1, maxY = -1;
	CHECK(computeOccupancyY(types.data(), minY, maxY), "occupancy finds solids");
	CHECK(minY == 40 && maxY == 40, "occupancy Y span matches single layer");

	std::fill(types.begin(), types.end(), static_cast<uint8_t>(AIR));
	CHECK(!computeOccupancyY(types.data(), minY, maxY), "empty chunk has no occupancy");
}

static void testTerrainBoundedGeneration()
{
	// Generate a real chunk via shipped TerrainGenerator; solids must not appear
	// above the cave Y range derived from max surface (+margin).
	TerrainGenerator gen(424242);
	ChunkData data = gen.generateChunk(0, 0);

	int solidMax = 0;
	int solidCount = 0;
	for (int y = 0; y < CHUNK_HEIGHT; ++y)
		for (int z = 0; z < CHUNK_SIZE; ++z)
			for (int x = 0; x < CHUNK_SIZE; ++x)
			{
				const size_t idx = static_cast<size_t>(y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x);
				if (data.voxels[idx].type != static_cast<uint8_t>(AIR))
				{
					solidMax = std::max(solidMax, y);
					++solidCount;
				}
			}
	CHECK(solidCount > 0, "generated chunk has non-air voxels");

	// Trees may extend a bit; high sky should stay air.
	if (solidMax + 48 < CHUNK_HEIGHT)
	{
		const int highY = solidMax + 48;
		for (int z = 0; z < CHUNK_SIZE; ++z)
			for (int x = 0; x < CHUNK_SIZE; ++x)
			{
				const size_t idx = static_cast<size_t>(highY * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x);
				CHECK(data.voxels[idx].type == static_cast<uint8_t>(AIR),
					  "high sky remains AIR (bounded generation)");
			}
	}
}

static void testChunkYFillBoundsCoversSeaLevel()
{
	// Exact production helper: when chunk max surface is deep, noise ends at/before
	// SEA_LEVEL and fill must extend past noise (otherwise SEA_LEVEL is never written).
	const int sea = TerrainGenerator::SEA_LEVEL;
	const int margin = 16;
	const int maxSurface = 47; // deep seafloor
	const ChunkYFillBounds b = computeChunkYFillBounds(maxSurface, sea, margin, CHUNK_HEIGHT);
	CHECK(b.yNoiseEnd == maxSurface + margin + 1, "yNoiseEnd = surface+margin+1");
	CHECK(b.yNoiseEnd <= sea, "deep surface: noise ends at or before SEA_LEVEL");
	CHECK(b.yFillEnd == sea + 1, "fill exclusive end is SEA_LEVEL+1");
	CHECK(b.yFillEnd > b.yNoiseEnd, "fill must exceed noise (yFillEnd is required)");
}

/// Regression against clipping ocean fill to yNoiseEnd.
///
/// TerrainGenerator uses **chunk-wide** maxSurfaceHeight for computeChunkYFillBounds.
/// Pin seed=99991, cx=32, cz=25: chunk maxSolid≈47 so yNoiseEnd=64 (exclusive loop
/// max y=63). SEA_LEVEL=64 is only filled when yFillEnd = computeColumnFillEnd(...).
/// Setting yFillEnd = yNoiseEnd alone leaves AIR at y=64 → this test fails.
static void testOceanWaterNotClippedByCaveYBound()
{
	const int sea = TerrainGenerator::SEA_LEVEL;
	const int margin = 16;
	constexpr int kSeed = 99991;
	constexpr int kCx = 32;
	constexpr int kCz = 25;

	TerrainGenerator gen(kSeed);
	ChunkData data = gen.generateChunk(kCx, kCz);

	// Chunk-wide max solid (non-air, non-water) — same class of input as maxSurfaceHeight.
	int maxSolid = 0;
	for (int y = 0; y < CHUNK_HEIGHT; ++y)
	{
		for (int z = 0; z < CHUNK_SIZE; ++z)
		{
			for (int x = 0; x < CHUNK_SIZE; ++x)
			{
				const size_t idx = static_cast<size_t>(y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x);
				const uint8_t t = data.voxels[idx].type;
				if (t != static_cast<uint8_t>(AIR) && t != static_cast<uint8_t>(WATER) &&
					t != static_cast<uint8_t>(GLASS))
					maxSolid = std::max(maxSolid, y);
			}
		}
	}

	const ChunkYFillBounds bounds = computeChunkYFillBounds(maxSolid, sea, margin, CHUNK_HEIGHT);
	std::cout << "  pin seed=" << kSeed << " cx=" << kCx << " cz=" << kCz
			  << " maxSolid=" << maxSolid << " yNoiseEnd=" << bounds.yNoiseEnd
			  << " yFillEnd=" << bounds.yFillEnd << "\n";

	CHECK(maxSolid > 0, "pinned pure-deep ocean chunk has solids");
	CHECK(bounds.yNoiseEnd <= sea,
		  "pinned chunk: chunk-wide yNoiseEnd must be <= SEA_LEVEL (else fill==noise still covers sea)");
	CHECK(bounds.yFillEnd > bounds.yNoiseEnd,
		  "pinned chunk: production yFillEnd must exceed yNoiseEnd");

	int oceanCols = 0;
	int oceanOk = 0;
	int oceanMissing = 0;
	for (int z = 0; z < CHUNK_SIZE; ++z)
	{
		for (int x = 0; x < CHUNK_SIZE; ++x)
		{
			const BiomeType colBiome = data.biomes[static_cast<size_t>(z * CHUNK_SIZE + x)];
			if (colBiome != BIOME_OCEAN && colBiome != BIOME_FROZEN_OCEAN)
				continue;
			++oceanCols;
			const size_t seaIdx =
				static_cast<size_t>(sea * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x);
			const uint8_t seaT = data.voxels[seaIdx].type;
			// WATER, or GLASS ice on frozen ocean surface.
			if (seaT == static_cast<uint8_t>(WATER) || seaT == static_cast<uint8_t>(GLASS))
				++oceanOk;
			else
				++oceanMissing;
		}
	}

	CHECK(oceanCols > 0, "pinned chunk has ocean biome columns");
	CHECK(oceanMissing == 0,
		  "ocean columns must have WATER/ice at SEA_LEVEL (fails if yFillEnd=yNoiseEnd)");
	CHECK(oceanOk == oceanCols, "all ocean columns filled at sea level");
	std::cout << "  ocean columns: " << oceanCols << " water/ice at SEA_LEVEL: " << oceanOk << "\n";
}

int main()
{
	testLoadPriorityNearestFirst();
	testPruneFarLoads();
	testCaveYRangeBounds();
	testChunkYFillBoundsCoversSeaLevel();
	testRemainingBudget();
	testOccupancyY();
	testTerrainBoundedGeneration();
	testOceanWaterNotClippedByCaveYBound();

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "PASS: stream optimization helpers + bounded terrain gen + ocean water\n";
	return 0;
}
