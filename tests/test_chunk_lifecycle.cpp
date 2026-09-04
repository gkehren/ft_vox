// Chunk lifecycle (issue #78 review): move semantics must carry the FULL
// generation state (including the per-column biomeTypes/heightMap added by
// the direct-to-pooled-storage change), and generateTerrain() must produce
// data identical to the owning generateChunk() path directly into reusable
// pooled-style storage across reset/regenerate cycles - with the
// activeVoxels cache staying in sync and no capacity churn.
#include <Chunk/Chunk.hpp>
#include <Chunk/ChunkManager.hpp>
#include <Chunk/ChunkPool.hpp>
#include <Chunk/TerrainGenerator.hpp>

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>
#include <thread>
#include <set>

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

struct VoxelView : std::span<const Voxel>
{
	using std::span<const Voxel>::span;
	size_t capacity() const { return size(); }
};

// Friend probe declared in Chunk.hpp: verifies full private state without
// exposing per-column generation data through the public API.
struct ChunkStateProbe
{
	static bool hasStorage(const Chunk &c) { return c.hasVoxelStorage(); }
	static VoxelView voxels(const Chunk &c)
	{
		if (c.m_storage)
			return VoxelView(c.m_storage->voxels.data(), CHUNK_VOLUME);
		return VoxelView();
	}
	static const ChunkNeighborBorders *borders(const Chunk &c)
	{
		return c.m_borders;
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
	const ChunkNeighborBorders *borders;
	std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> grass{};
	std::array<uint32_t, CHUNK_SIZE * CHUNK_SIZE> foliage{};
	std::array<BiomeType, CHUNK_SIZE * CHUNK_SIZE> biomes{};
	std::array<int, CHUNK_SIZE * CHUNK_SIZE> heights{};
	std::bitset<CHUNK_VOLUME> active;
	ChunkState state{ChunkState::UNLOADED};

	static ChunkSnapshot capture(const Chunk &c)
	{
		ChunkSnapshot s;
		const auto v = ChunkStateProbe::voxels(c);
		s.voxels.assign(v.begin(), v.end());
		s.borders = ChunkStateProbe::borders(c);
		s.grass = ChunkStateProbe::grass(c);
		s.foliage = ChunkStateProbe::foliage(c);
		s.biomes = ChunkStateProbe::biomes(c);
		s.heights = ChunkStateProbe::heights(c);
		s.active = ChunkStateProbe::active(c);
		s.state = c.getState();
		return s;
	}
};

static bool sameVoxels(std::span<const Voxel> a, std::span<const Voxel> b)
{
	return a.size() == b.size() &&
		   (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Voxel)) == 0);
}

static bool sameState(const ChunkSnapshot &a, const ChunkSnapshot &b)
{
	return a.state == b.state && sameVoxels(a.voxels, b.voxels) &&
		   a.borders == b.borders && a.grass == b.grass &&
		   a.foliage == b.foliage && a.biomes == b.biomes &&
		   a.heights == b.heights && a.active == b.active;
}

/// Every non-air voxel must have its activeVoxels bit set and vice versa -
/// the direct-generation path rebuilds this cache from the same buffer it
/// generated into, so a storage change could desynchronize them.
static void checkActiveCacheInSync(const Chunk &chunk, const char *label)
{
	const auto voxels = ChunkStateProbe::voxels(chunk);
	const auto &active = ChunkStateProbe::active(chunk);
	bool inSync = !ChunkStateProbe::hasStorage(chunk) ? active.none() : voxels.size() == CHUNK_VOLUME;
	for (size_t i = 0; inSync && i < voxels.size(); ++i)
		inSync = active[i] == (voxels[i].type != static_cast<uint8_t>(AIR));
	CHECK(inSync, label);
}

static void checkMatchesOwning(const Chunk &chunk, TerrainGenerator &gen,
							   int chunkX, int chunkZ, const char *label)
{
	const ChunkData reference = gen.generateChunk(chunkX, chunkZ);
	const ChunkNeighborBorders *borders = ChunkStateProbe::borders(chunk);
	CHECK(borders != nullptr, "border block present after generation");
	CHECK(std::memcmp(reference.voxels.data(),
					  ChunkStateProbe::voxels(chunk).data(),
					  CHUNK_VOLUME * sizeof(Voxel)) == 0,
		  "voxels match owning path");
	// Compact borders must answer every padded coordinate the old dense
	// shell answered (faces + corner columns; vertical padding rows were
	// always AIR there).
	bool bordersMatch = true;
	for (int y = -1; bordersMatch && y <= static_cast<int>(CHUNK_HEIGHT); ++y)
		for (int z = -1; z <= static_cast<int>(CHUNK_SIZE) && bordersMatch; ++z)
			for (int x = -1; x <= static_cast<int>(CHUNK_SIZE); ++x)
			{
				if (x >= 0 && x < static_cast<int>(CHUNK_SIZE) &&
					z >= 0 && z < static_cast<int>(CHUNK_SIZE))
					continue;
				const uint8_t dense = reference.borderVoxels[
					(y + 1) * 18 * 18 + (z + 1) * 18 + (x + 1)];
				if (borders->at(x, y, z) != dense)
				{
					bordersMatch = false;
					break;
				}
			}

	CHECK(bordersMatch, "compact borders match the dense owning-path shell");
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

// Isolate the acquisition reset cost: alternate both modes over the same
// preallocated chunk. No generation/allocator/renderer work is timed here.
static int runResetPerf()
{
	using Clock = std::chrono::steady_clock;
	Chunk chunk(glm::vec3(0.0f));
	constexpr int iterations = 100000;
	double fullMs = 0.0, generationMs = 0.0;
	for (int round = 0; round < 6; ++round)
	{
		for (int phase = 0; phase < 2; ++phase)
		{
			const bool full = ((round + phase) % 2) == 0;
			const auto mode = full ? Chunk::ResetMode::Full : Chunk::ResetMode::ForGeneration;
			const auto start = Clock::now();
			for (int i = 0; i < iterations; ++i)
				chunk.reset(glm::vec3(static_cast<float>(i % 64), 0.0f, 0.0f), mode);
			const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
			(full ? fullMs : generationMs) += ms;
		}
	}
	std::cout << "[Reset Perf] 600000 resets/mode, alternating order\n"
			  << "full: " << fullMs << " ms total, " << fullMs / 600000 << " ms/reset\n"
			  << "for-generation: " << generationMs << " ms total, "
			  << generationMs / 600000 << " ms/reset\n";
	return 0;
}

int main(int argc, char **argv)
{
    // Published memory includes free pool storage and survives ownership moves.
    if (telemetry::registry().enabled) {
        using namespace telemetry;
        const auto before = registry().snapshot();
        {
            Chunk a(glm::vec3(0));
            const auto allocated = registry().snapshot();
            CHECK(allocated.current[VoxelBytes] == before.current[VoxelBytes], "unloaded chunk has no voxel allocation");
            CHECK(allocated.current[ShellCapacity] == before.current[ShellCapacity], "unloaded chunk holds no border storage");
            CHECK(a.prepareVoxelStorageForGeneration(), "prepare storage succeeds");
            const auto withStorage = registry().snapshot();
            CHECK(withStorage.current[VoxelBytes] - before.current[VoxelBytes] == CHUNK_VOLUME * sizeof(Voxel), "resident voxel allocation");
            CHECK(withStorage.current[ShellBytes] - before.current[ShellBytes] == sizeof(ChunkNeighborBorders), "border block borrowed on prepare");
            a.freeShellVoxels();
            auto cleared = registry().snapshot();
            CHECK(cleared.current[ShellBytes] == before.current[ShellBytes], "released borders report zero shell bytes");
            CHECK(cleared.current[ShellCapacity] == before.current[ShellCapacity], "released borders retain no per-chunk capacity");
            Chunk b(std::move(a));
            CHECK(registry().snapshot().current[VoxelBytes] == withStorage.current[VoxelBytes], "move preserves total vector ownership");
            Chunk c(glm::vec3(0));
            c = std::move(b);
            CHECK(registry().snapshot().current[VoxelBytes] == withStorage.current[VoxelBytes], "move assignment releases destination storage");
            c.setVoxel(1, 1, 1, STONE);
            c.generateMesh();
            const auto meshed = registry().snapshot();
            CHECK(meshed.current[OpaqueVertexBytes] > before.current[OpaqueVertexBytes], "mesh publication includes vertex data");
            CHECK(meshed.current[CpuMeshCapacity] >= meshed.current[OpaqueVertexBytes], "retained mesh capacity covers its data");
            c.reset(glm::vec3(0));
            const auto reset = registry().snapshot();
            CHECK(reset.current[VoxelBytes] == before.current[VoxelBytes], "full reset returns voxel storage to pool");
            CHECK(reset.current[OpaqueVertexBytes] == before.current[OpaqueVertexBytes], "reset clears logical mesh data");
            CHECK(reset.current[CpuMeshCapacity] == meshed.current[CpuMeshCapacity], "reset retains mesh allocations");
        }
        const auto after = registry().snapshot();
        for (size_t i=0; i<=OccupancyBytes; ++i)
            CHECK(before.current[i] == after.current[i], "CPU telemetry balances after destruction");
    }
	if (argc > 1 && std::strcmp(argv[1], "--reset-perf") == 0)
		return runResetPerf();
	constexpr int kSeed = 4242;
	TerrainGenerator gen(kSeed);

	// 1) generateTerrain fills complete, correct state at position A.
	Chunk a(glm::vec3(0.0f, 0.0f, 0.0f));
	CHECK(a.getState() == ChunkState::UNLOADED, "fresh chunk starts UNLOADED");
	CHECK(!ChunkStateProbe::hasStorage(a), "fresh chunk starts without voxel storage");
	CHECK(a.prepareVoxelStorageForGeneration(), "prepare storage succeeds");
	const void *storagePtrA = ChunkStateProbe::voxels(a).data();
	CHECK(storagePtrA != nullptr, "storage pointer is non-null after prepare");
	a.generateTerrain(gen);
	CHECK(ChunkStateProbe::voxels(a).data() == storagePtrA,
		  "generation preserves exact storage pointer prepared on main thread");
	CHECK(a.getState() == ChunkState::GENERATED, "generation completes");
	CHECK(ChunkStateProbe::hasStorage(a), "generated chunk has voxel storage");
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
	CHECK(!ChunkStateProbe::hasStorage(a), "moved-from chunk has no storage");
	CHECK(ChunkStateProbe::hasStorage(b), "moved-to chunk has storage");
	const ChunkSnapshot snapB = ChunkSnapshot::capture(b);
	CHECK(sameState(snapA, snapB),
		  "move-constructor must preserve full generation state");
	CHECK(b.getState() == ChunkState::GENERATED, "moved chunk keeps GENERATED");

	// 3) Move-assignment carries the full generation state.
	Chunk c(glm::vec3(9876.0f, 0.0f, -4321.0f));
	c = std::move(b);
	CHECK(!ChunkStateProbe::hasStorage(b), "moved-from chunk has no storage");
	CHECK(ChunkStateProbe::hasStorage(c), "moved-to chunk has storage");
	const ChunkSnapshot snapC = ChunkSnapshot::capture(c);
	CHECK(sameState(snapA, snapC),
		  "move-assignment must preserve full generation state");

	// 4) Pool-like recycle: reset + regenerate at a new position must match
	// the owning path for the new coordinate, keep the active cache in sync,
	// and never grow the buffers (capacity churn would defeat ChunkPool).
	const size_t voxelCapacity = ChunkStateProbe::voxels(c).capacity();

	c.reset(glm::vec3(static_cast<float>(CHUNK_SIZE), 0.0f,
					  static_cast<float>(CHUNK_SIZE)), Chunk::ResetMode::ForGeneration);
	CHECK(c.getState() == ChunkState::UNLOADED, "reset returns chunk to UNLOADED");
	CHECK(sameVoxels(snapC.voxels, ChunkStateProbe::voxels(c)),
		  "generation reset retains dirty voxels until generation overwrites them");
	CHECK(ChunkStateProbe::active(c).none(), "reset invalidates active cache");
	// ForGeneration keeps voxel storage but frees borders; the prepare step
	// re-borrows them on the calling thread per the generation contract.
	CHECK(c.prepareVoxelStorageForGeneration(), "re-prepare after ForGeneration reset");
	c.generateTerrain(gen);
	CHECK(c.getState() == ChunkState::GENERATED, "regeneration completes");
	checkMatchesOwning(c, gen, CHUNK_SIZE, CHUNK_SIZE, "recycled generation");
	checkActiveCacheInSync(c, "activeVoxels cache in sync after recycle");
	CHECK(ChunkStateProbe::voxels(c).capacity() == voxelCapacity,
		  "voxel capacity stable across recycle");
	CHECK(ChunkStateProbe::borders(c) != nullptr,
		  "borders re-borrowed for recycled generation");

	// Full reset returns storage when a chunk is retired.
	c.reset(glm::vec3(0.0f));
	CHECK(!ChunkStateProbe::hasStorage(c), "default reset returns storage to pool");
	CHECK(std::all_of(ChunkStateProbe::voxels(c).begin(),
					  ChunkStateProbe::voxels(c).end(),
					  [](Voxel v) { return v.type == AIR; }),
		  "default reset clears every voxel");
	checkActiveCacheInSync(c, "full reset leaves an empty active cache");

	// Exercise the real pool boundary, including cancelled generation.
	ChunkPool pool(64);
	CHECK(pool.voxelStorageCapacity() == 0, "constructing ChunkPool allocates 0 voxel storage");
	Chunk *pooled = pool.acquire(glm::vec3(0.0f));
	CHECK(pooled != nullptr, "pool acquisition succeeds");
	CHECK(!ChunkStateProbe::hasStorage(*pooled), "acquired chunk has no storage before generation");
	if (pooled)
	{
		CHECK(pooled->prepareVoxelStorageForGeneration(), "prepare pooled storage succeeds");
		pooled->generateTerrain(gen);
		CHECK(ChunkStateProbe::hasStorage(*pooled), "pooled chunk has storage after generation");
		CHECK(pool.voxelStorageCapacity() == 1, "voxel storage capacity grew on demand");
		CHECK(pool.voxelStorageActive() == 1, "1 active voxel storage");
		pool.release(pooled);
		CHECK(!ChunkStateProbe::hasStorage(*pooled), "pool release returns voxel storage to pool");
		CHECK(pool.voxelStorageActive() == 0, "0 active voxel storage after release");
		CHECK(pool.voxelStorageFree() == 1, "released storage returned to free list");

		Chunk *reused = pool.acquire(glm::vec3(-CHUNK_SIZE, 0.0f, CHUNK_SIZE));
		CHECK(reused == pooled, "pool reuses released chunk slot");
		CHECK(!ChunkStateProbe::hasStorage(*reused), "reacquired chunk slot has no storage before generation");
		CHECK(reused->prepareVoxelStorageForGeneration(), "prepare storage on reuse succeeds");
		reused->generateTerrain(gen);
		CHECK(pool.voxelStorageCapacity() == 1, "generation reused pooled storage without allocating");
		CHECK(pool.voxelStorageActive() == 1, "1 active voxel storage");
		checkMatchesOwning(*reused, gen, -CHUNK_SIZE, CHUNK_SIZE, "pool reuse");
		checkActiveCacheInSync(*reused, "pool reuse active cache");
		pool.release(reused);
		pooled = pool.acquire(glm::vec3(0.0f));
		pool.release(pooled); // cancelled before generation
		CHECK(pool.acquiredCount() == 0, "cancelled acquisition returns to pool");
		CHECK(pool.voxelStorageActive() == 0, "cancelled acquisition leaves no active storage");
	}

	// 5) setVoxel(..., AIR) on empty chunk does not allocate storage.
	{
		Chunk emptyChunk(glm::vec3(0.0f));
		CHECK(!ChunkStateProbe::hasStorage(emptyChunk), "starts without storage");
		CHECK(emptyChunk.getVoxel(0, 0, 0).type == AIR, "read on null storage returns AIR");
		emptyChunk.setVoxel(0, 0, 0, AIR);
		CHECK(!ChunkStateProbe::hasStorage(emptyChunk), "setting AIR does not allocate storage");
		emptyChunk.setVoxel(0, 0, 0, STONE);
		CHECK(ChunkStateProbe::hasStorage(emptyChunk), "setting non-AIR allocates storage");
		CHECK(emptyChunk.getVoxel(0, 0, 0).type == STONE, "read back stone block");
	}

	// 6) Cross-pool move semantics (issue #112 review): a move transfers the
	// storage together with the pool that owns it — O(1), noexcept, no
	// reallocation and no uninitialized reads. The destination therefore
	// ends up referencing the source's pool.
	{
		VoxelPool poolA;
		VoxelPool poolB;
		const int crossSeed = 2024;
		TerrainGenerator crossGen(crossSeed);
		const ChunkData reference = crossGen.generateChunk(10, 0);

		Chunk chunkA(glm::vec3(10.0f, 0.0f, 0.0f), ChunkState::UNLOADED, &poolA);
		Chunk chunkB(glm::vec3(20.0f, 0.0f, 0.0f), ChunkState::UNLOADED, &poolB);
		CHECK(chunkA.prepareVoxelStorageForGeneration(), "prepare chunkA storage");
		chunkA.generateTerrain(crossGen);
		CHECK(poolA.activeCount() == 1, "poolA active count 1");
		CHECK(poolB.activeCount() == 0, "poolB active count 0");

		chunkB = std::move(chunkA);
		CHECK(chunkB.getVoxelPool() == &poolA, "destination adopts source pool");
		CHECK(poolA.activeCount() == 1, "poolA active count 1 after cross-pool move");
		CHECK(poolB.activeCount() == 0, "poolB active count 0 after cross-pool move");
		CHECK(!ChunkStateProbe::hasStorage(chunkA), "chunkA lost storage");
		CHECK(ChunkStateProbe::hasStorage(chunkB), "chunkB owns the transferred storage");
		CHECK(std::memcmp(ChunkStateProbe::voxels(chunkB).data(), reference.voxels.data(),
						  CHUNK_VOLUME * sizeof(Voxel)) == 0,
			  "cross-pool move carries the generated content, not uninitialized bytes");
		chunkB.reset(glm::vec3(0.0f));
		CHECK(poolA.activeCount() == 0, "storage returns to the traveled pool on reset");
	}

	// 6b) Cross-pool move-construction: same transfer model, plus destructor
	// returning the storage to the pool the chunk references.
	{
		VoxelPool poolA;
		VoxelPool poolB;
		Chunk chunkA(glm::vec3(0.0f), ChunkState::UNLOADED, &poolA);
		CHECK(chunkA.prepareVoxelStorageForGeneration(), "prepare chunkA storage (move-ctor)");
		const void *storage = ChunkStateProbe::voxels(chunkA).data();
		Chunk chunkB(std::move(chunkA));
		CHECK(chunkB.getVoxelPool() == &poolA, "move-ctor adopts source pool");
		CHECK(ChunkStateProbe::voxels(chunkB).data() == storage, "move-ctor keeps exact storage");
		CHECK(!ChunkStateProbe::hasStorage(chunkA), "source left without storage");
		CHECK(poolA.activeCount() == 1, "poolA active count 1 during move-ctor");
		{
			Chunk consumer(std::move(chunkB));
			CHECK(poolA.activeCount() == 1, "storage follows the second move");
		}
		CHECK(poolA.activeCount() == 0, "destruction returns storage to the owning pool");
		(void)poolB;
	}

	// 6c) ChunkPool destructor must retire chunks that still hold live
	// voxel storage — no use-after-free, no double release.
	{
		TerrainGenerator dtorGen(9);
		{
			ChunkPool pool(8);
			Chunk *c = pool.acquire(glm::vec3(0.0f));
			CHECK(c != nullptr, "destructor-test acquisition succeeds");
			if (c)
			{
				CHECK(c->prepareVoxelStorageForGeneration(), "destructor-test prepare succeeds");
				c->generateTerrain(dtorGen);
			}
			// Intentionally no release: the ChunkPool destructor retires
			// every chunk and returns voxel storage through the VoxelPool,
			// which must outlive all Chunk destructors.
		}
		std::cout << "chunk pool destructor completed cleanly with live storage\n";
	}

	// 7) Steady-state VoxelPool recycling high-water mark.
	{
		VoxelPool vp;
		for (int cycle = 0; cycle < 1000; ++cycle)
		{
			VoxelStorage *s = vp.acquire();
			vp.release(s);
		}
		CHECK(vp.capacity() == 1, "capacity capped at 1 for single-item recycling");
		CHECK(vp.activeCount() == 0, "active count 0");
		CHECK(vp.freeCount() == 1, "free count 1");
	}

	// 8) Multi-threaded VoxelPool stress test.
	{
		VoxelPool vp;
		constexpr int kThreads = 4;
		constexpr int kOpsPerThread = 2500;
		std::vector<std::thread> workers;
		workers.reserve(kThreads);
		for (int t = 0; t < kThreads; ++t)
		{
			workers.emplace_back([&vp]() {
				for (int i = 0; i < kOpsPerThread; ++i)
				{
					VoxelStorage *s = vp.acquire();
					s->voxels[0].type = static_cast<uint8_t>(STONE);
					vp.release(s);
				}
			});
		}
		for (auto &w : workers)
			w.join();

		CHECK(vp.activeCount() == 0, "concurrent stress ends with 0 active");
		CHECK(vp.capacity() == vp.freeCount(), "capacity matches free list count");
		CHECK(vp.capacity() <= static_cast<size_t>(kThreads), "capacity bounded by thread count");
	}

	// 9) Synchronous bootstrap contract (issue #112): the ChunkManager
	// helper prepares storage on the calling thread, generates, and returns
	// the chunk to the pool when preparation fails.
	{
		ChunkPool pool(4);
		ChunkManager manager(&gen, nullptr, &pool);
		Chunk *chunk = pool.acquire(glm::vec3(0.0f));
		CHECK(chunk != nullptr, "bootstrap acquisition succeeds");
		CHECK(manager.prepareAndGenerateChunk(chunk, gen), "bootstrap prepare+generate succeeds");
		CHECK(chunk->getState() == ChunkState::GENERATED, "bootstrap chunk generated");
		CHECK(ChunkStateProbe::hasStorage(*chunk), "bootstrap chunk owns voxel storage");
		CHECK(pool.voxelStorageActive() == 1, "bootstrap storage accounted active");
		CHECK(!manager.prepareAndGenerateChunk(nullptr, gen), "null chunk rejected");
		pool.release(chunk);
		CHECK(pool.voxelStorageActive() == 0, "bootstrap release returns storage");
	}

	// 10) Border sampling contract (issue #103): faces, corners, vertical
	// padding, missing-neighbor defaults and lazy re-borrow on edits.
	{
		TerrainGenerator bgen(31);
		Chunk bc(glm::vec3(0.0f));
		CHECK(bc.prepareVoxelStorageForGeneration(), "border-test prepare");
		bc.generateTerrain(bgen);

		// In-chunk sampling matches direct voxel reads.
		CHECK(bc.sampleForMeshing(3, 5, 7) ==
				  static_cast<TextureType>(bc.getVoxel(3, 5, 7).type),
			  "in-chunk sampling matches getVoxel");

		// All four faces, both corner kinds and vertical padding, written
		// directly into the border block and read via the layout-independent
		// helper.
		auto *borders = const_cast<ChunkNeighborBorders *>(ChunkStateProbe::borders(bc));
		CHECK(borders != nullptr, "borders present for sampling test");
		borders->mutableAt(-1, 40, 7) = static_cast<uint8_t>(STONE);   // west face
		borders->mutableAt(16, 41, 8) = static_cast<uint8_t>(BRICKS);  // east face
		borders->mutableAt(6, 42, -1) = static_cast<uint8_t>(DIRT);    // south face
		borders->mutableAt(9, 43, 16) = static_cast<uint8_t>(GLASS);   // north face
		borders->mutableAt(-1, 44, -1) = static_cast<uint8_t>(STONE);  // SW corner
		borders->mutableAt(16, 45, 16) = static_cast<uint8_t>(BRICKS); // NE corner
		borders->mutableAt(-1, 46, 16) = static_cast<uint8_t>(DIRT);   // NW corner
		borders->mutableAt(16, 47, -1) = static_cast<uint8_t>(GLASS);  // SE corner
		CHECK(bc.sampleForMeshing(-1, 40, 7) == STONE, "west face sample");
		CHECK(bc.sampleForMeshing(16, 41, 8) == BRICKS, "east face sample");
		CHECK(bc.sampleForMeshing(6, 42, -1) == DIRT, "south face sample");
		CHECK(bc.sampleForMeshing(9, 43, 16) == GLASS, "north face sample");
		CHECK(bc.sampleForMeshing(-1, 44, -1) == STONE, "SW corner sample");
		CHECK(bc.sampleForMeshing(16, 45, 16) == BRICKS, "NE corner sample");
		CHECK(bc.sampleForMeshing(-1, 46, 16) == DIRT, "NW corner sample");
		CHECK(bc.sampleForMeshing(16, 47, -1) == GLASS, "SE corner sample");
		// Vertical padding reads AIR even with borders present.
		CHECK(bc.sampleForMeshing(-1, -1, 7) == AIR, "y=-1 padding reads AIR");
		CHECK(bc.sampleForMeshing(16, CHUNK_HEIGHT, 8) == AIR, "y=top padding reads AIR");

		// Missing borders (freed after upload) read AIR.
		bc.freeShellVoxels();
		CHECK(bc.isShellEmpty(), "freed borders leave the shell empty");
		CHECK(bc.sampleForMeshing(-1, 40, 7) == AIR, "freed borders sample AIR");

		// Edits at the boundary lazily re-borrow and stay observable.
		bc.setVoxel(-1, 12, 5, STONE);
		CHECK(!bc.isShellEmpty(), "boundary edit re-borrows borders");
		CHECK(bc.sampleForMeshing(-1, 12, 5) == STONE, "boundary edit observable via sampling");

		// rebuildShellFromNeighbors: face values come from the neighbor
		// chunk's opposite column; missing neighbors stay AIR; corners stay
		// AIR exactly as in the previous rebuild path.
		Chunk neighbor(glm::vec3(static_cast<float>(CHUNK_SIZE), 0.0f, 0.0f));
		CHECK(neighbor.prepareVoxelStorageForGeneration(), "neighbor prepare");
		neighbor.generateTerrain(bgen);
		neighbor.setVoxel(0, 30, 4, STONE);
		bc.rebuildShellFromNeighbors(nullptr, &neighbor, nullptr, nullptr);
		CHECK(bc.sampleForMeshing(16, 30, 4) == STONE, "east face rebuilt from neighbor");




		CHECK(bc.sampleForMeshing(-1, 30, 4) == AIR, "missing west neighbor samples AIR");
		CHECK(bc.sampleForMeshing(-1, 30, -1) == AIR, "rebuild leaves corners AIR");
	}

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "PASS: chunk lifecycle - moves carry full state, "
			  << "recycled generateTerrain matches owning path, capacities stable\n";
	return 0;
}
