// Chunk lifecycle (issue #78 review): move semantics must carry the FULL
// generation state (including the per-column biomeTypes/heightMap added by
// the direct-to-pooled-storage change), and generateTerrain() must produce
// data identical to the owning generateChunk() path directly into reusable
// pooled-style storage across reset/regenerate cycles - with the
// activeVoxels cache staying in sync and no capacity churn.
#include <Chunk/Chunk.hpp>
#include <Chunk/TerrainGenerator.hpp>

#include <algorithm>
#include <array>
#include <bitset>
#include <cstring>
#include <iostream>
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

// Friend probe declared in Chunk.hpp: verifies full private state without
// exposing per-column generation data through the public API.
struct ChunkStateProbe
{
	static const std::vector<Voxel> &voxels(const Chunk &c) { return c.voxels; }
	static const std::vector<uint8_t> &shell(const Chunk &c)
	{
		return c.neighborShellVoxels;
	}
	static const std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> &grass(const Chunk &c)
	{
		return c.biomeGrassColors;
	}
	static const std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> &foliage(const Chunk &c)
	{
		return c.biomeFoliageColors;
	}
	static const std::array<BiomeType, CHUNK_SIZE * CHUNK_SIZE> &biomes(const Chunk &c)
	{
		return c.biomeTypes;
	}
	static const std::array<int, CHUNK_SIZE * CHUNK_SIZE> &heights(const Chunk &c)
	{
		return c.heightMap;
	}
	static const std::bitset<CHUNK_VOLUME> &active(const Chunk &c)
	{
		return c.activeVoxels;
	}
};

struct ChunkSnapshot
{
	std::vector<Voxel> voxels;
	std::vector<uint8_t> shell;
	std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> grass{};
	std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> foliage{};
	std::array<BiomeType, CHUNK_SIZE * CHUNK_SIZE> biomes{};
	std::array<int, CHUNK_SIZE * CHUNK_SIZE> heights{};
	std::bitset<CHUNK_VOLUME> active;
	ChunkState state{ChunkState::UNLOADED};

	static ChunkSnapshot capture(const Chunk &c)
	{
		ChunkSnapshot s;
		s.voxels = ChunkStateProbe::voxels(c);
		s.shell = ChunkStateProbe::shell(c);
		s.grass = ChunkStateProbe::grass(c);
		s.foliage = ChunkStateProbe::foliage(c);
		s.biomes = ChunkStateProbe::biomes(c);
		s.heights = ChunkStateProbe::heights(c);
		s.active = ChunkStateProbe::active(c);
		s.state = c.getState();
		return s;
	}
};

static bool sameVoxels(const std::vector<Voxel> &a, const std::vector<Voxel> &b)
{
	return a.size() == b.size() &&
		   std::memcmp(a.data(), b.data(), a.size() * sizeof(Voxel)) == 0;
}

static bool sameState(const ChunkSnapshot &a, const ChunkSnapshot &b)
{
	return a.state == b.state && sameVoxels(a.voxels, b.voxels) &&
		   a.shell == b.shell && a.grass == b.grass &&
		   a.foliage == b.foliage && a.biomes == b.biomes &&
		   a.heights == b.heights && a.active == b.active;
}

/// Every non-air voxel must have its activeVoxels bit set and vice versa -
/// the direct-generation path rebuilds this cache from the same buffer it
/// generated into, so a storage change could desynchronize them.
static void checkActiveCacheInSync(const Chunk &chunk, const char *label)
{
	const auto &voxels = ChunkStateProbe::voxels(chunk);
	const auto &active = ChunkStateProbe::active(chunk);
	bool inSync = voxels.size() == CHUNK_VOLUME;
	for (size_t i = 0; inSync && i < CHUNK_VOLUME; ++i)
		inSync = active[i] == (voxels[i].type != static_cast<uint8_t>(AIR));
	CHECK(inSync, label);
}

static void checkMatchesOwning(const Chunk &chunk, TerrainGenerator &gen,
							   int chunkX, int chunkZ, const char *label)
{
	const ChunkData reference = gen.generateChunk(chunkX, chunkZ);
	const auto &shell = ChunkStateProbe::shell(chunk);
	CHECK(shell.size() == kBorderVoxelCount, "shell fully sized after generation");
	CHECK(std::memcmp(reference.voxels.data(),
					  ChunkStateProbe::voxels(chunk).data(),
					  CHUNK_VOLUME * sizeof(Voxel)) == 0,
		  "voxels match owning path");
	CHECK(std::memcmp(reference.borderVoxels.data(), shell.data(),
					  shell.size()) == 0,
		  "shell matches owning path");
	CHECK(std::equal(reference.grassColors.begin(), reference.grassColors.end(),
					 ChunkStateProbe::grass(chunk).begin()),
		  "grass colors match owning path");
	CHECK(std::equal(reference.foliageColors.begin(), reference.foliageColors.end(),
					 ChunkStateProbe::foliage(chunk).begin()),
		  "foliage colors match owning path");
	CHECK(std::equal(reference.biomes.begin(), reference.biomes.end(),
					 ChunkStateProbe::biomes(chunk).begin()),
		  "biomes match owning path");
	CHECK(std::equal(reference.heightMap.begin(), reference.heightMap.end(),
					 ChunkStateProbe::heights(chunk).begin()),
		  "height map matches owning path");
}

int main()
{
	constexpr int kSeed = 4242;
	TerrainGenerator gen(kSeed);

	// 1) generateTerrain fills complete, correct state at position A.
	Chunk a(glm::vec3(0.0f, 0.0f, 0.0f));
	CHECK(a.getState() == ChunkState::UNLOADED, "fresh chunk starts UNLOADED");
	a.generateTerrain(gen);
	CHECK(a.getState() == ChunkState::GENERATED, "generation completes");
	CHECK(ChunkStateProbe::voxels(a).size() == static_cast<size_t>(CHUNK_VOLUME),
		  "voxel storage fully sized");
	checkActiveCacheInSync(a, "activeVoxels cache in sync after generation");
	checkMatchesOwning(a, gen, 0, 0, "initial generation");
	const ChunkSnapshot snapA = ChunkSnapshot::capture(a);
	// A generated chunk must not be trivially empty (a real surface chunk
	// carries thousands of solid blocks; an empty/reset one would be ~0).
	int solid = 0;
	for (size_t i = 0; i < CHUNK_VOLUME; ++i)
		solid += snapA.voxels[i].type != static_cast<uint8_t>(AIR);
	CHECK(solid > 1024, "generation produced substantial terrain");

	// 2) Move-construction carries the full generation state.
	Chunk b(std::move(a));
	const ChunkSnapshot snapB = ChunkSnapshot::capture(b);
	CHECK(sameState(snapA, snapB),
		  "move-constructor must preserve full generation state");
	CHECK(b.getState() == ChunkState::GENERATED, "moved chunk keeps GENERATED");

	// 3) Move-assignment carries the full generation state.
	Chunk c(glm::vec3(9876.0f, 0.0f, -4321.0f));
	c = std::move(b);
	const ChunkSnapshot snapC = ChunkSnapshot::capture(c);
	CHECK(sameState(snapA, snapC),
		  "move-assignment must preserve full generation state");

	// 4) Pool-like recycle: reset + regenerate at a new position must match
	// the owning path for the new coordinate, keep the active cache in sync,
	// and never grow the buffers (capacity churn would defeat ChunkPool).
	const size_t voxelCapacity = ChunkStateProbe::voxels(c).capacity();
	const size_t shellCapacity = ChunkStateProbe::shell(c).capacity();

	c.reset(glm::vec3(static_cast<float>(CHUNK_SIZE), 0.0f,
					  static_cast<float>(CHUNK_SIZE)));
	CHECK(c.getState() == ChunkState::UNLOADED, "reset returns chunk to UNLOADED");
	c.generateTerrain(gen);
	CHECK(c.getState() == ChunkState::GENERATED, "regeneration completes");
	checkMatchesOwning(c, gen, CHUNK_SIZE, CHUNK_SIZE, "recycled generation");
	checkActiveCacheInSync(c, "activeVoxels cache in sync after recycle");
	CHECK(ChunkStateProbe::voxels(c).capacity() == voxelCapacity,
		  "voxel capacity stable across recycle");
	CHECK(ChunkStateProbe::shell(c).capacity() == shellCapacity,
		  "shell capacity stable across recycle");

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "PASS: chunk lifecycle - moves carry full state, "
			  << "recycled generateTerrain matches owning path, capacities stable\n";
	return 0;
}
