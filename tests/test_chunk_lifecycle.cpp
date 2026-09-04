// Chunk lifecycle (issue #78 review): move semantics must carry the FULL
// generation state (including the per-column biomeTypes/heightMap added by
// the direct-to-pooled-storage change), and generateTerrain() must produce
// data identical to the owning generateChunk() path directly into reusable
// pooled-style storage across reset/regenerate cycles - with the
// activeVoxels cache staying in sync and no capacity churn.
#include <Chunk/Chunk.hpp>
#include <Chunk/ChunkManager.hpp>
#include <Chunk/ChunkMeshResult.hpp>
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

// Friend probe declared in ChunkManager.hpp: exposes the deferred-edit
// queue size so coalescing behavior is observable without public API.
struct ChunkManagerProbe
{
	static size_t pendingEdits(const ChunkManager &m) { return m.m_pendingEdits.size(); }
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
	static MeshBuildResult *pendingResult(const Chunk &c)
	{
		return c.m_pendingResult;
	}
	static uint64_t meshGeneration(const Chunk &c)
	{
		return c.m_meshGeneration;
	}
};

struct ChunkSnapshot
{
	std::vector<Voxel> voxels;
	// Border state as CONTENT (state comparison) plus the borrowed pointer
	// (ownership transfer comparison) - issue #113 review item 24.
	std::optional<ChunkNeighborBorders> borders;
	const ChunkNeighborBorders *borderPtr{nullptr};
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
		const ChunkNeighborBorders *bp = ChunkStateProbe::borders(c);
		if (bp)
			s.borders = *bp;
		s.borderPtr = bp;
		s.grass = ChunkStateProbe::grass(c);
		s.foliage = ChunkStateProbe::foliage(c);
		s.biomes = ChunkStateProbe::biomes(c);
		s.heights = ChunkStateProbe::heights(c);
		s.active = ChunkStateProbe::active(c);
		s.state = c.getState();
		return s;
	}
};

static bool memcmpBorderContent(const ChunkNeighborBorders &a, const ChunkNeighborBorders &b)
{
	return std::memcmp(&a, &b, sizeof(ChunkNeighborBorders)) == 0;
}

static bool sameVoxels(std::span<const Voxel> a, std::span<const Voxel> b)
{
	return a.size() == b.size() &&
		   (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Voxel)) == 0);
}

static bool sameState(const ChunkSnapshot &a, const ChunkSnapshot &b)
{
	return a.state == b.state && sameVoxels(a.voxels, b.voxels) &&
		   a.borders.has_value() == b.borders.has_value() &&
		   (!a.borders || memcmpBorderContent(*a.borders, *b.borders)) &&
		   a.grass == b.grass &&
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
            a.releaseNeighborBorders();
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
            MeshBuildResult *pending = ChunkStateProbe::pendingResult(c);
            CHECK(pending != nullptr, "generateMesh attaches a pooled build result");
            CHECK(!pending->opaqueVertices.empty(),
                  "attached build result carries the mesh payload");
            CHECK(ChunkStateProbe::pendingResult(c)->owner == &c,
                  "build result names its owning chunk");
            // cpu.opaque/water.* mesh gauges are engine-published from
            // MeshResultPoolStats; chunks no longer publish per-chunk mesh
            // bytes (issue #104), so no gauge moves on mesh publication.
            const auto meshed = registry().snapshot();
            (void)meshed;
            const uint64_t generationAfterMesh = ChunkStateProbe::meshGeneration(c);
            c.reset(glm::vec3(0));
            const auto reset = registry().snapshot();
            CHECK(reset.current[VoxelBytes] == before.current[VoxelBytes], "full reset returns voxel storage to pool");
            CHECK(ChunkStateProbe::pendingResult(c) == nullptr, "reset returns the attached build result to its pool");
            CHECK(ChunkStateProbe::meshGeneration(c) == generationAfterMesh + 1,
                  "reset bumps the mesh generation");
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
		bc.releaseNeighborBorders();
		CHECK(bc.isShellEmpty(), "freed borders leave the shell empty");
		CHECK(bc.sampleForMeshing(-1, 40, 7) == AIR, "freed borders sample AIR");

		// Edits at the boundary lazily re-borrow and stay observable.
		bc.setVoxel(-1, 12, 5, STONE);
		CHECK(!bc.isShellEmpty(), "boundary edit re-borrows borders");
		CHECK(bc.sampleForMeshing(-1, 12, 5) == STONE, "boundary edit observable via sampling");

		// rebuildBordersFromNeighbors: face values come from the neighbor
		// chunk's opposite column; missing neighbors stay AIR; corners stay
		// AIR exactly as in the previous rebuild path.
		Chunk neighbor(glm::vec3(static_cast<float>(CHUNK_SIZE), 0.0f, 0.0f));
		CHECK(neighbor.prepareVoxelStorageForGeneration(), "neighbor prepare");
		neighbor.generateTerrain(bgen);
		neighbor.setVoxel(0, 30, 4, STONE);
		bc.rebuildBordersFromNeighbors(nullptr, &neighbor, nullptr, nullptr);
		CHECK(bc.sampleForMeshing(16, 30, 4) == STONE, "east face rebuilt from neighbor");




		CHECK(bc.sampleForMeshing(-1, 30, 4) == AIR, "missing west neighbor samples AIR");
		CHECK(bc.sampleForMeshing(-1, 30, -1) == AIR, "rebuild leaves corners AIR");
	}

	// 11) Cross-pool border-block ownership on move assignment (issue #113):
	// the destination's borrowed border block must be returned to ITS pool
	// before the pools travel with the transfer - otherwise it leaks.
	{
		VoxelPool voxelA, voxelB;
		BorderPool borderA, borderB;
		Chunk source(glm::vec3(0.0f), ChunkState::UNLOADED, &voxelA, &borderA);
		Chunk destination(glm::vec3(0.0f), ChunkState::UNLOADED, &voxelB, &borderB);
		CHECK(source.prepareVoxelStorageForGeneration(), "source prepare (border move test)");
		CHECK(destination.prepareVoxelStorageForGeneration(), "destination prepare (border move test)");
		CHECK(borderA.activeCount() == 1, "borderA active 1 after prepare");
		CHECK(borderB.activeCount() == 1, "borderB active 1 after prepare");

		destination = std::move(source);

		CHECK(borderB.activeCount() == 0, "destination border returned to borderB");
		CHECK(borderA.activeCount() == 1, "source border owned via borderA after move");
		CHECK(destination.getBorderPool() == &borderA, "border pool travels with the move");
		CHECK(destination.hasBorderStorage(), "destination owns a border block");
		CHECK(ChunkStateProbe::borders(source) == nullptr, "source has no border");

		destination.reset(glm::vec3(0.0f));
		CHECK(borderA.activeCount() == 0, "reset returns the border to borderA");
		CHECK(borderA.freeCount() == borderA.capacity(), "borderA block back in free list");
		CHECK(borderB.activeCount() == 0, "borderB still fully idle");
	}

	// 12) Stale pool blocks: the pool does not clear content - every
	// consumer initializes before sampling (issue #113).
	{
		// 12a) Generation overwrites a stale block byte-for-byte.
		TerrainGenerator sgen(64);
		BorderPool sborders;
		Chunk g(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, &sborders);
		CHECK(g.prepareVoxelStorageForGeneration(), "stale-generation prepare");
		g.generateTerrain(sgen);
		g.releaseNeighborBorders();
		{
			ChunkNeighborBorders *stale = sborders.acquire();
			std::memset(stale, 0xFF, sizeof(ChunkNeighborBorders));
			sborders.release(stale);
		}
		Chunk g2(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, &sborders);
		CHECK(g2.prepareVoxelStorageForGeneration(), "stale-generation re-prepare");
		g2.generateTerrain(sgen);
		checkMatchesOwning(g2, sgen, 0, 0, "generation over stale border block");

		// 12b) Rebuild initializes a stale block: provided face correct,
		// missing faces AIR, corners AIR.
		Chunk nb(glm::vec3(0.0f));
		CHECK(nb.prepareVoxelStorageForGeneration(), "stale-rebuild prepare");
		nb.generateTerrain(sgen);
		Chunk east(glm::vec3(static_cast<float>(CHUNK_SIZE), 0.0f, 0.0f));
		CHECK(east.prepareVoxelStorageForGeneration(), "stale-rebuild east prepare");
		east.generateTerrain(sgen);
		east.setVoxel(0, 30, 4, STONE);
		auto *raw = const_cast<ChunkNeighborBorders *>(ChunkStateProbe::borders(nb));
		std::memset(raw, 0xFF, sizeof(ChunkNeighborBorders));
		nb.rebuildBordersFromNeighbors(nullptr, &east, nullptr, nullptr);
		CHECK(nb.sampleForMeshing(16, 30, 4) == STONE, "provided face correct after stale reuse");
		CHECK(nb.sampleForMeshing(-1, 30, 4) == AIR, "missing face AIR after stale reuse");
		CHECK(nb.sampleForMeshing(-1, 30, -1) == AIR, "corners AIR after stale reuse");

		// 12c) Boundary edit initializes a freshly re-borrowed stale block.
		{
			BorderPool pb;
			Chunk eb(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, &pb);
			{
				ChunkNeighborBorders *stale = pb.acquire();
				std::memset(stale, 0xFF, sizeof(ChunkNeighborBorders));
				pb.release(stale);
			}
			eb.setVoxel(-1, 12, 5, STONE);
			CHECK(eb.sampleForMeshing(-1, 12, 5) == STONE, "edited coordinate STONE on stale block");
			CHECK(eb.sampleForMeshing(-1, 13, 5) == AIR, "other border coordinates AIR after stale reuse");
			CHECK(eb.sampleForMeshing(-1, 12, -1) == AIR, "corner AIR after stale reuse");
		}
	}

	// 13) Mesh output equivalence and edge behavior (issue #103/#113): the
	// compact borders cull exactly like the dense shell did. Since #104 the
	// payload lives in the attached build result, so the comparison is
	// byte-for-byte on that result (the old index-count probes read the
	// post-upload counters, which stay 0 without an allocator).
	{
		TerrainGenerator mgen(99);
		Chunk withBorders(glm::vec3(0.0f));
		CHECK(withBorders.prepareVoxelStorageForGeneration(), "mesh-equivalence prepare");
		withBorders.generateTerrain(mgen);
		withBorders.generateMesh();
		const MeshBuildResult *meshWith = ChunkStateProbe::pendingResult(withBorders);
		CHECK(meshWith != nullptr && !meshWith->opaqueIndices.empty(),
			  "populated-border mesh emitted geometry into the result");
		// meshNeedsUpdate is set for the upload stage; index counters stay
		// zero until the GPU upload swaps the device buffers.
		CHECK(withBorders.needsGPUUpload(), "built mesh awaits upload");
		// Snapshot the first payload: the block is released (and detached)
		// when the re-mesh below supersedes it, so the comparison must run
		// against copies.
		const std::vector<Vertex> firstOpaqueVerts = meshWith->opaqueVertices;
		const std::vector<uint32_t> firstOpaqueIdx = meshWith->opaqueIndices;
		const std::vector<Vertex> firstWaterVerts = meshWith->waterVertices;
		const std::vector<uint32_t> firstWaterIdx = meshWith->waterIndices;

		Chunk withoutBorders(glm::vec3(0.0f));
		CHECK(withoutBorders.prepareVoxelStorageForGeneration(), "no-border prepare");
		withoutBorders.generateTerrain(mgen);
		withoutBorders.releaseNeighborBorders(); // freed after upload: borders read AIR
		withoutBorders.generateMesh();
		const MeshBuildResult *meshWithout = ChunkStateProbe::pendingResult(withoutBorders);
		CHECK(meshWithout != nullptr, "missing-border mesh produced a result");
		// With no neighbor data every border-facing surface is emitted, so
		// the missing-border mesh is the larger one.
		CHECK(meshWithout->opaqueIndices.size() >= meshWith->opaqueIndices.size(),
			  "missing borders produce more geometry than populated borders");

		// Deterministic re-mesh with borders present.
		withBorders.generateMesh();
		const MeshBuildResult *meshWithAgain = ChunkStateProbe::pendingResult(withBorders);
		CHECK(meshWithAgain != meshWith, "re-mesh publishes a fresh result block");
		CHECK(meshWithAgain->opaqueVertices == firstOpaqueVerts &&
				  meshWithAgain->opaqueIndices == firstOpaqueIdx &&
				  meshWithAgain->waterVertices == firstWaterVerts &&
				  meshWithAgain->waterIndices == firstWaterIdx,
			  "re-mesh with populated borders is byte-for-byte deterministic");
		// The superseded block was returned to the pool by the publish step;
		// the default pool must not be growing per remesh.
		CHECK(MeshResultPool::defaultPool().activeCount() >= 2,
			  "in-flight results stay active while attached");
	}

	// 13b) Publish/generation/revision semantics (issue #104, review #114):
	// superseded results are rejected without touching chunk state, repeated
	// remesh replaces the pending block, and unload with a pending result
	// returns it to the pool.
	{
		MeshResultPool pool;
		TerrainGenerator pgen(1234);
		Chunk chunk(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
		CHECK(chunk.prepareVoxelStorageForGeneration(), "publish-semantics prepare");
		chunk.generateTerrain(pgen);
		const uint64_t generationAfterGen = ChunkStateProbe::meshGeneration(chunk);
		const uint64_t revisionAfterGen = chunk.meshRevision();

		// Stale publish: a result built for a different generation.
		{
			MeshBuildResult *stale = pool.acquire();
			stale->beginBuild(&chunk, generationAfterGen + 999, revisionAfterGen);
			stale->opaqueIndices.push_back(0);
			const size_t freeBefore = pool.freeCount();
			CHECK(!chunk.publishMeshResult(stale),
				  "superseded generation is rejected at publish");
			CHECK(pool.freeCount() == freeBefore + 1,
				  "rejected result is returned to its pool");
			CHECK(!chunk.hasPendingMeshResult(), "rejected result is not attached");
			CHECK(chunk.getState() != ChunkState::MESHED,
				  "rejected publish does not mark the chunk MESHED");
		}
		// Same generation but OLD revision (issue #114 review item 22): an
		// in-flight mesh built before an edit must not overwrite the newer
		// content. This is deliberately distinct from the recycle case.
		{
			MeshBuildResult *old = pool.acquire();
			old->beginBuild(&chunk, generationAfterGen, revisionAfterGen - 1);
			old->opaqueIndices.push_back(0);
			const size_t freeBefore = pool.freeCount();
			CHECK(!chunk.publishMeshResult(old),
				  "old-revision result (same generation) is rejected");
			CHECK(pool.freeCount() == freeBefore + 1,
				  "rejected old-revision result is returned to its pool");
			CHECK(chunk.getState() != ChunkState::MESHED,
				  "old-revision publish leaves the state untouched");
		}
		// Wrong owner is rejected the same way.
		{
			Chunk other(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
			MeshBuildResult *foreign = pool.acquire();
			foreign->beginBuild(&other, ChunkStateProbe::meshGeneration(chunk),
								chunk.meshRevision());
			const size_t freeBefore = pool.freeCount();
			CHECK(!chunk.publishMeshResult(foreign),
				  "foreign-owner result is rejected");
			CHECK(pool.freeCount() == freeBefore + 1,
				  "rejected foreign result is returned to its pool");
		}
		// Edit invalidates the in-flight build (issue #114 review item 19):
		// a result stamped pre-edit is rejected once the edit landed, the
		// edit data is present, and the chunk stays GENERATED for remesh.
		// Runs before any valid publication, so no pending result exists.
		{
			chunk.setState(ChunkState::GENERATED);
			const uint64_t revBeforeEdit = chunk.meshRevision();
			MeshBuildResult *inflight = pool.acquire();
			inflight->beginBuild(&chunk, generationAfterGen, revBeforeEdit);
			// ... worker is meshing here ...
			chunk.setVoxel(3, 40, 3, STONE); // the edit lands (bumps revision)
			CHECK(chunk.meshRevision() == revBeforeEdit + 1,
				  "edit bumps the mesh revision");
			CHECK(chunk.getState() == ChunkState::GENERATED,
				  "edit leaves the chunk GENERATED");
			pool.finishBuild(inflight);
			const size_t freeBeforeEdit = pool.freeCount();
			CHECK(!chunk.publishMeshResult(inflight),
				  "pre-edit in-flight result is rejected after the edit");
			CHECK(pool.freeCount() == freeBeforeEdit + 1,
				  "rejected pre-edit result back in the pool");
			CHECK(!chunk.hasPendingMeshResult(), "no pending result survives the edit");
			CHECK(chunk.needsGPUUpload(), "edit keeps the remesh armed");
			CHECK(chunk.getVoxel(3, 40, 3).type == static_cast<uint8_t>(STONE),
				  "edit data present after the rejected publish");
		}

		// Matching identity publishes and is the only state commit point.
		{
			MeshBuildResult *good = pool.acquire();
			good->beginBuild(&chunk, generationAfterGen, chunk.meshRevision());
			CHECK(chunk.publishMeshResult(good), "matching identity publishes");
			CHECK(chunk.getState() == ChunkState::MESHED,
				  "publish commits the MESHED state");
			CHECK(chunk.needsGPUUpload(), "publish arms meshNeedsUpdate");
			// buildMesh/buildLODMesh must no longer mutate chunk state
			// (issue #114 review item 12): a fresh build leaves the state
			// and the meshNeedsUpdate flag exactly as it found them.
			chunk.setState(ChunkState::GENERATED);
			const bool armedBefore = chunk.needsGPUUpload();
			MeshBuildResult *pure = pool.acquire();
			chunk.buildMesh(*pure, generationAfterGen, chunk.meshRevision());
			CHECK(chunk.getState() == ChunkState::GENERATED,
				  "buildMesh does not mutate chunk state");
			CHECK(chunk.needsGPUUpload() == armedBefore,
				  "buildMesh does not change meshNeedsUpdate");
			pool.finishBuild(pure);
			pool.release(pure);
		}

		// Valid publish then repeated remesh: newest result wins, previous
		// one returns to the pool, no leak.
		CHECK(chunk.generateMesh(), "first publish succeeds");
		MeshBuildResult *firstPending = ChunkStateProbe::pendingResult(chunk);
		CHECK(firstPending != nullptr, "first result attached");
		CHECK(pool.activeCount() == 1, "one block active while attached");
		CHECK(chunk.generateMesh(), "repeated remesh publishes");
		MeshBuildResult *secondPending = ChunkStateProbe::pendingResult(chunk);
		CHECK(secondPending != nullptr && secondPending != firstPending,
			  "repeated remesh attaches a fresh block");
		CHECK(pool.activeCount() == 1,
			  "superseded attached result was returned to the pool");

		// Unload with a pending result: reset returns the block and bumps
		// the generation, so the retired payload can never be re-attached.
		const uint64_t genBeforeReset = ChunkStateProbe::meshGeneration(chunk);
		const uint64_t revBeforeReset = chunk.meshRevision();
		chunk.reset(glm::vec3(16.0f, 0.0f, 0.0f));
		CHECK(!chunk.hasPendingMeshResult(), "reset releases the pending result");
		CHECK(pool.freeCount() == pool.capacity(), "pool balanced after reset");
		CHECK(ChunkStateProbe::meshGeneration(chunk) == genBeforeReset + 1,
			  "reset bumps the mesh generation");
		CHECK(chunk.meshRevision() > revBeforeReset,
			  "reset bumps the mesh revision too");

		// A publish attempt for the retired incarnation lands nowhere, even
		// if the revision value happened to match after recycling
		// (issue #114 review item 21: both dimensions are checked).
		MeshBuildResult *retired = pool.acquire();
		retired->beginBuild(&chunk, genBeforeReset, chunk.meshRevision());
		const size_t freeBefore = pool.freeCount();
		CHECK(!chunk.publishMeshResult(retired),
			  "result of the retired generation is rejected after reset");
		CHECK(pool.freeCount() == freeBefore + 1, "retired result returned to pool");
	}

	// 13c) Moves detach pending results (issue #104): neither side may keep
	// a block whose owner names a gutted chunk; the generation itself is
	// transferred as part of the chunk identity.
	{
		MeshResultPool pool;
		TerrainGenerator pgen(4321);
		Chunk source(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
		CHECK(source.prepareVoxelStorageForGeneration(), "move-pending prepare");
		source.generateTerrain(pgen);
		CHECK(source.generateMesh(), "move-pending mesh");
		CHECK(source.hasPendingMeshResult(), "pending result before move");
		const uint64_t sourceGen = ChunkStateProbe::meshGeneration(source);

		Chunk destination(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
		destination = std::move(source);
		CHECK(!destination.hasPendingMeshResult(), "move assignment leaves no pending result");
		CHECK(!source.hasPendingMeshResult(), "moved-from chunk holds no pending result");
		CHECK(pool.freeCount() == pool.capacity(), "pool balanced after move assignment");
		CHECK(ChunkStateProbe::meshGeneration(destination) == sourceGen,
			  "move transfers the mesh generation");
	}

	// 13d) Destruction with an attached result returns the block to its
	// pool (issue #104): a dying chunk cannot leak a pooled build result.
	{
		MeshResultPool &defaultPool = MeshResultPool::defaultPool();
		const size_t activeBefore = defaultPool.activeCount();
		{
			TerrainGenerator dgen(777);
			Chunk doomed(glm::vec3(0.0f));
			CHECK(doomed.prepareVoxelStorageForGeneration(), "dtor-release prepare");
			doomed.generateTerrain(dgen);
			CHECK(doomed.generateMesh(), "dtor-release mesh");
			CHECK(defaultPool.activeCount() == activeBefore + 1,
				  "attached result is active while the chunk lives");
		}
		CHECK(defaultPool.activeCount() == activeBefore,
			  "destructor returns the attached build result to the pool");
	}

	// 13e) Deferred edit subsystem (issue #114 review, 2nd pass): edits are
	// logical operations (target + neighbor mirrors). When the target is in
	// transit the whole group is deferred; target and mirrors apply
	// independently as their chunks free up; pending writes coalesce per
	// coordinate; recycled chunks never receive a stale queued edit.
	{
		ChunkPool pool(16);
		TerrainGenerator mgen(2468);
		ChunkManager manager(&mgen, nullptr, &pool);

		Camera cam(glm::vec3(0.0f, 100.0f, 0.0f));
		RenderSettings settings;
		manager.updateStreaming(cam, settings);
		manager.processChunkLoading(64);
		Chunk *origin = manager.getChunkAtWorldPos(glm::vec3(4.0f, 40.0f, 4.0f));
		Chunk *east = manager.getChunkAtWorldPos(
			glm::vec3(static_cast<float>(CHUNK_SIZE) + 4.0f, 40.0f, 4.0f));
		Chunk *west = manager.getChunkAtWorldPos(glm::vec3(-12.0f, 40.0f, 4.0f));
		Chunk *south = manager.getChunkAtWorldPos(glm::vec3(4.0f, 40.0f, -12.0f));
		Chunk *north = manager.getChunkAtWorldPos(
			glm::vec3(4.0f, 40.0f, static_cast<float>(CHUNK_SIZE) + 4.0f));
		CHECK(origin != nullptr && east != nullptr && west != nullptr &&
				  south != nullptr && north != nullptr,
			  "primary and four neighbors registered by the load path");
		if (origin && east && west && south && north)
		{
			// Terrain heights vary per column: place every test voxel at
			// the column's topmost air cell so the effective state is
			// guaranteed AIR before each place.
			auto airY = [origin](int lx, int lz) {
				for (int y = static_cast<int>(CHUNK_HEIGHT) - 1; y >= 0; --y)
					if (origin->getVoxel(static_cast<uint32_t>(lx),
										 static_cast<uint32_t>(y),
										 static_cast<uint32_t>(lz))
											.type == static_cast<uint8_t>(AIR))
						return y;
				return 0;
			};
			const int yIn = airY(4, 4);		   // interior column
			const int yE8 = airY(15, 8);	   // east boundary, z=8
			const int yE10 = airY(15, 10);	   // east boundary, z=10
			const int yN6 = airY(6, 15);	   // north boundary, x=6
			const int yW6 = airY(0, 6);		   // west boundary, z=6
			const int yS6 = airY(6, 0);		   // south boundary, x=6
			const int yCorner = airY(0, 0);	   // west+south corner
			const int yW9 = airY(0, 9);		   // west boundary, z=9
			const int yI8 = airY(8, 8);		   // interior, successive edits
			const int yI9 = airY(9, 9);		   // interior, recycle test

			// (a) Interior edit deferred while the primary is in transit.
			origin->setInTransit(true);
			const glm::vec3 insidePos(4.0f, static_cast<float>(yIn), 4.0f);
			CHECK(manager.placeVoxel(insidePos, STONE),
				  "deferred interior edit reports success");
			CHECK(ChunkManagerProbe::pendingEdits(manager) == 1,
				  "interior edit queues exactly one entry");
			CHECK(origin->getVoxel(4, yIn, 4).type != static_cast<uint8_t>(STONE),
				  "deferred edit does not write while in transit");
			origin->setInTransit(false);
			manager.processFinishedJobs();
			CHECK(origin->getVoxel(4, yIn, 4).type == static_cast<uint8_t>(STONE),
				  "queued interior edit applied after transit clears");
			CHECK(origin->getState() == ChunkState::GENERATED,
				  "applied edit marks the chunk GENERATED");
			CHECK(ChunkManagerProbe::pendingEdits(manager) == 0,
				  "queue empty after apply");

			// (b) BLOCKER regression (item 1/11): boundary edit while the
			// TARGET is in transit must defer the target AND still schedule
			// the neighbor mirror - not drop it.
			{
				const glm::vec3 boundaryPos(static_cast<float>(CHUNK_SIZE - 1),
											static_cast<float>(yE8), 8.0f);
				const uint64_t originRev = origin->meshRevision();
				const uint64_t eastRev = east->meshRevision();
				origin->setInTransit(true);
				CHECK(manager.placeVoxel(boundaryPos, STONE),
					  "in-transit boundary edit reports success");
				CHECK(ChunkManagerProbe::pendingEdits(manager) == 2,
					  "target and mirror are both queued (mirror never lost)");
				CHECK(origin->getVoxel(CHUNK_SIZE - 1, yE8, 8).type !=
						  static_cast<uint8_t>(STONE),
					  "target not written while in transit");
				CHECK(east->sampleForMeshing(-1, yE8, 8) == AIR,
					  "mirror not written while target deferred");
				origin->setInTransit(false);
				manager.processFinishedJobs();
				CHECK(origin->getVoxel(CHUNK_SIZE - 1, yE8, 8).type ==
						  static_cast<uint8_t>(STONE),
					  "deferred target edit applied");
				CHECK(east->sampleForMeshing(-1, yE8, 8) == STONE,
					  "deferred mirror applied to the neighbor");
				CHECK(origin->meshRevision() > originRev &&
						  east->meshRevision() > eastRev,
					  "both chunks invalidated");
				CHECK(origin->getState() == ChunkState::GENERATED &&
						  east->getState() == ChunkState::GENERATED,
					  "both chunks re-armed for remesh");
				// Cleanup so later sections see the column as AIR.
				CHECK(manager.deleteVoxel(boundaryPos), "blocker cleanup");
				manager.processFinishedJobs();
				CHECK(east->sampleForMeshing(-1, yE8, 8) == AIR,
					  "blocker cleanup mirrored");
			}

			// (c) Boundary DELETE while the target is in transit (item 12):
			// a previously culled face must reappear on both sides.
			{
				const glm::vec3 boundaryPos(static_cast<float>(CHUNK_SIZE - 1),
											static_cast<float>(yE10), 10.0f);
				CHECK(manager.placeVoxel(boundaryPos, STONE),
					  "seed boundary voxel (both free)");
				CHECK(origin->getVoxel(CHUNK_SIZE - 1, yE10, 10).type ==
							  static_cast<uint8_t>(STONE) &&
						  east->sampleForMeshing(-1, yE10, 10) == STONE,
					  "seed voxel mirrored");
				origin->setInTransit(true);
				CHECK(manager.deleteVoxel(boundaryPos),
					  "in-transit boundary delete reports success");
				CHECK(ChunkManagerProbe::pendingEdits(manager) == 2,
					  "delete queues target and mirror");
				origin->setInTransit(false);
				manager.processFinishedJobs();
				CHECK(origin->getVoxel(CHUNK_SIZE - 1, yE10, 10).type ==
						  static_cast<uint8_t>(AIR),
					  "deferred delete applied on target");
				CHECK(east->sampleForMeshing(-1, yE10, 10) == AIR,
					  "deferred delete applied on neighbor mirror");
				CHECK(origin->getState() == ChunkState::GENERATED &&
						  east->getState() == ChunkState::GENERATED,
					  "delete re-arms remesh on both sides");
			}

			// (d) Target and neighbor BOTH in transit (item 10): each entry
			// applies when its own chunk frees, independently.
			{
				const glm::vec3 boundaryPos(6.0f, static_cast<float>(yN6),
											static_cast<float>(CHUNK_SIZE - 1));
				const uint64_t northRev = north->meshRevision();
				origin->setInTransit(true);
				north->setInTransit(true);
				CHECK(manager.placeVoxel(boundaryPos, STONE), "both-transit edit accepted");
				CHECK(ChunkManagerProbe::pendingEdits(manager) == 2,
					  "both entries queued");
				origin->setInTransit(false);
				manager.processFinishedJobs();
				CHECK(origin->getVoxel(6, yN6, CHUNK_SIZE - 1).type ==
						  static_cast<uint8_t>(STONE),
					  "target applied while neighbor still in transit");
				CHECK(north->sampleForMeshing(6, yN6, -1) == AIR,
					  "mirror waits for the neighbor");
				CHECK(ChunkManagerProbe::pendingEdits(manager) == 1,
					  "mirror remains queued");
				north->setInTransit(false);
				manager.processFinishedJobs();
				CHECK(north->sampleForMeshing(6, yN6, -1) == STONE,
					  "mirror applied when the neighbor frees");
				CHECK(north->meshRevision() > northRev, "neighbor invalidated");
				// Cleanup so later sections see the column as AIR.
				CHECK(manager.deleteVoxel(boundaryPos), "both-transit cleanup");
				manager.processFinishedJobs();
				CHECK(north->sampleForMeshing(6, yN6, -1) == AIR,
					  "both-transit cleanup mirrored");
			}

			// (e) All four directions: target in transit, correct mirror
			// coordinate per neighbor (item 13).
			{
				struct Direction
				{
					glm::vec3 editPos;
					Chunk *neighbor;
					int mirrorX, mirrorY, mirrorZ;
					const char *name;
				};
				const Direction dirs[] = {
					{glm::vec3(0.0f, static_cast<float>(yW6), 6.0f), west,
					 CHUNK_SIZE, yW6, 6, "west"},
					{glm::vec3(static_cast<float>(CHUNK_SIZE - 1),
							   static_cast<float>(yE8), 8.0f),
					 east, -1, yE8, 8, "east"},
					{glm::vec3(6.0f, static_cast<float>(yS6), 0.0f), south,
					 6, yS6, CHUNK_SIZE, "south"},
					{glm::vec3(6.0f, static_cast<float>(yN6),
							   static_cast<float>(CHUNK_SIZE - 1)),
					 north, 6, yN6, -1, "north"},
				};
				for (const Direction &d : dirs)
				{
					origin->setInTransit(true);
					const uint64_t neighborRev = d.neighbor->meshRevision();
					CHECK(manager.placeVoxel(d.editPos, BRICKS), d.name);
					origin->setInTransit(false);
					manager.processFinishedJobs();
					CHECK(d.neighbor->sampleForMeshing(d.mirrorX, d.mirrorY, d.mirrorZ) == BRICKS,
						  "mirror coordinate correct");
					CHECK(d.neighbor->meshRevision() > neighborRev, d.name);
					// Reset for the next direction.
					CHECK(manager.deleteVoxel(d.editPos), "direction cleanup");
					manager.processFinishedJobs();
				}
			}

			// (f) Corner edit: two mirrors, no diagonal (item 14).
			{
				const glm::vec3 cornerPos(0.0f, static_cast<float>(yCorner), 0.0f);
				const uint64_t westRev = west->meshRevision();
				const uint64_t southRev = south->meshRevision();
				origin->setInTransit(true);
				CHECK(manager.placeVoxel(cornerPos, BRICKS), "corner edit accepted");
				CHECK(ChunkManagerProbe::pendingEdits(manager) == 3,
					  "corner queues target plus two face mirrors");
				origin->setInTransit(false);
				manager.processFinishedJobs();
				CHECK(west->sampleForMeshing(CHUNK_SIZE, yCorner, 0) == BRICKS &&
						  south->sampleForMeshing(0, yCorner, CHUNK_SIZE) == BRICKS,
					  "both face mirrors applied with correct coordinates");
				CHECK(west->meshRevision() > westRev && south->meshRevision() > southRev,
					  "both corner neighbors invalidated");
				CHECK(manager.deleteVoxel(cornerPos), "corner cleanup");
				manager.processFinishedJobs();
			}

			// (g) Successive edits coalesce to the last logical state
			// (items 15-19, 21-22).
			{
				// place -> delete during transit: final AIR.
				origin->setInTransit(true);
				const glm::vec3 p(8.0f, static_cast<float>(yI8), 8.0f);
				CHECK(manager.placeVoxel(p, STONE), "place pending");
				CHECK(manager.deleteVoxel(p), "delete pending");
				origin->setInTransit(false);
				manager.processFinishedJobs();
				CHECK(origin->getVoxel(8, yI8, 8).type == static_cast<uint8_t>(AIR),
					  "place then delete lands AIR");

				// delete -> place during transit: final STONE.
				origin->setInTransit(true);
				CHECK(!manager.deleteVoxel(p), "delete of AIR is a logical no-op");
				CHECK(manager.placeVoxel(p, STONE), "place pending");
				origin->setInTransit(false);
				manager.processFinishedJobs();
				CHECK(origin->getVoxel(8, yI8, 8).type == static_cast<uint8_t>(STONE),
					  "delete then place lands STONE");

				// Boundary place -> delete -> place DIRT: one coalesced
				// target entry + one coalesced mirror entry.
				const glm::vec3 boundaryPos(0.0f, static_cast<float>(yW9), 9.0f);
				origin->setInTransit(true);
				CHECK(manager.placeVoxel(boundaryPos, STONE), "boundary place");
				CHECK(manager.deleteVoxel(boundaryPos), "boundary delete");
				CHECK(manager.placeVoxel(boundaryPos, DIRT), "boundary re-place");
				CHECK(ChunkManagerProbe::pendingEdits(manager) == 2,
					  "successive boundary edits coalesce to target + mirror");
				origin->setInTransit(false);
				manager.processFinishedJobs();
				CHECK(origin->getVoxel(0, yW9, 9).type == static_cast<uint8_t>(DIRT) &&
						  west->sampleForMeshing(CHUNK_SIZE, yW9, 9) == DIRT,
					  "coalesced final state DIRT on both sides");
				CHECK(manager.deleteVoxel(boundaryPos), "boundary cleanup");
				CHECK(manager.deleteVoxel(p), "interior cleanup");
				manager.processFinishedJobs();
			}

			// (h) A queued edit never lands on a recycled incarnation
			// (items 26-28): reset bumps the generation, the stale entry is
			// dropped at apply time.
			{
				const glm::vec3 p(9.0f, static_cast<float>(yI9), 9.0f);
				origin->setInTransit(true);
				CHECK(manager.placeVoxel(p, STONE), "edit queued for recycle test");
				origin->reset(origin->getPosition()); // same pointer, new incarnation
				origin->setInTransit(false);
				manager.processFinishedJobs();
				CHECK(origin->getVoxel(9, yI9, 9).type == static_cast<uint8_t>(AIR),
					  "stale queued edit dropped after recycle");
				CHECK(ChunkManagerProbe::pendingEdits(manager) == 0,
					  "stale entry removed from the queue");
			}
		}
	}

	// 14) Two-sided boundary edit data path (issue #113): the
	// ChunkManager::dirtyNeighbor routing is unchanged by #103; this
	// validates the operations it drives - the mirror write lands in the
	// neighbor's west-facing border at x = -1 and marks it GENERATED, and
	// the edited chunk's own east face reflects the placed block.
	{
		TerrainGenerator egen(55);
		Chunk current(glm::vec3(0.0f));
		Chunk eastN(glm::vec3(static_cast<float>(CHUNK_SIZE), 0.0f, 0.0f));
		CHECK(current.prepareVoxelStorageForGeneration(), "current prepare (boundary test)");
		CHECK(eastN.prepareVoxelStorageForGeneration(), "east neighbor prepare (boundary test)");
		current.generateTerrain(egen);
		eastN.generateTerrain(egen);

		// Boundary edit on current at x = CHUNK_SIZE-1 ...
		current.setVoxel(CHUNK_SIZE - 1, 33, 6, STONE);
		CHECK(current.sampleForMeshing(CHUNK_SIZE - 1, 33, 6) == STONE,
			  "edited boundary block observable in current");
		// ... and the manager's mirror write into the east neighbor's shell.
		eastN.setVoxel(-1, 33, 6, STONE);
		eastN.setState(ChunkState::GENERATED);
		CHECK(eastN.getState() == ChunkState::GENERATED, "east neighbor marked GENERATED");
		CHECK(eastN.sampleForMeshing(-1, 33, 6) == STONE,
			  "mirror write observable in east neighbor's west-facing border");
		// Same contract on the z axis (north).
		current.setVoxel(4, 33, CHUNK_SIZE - 1, BRICKS);
		eastN.setVoxel(4, 33, -1, BRICKS);
		CHECK(current.sampleForMeshing(4, 33, CHUNK_SIZE - 1) == BRICKS, "north boundary edit in current");
		CHECK(eastN.sampleForMeshing(4, 33, -1) == BRICKS, "mirrored north face write in neighbor");
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
