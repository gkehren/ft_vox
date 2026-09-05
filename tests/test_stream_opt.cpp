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

static void testFrontBiasedLoadDistance()
{
	const glm::vec3 cam(0.f, 0.f, 0.f);
	const glm::vec2 fwd(1.f, 0.f); // looking +X
	const float bias = 0.30f;

	// Front chunk counts as closer, behind chunk as farther, side unchanged.
	const float front = biasedLoadDistSq(cam, glm::vec3(100.f, 0.f, 0.f), fwd, bias);
	const float behind = biasedLoadDistSq(cam, glm::vec3(-100.f, 0.f, 0.f), fwd, bias);
	const float side = biasedLoadDistSq(cam, glm::vec3(0.f, 0.f, 100.f), fwd, bias);
	CHECK(front < 100.f * 100.f, "front chunk biased closer");
	CHECK(behind > 100.f * 100.f, "behind chunk biased farther");
	CHECK(std::abs(side - 100.f * 100.f) < 1.f, "side chunk ~unbiased");

	// Ahead reach extension: a chunk beyond maxRd in front can stay inside the
	// biased budget while the same distance behind is pruned.
	std::vector<LoadCandidate> c = {
		{{6, 0, 0}, 0.f},	// center ~104 ahead  → 104² × 0.7 < 100²  (kept)
		{{-6, 0, 0}, 0.f},	// center ~88 behind  →  88² × 1.3 > 100²  (pruned)
	};
	const float maxDistSq = 100.f * 100.f;
	pruneLoadCandidatesByDistance(c, cam, maxDistSq, fwd, bias);
	CHECK(c.size() == 1, "biased prune keeps only front chunk");
	CHECK(!c.empty() && c[0].pos.x == 6, "front chunk survives biased prune");

	// Zero bias reproduces the plain radial behavior (back-compat).
	const float plain = biasedLoadDistSq(cam, glm::vec3(100.f, 0.f, 0.f), fwd, 0.f);
	CHECK(std::abs(plain - 100.f * 100.f) < 1.f, "zero bias = plain distance");
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
/// The reworked continentalness moved coastlines, so the chunk is located
/// dynamically instead of pinning seed coordinates that drift on every
/// calibration: the scan demands a chunk whose columns are all deep ocean,
/// i.e. maxSolid stays on the seafloor so yNoiseEnd <= SEA_LEVEL and
/// SEA_LEVEL is only filled when yFillEnd = computeColumnFillEnd(...)
/// extends past yNoiseEnd. Setting yFillEnd = yNoiseEnd alone leaves AIR at
/// y=64 → this test fails.
static void testOceanWaterNotClippedByCaveYBound()
{
	const int sea = TerrainGenerator::SEA_LEVEL;
	const int margin = 16;
	constexpr int kSeed = 99991;
	constexpr int kScanRadius = 96;

	TerrainGenerator gen(kSeed);
	int kCx = 0;
	int kCz = 0;
	bool found = false;
	for (int cz = -kScanRadius; cz <= kScanRadius && !found; ++cz)
	{
		for (int cx = -kScanRadius; cx <= kScanRadius && !found; ++cx)
		{
			const int wx = cx * CHUNK_SIZE, wz = cz * CHUNK_SIZE;
			const auto centre = gen.getTerrainSample(wx + 8, wz + 8);
			if (centre.biome != BIOME_OCEAN || centre.height > sea - 6)
				continue;
			bool deepOcean = true;
			for (int z = 0; z < CHUNK_SIZE && deepOcean; z += 5)
				for (int x = 0; x < CHUNK_SIZE && deepOcean; x += 5)
				{
					const auto s = gen.getTerrainSample(wx + x, wz + z);
					deepOcean = s.biome == BIOME_OCEAN && s.height <= sea - 2;
				}
			if (deepOcean)
			{
				kCx = wx;
				kCz = wz;
				found = true;
			}
		}
	}
	CHECK(found, "deep open-ocean chunk reachable within the scan radius");

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

static void testPoolCapacityEstimate()
{
	const size_t c128 = estimateChunkPoolCapacity(128);
	const size_t c256 = estimateChunkPoolCapacity(256);
	const size_t c512 = estimateChunkPoolCapacity(512);
	CHECK(c128 >= 64, "min capacity floor");
	CHECK(c256 > c128, "higher view distance needs more pool");
	CHECK(c512 > c256, "512 blocks needs more than 256");
	// Unload disk ~1.5×512/16 → r≈49 → πr² > 7000 before headroom; clamp at 16384.
	CHECK(c512 > 4000, "512-block view needs several thousand slots");
	CHECK(c512 <= 16384, "soft cap");
}

/// Ice spikes use treeDensity=0 and hasCacti=false; generateVegetation must still run
/// placeIceSpike (not skip the whole biome on the density gate).
/// The chunk is located dynamically: the continental rework invalidated the
/// old seed-1337 pin, and future calibrations must not break the regression.
static void testIceSpikeVegetationReachable()
{
	constexpr int kSeed = 1337;
	constexpr int kScanRadius = 96;
	constexpr int kMaxAttempts = 24;

	const BiomeConfig &cfg = TerrainGenerator::getBiomeConfig(BIOME_ICE_SPIKES);
	CHECK(cfg.treeDensity == 0.0f, "ice spikes config treeDensity is 0 (regression: density gate)");
	CHECK(!cfg.hasCacti, "ice spikes config hasCacti is false (regression: density gate)");

	TerrainGenerator gen(kSeed);
	int spikeBlocks = 0;
	int iceBiomeColsInChunk = 0;
	int attempts = 0;
	for (int cz = -kScanRadius; cz <= kScanRadius && spikeBlocks == 0 && attempts < kMaxAttempts; ++cz)
	{
		for (int cx = -kScanRadius; cx <= kScanRadius && spikeBlocks == 0 && attempts < kMaxAttempts; ++cx)
		{
			const int kOriginX = cx * CHUNK_SIZE;
			const int kOriginZ = cz * CHUNK_SIZE;
			if (gen.getBiomeAt(kOriginX + 8, kOriginZ + 8) != BIOME_ICE_SPIKES)
				continue;
			++attempts;

			int iceCols = 0;
			for (int z = 0; z < CHUNK_SIZE; ++z)
				for (int x = 0; x < CHUNK_SIZE; ++x)
					if (gen.getBiomeAt(kOriginX + x, kOriginZ + z) == BIOME_ICE_SPIKES)
						++iceCols;
			if (iceCols == 0)
				continue;

			ChunkData data = gen.generateChunk(kOriginX, kOriginZ);
			for (int z = 0; z < CHUNK_SIZE; ++z)
			{
				for (int x = 0; x < CHUNK_SIZE; ++x)
				{
					const size_t col = static_cast<size_t>(z * CHUNK_SIZE + x);
					if (data.biomes[col] != BIOME_ICE_SPIKES)
						continue;
					++iceBiomeColsInChunk;
					const int h = data.heightMap[col];
					for (int y = h + 1; y < CHUNK_HEIGHT && y <= h + 30; ++y)
					{
						const size_t idx =
							static_cast<size_t>(y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x);
						const uint8_t t = data.voxels[idx].type;
						if (t == static_cast<uint8_t>(PACKED_ICE) || t == static_cast<uint8_t>(BLUE_ICE))
							++spikeBlocks;
					}
				}
			}
			if (spikeBlocks > 0)
				std::cout << "  located ice-spike chunk cx=" << kOriginX << " cz=" << kOriginZ << "\n";
		}
	}
	CHECK(iceBiomeColsInChunk > 0, "generated chunk records ice-spike biomes");
	CHECK(spikeBlocks > 0,
		  "placeIceSpike must deposit PACKED_ICE/BLUE_ICE above surface (unreachable if density gate skips ice spikes)");
	std::cout << "  ice-spike columns: " << iceBiomeColsInChunk << " spike blocks above surface: "
			  << spikeBlocks << "\n";
}

int main()
{
	testLoadPriorityNearestFirst();
	testPruneFarLoads();
	testFrontBiasedLoadDistance();
	testCaveYRangeBounds();
	testChunkYFillBoundsCoversSeaLevel();
	testRemainingBudget();
	testTerrainBoundedGeneration();
	testOceanWaterNotClippedByCaveYBound();
	testPoolCapacityEstimate();
	testIceSpikeVegetationReachable();

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "PASS: stream optimization helpers + bounded terrain gen + ocean water + pool estimate + ice spikes\n";
	return 0;
}
