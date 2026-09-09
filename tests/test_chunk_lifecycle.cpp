// Chunk lifecycle (issue #78 review): move semantics must carry the FULL
// generation state (including the per-column biomeTypes/heightMap added by
// the direct-to-pooled-storage change), and generateTerrain() must produce
// data identical to the owning generateChunk() path directly into reusable
// pooled-style storage across reset/regenerate cycles - with the compact
// occupancy metadata staying in sync and no capacity churn.
#include <Chunk/Chunk.hpp>
#include <Chunk/ChunkManager.hpp>
#include <Chunk/ChunkMeshResult.hpp>
#include <Chunk/ChunkPool.hpp>
#include <Chunk/ChunkCollisionView.hpp>
#include <Chunk/ChunkMobWorld.hpp>
#include <Physics/PlayerController.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Camera/Camera.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
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
// queue size so coalescing behavior is observable without public API, and
// the effective-voxel decision helper for the UNLOADED-backing contract.
struct ChunkManagerProbe
{
	static size_t pendingEdits(const ChunkManager &m) { return m.m_pendingEdits.size(); }
	static TextureType effective(const ChunkManager &m, const Chunk *chunk, int x, int y, int z)
	{
		return m.effectiveVoxelType(chunk, x, y, z);
	}
	// Test hook for the superseded-result drop path (PR #117 review phases
	// 11-12): inject a completed mesh job as a worker would, without
	// running the async machinery.
	static void injectCompletedMeshJob(ChunkManager &m, Chunk *chunk, MeshBuildResult *result)
	{
		std::lock_guard<std::mutex> lock(m.m_completedJobsMutex);
		m.m_completedMeshJobs.push_back({chunk, result});
	}
	// Test hook for the cross-chunk light arrival path (issue #141 review
	// round 2, section 26): inject a completed generation chunk as a worker
	// would, so processFinishedJobs() runs dirtyNeighborsForArrivedLight()
	// under its lock without the async machinery.
	static void injectCompletedGenChunk(ChunkManager &m, Chunk *c)
	{
		std::lock_guard<std::mutex> lock(m.m_completedJobsMutex);
		m.m_completedGenerationChunks.push_back(c);
	}
	// Test hook for the cross-chunk light invalidation tests (issue #141
	// review fix, section 25): register an externally-owned chunk at a
	// coordinate without running the streaming load path.
	static void registerChunk(ChunkManager &m, const glm::ivec3 &pos, Chunk *c)
	{
		m.m_chunks[pos] = c;
		m.m_activeChunks.push_back(c);
		c->setActiveIndex(m.m_activeChunks.size() - 1);
	}
};

// Full-quality payload lives in per-section slots since issue #107; these
// helpers keep whole-payload assertions readable.
static size_t totalOpaqueIndices(const MeshBuildResult &r)
{
	size_t n = 0;
	for (const SectionMeshPayload &s : r.sections)
		n += s.opaqueIndices.size();
	return n;
}
static size_t totalOpaqueVertices(const MeshBuildResult &r)
{
	size_t n = 0;
	for (const SectionMeshPayload &s : r.sections)
		n += s.opaqueVertices.size();
	return n;
}

// Vertex decode helpers for block-light scans (issue #141). packedPos is
// 9b X / 14b Y / 9b Z quantized at 1/16 voxel units (Vertex::packPosition);
// packedData carries sky @14, block R @18, G @22, B @26 (issue #110 layout,
// RGB extension #141). All 4 vertices of a greedy quad share one light
// packing, so scanning vertices is equivalent to scanning quads.
static uint32_t vQuantX(const Vertex &v) { return v.packedPos & 0x1FFu; }
static uint32_t vQuantY(const Vertex &v) { return (v.packedPos >> 9) & 0x3FFFu; }
static uint32_t vQuantZ(const Vertex &v) { return (v.packedPos >> 23) & 0x1FFu; }
static uint32_t vBlockR(const Vertex &v) { return (v.packedData >> 18) & 0xFu; }
static uint32_t vBlockB(const Vertex &v) { return (v.packedData >> 26) & 0xFu; }

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
	// Mutable backing access for poison/fill tests of the lifecycle
	// contract (read-only VoxelView cannot express them).
	static Voxel *voxelsMut(Chunk &c)
	{
		return c.m_storage ? c.m_storage->voxels.data() : nullptr;
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
	static const std::array<uint16_t, Chunk::kOccupancySections> &sections(const Chunk &c)
	{
		return c.m_sectionNonAir;
	}
	static bool occupiedSpan(const Chunk &c, int &minY, int &maxY)
	{
		return c.occupiedSpanY(minY, maxY);
	}
	static uint16_t sectionMask(const Chunk &c)
	{
		return c.occupiedSectionMask();
	}
	// GPU slot layout + dirty mask state for the move-semantics tests
	// (issue #107, PR #117 review phases 23-24).
	static std::array<Chunk::SectionGpuSlot, Chunk::kOccupancySections> &sectionGpuMut(Chunk &c)
	{
		return c.m_sectionGpu;
	}
	static std::array<Chunk::SectionGpuSlot, Chunk::kOccupancySections> &sectionGpuWaterMut(Chunk &c)
	{
		return c.m_sectionGpuWater;
	}
	// LOD arena ranges + shared-arenas pointer (issue #109 move semantics).
	static MeshArena::Range &lodOpaqueVMut(Chunk &c) { return c.m_lodOpaqueVertices; }
	static MeshArena::Range &lodOpaqueIMut(Chunk &c) { return c.m_lodOpaqueIndices; }
	static MeshArena::Range &lodWaterVMut(Chunk &c) { return c.m_lodWaterVertices; }
	static MeshArena::Range &lodWaterIMut(Chunk &c) { return c.m_lodWaterIndices; }
	static void setArenas(Chunk &c, MeshArenas &a) { c.m_arenas = &a; }
	static void clearArenas(Chunk &c) { c.m_arenas = nullptr; }
	static const MeshArenas *arenas(const Chunk &c) { return c.m_arenas; }
	static void buildMeshRanged(Chunk &c, MeshBuildResult &out, uint64_t generation,
								uint64_t revision, int minY, int maxY)
	{
		c.buildMeshRanged(out, generation, revision, minY, maxY);
	}
	static void buildLODMeshRanged(Chunk &c, MeshBuildResult &out, int scanTopY)
	{
		c.buildLODMeshRanged(out, scanTopY);
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
	std::array<uint16_t, Chunk::kOccupancySections> sections{};
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
		s.sections = ChunkStateProbe::sections(c);
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
		   a.heights == b.heights && a.sections == b.sections;
}

/// The per-section occupancy counters must match a brute-force scan of the
/// voxel storage exactly - after generation AND after every edit (issue
/// #105). Sections are 16 voxels tall; counts saturate well below uint16.
static void checkOccupancyMetadataInSync(const Chunk &chunk, const char *label)
{
	const auto voxels = ChunkStateProbe::voxels(chunk);
	std::array<uint16_t, Chunk::kOccupancySections> brute{};
	if (ChunkStateProbe::hasStorage(chunk))
	{
		CHECK(voxels.size() == CHUNK_VOLUME, label);
		constexpr size_t kSectionVolume =
		    static_cast<size_t>(Chunk::kOccupancySectionSize) * CHUNK_SIZE * CHUNK_SIZE;
		for (size_t i = 0; i < voxels.size(); ++i)
			brute[i / kSectionVolume] +=
			    voxels[i].type != static_cast<uint8_t>(AIR) ? 1 : 0;
	}
	CHECK(brute == ChunkStateProbe::sections(chunk), label);
	// Derived view consistency: the mask bits must match the counts, and a
	// non-empty chunk must expose a span while an empty one must not.
	uint16_t mask = 0;
	int expMin = -1, expMax = -1;
	for (int s = 0; s < Chunk::kOccupancySections; ++s)
	{
		if (brute[s] == 0)
			continue;
		mask |= static_cast<uint16_t>(1u << s);
		if (expMin < 0)
			expMin = s * Chunk::kOccupancySectionSize;
		expMax = s * Chunk::kOccupancySectionSize + Chunk::kOccupancySectionSize - 1;
	}
	CHECK(ChunkStateProbe::sectionMask(chunk) == mask, label);
	int minY = -1, maxY = -1;
	const bool has = ChunkStateProbe::occupiedSpan(chunk, minY, maxY);
	CHECK(has == (mask != 0), label);
	CHECK(!has || (minY == expMin && maxY == expMax), label);
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

// Streaming maintenance cost, isolated from chunk loading (issue #108): a
// full ChunkManager runs its updateStreaming dispatch with an empty queue
// region per regime — stationary (zero-work path) and forced incremental
// (anchor crossing every call). No chunks are acquired, so the number IS the
// maintenance cost, not masked by allocation/gen/mesh work.
static int runStreamPerf()
{
	using Clock = std::chrono::steady_clock;
	TerrainGenerator generator(42);
	ChunkPool pool(64);
	ChunkManager manager(&generator, nullptr, &pool);
	RenderSettings settings;
	settings.streamFrontBias = 0.3f;
	settings.maxRenderDistance = 512;
	Camera camera(glm::vec3(8.f, 100.f, 8.f));
	manager.updateStreaming(camera, settings);

	constexpr int stationaryN = 200000;
	const auto start = Clock::now();
	for (int i = 0; i < stationaryN; ++i)
		manager.updateStreaming(camera, settings);
	const double stationaryMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();

	constexpr int incrementalN = 100000;
	const auto start2 = Clock::now();
	for (int i = 1; i <= incrementalN; ++i)
	{
		camera.setPosition(glm::vec3(8.f + static_cast<float>(i) * 4.1f, 100.f, 8.f)); // new anchor each call
		manager.updateStreaming(camera, settings);
	}
	const double incrementalMs = std::chrono::duration<double, std::milli>(Clock::now() - start2).count();

	const auto s = manager.streamingMaintenanceStats();
	std::cout << "[Stream Perf] view=512 bias=0.3; queue stays FULL (nothing consumed)\n"
			  << "  -> incremental is the pessimistic sort-dominated bound; in-game the\n"
			  << "  load budget drains the queue every frame, shrinking the sort.\n"
			  << "stationary:  " << stationaryN << " calls, " << stationaryMs << " ms total, "
			  << stationaryMs / stationaryN * 1000.0 << " us/call (zeroWork=" << s.zeroWork << ")\n"
			  << "incremental: " << incrementalN << " calls, " << incrementalMs << " ms total, "
			  << incrementalMs / incrementalN * 1000.0 << " us/call (incremental=" << s.incrementalUpdates << ")\n";
	return 0;
}

static int profileMobs()
{
    TerrainGenerator generator(42);
    glm::ivec2 center{};
    bool found = false;
    for (int radius = 0; radius <= 32 && !found; ++radius)
    for (int z = -radius; z <= radius && !found; ++z)
    for (int x = -radius; x <= radius && !found; ++x)
    {
        if (std::max(std::abs(x), std::abs(z)) != radius) continue;
        auto biome = generator.getBiomeAt(x * 64, z * 64);
        if (biome == BIOME_PLAINS || biome == BIOME_FLOWER_MEADOW)
        { center = {x * 64, z * 64}; found = true; }
    }
    if (!found) { std::cerr << "No meadow fixture found\n"; return 1; }
    ChunkPool pool(512);
    ChunkManager manager(&generator, nullptr, &pool);
    Camera camera({float(center.x), 100.f, float(center.y)});
    RenderSettings settings;
    settings.minRenderDistance = 112; settings.maxRenderDistance = 112;
    manager.updateStreaming(camera, settings);
    manager.processChunkLoading(512);
    {
        ChunkMobWorld world(manager, generator);
        CHECK(!world.surface(center.x, center.y), "unpublished terrain cannot spawn mobs");
    }
    for (auto *chunk : manager.getActiveChunks())
        if (!manager.prepareAndGenerateChunk(chunk, generator)) return 1;
    entities::MobSystem mobs;
    mobs.reset(42);
    for (int i = 0; i < 600; ++i) {
        ChunkMobWorld world(manager, generator);
        mobs.update(1.0/60, world, {center.x,100,center.y},112);
    }
    const auto natural = mobs.mobs().size();
    CHECK(natural > 0, "natural mobs spawn on generated meadow voxels");
    {
        ChunkMobWorld world(manager, generator);
        for (int z = -72; z <= 72 && mobs.mobs().size() < 48; z += 3)
        for (int x = -72; x <= 72 && mobs.mobs().size() < 48; x += 3) {
            auto feet = world.surface(center.x+x,center.y+z);
            if(feet) mobs.add(entities::MobSpecies(mobs.mobs().size()%4),*feet,
                100000+mobs.mobs().size(),100000+mobs.mobs().size(),world);
        }
    }
    CHECK(mobs.mobs().size()==48, "48 real-terrain fixture mobs");
    std::cout << "MOB FIXTURE seed=42 center=" << center.x << "," << center.y << " natural=" << natural << "\n";
    for (const auto &mob : mobs.mobs()) std::cout << "mob " << int(mob.species) << " at " << mob.body.position.x << " " << mob.body.position.y << " " << mob.body.position.z << "\n";
    std::vector<double> timings; timings.reserve(6000);
    uint64_t cells = 0; double distance = 0;
    for (int i=0; i<6600; ++i) {
        std::array<glm::dvec3,48> before{};
        for(size_t j=0;j<mobs.mobs().size();++j) before[j]=mobs.mobs()[j].body.position;
        auto start=std::chrono::steady_clock::now();
        {
            ChunkMobWorld world(manager,generator);
            mobs.update(1.0/60,world,{center.x,100,center.y},112);
        }
        double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(i>=600) {
            timings.push_back(ms);cells+=mobs.queryStats().cells;
            for(size_t j=0;j<mobs.mobs().size();++j) distance+=glm::length(mobs.mobs()[j].body.position-before[j]);
        }
    }
    double total=0;for(auto ms:timings)total+=ms;std::sort(timings.begin(),timings.end());
    std::cout << "Mobs real voxels count=" << mobs.mobs().size() << " mean_ms=" << total/timings.size()
        << " p95_ms=" << timings[timings.size()*95/100] << " cells/tick=" << cells/timings.size()
        << " travelled=" << distance << " dropped=" << mobs.droppedSteps() << "\n";

    // Measure local-light sampling CPU time for all 48 mobs (issue #128 performance validation)
    std::vector<double> sampleTimings; sampleTimings.reserve(6000);
    std::vector<entities::MobRenderState> states;
    mobs.renderStates(states);
    {
        ChunkMobWorld world(manager, generator);
        for (int i = 0; i < 6000; ++i) {
            auto start = std::chrono::steady_clock::now();
            for (auto &ms : states) {
                const auto pos = entities::mobLightSamplePosition(ms);
                const auto light = world.sampleLight(pos);
                ms.localSkylight = light.skylight;
                ms.localBlockRgb = light.blockRgb;
            }
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (i >= 600) sampleTimings.push_back(ms);
        }
    }
    double sampleTotal = 0; for (auto ms : sampleTimings) sampleTotal += ms;
    std::sort(sampleTimings.begin(), sampleTimings.end());
    std::cout << "48 mobs local light sampling CPU mean_ms=" << sampleTotal / sampleTimings.size()
              << " p95_ms=" << sampleTimings[sampleTimings.size() * 95 / 100]
              << " per_mob_us=" << (sampleTotal / sampleTimings.size() / 48.0) * 1000.0 << "\n";
    CHECK(distance>100, "animals actually move across generated terrain");
    CHECK(mobs.mobs().size()<=48, "real-world population bounded");
    return g_fails?1:0;
}

static int profilePlayerPhysics()
{
	// A deterministic controller path through real, generated voxel data.
	// No rendering/GPU work is included; the scoped adapter cost IS included.
	ChunkPool pool(64);
	TerrainGenerator generator(42);
	ChunkManager manager(&generator, nullptr, &pool);
	Camera camera({0.f, 100.f, 0.f});
	RenderSettings settings;
	manager.updateStreaming(camera, settings);
	manager.processChunkLoading(64);
	for (int x = -2; x <= 2; ++x)
	for (int z = -2; z <= 2; ++z)
	{
		Chunk *chunk = manager.getChunk({x, 0, z});
		if (chunk && !manager.prepareAndGenerateChunk(chunk, generator)) return 1;
	}
	glm::dvec3 spawn(0.5, 255, 0.5);
	{
		ChunkCollisionView view(manager);
		for (int y = 255; y >= 0; --y)
			if (view.sample({0, y, 0}).solid) { spawn.y = y + 1.0 + physics::skin; break; }
	}
	physics::PlayerController player;
	std::vector<double> times;
	times.reserve(12000);
	uint64_t cells = 0, waits = 0;
	double travelled = 0;
	for (int i = 0; i < 13200; ++i)
	{
		if (i % 600 == 0) player.reset(spawn);
		physics::PlayerInput input;
		const double angle = (i % 600) * 0.012;
		input.move = {std::cos(angle), std::sin(angle)};
		input.sprint = (i / 300) % 2 == 1;
		input.jumpPressed = i % 90 == 0;
		input.swimUp = true;
		const auto before = player.body.position;
		const auto start = std::chrono::steady_clock::now();
		{
			ChunkCollisionView view(manager);
			player.advance(physics::PlayerController::fixedStep, input, view);
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		if (i >= 1200)
		{
			times.push_back(ms);
			cells += player.metrics.queries.cells;
			waits += player.body.waitingForTerrain ? 1 : 0;
			travelled += glm::length(player.body.position - before);
		}
	}
	double total = 0;
	for (double ms : times) total += ms;
	std::sort(times.begin(), times.end());
	std::cout << "Physics real voxels seed=42 ticks=" << times.size()
		<< " avg_ms=" << total / times.size() << " p95_ms=" << times[times.size() * 95 / 100]
		<< " cells_per_tick=" << double(cells) / times.size()
		<< " waiting_ticks=" << waits << " travelled_blocks=" << travelled
		<< " dropped_steps=" << player.metrics.droppedSteps << '\n';
	return 0;
}

/// Locate a chunk whose centre and corners sit solidly inland. The reworked
/// continentalness moved coastlines, so fixed chunk pins drift between
/// calibrations; a located origin keeps the mesh-equivalence checks on real
/// opaque terrain regardless of where the seas end up.
static glm::ivec2 locateInlandChunkOrigin(TerrainGenerator &gen)
{
	constexpr int kScanRadius = 64;
	for (int cz = -kScanRadius; cz <= kScanRadius; ++cz)
	{
		for (int cx = -kScanRadius; cx <= kScanRadius; ++cx)
		{
			const int wx = cx * CHUNK_SIZE, wz = cz * CHUNK_SIZE;
			bool inland = true;
			for (const auto offset : {glm::ivec2(2, 2), {14, 2}, {2, 14}, {14, 14}, {8, 8}})
			{
				const auto s = gen.getTerrainSample(wx + offset.x, wz + offset.y);
				if (s.postErosionHeight <= TerrainGenerator::SEA_LEVEL + 6)
				{
					inland = false;
					break;
				}
			}
			if (inland)
				return {wx, wz};
		}
	}
	CHECK(false, "inland terrain reachable within the scan radius");
	return {0, 0};
}

// -----------------------------------------------------------------------------
// Streaming dispatch contract (issue #108 review): drive a real ChunkManager
// through updateStreaming and validate the desired footprint and the load
// queue against the brute-force oracle at every reconciliation boundary.
// -----------------------------------------------------------------------------
struct ChunkManagerStreamProbe
{
	static const ChunkDesiredFootprint &footprint(const ChunkManager &m) { return m.m_desiredFootprint; }
	static const std::unordered_set<glm::ivec3, IVec3Hash> &enqueued(const ChunkManager &m) { return m.m_enqueuedLoads; }
	static size_t queueHead(const ChunkManager &m) { return m.m_loadQueueHead; }
	static const std::vector<LoadCandidate> &queue(const ChunkManager &m) { return m.m_loadQueue; }
	static const ChunkManager::StreamState &state(const ChunkManager &m) { return m.m_streamState; }
	static const std::unordered_map<glm::ivec3, Chunk *, IVec3Hash> &chunks(const ChunkManager &m) { return m.m_chunks; }
};

struct StreamIVec3Less
{
	bool operator()(const glm::ivec3 &a, const glm::ivec3 &b) const
	{
		if (a.x != b.x) return a.x < b.x;
		if (a.y != b.y) return a.y < b.y;
		return a.z < b.z;
	}
};

static std::set<glm::ivec3, StreamIVec3Less> toOrderedSet(const std::vector<glm::ivec3> &v)
{
	return std::set<glm::ivec3, StreamIVec3Less>(v.begin(), v.end());
}

static glm::ivec3 chunkOfPosition(const glm::vec3 &p)
{
	return {static_cast<int>(std::floor(p.x / static_cast<float>(CHUNK_SIZE))), 0,
			static_cast<int>(std::floor(p.z / static_cast<float>(CHUNK_SIZE)))};
}

static std::set<glm::ivec3, StreamIVec3Less> managerDesiredSet(const ChunkManager &manager)
{
	const auto &list = footprintToCoordList(ChunkManagerStreamProbe::footprint(manager));
	return std::set<glm::ivec3, StreamIVec3Less>(list.begin(), list.end());
}

static void checkManagerDesiredMatchesBrute(const ChunkManager &manager, const Camera &camera,
											float bias, int viewDist, const char *what)
{
	const glm::vec3 pos = camera.getPosition();
	const glm::vec3 front = camera.getFront();
	glm::vec2 fwd(front.x, front.z);
	if (glm::dot(fwd, fwd) < 1e-6f)
		fwd = glm::vec2(0.f, 1.f);
	fwd = glm::normalize(fwd);
	const auto expected = toOrderedSet(
		computeDesiredChunkSetBruteForce(chunkOfPosition(pos), pos, fwd, bias, viewDist));
	const auto actual = managerDesiredSet(manager);
	if (actual != expected)
	{
		// First divergence for debuggability.
		for (const auto &c : expected)
			if (actual.count(c) == 0) { std::cerr << "  missing " << c.x << "," << c.z << "\n"; break; }
		for (const auto &c : actual)
			if (expected.count(c) == 0) { std::cerr << "  extra   " << c.x << "," << c.z << "\n"; break; }
	}
	CHECK(actual == expected, what);
}

/// Queue invariants at rest (these tests never call processChunkLoading, so
/// nothing is in flight or consumed): every live queue entry (index >= head)
/// is marked enqueued and the enqueued set holds exactly the distinct live
/// entries; nothing queued is already loaded; queued set == desired
/// footprint minus loaded chunks.
static void checkStreamingQueueInvariants(const ChunkManager &manager, const char *what)
{
	const auto &q = ChunkManagerStreamProbe::queue(manager);
	const size_t head = ChunkManagerStreamProbe::queueHead(manager);
	const auto &enq = ChunkManagerStreamProbe::enqueued(manager);

	CHECK(head <= q.size(), "queue head within bounds");
	std::set<glm::ivec3, StreamIVec3Less> live;
	for (size_t i = head; i < q.size(); ++i)
	{
		CHECK(enq.count(q[i].pos) == 1, "live queue entry is marked enqueued");
		live.insert(q[i].pos);
	}
	CHECK(enq.size() == live.size(), "enqueued set holds exactly the live queue entries");
	CHECK(live.size() == q.size() - head, "no duplicate coordinates in the live queue");

	for (const auto &c : live)
		CHECK(ChunkManagerStreamProbe::chunks(manager).count(c) == 0,
			  "queued coordinate is not already loaded");

	std::set<glm::ivec3, StreamIVec3Less> desiredMinusLoaded = managerDesiredSet(manager);
	for (const auto &c : ChunkManagerStreamProbe::chunks(manager))
		desiredMinusLoaded.erase(c.first);
	CHECK(live == desiredMinusLoaded, "queued set equals desired footprint minus loaded chunks");
}

static void runStreamingDispatchTests()
{
	const float bias = 0.35f;
	TerrainGenerator generator(42);
	ChunkPool pool(64);

	{
		ChunkManager manager(&generator, nullptr, &pool);
		RenderSettings settings;
		settings.streamFrontBias = bias;
		settings.maxRenderDistance = 256;

		// Startup: FullRebuild with an exact desired set.
		Camera camera(glm::vec3(8.f, 100.f, 8.f));
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::FullRebuild,
			  "startup takes the full-rebuild path");
		checkManagerDesiredMatchesBrute(manager, camera, bias, 256, "startup footprint matches brute force");
		checkStreamingQueueInvariants(manager, "startup queue invariants");
		CHECK(ChunkManagerStreamProbe::queueHead(manager) == 0, "full rebuild resets the queue head");

		// Steady state: same spot → zero work, queue untouched.
		const size_t queueSizeBefore = ChunkManagerStreamProbe::queue(manager).size();
		const auto statsBefore = manager.streamingMaintenanceStats();
		for (int i = 0; i < 100; ++i)
			CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::None,
				  "stationary frame is zero-work");
		const auto statsAfter = manager.streamingMaintenanceStats();
		CHECK(statsAfter.zeroWork - statsBefore.zeroWork == 100,
			  "100 stationary frames count as 100 zero-work ticks");
		CHECK(statsAfter.unloadScans - statsBefore.unloadScans <= 1,
			  "stationary frames do not spam unload scans (60-frame floor)");
		CHECK(ChunkManagerStreamProbe::queue(manager).size() == queueSizeBefore,
			  "stationary frames leave the queue untouched");

		// Sub-anchor movement (< 4 blocks): still zero work.
		camera.setPosition(glm::vec3(9.5f, 100.f, 8.2f));
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::None,
			  "sub-anchor movement stays zero-work");

		// Anchor crossing: incremental reconcile, footprint exact again.
		camera.setPosition(glm::vec3(12.5f, 100.f, 8.2f));
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::Incremental,
			  "movement-anchor crossing reconciles incrementally");
		checkManagerDesiredMatchesBrute(manager, camera, bias, 256, "post-anchor footprint matches brute force");
		checkStreamingQueueInvariants(manager, "post-anchor queue invariants");
	}

	// Walk a full chunk WITHOUT crossing the chunk boundary (x: 0.5 → 15.5):
	// each movement-anchor transition (x = 4, 8, 12) must reconcile and keep
	// the desired set exact — this is the stale-footprint blocker scenario.
	{
		ChunkManager manager(&generator, nullptr, &pool);
		RenderSettings settings;
		settings.streamFrontBias = bias;
		settings.maxRenderDistance = 256;
		Camera camera(glm::vec3(0.5f, 100.f, 8.f));
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::FullRebuild,
			  "boundary walk: startup rebuild");
		int incrementalCount = 0;
		for (float x = 1.5f; x <= 15.5f; x += 1.0f)
		{
			camera.setPosition(glm::vec3(x, 100.f, 8.f));
			const StreamingUpdateKind kind = manager.updateStreaming(camera, settings);
			if (kind == StreamingUpdateKind::Incremental)
			{
				++incrementalCount;
				checkManagerDesiredMatchesBrute(manager, camera, bias, 256,
												"boundary walk: footprint exact after every anchor reconciliation");
				checkStreamingQueueInvariants(manager, "boundary walk: queue invariants after reconciliation");
			}
			else
			{
				CHECK(kind == StreamingUpdateKind::None, "boundary walk: only None or Incremental within one chunk");
			}
		}
		CHECK(incrementalCount == 3, "boundary walk crossed exactly the 4/8/12 anchor boundaries");
	}

	// Chunk crossings in every direction + diagonal; then movement + a big
	// rotation in the SAME frame (heading has dispatch priority).
	{
		ChunkManager manager(&generator, nullptr, &pool);
		RenderSettings settings;
		settings.streamFrontBias = bias;
		settings.maxRenderDistance = 256;
		Camera camera(glm::vec3(15.9f, 100.f, 15.9f));
		manager.updateStreaming(camera, settings);
		const std::vector<glm::vec3> crossings = {
			glm::vec3(16.1f, 100.f, 15.9f), // +X
			glm::vec3(15.9f, 100.f, 15.9f), // -X (back)
			glm::vec3(15.9f, 100.f, 16.1f), // +Z
			glm::vec3(15.9f, 100.f, 15.9f), // -Z (back)
			glm::vec3(16.1f, 100.f, 16.1f), // diagonal +X+Z
		};
		for (const glm::vec3 &p : crossings)
		{
			camera.setPosition(p);
			CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::Incremental,
				  "chunk crossing reconciles incrementally");
			CHECK(ChunkManagerStreamProbe::state(manager).lastCamChunk == chunkOfPosition(p),
				  "chunk crossing publishes the new camera chunk");
			checkManagerDesiredMatchesBrute(manager, camera, bias, 256, "chunk-cross footprint matches brute force");
			checkStreamingQueueInvariants(manager, "chunk-cross queue invariants");
		}

		// Cross two chunks (+X) and rotate 180° in the same frame → the
		// heading path wins and must still publish the moved-to chunk.
		camera.setPosition(glm::vec3(32.1f, 100.f, 16.1f));
		camera.setYawPitch(180.f, 0.f);
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::HeadingRebuild,
			  "movement + large rotation takes the heading path");
		checkManagerDesiredMatchesBrute(manager, camera, bias, 256, "heading-rebuild footprint matches brute force");
		checkStreamingQueueInvariants(manager, "heading-rebuild queue invariants");
		CHECK(ChunkManagerStreamProbe::state(manager).lastCamChunk == chunkOfPosition(camera.getPosition()),
			  "heading path publishes the camera chunk on combined movement");

		// Small rotation (~5°) with an anchor-crossing move → plain
		// incremental (the drift alone stays below the heading threshold).
		camera.setYawPitch(185.f, 0.f);
		camera.setPosition(glm::vec3(36.1f, 100.f, 16.1f));
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::Incremental,
			  "small heading drift with anchor move stays incremental");
		checkManagerDesiredMatchesBrute(manager, camera, bias, 256, "heading-drift footprint matches brute force");
		checkStreamingQueueInvariants(manager, "heading-drift queue invariants");

		// A sub-anchor move with the same drift stays zero-work (footprint
		// staleness bounded by one anchor cell).
		camera.setPosition(glm::vec3(36.9f, 100.f, 16.5f));
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::None,
			  "sub-anchor move with small drift stays zero-work");
	}

	// Front-bias invalidation: float noise below the epsilon is ignored, a
	// real change rebuilds exactly once, and clamped settings compare on the
	// normalized value actually used by the algorithm.
	{
		ChunkManager manager(&generator, nullptr, &pool);
		RenderSettings settings;
		settings.streamFrontBias = 0.30f;
		settings.maxRenderDistance = 256;
		Camera camera(glm::vec3(8.f, 100.f, 8.f));
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::FullRebuild,
			  "bias: startup rebuild");
		settings.streamFrontBias = 0.30005f;
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::None,
			  "bias float noise below epsilon does not rebuild");
		settings.streamFrontBias = 0.301f;
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::FullRebuild,
			  "real bias change takes the full-rebuild path");
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::None,
			  "rebuild publishes state: next identical frame is zero-work");

		settings.streamFrontBias = 0.95f;
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::FullRebuild,
			  "out-of-range bias rebuilds (normalized to the safe maximum)");
		settings.streamFrontBias = 0.99f;
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::None,
			  "clamped bias change (0.95 -> 0.99) is a no-op");
		checkManagerDesiredMatchesBrute(manager, camera, kSafeMaxStreamFrontBias, 256,
										"clamped-bias footprint matches brute force");
	}

	// Render-distance changes rebuild and converge; teleports rebuild.
	{
		ChunkManager manager(&generator, nullptr, &pool);
		RenderSettings settings;
		settings.streamFrontBias = bias;
		settings.maxRenderDistance = 512;
		Camera camera(glm::vec3(8.f, 100.f, 8.f));
		manager.updateStreaming(camera, settings);
		checkManagerDesiredMatchesBrute(manager, camera, bias, 512, "view 512 footprint matches brute force");
		checkStreamingQueueInvariants(manager, "view 512 queue invariants");

		settings.maxRenderDistance = 128;
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::FullRebuild, "view shrink rebuilds");
		checkManagerDesiredMatchesBrute(manager, camera, bias, 128, "view 128 footprint matches brute force");
		checkStreamingQueueInvariants(manager, "view 128 queue invariants");

		settings.maxRenderDistance = 384;
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::FullRebuild, "view grow rebuilds");
		checkManagerDesiredMatchesBrute(manager, camera, bias, 384, "view 384 footprint matches brute force");

		camera.setPosition(glm::vec3(50 * 16 + 8.f, 100.f, -30 * 16 + 3.f));
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::FullRebuild, "teleport rebuilds");
		CHECK(ChunkManagerStreamProbe::state(manager).lastCamChunk == chunkOfPosition(camera.getPosition()),
			  "teleport publishes the new camera chunk");
		CHECK(ChunkManagerStreamProbe::queueHead(manager) == 0, "teleport resets the queue head");
		checkManagerDesiredMatchesBrute(manager, camera, bias, 384, "post-teleport footprint matches brute force");
		checkStreamingQueueInvariants(manager, "post-teleport queue invariants");
	}

	// Geometric invariant: desired footprint ⊆ unload hysteresis radius.
	// Raw bias 0.9 clamps to the safe maximum; for every heading, every
	// desired chunk CENTER must lie within maxRenderDistance * 1.5 (XZ) of
	// the camera. A small epsilon absorbs float noise; the center-based rule
	// matches the unload check, which also measures to chunk centers.
	{
		constexpr int view = 128;
		const glm::vec3 basePos(200.3f, 100.f, 200.7f);
		const float unloadDistSq = view * kChunkUnloadDistanceFactor * (view * kChunkUnloadDistanceFactor);
		const std::vector<glm::vec2> headings = {
			{1.f, 0.f}, {-1.f, 0.f}, {0.f, 1.f}, {0.f, -1.f},
			{0.70710678f, 0.70710678f}, // diagonal
		};
		for (const glm::vec2 &fwd : headings)
		{
			ChunkManager manager(&generator, nullptr, &pool);
			RenderSettings settings;
			settings.streamFrontBias = 0.9f; // raw, above the safe cap on purpose
			settings.maxRenderDistance = view;
			Camera camera(basePos);
			camera.setYawPitch(glm::degrees(std::atan2(fwd.y, fwd.x)), 0.f);
			manager.updateStreaming(camera, settings);
			const auto desired = managerDesiredSet(manager);
			CHECK(!desired.empty(), "bias-max footprint non-empty");
			float worstDistSq = 0.f;
			for (const auto &c : desired)
			{
				const glm::vec3 center(c.x * 16 + 8.f, 0.f, c.z * 16 + 8.f);
				const float dx = basePos.x - center.x;
				const float dz = basePos.z - center.z;
				const float distSq = dx * dx + dz * dz;
				worstDistSq = std::max(worstDistSq, distSq);
				CHECK(distSq <= unloadDistSq + 4.f,
					  "desired chunk center lies inside the unload hysteresis radius");
			}
			(void)worstDistSq;
		}
	}

	// Anti-starvation: a loaded, still-desired chunk must never be evicted by
	// the unload scan (desired ∩ unloadCandidates = ∅). Raw bias 0.9 → safe
	// cap; the farthest ahead chunk is loaded, then the 60-frame unload
	// cadence fires several times — the chunk must survive all of them.
	{
		ChunkPool bigPool(512);
		ChunkManager manager(&generator, nullptr, &bigPool);
		RenderSettings settings;
		settings.streamFrontBias = 0.9f;
		settings.maxRenderDistance = 64;
		Camera camera(glm::vec3(8.f, 100.f, 8.f));
		manager.updateStreaming(camera, settings);
		for (int i = 0; i < 40; ++i)
			manager.processChunkLoading(4096);

		// Farthest desired chunk ahead of the camera (+X heading).
		glm::ivec3 farAhead{0, 0, 0};
		float bestDist = -1.f;
		for (const auto &c : managerDesiredSet(manager))
		{
			if (c.x <= 0)
				continue;
			const glm::vec3 center(c.x * 16 + 8.f, 0.f, c.z * 16 + 8.f);
			const float d = center.x - camera.getPosition().x;
			if (d > bestDist)
			{
				bestDist = d;
				farAhead = c;
			}
		}
		CHECK(bestDist > 0.f, "found desired chunks ahead of the camera");
		CHECK(ChunkManagerStreamProbe::chunks(manager).count(farAhead) == 1,
			  "farthest ahead chunk got loaded");

		const auto before = manager.streamingMaintenanceStats();
		for (int i = 0; i < 200; ++i)
			manager.updateStreaming(camera, settings);
		const auto after = manager.streamingMaintenanceStats();
		CHECK(after.unloadScans > before.unloadScans, "unload cadence fired during the stationary window");
		CHECK(ChunkManagerStreamProbe::chunks(manager).count(farAhead) == 1,
			  "still-desired ahead chunk survives every unload scan");
		// Full invariant: every desired coordinate is loaded — none was evicted.
		for (const auto &c : managerDesiredSet(manager))
			CHECK(ChunkManagerStreamProbe::chunks(manager).count(c) == 1,
				  "desired chunk remains loaded after unload scans (no thrash)");
	}

	// Negative coordinates: floor-quantized anchors and chunk math must behave
	// across zero and negative boundaries (-0.1 walks to -16.1, crossing the
	// chunk -1 → -2 boundary).
	{
		ChunkManager manager(&generator, nullptr, &pool);
		RenderSettings settings;
		settings.streamFrontBias = bias;
		settings.maxRenderDistance = 128;
		Camera camera(glm::vec3(-0.1f, 100.f, -0.1f));
		CHECK(manager.updateStreaming(camera, settings) == StreamingUpdateKind::FullRebuild,
			  "negative startup rebuild");
		CHECK(ChunkManagerStreamProbe::state(manager).lastCamChunk == glm::ivec3(-1, 0, -1),
			  "negative coords floor into chunk (-1,-1)");
		for (float x = -1.1f; x >= -16.2f; x -= 1.0f)
		{
			camera.setPosition(glm::vec3(x, 100.f, -0.1f));
			const StreamingUpdateKind kind = manager.updateStreaming(camera, settings);
			if (kind != StreamingUpdateKind::None)
			{
				checkManagerDesiredMatchesBrute(manager, camera, bias, 128, "negative walk footprint matches brute force");
				checkStreamingQueueInvariants(manager, "negative walk queue invariants");
			}
		}
		CHECK(ChunkManagerStreamProbe::state(manager).lastCamChunk == glm::ivec3(-2, 0, -1),
			  "negative walk crossed into chunk (-2,-1)");
	}

	// Deterministic pseudo-random transitions: after every actual
	// reconciliation the footprint must equal the brute-force oracle and the
	// queue must satisfy its invariants. None frames are allowed to be stale
	// by up to one anchor cell (that is the documented quantization).
	{
		ChunkManager manager(&generator, nullptr, &pool);
		RenderSettings settings;
		settings.streamFrontBias = 0.3f;
		settings.maxRenderDistance = 256;
		Camera camera(glm::vec3(8.f, 100.f, 8.f));
		float yaw = 0.f;
		manager.updateStreaming(camera, settings);

		uint64_t rng = 0x9E3779B97F4A7C15ull; // fixed seed: deterministic run
		auto next = [&rng]() {
			rng ^= rng << 13;
			rng ^= rng >> 7;
			rng ^= rng << 17;
			return rng;
		};
		int reconciliations = 0;
		for (int step = 0; step < 300; ++step)
		{
			const uint64_t r = next();
			glm::vec3 pos = camera.getPosition();
			switch (r % 10)
			{
			case 0: // rare teleport
				pos.x += static_cast<float>(next() % 200) - 100.f;
				pos.z += static_cast<float>(next() % 200) - 100.f;
				break;
			case 1:
			case 2: // multi-chunk move
				pos.x += static_cast<float>(next() % 40) - 20.f;
				pos.z += static_cast<float>(next() % 40) - 20.f;
				break;
			default: // sub-chunk wander (0..8 blocks)
				pos.x += static_cast<float>(next() % 9) - 4.f;
				pos.z += static_cast<float>(next() % 9) - 4.f;
				break;
			}
			camera.setPosition(pos);

			const uint64_t h = next();
			if (h % 5 == 0)
				yaw += 30.f + static_cast<float>(next() % 151); // 30..180° turn
			else if (h % 5 == 1)
				yaw += static_cast<float>(next() % 7) - 3.f; // small drift
			camera.setYawPitch(yaw, 0.f);

			const uint64_t b = next();
			if (b % 7 == 0)
				settings.streamFrontBias = static_cast<float>(b % 3) * 0.3f; // 0 / 0.3 / 0.6
			const uint64_t v = next();
			if (v % 11 == 0)
				settings.maxRenderDistance = 128 + static_cast<int>(v % 3) * 128; // 128/256/384

			const StreamingUpdateKind kind = manager.updateStreaming(camera, settings);
			if (kind != StreamingUpdateKind::None)
			{
				++reconciliations;
				checkManagerDesiredMatchesBrute(manager, camera, normalizedStreamFrontBias(settings.streamFrontBias),
												settings.maxRenderDistance, "randomized: footprint matches brute force");
				checkStreamingQueueInvariants(manager, "randomized: queue invariants");
			}
		}
		CHECK(reconciliations > 100, "randomized path exercised plenty of reconciliations");
	}
}

int main(int argc, char **argv)
{
	std::cout.setf(std::ios::unitbuf);
	std::cerr.setf(std::ios::unitbuf);
	if (argc > 1 && std::string_view(argv[1]) == "--mobs-profile") return profileMobs();
	if (argc > 1 && std::string_view(argv[1]) == "--physics-profile") return profilePlayerPhysics();
	if (argc > 1 && std::string_view(argv[1]) == "--stream-perf") return runStreamPerf();
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
            // The moved-in backing is prepared-but-stale (ownership-only
            // prepare, issue #115 review): borders were released earlier in
            // this section, so re-borrow them, then let generation
            // initialize the backing before edits/meshing read it.
            CHECK(c.prepareVoxelStorageForGeneration(), "re-prepare borders on the moved chunk");
            TerrainGenerator tgen(20260905);
            c.generateTerrain(tgen);
            CHECK(c.getState() == ChunkState::GENERATED, "moved chunk generates in place");
            c.setVoxel(1, 1, 1, STONE);
            c.generateMesh();
            MeshBuildResult *pending = ChunkStateProbe::pendingResult(c);
            CHECK(pending != nullptr, "generateMesh attaches a pooled build result");
            CHECK(totalOpaqueVertices(*pending) > 0,
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
	checkOccupancyMetadataInSync(a, "occupancy metadata in sync after generation");
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
	CHECK(ChunkStateProbe::sectionMask(a) == 0,
		  "moved-from chunk has empty occupancy metadata");
	checkOccupancyMetadataInSync(a, "move-ctor moved-from chunk satisfies the occupancy contract");
	CHECK(ChunkStateProbe::hasStorage(b), "moved-to chunk has storage");
	const ChunkSnapshot snapB = ChunkSnapshot::capture(b);
	CHECK(sameState(snapA, snapB),
		  "move-constructor must preserve full generation state");
	CHECK(b.getState() == ChunkState::GENERATED, "moved chunk keeps GENERATED");

	// 3) Move-assignment carries the full generation state.
	Chunk c(glm::vec3(9876.0f, 0.0f, -4321.0f));
	c = std::move(b);
	CHECK(!ChunkStateProbe::hasStorage(b), "moved-from chunk has no storage");
	CHECK(ChunkStateProbe::sectionMask(b) == 0,
		  "move-assigned moved-from chunk has empty occupancy metadata");
	checkOccupancyMetadataInSync(b, "move-assign moved-from chunk satisfies the occupancy contract");
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
	CHECK(ChunkStateProbe::sectionMask(c) == 0, "reset invalidates occupancy metadata");
	// ForGeneration keeps voxel storage but frees borders; the prepare step
	// re-borrows them on the calling thread per the generation contract.
	CHECK(c.prepareVoxelStorageForGeneration(), "re-prepare after ForGeneration reset");
	c.generateTerrain(gen);
	CHECK(c.getState() == ChunkState::GENERATED, "regeneration completes");
	checkMatchesOwning(c, gen, CHUNK_SIZE, CHUNK_SIZE, "recycled generation");
	checkOccupancyMetadataInSync(c, "occupancy metadata in sync after recycle");
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
	checkOccupancyMetadataInSync(c, "full reset leaves empty occupancy metadata");

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
		checkOccupancyMetadataInSync(*reused, "pool reuse keeps occupancy metadata in sync");
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
		CHECK(meshWith != nullptr && totalOpaqueIndices(*meshWith) > 0,
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
		// Generate the loaded chunks synchronously (the tests run without a
		// thread pool, so generatePendingVoxels is a no-op): the edit
		// contract applies immediate edits only to GENERATED chunks - on
		// UNLOADED chunks everything queues until generation lands (issue
		// #115 blocker).
		for (Chunk *c : {origin, east, west, south, north})
		{
			CHECK(c->prepareVoxelStorageForGeneration(), "13e synchronous prepare");
			c->generateTerrain(mgen);
		}
		if (origin && east && west && south && north)
		{
			// (0) Coordinate validation gates the whole edit pipeline
			// (final review): out-of-range world Y - exactly what a
			// right-click on the top face of a y = CHUNK_HEIGHT-1 voxel
			// produces (prev.y = CHUNK_HEIGHT) - is refused before any
			// voxel backing access, with no state change of any kind.
			{
				const uint64_t revisionBefore = origin->meshRevision();
				const size_t pendingBefore = ChunkManagerProbe::pendingEdits(manager);
				CHECK(!manager.placeVoxel(glm::vec3(4.0f, -1.0f, 4.0f), STONE),
					  "placing below world height is refused");
				CHECK(!manager.placeVoxel(glm::vec3(4.0f, static_cast<float>(CHUNK_HEIGHT), 4.0f), STONE),
					  "placing above world height is refused");
				CHECK(!manager.deleteVoxel(glm::vec3(4.0f, -1.0f, 4.0f)),
					  "deleting below world height is refused");
				CHECK(!manager.deleteVoxel(glm::vec3(4.0f, static_cast<float>(CHUNK_HEIGHT), 4.0f)),
					  "deleting above world height is refused");
				CHECK(origin->meshRevision() == revisionBefore,
					  "invalid edits do not bump mesh revision");
				CHECK(ChunkManagerProbe::pendingEdits(manager) == pendingBefore,
					  "invalid edits do not queue anything");

				// Off-by-one lock: the top valid voxel coordinate is usable.
				const glm::vec3 topPos(4.0f, static_cast<float>(CHUNK_HEIGHT - 1), 4.0f);
				CHECK(manager.placeVoxel(topPos, STONE),
					  "top valid voxel coordinate is accepted");
				CHECK(origin->getVoxel(4, CHUNK_HEIGHT - 1, 4).type == static_cast<uint8_t>(STONE),
					  "top voxel written");
				CHECK(manager.deleteVoxel(topPos),
					  "top valid voxel can be deleted");
				CHECK(origin->getVoxel(4, CHUNK_HEIGHT - 1, 4).type == static_cast<uint8_t>(AIR),
					  "top voxel removed");
			}
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

	// 15) Compact occupancy metadata vs brute force under randomized edits
	// (issue #105): the per-section counters must track the canonical voxel
	// types through generation, thousands of deterministic place/delete/type
	// mutations, section first/last-block transitions, and back to a fully
	// empty chunk.
	{
		TerrainGenerator ogen(777);
		Chunk occ(glm::vec3(0.0f));
		CHECK(occ.prepareVoxelStorageForGeneration(), "occupancy prepare");
		occ.generateTerrain(ogen);
		checkOccupancyMetadataInSync(occ, "occupancy metadata matches brute force after generation");

		// The metadata skip must be observable: a generated chunk occupies
		// fewer than all 16 sections (no floating terrain at the world top).
		uint16_t genMask = ChunkStateProbe::sectionMask(occ);
		CHECK(genMask != 0 && (genMask & 0x8000) == 0,
			  "generated chunk leaves the sky sections empty");

		std::mt19937 rng(20260905u);
		auto rollCoord = [&rng](int lo, int hi)
		{ return static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1)) + lo; };
		const TextureType palette[] = {AIR, STONE, BRICKS, WATER, GLASS, DIRT};
		std::array<int64_t, Chunk::kOccupancySections> shadow{};
		{
			const auto v0 = ChunkStateProbe::voxels(occ);
			for (size_t i = 0; i < v0.size(); ++i)
				shadow[i / (Chunk::kOccupancySectionSize * CHUNK_SIZE * CHUNK_SIZE)] +=
				    v0[i].type != static_cast<uint8_t>(AIR) ? 1 : 0;
		}
		for (int i = 0; i < 3000; ++i)
		{
			const int x = rollCoord(0, CHUNK_SIZE - 1);
			const int y = rollCoord(0, CHUNK_HEIGHT - 1);
			const int z = rollCoord(0, CHUNK_SIZE - 1);
			const TextureType type = palette[rng() % 6];
			const size_t idx = static_cast<size_t>(y) * CHUNK_SIZE * CHUNK_SIZE +
					   z * CHUNK_SIZE + x;
			const bool cellWasAir = ChunkStateProbe::voxels(occ)[idx].type ==
						static_cast<uint8_t>(AIR);
			occ.setVoxel(x, y, z, type);
			if (cellWasAir != (type == AIR))
				shadow[y / Chunk::kOccupancySectionSize] += (type == AIR) ? -1 : 1;
			CHECK(shadow[y / Chunk::kOccupancySectionSize] ==
				      static_cast<int64_t>(
					  ChunkStateProbe::sections(occ)[y / Chunk::kOccupancySectionSize]),
				  "incremental section count matches a running shadow count");
			// Canonical point query must agree with the written type.
			CHECK(occ.isVoxelActive(x, y, z) == (type != AIR),
				  "isVoxelActive follows the canonical voxel type");
			if (i % 250 == 249)
				checkOccupancyMetadataInSync(occ, "occupancy metadata survives edit batch");
		}
		checkOccupancyMetadataInSync(occ, "occupancy metadata matches brute force after 3000 edits");

		// Drain every remaining voxel: a GENERATED chunk must come back to
		// zero metadata and no occupied span through edits alone.
		const auto residue = ChunkStateProbe::voxels(occ);
		for (size_t i = 0; i < residue.size(); ++i)
			if (residue[i].type != static_cast<uint8_t>(AIR))
				occ.setVoxel(static_cast<int>(i % CHUNK_SIZE),
					     static_cast<int>(i / (CHUNK_SIZE * CHUNK_SIZE)),
					     static_cast<int>((i / CHUNK_SIZE) % CHUNK_SIZE), AIR);
		checkOccupancyMetadataInSync(occ, "drained chunk has zero occupancy metadata");
		CHECK(ChunkStateProbe::sectionMask(occ) == 0,
			  "no section bit survives a fully drained chunk");
		int drainedMin = -1, drainedMax = -1;
		CHECK(!ChunkStateProbe::occupiedSpan(occ, drainedMin, drainedMax),
			  "drained chunk reports no occupied span");

		// Directed first/last-block-in-section transitions at the section
		// seams (y=15/16 and y=239/240) and at the world extremes - from a
		// clean slate so each place is genuinely the section's first block.
		for (int y : {0, 15, 16, 239, 240, CHUNK_HEIGHT - 1})
		{
			const int section = y / Chunk::kOccupancySectionSize;
			occ.setVoxel(3, y, 3, STONE);
			checkOccupancyMetadataInSync(occ, "first block sets its section");
			CHECK((ChunkStateProbe::sectionMask(occ) >> section) & 1,
				  "occupied section bit is set");
			occ.setVoxel(3, y, 3, AIR);
			checkOccupancyMetadataInSync(occ, "last block clears its section");
		}
		// All-air section between two occupied ones: block in section 0,
		// block in section 1, remove the section-0 one - bit 0 must clear
		// while bit 1 stays.
		occ.setVoxel(4, 15, 4, BRICKS);
		occ.setVoxel(4, 16, 4, BRICKS);
		checkOccupancyMetadataInSync(occ, "two adjacent sections occupied");
		occ.setVoxel(4, 15, 4, AIR);
		checkOccupancyMetadataInSync(occ, "inner section drains to air");
		CHECK((ChunkStateProbe::sectionMask(occ) & 0b11) == 0b10,
			  "section 0 bit cleared, section 1 bit kept");
		occ.setVoxel(4, 16, 4, AIR);

		// Empty-chunk transition: a storage-only chunk whose last block is
		// removed must drop to zero metadata and mesh to an empty payload.
		Chunk empty(glm::vec3(0.0f));
		empty.setVoxel(0, 0, 0, STONE);
		CHECK(ChunkStateProbe::sectionMask(empty) == 0x1, "single voxel occupies section 0");
		empty.setVoxel(0, 0, 0, AIR);
		checkOccupancyMetadataInSync(empty, "fully emptied chunk has zero occupancy metadata");
		CHECK(ChunkStateProbe::sectionMask(empty) == 0, "no section bit survives the last delete");
		CHECK(empty.generateMesh(), "empty chunk publishes an (empty) mesh");
		const MeshBuildResult *emptyMesh = ChunkStateProbe::pendingResult(empty);
		CHECK(emptyMesh != nullptr && emptyMesh->opaqueIndices.empty() &&
				  emptyMesh->waterIndices.empty(),
			  "empty occupancy meshes to an empty payload");
	}

	// 16) Metadata-driven mesh bounds are geometry-neutral (issue #105):
	// building through the public path (section-derived, refined bounds)
	// must produce byte-identical output to a forced full-volume range, for
	// both the greedy mesher and the LOD mesher.
	{
		MeshResultPool pool;
		TerrainGenerator bgen(4242);
		const glm::ivec2 inland = locateInlandChunkOrigin(bgen);
		Chunk chunk(glm::vec3(float(inland.x), 0.0f, float(inland.y)), ChunkState::UNLOADED,
					nullptr, nullptr, &pool);
		CHECK(chunk.prepareVoxelStorageForGeneration(), "bounds-equivalence prepare");
		chunk.generateTerrain(bgen);

		const uint64_t generation = chunk.meshGeneration();
		const uint64_t revision = chunk.meshRevision();

		// Forced full-range greedy build.
		MeshBuildResult *full = pool.acquire();
		full->beginBuild(&chunk, generation, revision);
		full->isLOD = false;
		ChunkStateProbe::buildMeshRanged(chunk, *full, generation, revision,
										 0, CHUNK_HEIGHT - 1);
		pool.finishBuild(full);
		CHECK(totalOpaqueIndices(*full) > 0, "full-range greedy build emits geometry");

		// Public path with metadata bounds.
		CHECK(chunk.generateMesh(), "metadata-bounds greedy build publishes");
		const MeshBuildResult *ranged = ChunkStateProbe::pendingResult(chunk);
		CHECK(ranged != nullptr, "metadata-bounds result attached");
		CHECK(ranged->sections == full->sections,
			  "greedy mesh identical under metadata-derived bounds");

		// LOD: forced top-down scan vs metadata-bounded scan start.
		MeshBuildResult *lodFull = pool.acquire();
		lodFull->beginBuild(&chunk, generation, revision);
		lodFull->isLOD = true;
		ChunkStateProbe::buildLODMeshRanged(chunk, *lodFull, CHUNK_HEIGHT - 1);
		pool.finishBuild(lodFull);
		CHECK(!lodFull->opaqueIndices.empty(), "full-range LOD build emits geometry");

		CHECK(chunk.generateLODMesh(), "metadata-bounded LOD build publishes");
		const MeshBuildResult *lodRanged = ChunkStateProbe::pendingResult(chunk);
		CHECK(lodRanged != nullptr && lodRanged->isLOD,
			  "metadata-bounded LOD result attached");
		CHECK(lodRanged->opaqueVertices == lodFull->opaqueVertices &&
				  lodRanged->opaqueIndices == lodFull->opaqueIndices &&
				  lodRanged->waterVertices == lodFull->waterVertices &&
				  lodRanged->waterIndices == lodFull->waterIndices,
			  "LOD mesh identical under metadata-derived scan start");

		pool.release(lodFull);
		pool.release(full);
	}

	// 17) Prepared-backing lifecycle (issue #115 review): prepare is
	// ownership only and must not clear a recycled block (issue #93's single
	// CHUNK_VOLUME generation clear lives in generateChunkInto), an
	// UNLOADED chunk's stale backing must not be read to decide edits, and
	// the ForGeneration reset documents the one state where buffer and
	// metadata are deliberately not in sync.
	{
		TerrainGenerator pgen(919);
		VoxelPool pool;
		ChunkPool cpool(4);

		// (a) prepare() leaves reused backing untouched.
		Chunk poison(glm::vec3(0.0f), ChunkState::UNLOADED, &pool);
		CHECK(poison.prepareVoxelStorageForGeneration(), "poison prepare");
		{
			Voxel *vv = ChunkStateProbe::voxelsMut(poison);
			std::fill(vv, vv + CHUNK_VOLUME, Voxel{static_cast<uint8_t>(GRASS_TOP)});
		}
		const Voxel *poisonedBlock = ChunkStateProbe::voxels(poison).data();
		poison.reset(glm::vec3(0.0f)); // Full: releases the poisoned block
		CHECK(!ChunkStateProbe::hasStorage(poison), "poisoned storage released");
		CHECK(poison.prepareVoxelStorageForGeneration(), "re-prepare acquires a block");
		{
			const auto vv = ChunkStateProbe::voxels(poison);
			// The pool holds exactly one block, so this must be the
			// poisoned one - and prepare must NOT have cleared it.
			CHECK(vv.data() == poisonedBlock, "pool recycles the same block");
			CHECK(vv.front().type == static_cast<uint8_t>(GRASS_TOP) &&
					  vv[CHUNK_VOLUME / 2].type == static_cast<uint8_t>(GRASS_TOP) &&
					  vv.back().type == static_cast<uint8_t>(GRASS_TOP),
				  "prepare does not clear generation backing (single clear stays in generateChunkInto)");
		}
		// generateChunkInto() alone initializes the backing: the final
		// terrain must be identical to the owning path despite the poison.
		poison.generateTerrain(pgen);
		checkOccupancyMetadataInSync(poison, "generation recounts poisoned backing");
		checkMatchesOwning(poison, pgen, 0, 0, "generation over poisoned backing is deterministic");

		// (b) Edits never consult stale backing: an UNLOADED (prepared,
		// not yet generated) chunk answers "logically empty", so a delete
		// is refused and a placement is accepted regardless of the poison.
		Chunk ungen(glm::vec3(0.0f), ChunkState::UNLOADED, &pool);
		CHECK(ungen.prepareVoxelStorageForGeneration(), "ungenerated prepare");
		{
			Voxel *vv = ChunkStateProbe::voxelsMut(ungen);
			std::fill(vv, vv + CHUNK_VOLUME, Voxel{static_cast<uint8_t>(GRASS_TOP)});
		}
		ungen.setInTransit(true); // generation in flight
		{
			ChunkManager probeHolder(&pgen, nullptr, &cpool);
			CHECK(ChunkManagerProbe::effective(probeHolder, &ungen, 4, 40, 4) == AIR,
				  "UNLOADED backing is not read for edit decisions");
		}
		ungen.setInTransit(false);

		// (c) ForGeneration: the documented desync window. Storage (with
		// stale bytes) is retained, metadata drops to zero, and generation
		// restores the exact match.
		Chunk lifecycle(glm::vec3(0.0f), ChunkState::UNLOADED, &pool);
		CHECK(lifecycle.prepareVoxelStorageForGeneration(), "lifecycle prepare");
		lifecycle.generateTerrain(pgen);
		CHECK(ChunkStateProbe::sectionMask(lifecycle) != 0,
			  "generated chunk has occupied sections");
		const Voxel *retainedStorage = ChunkStateProbe::voxels(lifecycle).data();
		lifecycle.reset(glm::vec3(0.0f), Chunk::ResetMode::ForGeneration);
		CHECK(ChunkStateProbe::hasStorage(lifecycle) &&
				  ChunkStateProbe::voxels(lifecycle).data() == retainedStorage,
			  "ForGeneration retains the storage block");
		CHECK(ChunkStateProbe::sectionMask(lifecycle) == 0,
			  "ForGeneration resets occupancy metadata to zero");
		CHECK(lifecycle.getState() == ChunkState::UNLOADED,
			  "ForGeneration returns to UNLOADED");
		CHECK(lifecycle.prepareVoxelStorageForGeneration(),
			  "prepare is a no-op success while storage is retained");
		CHECK(ChunkStateProbe::sectionMask(lifecycle) == 0,
			  "metadata stays zero through the prepared phase");
		lifecycle.generateTerrain(pgen);
		checkOccupancyMetadataInSync(lifecycle, "ForGeneration regeneration restores the match");
		checkMatchesOwning(lifecycle, pgen, 0, 0, "ForGeneration regeneration is deterministic");
	}

	// 18) Synthetic occupancy geometry (issue #115 review): an interior
	// run of empty sections, both section seam classes, and an isolated
	// water section must mesh identically under metadata bounds and a
	// forced full range - this pins the d==1 empty-slab slice skip.
	{
		MeshResultPool pool;
		Chunk synth(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
		// Edits on a storageless chunk air-initialize the backing (edit
		// path), giving a pristine all-air canvas once voxels are placed.
		auto place = [&synth](int x, int y, int z, TextureType t)
		{ synth.setVoxel(x, y, z, t); };
		// Section 1 patch (sections 2..8 stay empty).
		for (int x = 4; x < 8; ++x)
			for (int z = 4; z < 8; ++z)
				place(x, 20, z, STONE);
		// Section 9 isolated block.
		place(10, 150, 10, BRICKS);
		// Section 4 isolated water (also exercises the water mesh path).
		place(2, 70, 2, WATER);
		place(3, 70, 2, WATER);
		// Both seam classes: y=15/16 (sections 0/1) and y=239/240 (14/15).
		place(0, 15, 0, DIRT);
		place(0, 16, 0, STONE);
		place(5, 239, 5, STONE);
		place(5, 240, 5, DIRT);
		place(6, 240, 6, GLASS);

		uint16_t expectedMask = 0;
		for (int s : {0, 1, 4, 9, 14, 15})
			expectedMask |= static_cast<uint16_t>(1u << s);
		CHECK(ChunkStateProbe::sectionMask(synth) == expectedMask,
			  "synthetic chunk occupies exactly the intended sections");

		const uint64_t generation = synth.meshGeneration();
		const uint64_t revision = synth.meshRevision();

		MeshBuildResult *full = pool.acquire();
		full->beginBuild(&synth, generation, revision);
		full->isLOD = false;
		ChunkStateProbe::buildMeshRanged(synth, *full, generation, revision,
										 0, CHUNK_HEIGHT - 1);
		pool.finishBuild(full);
		CHECK(totalOpaqueIndices(*full) > 0,
			  "full-range synthetic build emits opaque and water geometry");

		CHECK(synth.generateMesh(), "synthetic metadata-bounds build publishes");
		const MeshBuildResult *ranged = ChunkStateProbe::pendingResult(synth);
		CHECK(ranged != nullptr, "synthetic metadata-bounds result attached");
		CHECK(ranged->sections == full->sections,
			  "greedy mesh identical across interior empty sections and seams");

		MeshBuildResult *lodFull = pool.acquire();
		lodFull->beginBuild(&synth, generation, revision);
		lodFull->isLOD = true;
		ChunkStateProbe::buildLODMeshRanged(synth, *lodFull, CHUNK_HEIGHT - 1);
		pool.finishBuild(lodFull);
		CHECK(!lodFull->opaqueIndices.empty(), "full-range synthetic LOD emits geometry");

		CHECK(synth.generateLODMesh(), "synthetic metadata-bounded LOD publishes");
		const MeshBuildResult *lodRanged = ChunkStateProbe::pendingResult(synth);
		CHECK(lodRanged != nullptr && lodRanged->isLOD,
			  "synthetic metadata-bounded LOD attached");
		CHECK(lodRanged->opaqueVertices == lodFull->opaqueVertices &&
				  lodRanged->opaqueIndices == lodFull->opaqueIndices &&
				  lodRanged->waterVertices == lodFull->waterVertices &&
				  lodRanged->waterIndices == lodFull->waterIndices,
			  "LOD mesh identical under isolated sections and seams");

		pool.release(lodFull);
		pool.release(full);
	}

	// 19) Edits on chunks still waiting for terrain generation (issue #115
	// blocker): an UNLOADED chunk - in transit or not - never takes a
	// direct edit. placeVoxel reports logical acceptance and queues;
	// deleteVoxel is the no-op its logical-AIR state implies; the queued
	// edits apply only after generation produces readable backing, and
	// generation itself is never short-circuited.
	{
		ChunkPool pool(32);
		TerrainGenerator wgen(4711);
		ChunkManager manager(&wgen, nullptr, &pool);

		Camera cam(glm::vec3(0.0f, 100.0f, 0.0f));
		RenderSettings settings;
		manager.updateStreaming(cam, settings);
		manager.processChunkLoading(64);

		// (a) Loaded but generation never dispatched.
		Chunk *waiting = manager.getChunkAtWorldPos(glm::vec3(4.0f, 40.0f, 4.0f));
		Chunk *eastN = manager.getChunkAtWorldPos(
			glm::vec3(static_cast<float>(CHUNK_SIZE) + 4.0f, 40.0f, 4.0f));
		CHECK(waiting != nullptr && eastN != nullptr,
			  "loaded-not-generated chunks registered");
		if (waiting && eastN)
		{
			CHECK(waiting->getState() == ChunkState::UNLOADED,
				  "chunk loaded but not yet generated");
			CHECK(!waiting->isInTransit(), "no generation dispatched yet");
			const size_t queuedBefore = ChunkManagerProbe::pendingEdits(manager);

			// Delete on a logically-empty UNLOADED chunk: refused, nothing
			// queued, backing untouched.
			CHECK(!manager.deleteVoxel(glm::vec3(4.0f, 80.0f, 4.0f)),
				  "delete on UNLOADED chunk is refused (logical AIR)");
			CHECK(ChunkManagerProbe::pendingEdits(manager) == queuedBefore,
				  "refused delete queues nothing");

			// Place: logical acceptance, physical deferral.
			CHECK(manager.placeVoxel(glm::vec3(4.0f, 80.0f, 4.0f), STONE),
				  "place on UNLOADED chunk is logically accepted");
			CHECK(waiting->getState() == ChunkState::UNLOADED,
				  "queued edit does not change the chunk state");
			CHECK(!waiting->isVoxelActive(4, 80, 4),
				  "queued edit does not touch the stale backing");
			CHECK(ChunkManagerProbe::pendingEdits(manager) == queuedBefore + 1,
				  "accepted place queues exactly one target entry");

			// (b) Coalescing while waiting for generation: place, delete
			// and re-place on the same coordinate collapse to one entry
			// whose type is the last logical state.
			CHECK(manager.deleteVoxel(glm::vec3(4.0f, 80.0f, 4.0f)),
				  "delete of the queued stone is accepted (overlay sees STONE)");
			CHECK(manager.placeVoxel(glm::vec3(4.0f, 80.0f, 4.0f), BRICKS),
				  "re-place on the queued coordinate is accepted");
			CHECK(ChunkManagerProbe::pendingEdits(manager) == queuedBefore + 1,
				  "successive edits coalesce to one pending entry");

			// (c) UNLOADED + inTransit defers exactly like UNLOADED alone.
			waiting->setInTransit(true); // generation job hypothetically in flight
			CHECK(manager.placeVoxel(glm::vec3(5.0f, 80.0f, 4.0f), STONE),
				  "place while UNLOADED+inTransit is logically accepted");
			waiting->setInTransit(false);
			CHECK(!waiting->isVoxelActive(5, 80, 4),
				  "transit deferral also skips the stale backing");
			CHECK(ChunkManagerProbe::pendingEdits(manager) == queuedBefore + 2,
				  "second coordinate queues a second entry");

			// (d) Generation is not short-circuited: the terrain lands in
			// full, then the queued edits apply on top of it.
			CHECK(waiting->prepareVoxelStorageForGeneration(),
				  "prepare before synchronous generation");
			waiting->generateTerrain(wgen);
			CHECK(waiting->getState() == ChunkState::GENERATED,
				  "terrain generation completes with queued edits pending");
			manager.processFinishedJobs();
			CHECK(waiting->getVoxel(4, 0, 4).type != static_cast<uint8_t>(AIR),
				  "generated terrain is present below the edits");
			CHECK(waiting->getVoxel(4, 80, 4).type == static_cast<uint8_t>(BRICKS),
				  "coalesced edit applies its last logical state after generation");
			CHECK(waiting->getVoxel(5, 80, 4).type == static_cast<uint8_t>(STONE),
				  "transit-queued edit applies after generation");
			checkOccupancyMetadataInSync(*waiting,
										 "metadata in sync after queued edits land on generated terrain");
		}

		// (e) Mirror toward an UNLOADED neighbor: the GENERATED target
		// applies immediately while the neighbor's mirror write queues;
		// it applies once the neighbor's own generation completes.
		Chunk *target = manager.getChunkAtWorldPos(glm::vec3(4.0f, 40.0f, 4.0f));
		Chunk *eastAgain = manager.getChunkAtWorldPos(
			glm::vec3(static_cast<float>(CHUNK_SIZE) + 4.0f, 40.0f, 4.0f));
		CHECK(target != nullptr && eastAgain != nullptr, "mirror-scenario chunks found");
		if (target && eastAgain && target->getState() == ChunkState::GENERATED)
		{
			int yEdge = -1;
			for (int y = static_cast<int>(CHUNK_HEIGHT) - 1; y >= 0; --y)
				if (target->getVoxel(CHUNK_SIZE - 1, y, 8).type == static_cast<uint8_t>(AIR))
				{
					yEdge = y;
					break;
				}
			CHECK(yEdge > 0, "found an air cell on the target's east edge");

			CHECK(manager.placeVoxel(glm::vec3(15.0f, static_cast<float>(yEdge), 8.0f), STONE),
				  "boundary edit on the generated target is accepted");
			CHECK(target->getVoxel(CHUNK_SIZE - 1, yEdge, 8).type == static_cast<uint8_t>(STONE),
				  "generated target applies its own edit immediately");
			CHECK(eastAgain->getState() == ChunkState::UNLOADED,
				  "mirror neighbor is still UNLOADED");
			CHECK(eastAgain->sampleForMeshing(-1, yEdge, 8) == AIR,
				  "mirror write does not touch the UNLOADED neighbor");
			CHECK(ChunkManagerProbe::pendingEdits(manager) == 1,
				  "exactly the neighbor mirror stays queued");

			const uint64_t revAfterGen = eastAgain->meshRevision();
			CHECK(eastAgain->prepareVoxelStorageForGeneration(),
				  "neighbor prepare before its generation");
			eastAgain->generateTerrain(wgen);
			CHECK(eastAgain->getState() == ChunkState::GENERATED,
				  "neighbor generation completes");
			manager.processFinishedJobs();
			CHECK(eastAgain->sampleForMeshing(-1, yEdge, 8) == STONE,
				  "queued mirror applies after the neighbor generates");
			CHECK(eastAgain->meshRevision() > revAfterGen,
				  "applied mirror invalidates the neighbor for remesh");
			checkOccupancyMetadataInSync(*eastAgain,
										 "neighbor metadata in sync after mirror application");
		}
	}

	// 20) Section-local remeshing (issue #107): edits dirty only the
	// affected vertical sections, and a mask-restricted build reproduces the
	// full build's section byte for byte.
	{
		MeshResultPool pool;
		TerrainGenerator sgen(107);
		Chunk sec(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
		CHECK(sec.prepareVoxelStorageForGeneration(), "section test prepare");
		sec.generateTerrain(sgen);
		CHECK(sec.dirtySections() == kAllSectionMask,
			  "generation arms a full-chunk build");

		// A mid-section edit in solid rock: exactly its own section. Find a
		// column with solid rock below the surface first.
		int rockY = -1;
		for (int y = 100; y > 20; --y)
		{
			if (y % 16 != 0 && y % 16 != 15 &&
					sec.getVoxel(8, y, 8).type != static_cast<uint8_t>(AIR) &&
					sec.getVoxel(8, y - 1, 8).type != static_cast<uint8_t>(AIR) &&
					sec.getVoxel(8, y - 2, 8).type != static_cast<uint8_t>(AIR))
			{
				rockY = y;
				break;
			}
		}
		CHECK(rockY > 0, "found solid rock for the mid-section edit");
		if (rockY > 0)
		{
			const int section = rockY / 16;
			sec.takeDirtySections(); // drain the generation mask
			sec.setVoxel(8, rockY, 8, BRICKS);
			const uint16_t midMask = sec.dirtySections();
			CHECK(midMask == (1u << section),
				  "mid-section edit dirties exactly its own section");
			sec.takeDirtySections();

			// Partial rebuild equivalence: a mask-restricted build must
			// reproduce the full build's section byte for byte, and leave
			// the untouched sections' payloads empty in the result.
			{
				MeshBuildResult *fullRef = pool.acquire();
				sec.buildMesh(*fullRef, sec.meshGeneration(), sec.meshRevision());
				pool.finishBuild(fullRef);
				CHECK(fullRef->sectionsBuilt == kAllSectionMask,
					  "full build stamps every section");
				// Copy the reference section payload.
				SectionMeshPayload refSection = fullRef->sections[static_cast<size_t>(section)];

				MeshBuildResult *partial = pool.acquire();
				sec.buildMesh(*partial, sec.meshGeneration(), sec.meshRevision(),
							  static_cast<uint16_t>(1u << section));
				pool.finishBuild(partial);
				CHECK(partial->sectionsBuilt == (1u << section),
					  "partial build stamps only its section");
				CHECK(partial->sections[static_cast<size_t>(section)] == refSection,
					  "partial section rebuild matches the full build byte for byte");
				size_t otherQuads = 0;
				for (int s = 0; s < static_cast<int>(partial->sections.size()); ++s)
					if (s != section)
						otherQuads += partial->sections[static_cast<size_t>(s)].opaqueVertices.size();
				CHECK(otherQuads == 0,
					  "unmasked sections hold no payload in a partial build");
				pool.release(fullRef);
				pool.release(partial);
			}
		}

		// Y-boundary edit: an edit at y%16==0 additionally dirties the
		// section below (the boundary plane's other owner).
		{
			int boundaryY = -1;
			for (int y = 96; y > 32; --y)
			{
				if (y % 16 == 0 &&
						sec.getVoxel(3, y, 3).type != static_cast<uint8_t>(AIR) &&
						sec.getVoxel(3, y - 1, 3).type != static_cast<uint8_t>(AIR))
				{
					boundaryY = y;
					break;
				}
			}
			if (boundaryY > 0)
			{
				const int section = boundaryY / 16;
				sec.takeDirtySections();
				sec.setVoxel(3, boundaryY, 3, BRICKS);
				const uint16_t mask = sec.dirtySections();
				CHECK((mask & (1u << section)) != 0 &&
						  (mask & (1u << (section - 1))) != 0,
					  "y-boundary edit dirties the section below too");
				sec.takeDirtySections();
			}
			else
			{
				// Fall back to a synthetic placement at the boundary: solid
				// rock straddling the seam is guaranteed by construction.
				sec.takeDirtySections();
				sec.setVoxel(3, 64, 3, STONE); // y=64: first layer of section 4
				const uint16_t mask = sec.dirtySections();
				CHECK((mask & 0b11000) == 0b11000,
					  "synthetic y-boundary edit dirties sections 3 and 4");
				sec.takeDirtySections();
			}
		}

		// Emissive edit: block light radius reaches the adjacent sections.
		{
			sec.takeDirtySections();
			sec.setVoxel(5, 40, 5, LAVA);
			const uint16_t mask = sec.dirtySections();
			CHECK((mask & 0b1110) == 0b1110,
				  "emissive edit dirties sections 1..3 (light radius +-14)");
			sec.takeDirtySections();
		}

		// Skylight column: deleting the top of a lit column dirties every
		// section of the opened shaft down to the first blocker.
		{
			sec.takeDirtySections();
			// Delete a surface voxel: the opened column's light range is
			// conservative, so only assert the edited section is included
			// and that nothing ABOVE the edit is dirtied by the light range.
			int surfaceY = -1;
			for (int y = CHUNK_HEIGHT - 1; y > 0; --y)
				if (sec.getVoxel(6, y, 6).type != static_cast<uint8_t>(AIR))
				{
					surfaceY = y;
					break;
				}
			CHECK(surfaceY > 0, "found a surface voxel for the skylight edit");
			if (surfaceY > 0)
			{
				sec.setVoxel(6, surfaceY, 6, AIR);
				const uint16_t mask = sec.dirtySections();
				CHECK((mask & (1u << (surfaceY / 16))) != 0,
					  "skylight edit dirties its own section");
				// The light BFS spreads -1 per step in every direction, so
				// the range may reach up to 15 cells above the edit (PR #117
				// review) - but never beyond that section.
				const int topSection =
					std::min(Chunk::kOccupancySections - 1,
							 (surfaceY + 15) / Chunk::kOccupancySectionSize);
				const uint16_t beyondMask =
				    static_cast<uint16_t>(0xFFFFu << (topSection + 1)) &
				    static_cast<uint16_t>((1u << Chunk::kOccupancySections) - 1u);
				CHECK((mask & beyondMask) == 0 || topSection == Chunk::kOccupancySections - 1,
					  "skylight light range stops within 15 cells above the edit");
				sec.takeDirtySections();
			}
		}

		// X/Z chunk-boundary edit: the neighbor chunk's mirror write (what
		// enqueueOrApplyMirrorEdits performs) dirties exactly the y/16
		// section of the neighbor - the border strip face ownership lives
		// in that one section.
		{
			Chunk neighbor(glm::vec3(float(CHUNK_SIZE), 0.0f, 0.0f), ChunkState::UNLOADED,
						   nullptr, nullptr, &pool);
			CHECK(neighbor.prepareVoxelStorageForGeneration(), "neighbor prepare");
			neighbor.generateTerrain(sgen);
			neighbor.takeDirtySections();
			neighbor.setVoxel(-1, 70, 7, BRICKS); // our x=0 edit mirrored
			CHECK(neighbor.dirtySections() == (1u << (70 / 16)),
				  "border mirror write dirties exactly the y/16 section");
			neighbor.setVoxel(CHUNK_SIZE, 70, 7, BRICKS);
			CHECK(neighbor.dirtySections() == (1u << (70 / 16)),
				  "east border mirror write dirties exactly the y/16 section");
			// A mirror write on a section Y boundary also dirties the
			// adjacent section: edge faces' AO corners of the boundary
			// plane read the border strip from the neighboring section's
			// pass (PR #117 review).
			neighbor.takeDirtySections();
			neighbor.setVoxel(-1, 64, 7, BRICKS);
			CHECK(neighbor.dirtySections() == 0b11000,
				  "border mirror write at y=64 dirties sections 3 and 4");
			neighbor.takeDirtySections();
			neighbor.setVoxel(-1, 63, 7, BRICKS);
			CHECK(neighbor.dirtySections() == 0b11000,
				  "border mirror write at y=63 dirties sections 3 and 4");
		}
	}

	// 21) Dirty-mask completeness property (PR #117 review phases 16-20, 38):
	// the fundamental invariant of section-local remeshing. For every edit
	// scene, the sections whose payloads differ between the pre-edit and
	// post-edit full builds must be a subset of the sections the edit
	// dirtied, and composing pre-edit payloads with a partial rebuild at
	// exactly the dirty mask must reproduce the post-edit full build byte
	// for byte. This is what makes changedSections > dirtyMask impossible
	// to ship silently - the old mask (no block-light range for occlusion
	// changes, no BFS reach above the edit) fails the lava-shaft scene.
	{
		MeshResultPool pool;
		TerrainGenerator pgen(4242);

		auto fullBuild = [&](Chunk &chunk) -> MeshBuildResult *
		{
			MeshBuildResult *r = pool.acquire();
			chunk.buildMesh(*r, chunk.meshGeneration(), chunk.meshRevision());
			pool.finishBuild(r);
			return r;
		};
		auto changedSections = [](const MeshBuildResult &before,
								  const MeshBuildResult &after) -> uint16_t
		{
			uint16_t mask = 0;
			for (size_t s = 0; s < before.sections.size(); ++s)
				if (before.sections[s] != after.sections[s])
					mask |= static_cast<uint16_t>(1u << s);
			return mask;
		};
		auto topAt = [](Chunk &c, int x, int z) -> int
		{
			for (int y = static_cast<int>(CHUNK_HEIGHT) - 1; y >= 0; --y)
				if (c.getVoxel(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
							   static_cast<uint32_t>(z))
						.type != static_cast<uint8_t>(AIR))
					return y;
			return -1;
		};
		auto runScene = [&](const char *label, auto &&setup, auto &&edit)
		{
			Chunk chunk(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
			CHECK(chunk.prepareVoxelStorageForGeneration(), "property: prepare");
			chunk.generateTerrain(pgen);
			setup(chunk);
			chunk.takeDirtySections(); // drain generation + setup edits

			MeshBuildResult *before = fullBuild(chunk);
			edit(chunk);
			const uint16_t mask = chunk.takeDirtySections();
			MeshBuildResult *partial = pool.acquire();
			chunk.buildMesh(*partial, chunk.meshGeneration(), chunk.meshRevision(), mask);
			pool.finishBuild(partial);
			MeshBuildResult *after = fullBuild(chunk);

			const uint16_t changed = changedSections(*before, *after);
			if (std::getenv("FT_PROP_DEBUG"))
			{
				std::cerr << "[prop] " << label << " mask=0x" << std::hex << mask
				          << " changed=0x" << changed << std::dec << std::endl;
			}
			CHECK((changed & ~mask) == 0,
				  (std::string(label) + ": every changed section is dirty").c_str());
			bool composes = true;
			for (size_t s = 0; s < before->sections.size(); ++s)
			{
				const SectionMeshPayload &want = ((mask >> s) & 1u)
					                                 ? partial->sections[s]
					                                 : before->sections[s];
				if (!(want == after->sections[s]))
					composes = false;
			}
			CHECK(composes, (std::string(label) +
			                 ": before+partial composes into the after full build")
			                    .c_str());
			pool.release(before);
			pool.release(partial);
			pool.release(after);
		};

		const auto noSetup = [](Chunk &) {};
		const auto lavaShaftSetup = [](Chunk &c)
		{
			// Underground lava pocket + a 1-wide air shaft rising out of it:
			// block light from the lava climbs the shaft, so an occlusion
			// change inside the lit span reroutes the BFS far from the edit.
			for (int x = 9; x <= 11; ++x)
				for (int z = 9; z <= 11; ++z)
					for (int y = 39; y <= 41; ++y)
						c.setVoxel(x, y, z, AIR);
			for (int y = 41; y <= 56; ++y)
				c.setVoxel(10, y, 10, AIR);
		};
		const auto lavaShaftLitSetup = [&lavaShaftSetup](Chunk &c)
		{
			lavaShaftSetup(c);
			c.setVoxel(10, 40, 10, LAVA);
		};

		// Surface edits.
		runScene("surface stone place", noSetup, [&topAt](Chunk &c)
		{
			const int y = topAt(c, 8, 8);
			if (y >= 0 && y + 1 < static_cast<int>(CHUNK_HEIGHT))
				c.setVoxel(8, y + 1, 8, STONE);
		});
		runScene("surface top delete", noSetup, [&topAt](Chunk &c)
		{
			const int y = topAt(c, 8, 8);
			if (y > 0)
				c.setVoxel(8, y, 8, AIR);
		});

		// Skylight shaft: closing and re-opening a full-height lit column.
		const auto openShaftSetup = [](Chunk &c)
		{
			for (int y = 41; y <= 90; ++y)
				c.setVoxel(8, y, 8, AIR);
		};
		runScene("shaft blocker place (sky column)", openShaftSetup, [](Chunk &c)
		{
			c.setVoxel(8, 60, 8, STONE);
		});
		runScene("shaft blocker delete (sky reopen)",
				 [&openShaftSetup](Chunk &c)
		{
			openShaftSetup(c);
			c.setVoxel(8, 60, 8, STONE);
		},
		         [](Chunk &c) { c.setVoxel(8, 60, 8, AIR); });

		// Block light: emitter add/remove and occlusion reroutes near one.
		runScene("lava placement", lavaShaftSetup, [](Chunk &c)
		{
			c.setVoxel(10, 40, 10, LAVA);
		});
		runScene("lava removal", lavaShaftLitSetup, [](Chunk &c)
		{
			c.setVoxel(10, 40, 10, AIR);
		});
		// The pre-review mask (emitter check only, sky scan down the column)
		// misses the block-light BFS reroute of a plain opaque placement
		// inside the lit shaft: the light dies ABOVE the edit (cells 46..55,
		// section 3) while the edit itself sits mid-section 2 and the scan
		// only ever reaches downward. Mid-section y on purpose.
		runScene("stone placed inside lit shaft (BFS reroute)", lavaShaftLitSetup,
		         [](Chunk &c) { c.setVoxel(10, 45, 10, STONE); });
		runScene("stone removed inside lit shaft (BFS reroute)",
				 [&lavaShaftLitSetup](Chunk &c)
		{
			lavaShaftLitSetup(c);
			c.setVoxel(10, 45, 10, STONE);
		},
		         [](Chunk &c) { c.setVoxel(10, 45, 10, AIR); });

		// Lateral sky flood: a sealed tunnel branch off the lit shaft.
		runScene("cave tunnel opened (lateral sky flood)",
				 [&openShaftSetup](Chunk &c)
		{
			openShaftSetup(c);
			for (int x = 10; x <= 14; ++x)
				c.setVoxel(x, 60, 8, AIR);
			c.setVoxel(9, 60, 8, STONE); // seal
		},
		         [](Chunk &c) { c.setVoxel(9, 60, 8, AIR); });

		// Section-boundary edits in both directions.
		runScene("boundary edit at y=15",
				 [](Chunk &c) { c.setVoxel(8, 15, 8, AIR); },
		         [](Chunk &c) { c.setVoxel(8, 15, 8, STONE); });
		runScene("boundary edit at y=16",
				 [](Chunk &c) { c.setVoxel(8, 16, 8, AIR); },
		         [](Chunk &c) { c.setVoxel(8, 16, 8, STONE); });

		// Border mirror edit on the neighbor (its own before/after build).
		{
			Chunk neighbor(glm::vec3(float(CHUNK_SIZE), 0.0f, 0.0f),
			               ChunkState::UNLOADED, nullptr, nullptr, &pool);
			CHECK(neighbor.prepareVoxelStorageForGeneration(), "property border: prepare");
			neighbor.generateTerrain(pgen);
			neighbor.takeDirtySections();
			MeshBuildResult *before = fullBuild(neighbor);
			neighbor.setVoxel(-1, 63, 7, BRICKS);
			const uint16_t mask = neighbor.takeDirtySections();
			MeshBuildResult *after = fullBuild(neighbor);
			const uint16_t changed = changedSections(*before, *after);
			CHECK((changed & ~mask) == 0,
				  "border mirror edit at y=63: every changed section is dirty");
			pool.release(before);
			pool.release(after);
		}
	}

	// 22) Moves transfer the sectioned GPU layout, the LOD arena ranges and
	// the dirty mask (issue #107/#109): slots, arena ranges and the arenas
	// pointer travel with the chunk, and the moved-from chunk starts clean.
	{
		MeshResultPool pool;
		MeshArenas arenas; // non-initiated: the move test only checks pointers
		Chunk a(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
		{
			auto &slots = ChunkStateProbe::sectionGpuMut(a);
			slots[2] = {/*vertexPage*/ 0, /*vertexOffset*/ 256, /*vertexSlotBytes*/ 512,
			            /*vertexUsedBytes*/ 480, /*vertexBase*/ 64,
			            /*indexPage*/ 1, /*indexOffset*/ 768, /*indexSlotBytes*/ 384,
			            /*indexUsedBytes*/ 360, /*indexCount*/ 90};
			ChunkStateProbe::sectionGpuWaterMut(a)[5].indexCount = 12;
		}
		ChunkStateProbe::lodOpaqueVMut(a) = {/*page*/ 0, /*offset*/ 1024, /*bytes*/ 2048};
		ChunkStateProbe::lodOpaqueIMut(a) = {1, 128, 512};
		ChunkStateProbe::lodWaterVMut(a) = {2, 64, 128};
		ChunkStateProbe::lodWaterIMut(a) = {3, 32, 96};
		ChunkStateProbe::setArenas(a, arenas);
		a.markSectionsDirty(0b1010);

		Chunk b(std::move(a));
		CHECK(b.dirtySections() == 0b1010, "move ctor transfers the dirty mask");
		const auto &bs = ChunkStateProbe::sectionGpuMut(b)[2];
		CHECK(bs.vertexOffset == 256 && bs.vertexSlotBytes == 512 &&
				  bs.vertexUsedBytes == 480 && bs.vertexBase == 64 &&
				  bs.indexOffset == 768 && bs.indexSlotBytes == 384 &&
				  bs.indexUsedBytes == 360 && bs.indexCount == 90,
			  "move ctor transfers the opaque slot layout");
		CHECK(ChunkStateProbe::sectionGpuWaterMut(b)[5].indexCount == 12,
			  "move ctor transfers the water slot layout");
		CHECK(ChunkStateProbe::lodOpaqueVMut(b).offset == 1024 &&
				  ChunkStateProbe::lodOpaqueIMut(b).bytes == 512 &&
				  ChunkStateProbe::lodWaterVMut(b).offset == 64 &&
				  ChunkStateProbe::lodWaterIMut(b).bytes == 96,
			  "move ctor transfers the LOD arena ranges");
		CHECK(ChunkStateProbe::arenas(b) == &arenas,
			  "move ctor transfers the arenas pointer");
		CHECK(a.dirtySections() == 0 &&
				  ChunkStateProbe::sectionGpuMut(a)[2].indexCount == 0 &&
				  ChunkStateProbe::sectionGpuWaterMut(a)[5].indexCount == 0 &&
				  ChunkStateProbe::lodOpaqueIMut(a).empty() &&
				  ChunkStateProbe::lodWaterIMut(a).empty() &&
				  ChunkStateProbe::arenas(a) == nullptr,
			  "move ctor zeroes the source GPU/dirty state");

		Chunk c(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
		c.markSectionsDirty(0b1);
		ChunkStateProbe::lodOpaqueVMut(c) = {9, 9, 9};
		c = std::move(b);
		CHECK(c.dirtySections() == 0b1010 &&
				  ChunkStateProbe::sectionGpuMut(c)[2].indexCount == 90 &&
				  ChunkStateProbe::lodOpaqueVMut(c).offset == 1024,
			  "move assignment transfers the GPU/dirty state");
		CHECK(b.dirtySections() == 0 && ChunkStateProbe::lodOpaqueVMut(b).empty() &&
				  ChunkStateProbe::sectionGpuMut(b)[2].indexCount == 0,
			  "move assignment zeroes the source GPU/dirty state");

		// The test fabricated arena ranges that reference no real page:
		// drop the synthetic GPU state before the destructors run.
		for (auto *ch : {&a, &b, &c})
		{
			ChunkStateProbe::sectionGpuMut(*ch).fill({});
			ChunkStateProbe::sectionGpuWaterMut(*ch).fill({});
			ChunkStateProbe::lodOpaqueVMut(*ch) = {};
			ChunkStateProbe::lodOpaqueIMut(*ch) = {};
			ChunkStateProbe::lodWaterVMut(*ch) = {};
			ChunkStateProbe::lodWaterIMut(*ch) = {};
			ChunkStateProbe::clearArenas(*ch);
		}
	}

	// 23) A completed section build superseded by a queued edit must re-arm
	// the sections it rebuilt (PR #117 review phases 11-12): the edit only
	// re-arms its own sections when it is applied, so without the re-arm the
	// dropped job's sections would keep their stale GPU meshes.
	{
		MeshResultPool pool;
		ChunkPool chunkPool(8);
		TerrainGenerator mgen(1337);
		ChunkManager manager(&mgen, nullptr, &chunkPool);
		Camera cam(glm::vec3(0.0f, 100.0f, 0.0f));
		RenderSettings settings;
		manager.updateStreaming(cam, settings);
		manager.processChunkLoading(8);
		Chunk *chunk = manager.getChunkAtWorldPos(glm::vec3(4.0f, 40.0f, 4.0f));
		CHECK(chunk != nullptr, "superseded: chunk registered");
		if (chunk)
		{
			CHECK(chunk->prepareVoxelStorageForGeneration(), "superseded: prepare");
			chunk->generateTerrain(mgen);
			chunk->takeDirtySections();

			// A worker-style section build for section 2 completes while a
			// section-8 edit is queued behind the in-transit flag.
			MeshBuildResult *stale = pool.acquire();
			chunk->buildMesh(*stale, chunk->meshGeneration(), chunk->meshRevision(),
							 static_cast<uint16_t>(1u << 2));
			pool.finishBuild(stale);

			chunk->setInTransit(true);
			int editY = -1;
			for (int y = static_cast<int>(CHUNK_HEIGHT) - 1; y >= 0; --y)
				if (chunk->getVoxel(8, static_cast<uint32_t>(y), 8).type ==
					static_cast<uint8_t>(AIR))
				{
					editY = y;
					break;
				}
			CHECK(editY > 0, "superseded: air column for the queued edit");
			CHECK(manager.placeVoxel(glm::vec3(8.0f, static_cast<float>(editY), 8.0f),
			                         STONE),
				  "superseded: edit queued while in transit");
			CHECK(chunk->dirtySections() == 0,
				  "superseded: mask empty while the job is out");

			ChunkManagerProbe::injectCompletedMeshJob(manager, chunk, stale);
			manager.processFinishedJobs();

			// The queued edit arms its own sections (plus the conservative
			// light range of an occlusion change), and the dropped build
			// re-arms the sections it had rebuilt on top.
			const uint16_t expectedMask =
			    static_cast<uint16_t>((1u << 2) | (1u << (editY / 16)));
			CHECK((chunk->dirtySections() & expectedMask) == expectedMask,
				  "superseded build re-arms its sections next to the edit's");
			CHECK(ChunkManagerProbe::pendingEdits(manager) == 0,
				  "superseded: queued edit applied");
			CHECK(pool.activeCount() == 0, "superseded: dropped result back in pool");

			// The next dispatch must rebuild at least the re-armed sections.
			const uint16_t mask = chunk->takeDirtySections();
			CHECK((mask & expectedMask) == expectedMask,
				  "superseded: dispatch takes the merged mask");
			MeshBuildResult *rebuilt = pool.acquire();
			chunk->buildMesh(*rebuilt, chunk->meshGeneration(), chunk->meshRevision(), mask);
			pool.finishBuild(rebuilt);
			CHECK(rebuilt->sectionsBuilt == mask,
				  "superseded: next build stamps the merged mask");
			pool.release(rebuilt);
		}
	}

	// 24) RGB block light mesh equivalence (issue #141): the chunk-wide RGB4
	// light field must honor the same section-remeshing contract as the rest
	// of the mesher - partial rebuilds reproduce full builds byte for byte,
	// the per-channel max overlap combination is placement-order independent,
	// opaque blockers stop propagation, and emissive removal restores the
	// unlit field exactly.
	{
		MeshResultPool pool;

		// (a) Section/chunk-boundary equivalence: emitters at the 15/16
		// section seam and next to the x border, in a deterministic stone
		// volume. Re-placing the same edits re-arms the exact
		// markEditDirtySections mask; a mask-restricted rebuild must
		// reproduce the full build's sections byte for byte, because every
		// job recomputes the light field chunk-wide (no seams).
		{
			TerrainGenerator bgen(4141);
			Chunk sec(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
			CHECK(sec.prepareVoxelStorageForGeneration(), "rgb boundary: prepare");
			sec.generateTerrain(bgen);
			sec.takeDirtySections();

			// Stone volume overriding whatever the seed generated here.
			for (int x = 2; x <= 15; ++x)
				for (int z = 4; z <= 12; ++z)
					for (int y = 4; y <= 28; ++y)
						sec.setVoxel(x, y, z, STONE);
			sec.takeDirtySections();

			// Lava straddling the section 0/1 boundary, plus one near the
			// x border deeper in the volume.
			sec.setVoxel(8, 15, 8, LAVA);
			sec.setVoxel(8, 16, 8, LAVA);
			sec.setVoxel(15, 24, 8, LAVA);
			sec.takeDirtySections();

			MeshBuildResult *fullRef = pool.acquire();
			sec.buildMesh(*fullRef, sec.meshGeneration(), sec.meshRevision());
			pool.finishBuild(fullRef);
			CHECK(fullRef->sectionsBuilt == kAllSectionMask,
				  "rgb boundary: full build stamps every section");
			CHECK(totalOpaqueVertices(*fullRef) > 0,
				  "rgb boundary: scene produced geometry");

			// Re-placing the same emissive edits re-arms the widened mask:
			// own sections + the 15/16 seam pair + the +-15 light range.
			sec.setVoxel(8, 15, 8, LAVA);
			sec.setVoxel(8, 16, 8, LAVA);
			sec.setVoxel(15, 24, 8, LAVA);
			const uint16_t mask = sec.takeDirtySections();
			CHECK(mask == 0b0111,
				  "rgb boundary edits dirty sections 0..2 (seam pair + light range)");

			MeshBuildResult *partial = pool.acquire();
			sec.buildMesh(*partial, sec.meshGeneration(), sec.meshRevision(), mask);
			pool.finishBuild(partial);
			CHECK(partial->sectionsBuilt == mask,
				  "rgb boundary: partial build stamps only the dirty mask");
			bool identical = true;
			for (size_t s = 0; s < fullRef->sections.size(); ++s)
				if (((mask >> s) & 1u) && !(partial->sections[s] == fullRef->sections[s]))
					identical = false;
			CHECK(identical,
				  "rgb boundary: mask-restricted rebuild matches the full build byte for byte");
			size_t otherQuads = 0;
			for (size_t s = 0; s < partial->sections.size(); ++s)
				if (!((mask >> s) & 1u))
					otherQuads += partial->sections[s].opaqueVertices.size();
			CHECK(otherQuads == 0,
				  "rgb boundary: unmasked sections hold no payload in a partial build");
			pool.release(fullRef);
			pool.release(partial);
		}

		// (b) Overlapping sources: LAVA + REDSTONE_ORE a few blocks apart in
		// one air pocket, placed in opposite orders in two fresh chunk
		// instances. The per-channel max combination is commutative and
		// associative, so both full meshes must come out byte-identical.
		{
			TerrainGenerator ogen(4142);
			const glm::ivec3 lavaCell(9, 40, 9);
			const glm::ivec3 oreCell(11, 40, 11);
			auto buildOverlapScene = [&](bool lavaFirst) -> MeshBuildResult *
			{
				Chunk c(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
				CHECK(c.prepareVoxelStorageForGeneration(), "rgb overlap: prepare");
				c.generateTerrain(ogen);
				c.takeDirtySections();
				for (int x = 8; x <= 12; ++x)
					for (int z = 8; z <= 12; ++z)
						for (int y = 39; y <= 41; ++y)
							c.setVoxel(x, y, z, AIR);
				if (lavaFirst)
				{
					c.setVoxel(lavaCell.x, lavaCell.y, lavaCell.z, LAVA);
					c.setVoxel(oreCell.x, oreCell.y, oreCell.z, REDSTONE_ORE);
				}
				else
				{
					c.setVoxel(oreCell.x, oreCell.y, oreCell.z, REDSTONE_ORE);
					c.setVoxel(lavaCell.x, lavaCell.y, lavaCell.z, LAVA);
				}
				c.takeDirtySections();
				MeshBuildResult *r = pool.acquire();
				c.buildMesh(*r, c.meshGeneration(), c.meshRevision());
				pool.finishBuild(r);
				return r;
			};
			MeshBuildResult *lavaFirst = buildOverlapScene(true);
			MeshBuildResult *oreFirst = buildOverlapScene(false);
			bool orderIndependent = true;
			for (size_t s = 0; s < lavaFirst->sections.size(); ++s)
				if (!(lavaFirst->sections[s] == oreFirst->sections[s]))
					orderIndependent = false;
			CHECK(orderIndependent,
				  "rgb overlap: mesh is byte-identical regardless of source placement order");
			// Not vacuous: section 2 (y=40) carries vertices lit by the sources.
			int maxR = 0;
			for (const Vertex &v :
				 lavaFirst->sections[static_cast<size_t>(lavaCell.y / 16)].opaqueVertices)
				maxR = std::max(maxR, static_cast<int>(vBlockR(v)));
			CHECK(maxR > 0, "rgb overlap: sources light the pocket geometry");
			pool.release(lavaFirst);
			pool.release(oreFirst);
		}

		// (c) Opaque blocker: LAVA pocket | 2-thick stone wall | sealed probe
		// pocket, inside a stone box that overrides the seed's content. Faces
		// beyond the wall sample only the sealed probe cells, whose block
		// light is 0 - light must not cross the opaque barrier.
		{
			TerrainGenerator kgen(4143);
			Chunk blk(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
			CHECK(blk.prepareVoxelStorageForGeneration(), "rgb blocker: prepare");
			blk.generateTerrain(kgen);
			blk.takeDirtySections();
			for (int x = 2; x <= 13; ++x)
				for (int z = 2; z <= 13; ++z)
					for (int y = 4; y <= 12; ++y)
						blk.setVoxel(x, y, z, STONE);
			blk.takeDirtySections();
			// Lava pocket x=4..6 | wall x=7..8 (2 thick) | probe pocket x=9..11.
			for (int x = 4; x <= 6; ++x)
				for (int z = 8; z <= 10; ++z)
					for (int y = 8; y <= 10; ++y)
						blk.setVoxel(x, y, z, AIR);
			blk.setVoxel(5, 9, 9, LAVA);
			for (int x = 9; x <= 11; ++x)
				for (int z = 8; z <= 10; ++z)
					for (int y = 8; y <= 10; ++y)
						blk.setVoxel(x, y, z, AIR);
			blk.takeDirtySections();

			MeshBuildResult *full = pool.acquire();
			blk.buildMesh(*full, blk.meshGeneration(), blk.meshRevision());
			pool.finishBuild(full);

			// Wall faces adjacent to the lava own plane x=7 (sampling the
			// lit pocket); faces beyond the wall own plane x=9 (sampling the
			// unlit probe pocket). Quantized positions are voxel units * 16.
			// Greedy quads share one light packing sampled from the seed
			// cell, so the near side carries the pocket gradient's corner
			// value rather than the cell closest to the emitter.
			int nearMaxR = 0;
			int farMaxR = 0;
			for (const SectionMeshPayload &s : full->sections)
				for (const Vertex &v : s.opaqueVertices)
				{
					const uint32_t qx = vQuantX(v);
					const uint32_t qy = vQuantY(v);
					const uint32_t qz = vQuantZ(v);
					if (qy < 8u * 16u || qy > 11u * 16u || qz < 8u * 16u || qz > 11u * 16u)
						continue;
					if (qx == 7u * 16u)
						nearMaxR = std::max(nearMaxR, static_cast<int>(vBlockR(v)));
					else if (qx == 9u * 16u)
						farMaxR = std::max(farMaxR, static_cast<int>(vBlockR(v)));
				}
			if (std::getenv("FT_RGB_DEBUG"))
				std::cerr << "[rgb] blocker nearMaxR=" << nearMaxR
				          << " farMaxR=" << farMaxR << std::endl;
			CHECK(nearMaxR > 0,
				  "rgb blocker: wall faces adjacent to the lava are lit");
			CHECK(farMaxR < nearMaxR,
				  "rgb blocker: vertices beyond the opaque wall are strictly darker");
			CHECK(farMaxR == 0,
				  "rgb blocker: a 2-thick opaque wall fully blocks block light");
			pool.release(full);
		}

		// (d) Emissive removal: the add-side widened mask is already covered
		// by the section-20 emissive test. Removing the lava must widen the
		// dirty mask over the same +-15 light range, and the rebuild must
		// restore the never-lit field byte for byte. A 1-cell pocket at
		// y=40 (probe section 2) keeps removal deterministic: the fresh
		// chunk shares the carved pocket but never held lava.
		{
			TerrainGenerator rgen(4144);
			Chunk edited(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
			Chunk fresh(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
			CHECK(edited.prepareVoxelStorageForGeneration() &&
					  fresh.prepareVoxelStorageForGeneration(),
				  "rgb removal: prepare");
			edited.generateTerrain(rgen);
			fresh.generateTerrain(rgen);
			edited.takeDirtySections();
			fresh.takeDirtySections();

			edited.setVoxel(5, 40, 5, AIR);
			fresh.setVoxel(5, 40, 5, AIR);
			edited.takeDirtySections();
			fresh.takeDirtySections();

			edited.setVoxel(5, 40, 5, LAVA); // add (mask covered by section 20)
			edited.takeDirtySections();

			edited.setVoxel(5, 40, 5, AIR); // remove
			const uint16_t mask = edited.takeDirtySections();
			CHECK((mask & 0b1110) == 0b1110,
				  "rgb removal: emissive removal dirties sections 1..3 (light radius)");

			MeshBuildResult *after = pool.acquire();
			edited.buildMesh(*after, edited.meshGeneration(), edited.meshRevision());
			pool.finishBuild(after);
			MeshBuildResult *ref = pool.acquire();
			fresh.buildMesh(*ref, fresh.meshGeneration(), fresh.meshRevision());
			pool.finishBuild(ref);
			bool restored = true;
			for (size_t s = 0; s < after->sections.size(); ++s)
				if (!(after->sections[s] == ref->sections[s]))
					restored = false;
			CHECK(restored,
				  "rgb removal: rebuild matches a chunk that never had lava byte for byte");
			pool.release(after);
			pool.release(ref);
		}
	}

	// 25) Cross-chunk RGB block light (issue #141 review): with a halo
	// attached the block-light BFS runs on the center+15-ring domain, so
	// emissive light crosses chunk borders, and the ChunkManager dirties the
	// reachable neighbors when a light-relevant edit lands within the halo
	// radius of a border. NOTE on orientation: the engine's halo/mirror
	// convention is south = -z, north = +z (fillLightHaloFromNeighbors in
	// Chunk.cpp), so the corner scene below keeps its helper chunks on the
	// +z side of the center and passes them as north/northEast.
	{
		MeshResultPool pool;

		// Gallery face planes in quantized units (voxel * 16): floor y=39,
		// ceiling y=42, side walls z=7/z=10.
		constexpr int kTunYLo = 39 * 16, kTunYHi = 42 * 16;
		constexpr int kTunZLo = 7 * 16, kTunZHi = 10 * 16;

		// Sealed 3x3 air gallery along +x straddling the A|B border: center
		// line y=40, z 7..9, A-local x 8..15 continuing into B-local x 0..8.
		// The stone shell overrides the seed content, so the gallery is dark
		// regardless of the generated surface and holds no natural sources.
		auto carveTunnel = [](Chunk &a, Chunk &b)
		{
			for (int x = 7; x <= 15; ++x)
				for (int z = 6; z <= 10; ++z)
					for (int y = 38; y <= 42; ++y)
						a.setVoxel(x, y, z, STONE);
			for (int x = 0; x <= 9; ++x)
				for (int z = 6; z <= 10; ++z)
					for (int y = 38; y <= 42; ++y)
						b.setVoxel(x, y, z, STONE);
			for (int x = 8; x <= 15; ++x)
				for (int z = 7; z <= 9; ++z)
					for (int y = 39; y <= 41; ++y)
						a.setVoxel(x, y, z, AIR);
			for (int x = 0; x <= 8; ++x)
				for (int z = 7; z <= 9; ++z)
					for (int y = 39; y <= 41; ++y)
						b.setVoxel(x, y, z, AIR);
		};
		auto generatePair = [&](Chunk &a, Chunk &b)
		{
			CHECK(a.prepareVoxelStorageForGeneration(), "rgb cross: prepare A");
			CHECK(b.prepareVoxelStorageForGeneration(), "rgb cross: prepare B");
			a.generateTerrain(gen);
			b.generateTerrain(gen);
		};
		auto buildWithHalo = [&](Chunk &chunk, ChunkLightHalo &halo) -> MeshBuildResult *
		{
			chunk.setLightHalo(&halo);
			MeshBuildResult *r = pool.acquire();
			chunk.buildMesh(*r, chunk.meshGeneration(), chunk.meshRevision());
			pool.finishBuild(r);
			chunk.setLightHalo(nullptr);
			return r;
		};
		// Max block-light nibble among opaque vertices inside a quantized
		// position window; *count (optional) receives the vertex count.
		auto maxNibble = [](const MeshBuildResult &r, uint32_t (*nibble)(const Vertex &),
							int xLo, int xHi, int yLo, int yHi, int zLo, int zHi,
							int *count = nullptr) -> int
		{
			int best = 0;
			if (count)
				*count = 0;
			for (const SectionMeshPayload &s : r.sections)
				for (const Vertex &v : s.opaqueVertices)
				{
					const uint32_t qx = vQuantX(v);
					const uint32_t qy = vQuantY(v);
					const uint32_t qz = vQuantZ(v);
					if (qx < xLo || qx > xHi || qy < yLo || qy > yHi || qz < zLo || qz > zHi)
						continue;
					if (count)
						++*count;
					best = std::max<int>(best, static_cast<int>(nibble(v)));
				}
			return best;
		};

		// (a) Tunnel across the border (the P1 scenario): LAVA at the border
		// column of A must light B's gallery through the halo, brightest at
		// the border and strictly decaying with distance.
		{
			Chunk a(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
			Chunk b(glm::vec3(float(CHUNK_SIZE), 0.0f, 0.0f), ChunkState::UNLOADED,
					nullptr, nullptr, &pool);
			generatePair(a, b);
			carveTunnel(a, b);
			// Floor bumps in B: light is sampled once per merged greedy quad
			// at its origin cell, so a straight gallery would merge the whole
			// floor into one quad originating at the border and carry the
			// border brightness all the way out. The bumps split the floor so
			// quads with far origins exist, each sampling progressively
			// darker cells (11/9 at x=5, 8/6 at x=7).
			b.setVoxel(5, 39, 8, STONE);
			b.setVoxel(7, 39, 8, STONE);
			a.setVoxel(15, 40, 8, LAVA); // the border column, in the gallery

			auto haloA = std::make_unique<ChunkLightHalo>();
			auto haloB = std::make_unique<ChunkLightHalo>();
			haloA->resetToAir();
			fillLightHaloFromNeighbors(*haloA, &a, nullptr, &b, nullptr, nullptr,
									   nullptr, nullptr, nullptr, nullptr);
			haloB->resetToAir();
			fillLightHaloFromNeighbors(*haloB, &b, &a, nullptr, nullptr, nullptr,
									   nullptr, nullptr, nullptr, nullptr);

			MeshBuildResult *rb = buildWithHalo(b, *haloB);
			// Near window: vertices adjacent to B-local x=0..1 cells.
			const int nearR = maxNibble(*rb, vBlockR, 0, 16, kTunYLo, kTunYHi, kTunZLo, kTunZHi);
			// Far window: vertices at B-local x in [6..8]. The border quad's
			// own far corners sit at x=5 (excluded), so everything in this
			// window originates away from the border.
			const int farR = maxNibble(*rb, vBlockR, 6 * 16, 8 * 16, kTunYLo, kTunYHi, kTunZLo, kTunZHi);
			CHECK(nearR > 0, "rgb tunnel: lava light from A crosses into B's mesh near the border");
			CHECK(nearR >= 10, "rgb tunnel: B's border-adjacent vertices carry lava brightness (>= 10)");
			CHECK(farR > 0, "rgb tunnel: far window carries geometry (decay check is non-vacuous)");
			CHECK(nearR > farR, "rgb tunnel: block light decays with distance into B (near > far)");

			// Same-halo rebuild determinism.
			MeshBuildResult *rb2 = buildWithHalo(b, *haloB);
			bool sameB = rb->sections.size() == rb2->sections.size();
			for (size_t s = 0; sameB && s < rb->sections.size(); ++s)
				sameB = rb->sections[s] == rb2->sections[s];
			CHECK(sameB, "rgb tunnel: same-halo rebuild of B is byte-for-byte deterministic");

			// Stale-source contrast: a ring with no neighbor content
			// (missing neighbors contribute AIR) removes the crossing light
			// entirely - the border brightness really comes from the halo.
			auto haloStale = std::make_unique<ChunkLightHalo>();
			haloStale->resetToAir();
			fillLightHaloFromNeighbors(*haloStale, &b, nullptr, nullptr, nullptr,
									   nullptr, nullptr, nullptr, nullptr, nullptr);
			MeshBuildResult *rb3 = buildWithHalo(b, *haloStale);
			const int staleR = maxNibble(*rb3, vBlockR, 0, 16, kTunYLo, kTunYHi, kTunZLo, kTunZHi);
			CHECK(staleR == 0, "rgb tunnel: a sourceless ring leaves B's border dark");
			if (std::getenv("FT_RGB_DEBUG"))
				std::cerr << "[rgb] tunnel nearMaxR=" << nearR << " farMaxR=" << farR
						  << " staleMaxR=" << staleR << std::endl;
			pool.release(rb);
			pool.release(rb2);
			pool.release(rb3);
		}

		// (b) Manager-level invalidation on edit and removal: a light edit
		// within the halo radius of a border dirties the reachable neighbors
		// (markSectionsDirty + MESHED -> GENERATED), the x+1<=15 reach gate
		// excludes the far side, removal dirties again, and a non-light edit
		// dirties nobody.
		{
			ChunkPool chunkPool(8);
			ChunkManager manager(&gen, nullptr, &chunkPool);
			Chunk *a = chunkPool.acquire(glm::vec3(0.0f));
			Chunk *b = chunkPool.acquire(glm::vec3(float(CHUNK_SIZE), 0.0f, 0.0f));
			Chunk *w = chunkPool.acquire(glm::vec3(float(-CHUNK_SIZE), 0.0f, 0.0f));
			CHECK(a != nullptr && b != nullptr && w != nullptr, "rgb manager: pool acquisition");
			if (a && b && w)
			{
				// m_chunks keys are CHUNK INDICES, not world positions: the
				// chunks sit at world x 0 / 16 / -16, i.e. indices 0 / +1 / -1.
				ChunkManagerProbe::registerChunk(manager, glm::ivec3(0, 0, 0), a);
				ChunkManagerProbe::registerChunk(manager, glm::ivec3(1, 0, 0), b);
				ChunkManagerProbe::registerChunk(manager, glm::ivec3(-1, 0, 0), w);
				CHECK(manager.prepareAndGenerateChunk(a, gen), "rgb manager: generate A");
				CHECK(manager.prepareAndGenerateChunk(b, gen), "rgb manager: generate B");
				CHECK(manager.prepareAndGenerateChunk(w, gen), "rgb manager: generate W");
				carveTunnel(*a, *b);
				CHECK(a->generateMesh() && b->generateMesh() && w->generateMesh(),
					  "rgb manager: initial meshes published");
				CHECK(a->getState() == ChunkState::MESHED &&
						  b->getState() == ChunkState::MESHED &&
						  w->getState() == ChunkState::MESHED,
					  "rgb manager: chunks MESHED before the edit");
				a->takeDirtySections();
				b->takeDirtySections();
				w->takeDirtySections();

				// Light-relevant edit near the east border (x=14 is within
				// 15 of it): the east neighbor must be dirtied and re-armed.
				CHECK(manager.placeVoxel(glm::vec3(14.0f, 40.0f, 8.0f), LAVA),
					  "rgb manager: LAVA placement near the east border accepted");
				CHECK(b->dirtySections() != 0,
					  "rgb manager: light edit dirties the east neighbor's sections");
				CHECK(b->getState() == ChunkState::GENERATED,
					  "rgb manager: dirtied east neighbor re-armed GENERATED (was MESHED)");

				// Edge gating: x=15 is out of the WEST neighbor's reach
				// (x+1 = 16 > 15) but inside the east one's (16-15 = 1).
				w->takeDirtySections();
				b->takeDirtySections();
				CHECK(manager.placeVoxel(glm::vec3(15.0f, 41.0f, 8.0f), LAVA),
					  "rgb manager: border-column LAVA placement accepted");
				CHECK(w->dirtySections() == 0,
					  "rgb manager: LAVA at local x=15 does not reach the west neighbor");
				CHECK(b->dirtySections() != 0,
					  "rgb manager: LAVA at local x=15 still dirties the east neighbor");

				// Removal of the same emitter dirties the neighbor again.
				b->takeDirtySections();
				CHECK(manager.deleteVoxel(glm::vec3(14.0f, 40.0f, 8.0f)),
					  "rgb manager: removal of the border LAVA accepted");
				CHECK(b->dirtySections() != 0,
					  "rgb manager: removal dirties the east neighbor again");

				// A non-light edit (transparent placement: no emission, no
				// sky-transmission flip) must dirty nobody, even though the
				// cell is within the halo radius of the border.
				b->takeDirtySections();
				CHECK(manager.placeVoxel(glm::vec3(5.0f, 255.0f, 5.0f), GLASS),
					  "rgb manager: non-light GLASS placement accepted");
				CHECK(b->dirtySections() == 0,
					  "rgb manager: non-light edit leaves the east neighbor clean");
			}
		}

		// (c) Red/blue overlap across the border: a red source in A and a
		// blue source in B color each other's border geometry.
		{
			Chunk a(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
			Chunk b(glm::vec3(float(CHUNK_SIZE), 0.0f, 0.0f), ChunkState::UNLOADED,
					nullptr, nullptr, &pool);
			generatePair(a, b);
			carveTunnel(a, b);
			a.setVoxel(12, 40, 8, REDSTONE_ORE); // A-local, in the gallery
			b.setVoxel(3, 40, 8, LAPIS_ORE);	 // B-local (world x=19)
			// Floor bump near A's border: its top face's light is sampled at
			// the border-adjacent cell, where the lapis blue has crossed.
			a.setVoxel(14, 39, 8, STONE);

			auto haloA = std::make_unique<ChunkLightHalo>();
			auto haloB = std::make_unique<ChunkLightHalo>();
			haloA->resetToAir();
			fillLightHaloFromNeighbors(*haloA, &a, nullptr, &b, nullptr, nullptr,
									   nullptr, nullptr, nullptr, nullptr);
			haloB->resetToAir();
			fillLightHaloFromNeighbors(*haloB, &b, &a, nullptr, nullptr, nullptr,
									   nullptr, nullptr, nullptr, nullptr);
			MeshBuildResult *ra = buildWithHalo(a, *haloA);
			MeshBuildResult *rb = buildWithHalo(b, *haloB);
			const int bRed = maxNibble(*rb, vBlockR, 0, 4 * 16, kTunYLo, kTunYHi, kTunZLo, kTunZHi);
			const int aBlue = maxNibble(*ra, vBlockB, 14 * 16, 15 * 16, kTunYLo, kTunYHi, kTunZLo, kTunZHi);
			CHECK(bRed > 0, "rgb overlap: redstone light from A crosses into B near the border");
			CHECK(aBlue > 0, "rgb overlap: lapis blue light from B crosses westward into A near the border");
			if (std::getenv("FT_RGB_DEBUG"))
				std::cerr << "[rgb] overlap bRed=" << bRed << " aBlue=" << aBlue << std::endl;
			pool.release(ra);
			pool.release(rb);
		}

		// (d) 4-chunk corner: LAVA in the diagonal chunk lights the center
		// chunk's (maxX, maxZ) corner through the SE/NE ring quadrant. The
		// helpers sit on the +z side, so the engine's naming calls them
		// north/northEast (south = -z, see the section header note).
		{
			// Sealed stone shell + air pocket at one chunk corner.
			auto carveCorner = [](Chunk &chunk, int shellXLo, int shellXHi,
								  int shellZLo, int shellZHi,
								  int airXLo, int airXHi, int airZLo, int airZHi)
			{
				for (int x = shellXLo; x <= shellXHi; ++x)
					for (int z = shellZLo; z <= shellZHi; ++z)
						for (int y = 38; y <= 42; ++y)
							chunk.setVoxel(x, y, z, STONE);
				for (int x = airXLo; x <= airXHi; ++x)
					for (int z = airZLo; z <= airZHi; ++z)
						for (int y = 39; y <= 41; ++y)
							chunk.setVoxel(x, y, z, AIR);
			};
			Chunk cc(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
			Chunk ec(glm::vec3(float(CHUNK_SIZE), 0.0f, 0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
			Chunk nc(glm::vec3(0.0f, 0.0f, float(CHUNK_SIZE)), ChunkState::UNLOADED, nullptr, nullptr, &pool);
			Chunk dc(glm::vec3(float(CHUNK_SIZE), 0.0f, float(CHUNK_SIZE)), ChunkState::UNLOADED,
					 nullptr, nullptr, &pool);
			bool prepared = true;
			for (Chunk *ch : {&cc, &ec, &nc, &dc})
			{
				CHECK(ch->prepareVoxelStorageForGeneration(), "rgb corner: prepare");
				ch->generateTerrain(gen);
				prepared = prepared && ch->getState() == ChunkState::GENERATED;
			}
			CHECK(prepared, "rgb corner: corner chunks generated");
			carveCorner(cc, 12, 15, 12, 15, 13, 15, 13, 15); // center corner room
			carveCorner(nc, 12, 15, 0, 2, 13, 15, 0, 1);	   // gallery to the border
			carveCorner(dc, 0, 2, 0, 2, 0, 1, 0, 1);		   // pocket around the lava
			dc.setVoxel(1, 40, 1, LAVA); // center-relative (17,40,17), Manhattan 4

			auto haloC = std::make_unique<ChunkLightHalo>();
			haloC->resetToAir();
			fillLightHaloFromNeighbors(*haloC, &cc, nullptr, &ec, nullptr, &nc,
									   nullptr, nullptr, nullptr, &dc);
			MeshBuildResult *rc = buildWithHalo(cc, *haloC);
			int cornerCount = 0;
			const int cornerR = maxNibble(*rc, vBlockR, 12 * 16, 16 * 16,
										  38 * 16, 43 * 16, 12 * 16, 16 * 16, &cornerCount);
			CHECK(cornerCount > 0, "rgb corner: C has geometry in the corner window");
			CHECK(cornerR > 0, "rgb corner: diagonal lava light crosses into C's corner geometry");
			if (std::getenv("FT_RGB_DEBUG"))
				std::cerr << "[rgb] corner cornerMaxR=" << cornerR << std::endl;
			pool.release(rc);
		}

		// (e) Swapped-source order independence: the same two cross-border
		// sources (LAVA in A's gallery, REDSTONE_ORE in B's) placed and built
		// in opposite orders in two fresh same-seed worlds. Each build is a
		// pure function of its chunk + halo, so both payloads must match
		// byte for byte regardless of placement and build order.
		{
			const glm::ivec3 lavaCellA(11, 40, 8); // A-local, in the gallery
			const glm::ivec3 oreCellB(2, 40, 8);   // B-local (world x=18)
			auto buildOrderWorld = [&](bool lavaPlacedFirst, MeshBuildResult **outA,
									   MeshBuildResult **outB)
			{
				Chunk a(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
				Chunk b(glm::vec3(float(CHUNK_SIZE), 0.0f, 0.0f), ChunkState::UNLOADED,
						nullptr, nullptr, &pool);
				CHECK(a.prepareVoxelStorageForGeneration(), "rgb order: prepare A");
				CHECK(b.prepareVoxelStorageForGeneration(), "rgb order: prepare B");
				a.generateTerrain(gen);
				b.generateTerrain(gen);
				carveTunnel(a, b);
				if (lavaPlacedFirst)
				{
					a.setVoxel(lavaCellA.x, lavaCellA.y, lavaCellA.z, LAVA);
					b.setVoxel(oreCellB.x, oreCellB.y, oreCellB.z, REDSTONE_ORE);
				}
				else
				{
					b.setVoxel(oreCellB.x, oreCellB.y, oreCellB.z, REDSTONE_ORE);
					a.setVoxel(lavaCellA.x, lavaCellA.y, lavaCellA.z, LAVA);
				}
				auto haloA = std::make_unique<ChunkLightHalo>();
				auto haloB = std::make_unique<ChunkLightHalo>();
				haloA->resetToAir();
				fillLightHaloFromNeighbors(*haloA, &a, nullptr, &b, nullptr, nullptr,
										   nullptr, nullptr, nullptr, nullptr);
				haloB->resetToAir();
				fillLightHaloFromNeighbors(*haloB, &b, &a, nullptr, nullptr, nullptr,
										   nullptr, nullptr, nullptr, nullptr);
				if (lavaPlacedFirst)
				{
					*outA = buildWithHalo(a, *haloA);
					*outB = buildWithHalo(b, *haloB);
				}
				else
				{
					*outB = buildWithHalo(b, *haloB);
					*outA = buildWithHalo(a, *haloA);
				}
			};
			MeshBuildResult *w1a = nullptr, *w1b = nullptr, *w2a = nullptr, *w2b = nullptr;
			buildOrderWorld(true, &w1a, &w1b);
			buildOrderWorld(false, &w2a, &w2b);
			const int w1aR = maxNibble(*w1a, vBlockR, 0, 16 * 16, 0, CHUNK_HEIGHT * 16, 0, 16 * 16);
			const int w1bR = maxNibble(*w1b, vBlockR, 0, 16 * 16, 0, CHUNK_HEIGHT * 16, 0, 16 * 16);
			CHECK(w1aR > 0 && w1bR > 0,
				  "rgb order: both chunks carry block light (compare is non-vacuous)");
			bool sameA = w1a->sections.size() == w2a->sections.size();
			bool sameB = w1b->sections.size() == w2b->sections.size();
			for (size_t s = 0; s < w1a->sections.size(); ++s)
			{
				if (sameA && !(w1a->sections[s] == w2a->sections[s]))
					sameA = false;
				if (sameB && !(w1b->sections[s] == w2b->sections[s]))
					sameB = false;
			}
			CHECK(sameA, "rgb order: swapped placement/build order yields byte-identical A payloads");
			CHECK(sameB, "rgb order: swapped placement/build order yields byte-identical B payloads");
			pool.release(w1a);
			pool.release(w1b);
			pool.release(w2a);
			pool.release(w2b);
		}

		// (f) Border-edit remesh cost: a full halo build of A before and
		// after a border-column LAVA edit + halo refill. Printed with
		// FT_RGB_DEBUG (same convention as FT_PROP_DEBUG above).
		{
			Chunk a(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
			Chunk b(glm::vec3(float(CHUNK_SIZE), 0.0f, 0.0f), ChunkState::UNLOADED,
					nullptr, nullptr, &pool);
			generatePair(a, b);
			carveTunnel(a, b);
			auto haloA = std::make_unique<ChunkLightHalo>();
			auto haloB = std::make_unique<ChunkLightHalo>();
			haloA->resetToAir();
			fillLightHaloFromNeighbors(*haloA, &a, nullptr, &b, nullptr, nullptr,
									   nullptr, nullptr, nullptr, nullptr);
			haloB->resetToAir();
			fillLightHaloFromNeighbors(*haloB, &b, &a, nullptr, nullptr, nullptr,
									   nullptr, nullptr, nullptr, nullptr);
			using Clock = std::chrono::steady_clock;
			const auto t0 = Clock::now();
			MeshBuildResult *r1 = buildWithHalo(a, *haloA);
			const double beforeMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
			a.setVoxel(15, 40, 8, LAVA);
			haloA->resetToAir();
			fillLightHaloFromNeighbors(*haloA, &a, nullptr, &b, nullptr, nullptr,
									   nullptr, nullptr, nullptr, nullptr);
			const auto t1 = Clock::now();
			MeshBuildResult *r2 = buildWithHalo(a, *haloA);
			const double afterMs = std::chrono::duration<double, std::milli>(Clock::now() - t1).count();
			CHECK(totalOpaqueVertices(*r1) > 0 && totalOpaqueVertices(*r2) > 0,
				  "rgb perf: both border builds produced geometry");
			if (std::getenv("FT_RGB_DEBUG"))
				std::cerr << "[rgb perf] border-edit remesh: before=" << beforeMs
						  << " ms after=" << afterMs
						  << " ms (tunnel scene, refill excluded from both timings)"
						  << std::endl;
			pool.release(r1);
			pool.release(r2);
		}
	}

	// 26) Cross-chunk light lifecycle races (issue #141 review round 2):
	// the section mask recorded by a light edit must persist through an
	// in-transit mesh job (only the GENERATED scheduling is withheld), the
	// post-publish re-arm must reschedule MESHED chunks dirtied mid-flight,
	// chunk arrivals must invalidate DIAGONAL neighbors and arriving
	// BLOCKERS (not just emitters), and the border-edit remesh cost is
	// reported end to end (halo refill included).
	{
		MeshResultPool pool;

		// Gallery face planes in quantized units (voxel * 16) - the same
		// sealed 3x3 tunnel scene as section 25.
		constexpr int kTunYLo = 39 * 16, kTunYHi = 42 * 16;
		constexpr int kTunZLo = 7 * 16, kTunZHi = 10 * 16;

		// Sealed stone gallery along +x straddling the A|B border (section
		// 25 setup): center line y=40, z 7..9, A-local x 8..15 continuing
		// into B-local x 0..8. The shell overrides the seed content, so the
		// gallery holds no natural sources.
		auto carveTunnel = [](Chunk &a, Chunk &b)
		{
			for (int x = 7; x <= 15; ++x)
				for (int z = 6; z <= 10; ++z)
					for (int y = 38; y <= 42; ++y)
						a.setVoxel(x, y, z, STONE);
			for (int x = 0; x <= 9; ++x)
				for (int z = 6; z <= 10; ++z)
					for (int y = 38; y <= 42; ++y)
						b.setVoxel(x, y, z, STONE);
			for (int x = 8; x <= 15; ++x)
				for (int z = 7; z <= 9; ++z)
					for (int y = 39; y <= 41; ++y)
						a.setVoxel(x, y, z, AIR);
			for (int x = 0; x <= 8; ++x)
				for (int z = 7; z <= 9; ++z)
					for (int y = 39; y <= 41; ++y)
						b.setVoxel(x, y, z, AIR);
		};
		auto generatePair = [&](Chunk &a, Chunk &b)
		{
			CHECK(a.prepareVoxelStorageForGeneration(), "rgb life: prepare A");
			CHECK(b.prepareVoxelStorageForGeneration(), "rgb life: prepare B");
			a.generateTerrain(gen);
			b.generateTerrain(gen);
		};
		auto buildWithHalo = [&](Chunk &chunk, ChunkLightHalo &halo) -> MeshBuildResult *
		{
			chunk.setLightHalo(&halo);
			MeshBuildResult *r = pool.acquire();
			chunk.buildMesh(*r, chunk.meshGeneration(), chunk.meshRevision());
			pool.finishBuild(r);
			chunk.setLightHalo(nullptr);
			return r;
		};
		auto fillHalo = [](ChunkLightHalo &halo, const Chunk *center, const Chunk *west,
						   const Chunk *east, const Chunk *south, const Chunk *north,
						   const Chunk *southWest, const Chunk *southEast,
						   const Chunk *northWest, const Chunk *northEast)
		{
			halo.resetToAir();
			fillLightHaloFromNeighbors(halo, center, west, east, south, north,
									   southWest, southEast, northWest, northEast);
		};
		// Max block-light nibble among opaque vertices inside a quantized
		// position window (section 25 helper); *count receives the vertices.
		auto maxNibble = [](const MeshBuildResult &r, uint32_t (*nibble)(const Vertex &),
							int xLo, int xHi, int yLo, int yHi, int zLo, int zHi,
							int *count = nullptr) -> int
		{
			int best = 0;
			if (count)
				*count = 0;
			for (const SectionMeshPayload &s : r.sections)
				for (const Vertex &v : s.opaqueVertices)
				{
					const uint32_t qx = vQuantX(v);
					const uint32_t qy = vQuantY(v);
					const uint32_t qz = vQuantZ(v);
					if (qx < xLo || qx > xHi || qy < yLo || qy > yHi || qz < zLo || qz > zHi)
						continue;
					if (count)
						++*count;
					best = std::max<int>(best, static_cast<int>(nibble(v)));
				}
			return best;
		};

		// (A) Edit while neighbor is meshing (the P1 race): the lava edit
		// lands on A while B's mesh job owns it; the mask must persist
		// through transit, the re-arm must fire when the job completes, and
		// a halo-aware rebuild must carry the new light across the border.
		{
			ChunkPool chunkPool(8);
			ChunkManager manager(&gen, nullptr, &chunkPool);
			Chunk *a = chunkPool.acquire(glm::vec3(0.0f));
			Chunk *b = chunkPool.acquire(glm::vec3(float(CHUNK_SIZE), 0.0f, 0.0f));
			CHECK(a != nullptr && b != nullptr, "rgb life: pool acquisition");
			if (a && b)
			{
				// m_chunks keys are CHUNK INDICES (section 25b): world x 0/16.
				ChunkManagerProbe::registerChunk(manager, glm::ivec3(0, 0, 0), a);
				ChunkManagerProbe::registerChunk(manager, glm::ivec3(1, 0, 0), b);
				CHECK(manager.prepareAndGenerateChunk(a, gen) &&
						  manager.prepareAndGenerateChunk(b, gen),
					  "rgb life: generate A and B");
				carveTunnel(*a, *b);
				CHECK(a->generateMesh() && b->generateMesh(),
					  "rgb life: initial meshes published");
				CHECK(b->getState() == ChunkState::MESHED,
					  "rgb life: B has a published mesh (MESHED) before the race");
				b->takeDirtySections(); // the race mask must be attributable

				// The in-flight job's PRE-edit halo snapshot, filled before
				// the edit lands (that is the point of the race).
				auto haloPre = std::make_unique<ChunkLightHalo>();
				fillHalo(*haloPre, b, a, nullptr, nullptr, nullptr,
						 nullptr, nullptr, nullptr, nullptr);

				// B's mesh job owns the chunk; the border-column LAVA lands
				// on A in the middle of the build.
				b->setInTransit(true);
				CHECK(manager.placeVoxel(glm::vec3(15.0f, 40.0f, 8.0f), LAVA),
					  "rgb life: border-column LAVA placement accepted");
				CHECK(b->dirtySections() != 0,
					  "rgb life A: dirty mask persists DESPITE the neighbor being in transit");
				CHECK(b->getState() == ChunkState::MESHED,
					  "rgb life A: scheduling withheld in transit (B still MESHED)");
				CHECK(ChunkManagerProbe::pendingEdits(manager) == 1,
					  "rgb life A: the x=15 mirror edit deferred with the in-transit neighbor");

				// The job finishes with the stale pre-edit halo.
				MeshBuildResult *stale = pool.acquire();
				b->setLightHalo(haloPre.get());
				b->buildMesh(*stale, b->meshGeneration(), b->meshRevision());
				pool.finishBuild(stale);
				b->setLightHalo(nullptr);
				const int staleR = maxNibble(*stale, vBlockR, 0, 16, kTunYLo, kTunYHi,
											 kTunZLo, kTunZHi);
				CHECK(staleR == 0,
					  "rgb life A: the in-flight mesh really predates the edit (no crossing light)");
				ChunkManagerProbe::injectCompletedMeshJob(manager, b, stale);
				manager.processFinishedJobs();
				CHECK(!b->isInTransit(),
					  "rgb life A: in-transit cleared with the finished job");
				CHECK(b->getState() == ChunkState::GENERATED,
					  "rgb life A: chunk dirtied mid-flight re-armed GENERATED after its job completed");
				CHECK(ChunkManagerProbe::pendingEdits(manager) == 0,
					  "rgb life A: the deferred mirror edit applied with the finished job");

				// Publish-branch isolation: the x=15 edit above goes through
				// the superseded path (its mirror was queued), so redo the
				// race with an x=14 edit that queues nothing - the re-arm
				// decision then runs on a cleanly PUBLISHED result.
				b->takeDirtySections();
				auto haloPre2 = std::make_unique<ChunkLightHalo>();
				fillHalo(*haloPre2, b, a, nullptr, nullptr, nullptr,
						 nullptr, nullptr, nullptr, nullptr);
				b->setInTransit(true);
				CHECK(manager.placeVoxel(glm::vec3(14.0f, 41.0f, 8.0f), LAVA),
					  "rgb life: inner LAVA placement accepted");
				CHECK(b->dirtySections() != 0,
					  "rgb life B: mask persists through the second in-transit window");
				CHECK(b->getState() == ChunkState::GENERATED,
					  "rgb life B: B is GENERATED entering the window (re-armed by race A); "
					  "the queued publish must restore MESHED before the re-arm decision");
				MeshBuildResult *stale2 = pool.acquire();
				b->setLightHalo(haloPre2.get());
				b->buildMesh(*stale2, b->meshGeneration(), b->meshRevision());
				pool.finishBuild(stale2);
				b->setLightHalo(nullptr);
				ChunkManagerProbe::injectCompletedMeshJob(manager, b, stale2);
				manager.processFinishedJobs();
				CHECK(!b->isInTransit(), "rgb life B: in-transit cleared");
				CHECK(b->getState() == ChunkState::GENERATED,
					  "rgb life B: post-publish re-arm fires for a published MESHED chunk dirtied mid-flight");
				CHECK(b->dirtySections() != 0,
					  "rgb life B: publish does not drop the transit-armed invalidation");

				// End to end: rebuild B from a halo filled with the EDITED A.
				b->takeDirtySections();
				auto haloPost = std::make_unique<ChunkLightHalo>();
				fillHalo(*haloPost, b, a, nullptr, nullptr, nullptr,
						 nullptr, nullptr, nullptr, nullptr);
				MeshBuildResult *fresh = buildWithHalo(*b, *haloPost);
				const int nearR = maxNibble(*fresh, vBlockR, 0, 16, kTunYLo, kTunYHi,
											kTunZLo, kTunZHi);
				CHECK(nearR > 0,
					  "rgb life: the lost-invalidation hole is closed end to end (border light present)");
				CHECK(nearR >= 10,
					  "rgb life: border-adjacent B vertices carry lava brightness (>= 10)");
				if (std::getenv("FT_RGB_DEBUG"))
					std::cerr << "[rgb] race nearR=" << nearR
							  << " staleR=" << staleR << std::endl;
				ChunkManagerProbe::injectCompletedMeshJob(manager, b, fresh);
				manager.processFinishedJobs();
				CHECK(b->getState() == ChunkState::MESHED,
					  "rgb life: clean republish stays MESHED (no spurious re-arm at zero mask)");
			}
		}

		// (B) Diagonal arrival: D (NE diagonal) generates AFTER C was meshed
		// with a northEast=null halo. The arrival path must dirty C through
		// the diagonal (previously only N/S/E/W were covered), and the
		// rebuilt C must carry the diagonal lava light in its corner.
		{
			ChunkPool chunkPool(8);
			ChunkManager manager(&gen, nullptr, &chunkPool);
			Chunk *cc = chunkPool.acquire(glm::vec3(0.0f));
			Chunk *dc = chunkPool.acquire(glm::vec3(float(CHUNK_SIZE), 0.0f, float(CHUNK_SIZE)));
			CHECK(cc != nullptr && dc != nullptr, "rgb diag: pool acquisition");
			if (cc && dc)
			{
				// Sealed stone shell + air pocket at one chunk corner
				// (section 25d geometry).
				auto carveCorner = [](Chunk &chunk, int shellXLo, int shellXHi,
									  int shellZLo, int shellZHi,
									  int airXLo, int airXHi, int airZLo, int airZHi)
				{
					for (int x = shellXLo; x <= shellXHi; ++x)
						for (int z = shellZLo; z <= shellZHi; ++z)
							for (int y = 38; y <= 42; ++y)
								chunk.setVoxel(x, y, z, STONE);
					for (int x = airXLo; x <= airXHi; ++x)
						for (int z = airZLo; z <= airZHi; ++z)
							for (int y = 39; y <= 41; ++y)
								chunk.setVoxel(x, y, z, AIR);
				};

				ChunkManagerProbe::registerChunk(manager, glm::ivec3(0, 0, 0), cc);
				CHECK(manager.prepareAndGenerateChunk(cc, gen), "rgb diag: generate C");
				// Shell spans 10..15 so the scan window (x,z >= 12) sits
				// strictly inside deterministic stone (no seed ore noise).
				carveCorner(*cc, 10, 15, 10, 15, 13, 15, 13, 15); // corner room
				CHECK(cc->generateMesh(), "rgb diag: C meshed before the arrival");
				CHECK(cc->getState() == ChunkState::MESHED, "rgb diag: C MESHED");

				// Pre-arrival build: NE halo null -> the corner is dark.
				auto haloPre = std::make_unique<ChunkLightHalo>();
				fillHalo(*haloPre, cc, nullptr, nullptr, nullptr, nullptr,
						 nullptr, nullptr, nullptr, nullptr);
				MeshBuildResult *pre = buildWithHalo(*cc, *haloPre);
				int preCount = 0;
				const int preR = maxNibble(*pre, vBlockR, 12 * 16, 16 * 16,
										   38 * 16, 43 * 16, 12 * 16, 16 * 16, &preCount);
				CHECK(preCount > 0, "rgb diag: corner window has geometry pre-arrival");
				CHECK(preR == 0, "rgb diag: a northEast=null halo leaves the corner dark");
				pool.release(pre);

				// Generate D out-of-band, land it through the completion
				// queue (the real arrival path).
				CHECK(dc->prepareVoxelStorageForGeneration(), "rgb diag: prepare D");
				dc->generateTerrain(gen);
				carveCorner(*dc, 0, 2, 0, 2, 0, 1, 0, 1); // pocket around the lava
				dc->setVoxel(1, 40, 1, LAVA); // corner nearest C (world 17,40,17)
				CHECK(!dc->isInTransit(), "rgb diag: D not in transit at the arrival");
				ChunkManagerProbe::registerChunk(manager, glm::ivec3(1, 0, 1), dc);
				cc->takeDirtySections(); // arrival must own the dirty mask
				ChunkManagerProbe::injectCompletedGenChunk(manager, dc);
				manager.processFinishedJobs();

				CHECK(cc->dirtySections() != 0,
					  "rgb diag: DIAGONAL arrival dirties the center chunk");
				CHECK(cc->getState() == ChunkState::GENERATED,
					  "rgb diag: dirtied center chunk re-armed GENERATED");

				// Rebuild with the arrival in the ring: the corner lights up.
				auto haloPost = std::make_unique<ChunkLightHalo>();
				fillHalo(*haloPost, cc, nullptr, nullptr, nullptr, nullptr,
						 nullptr, nullptr, nullptr, dc);
				MeshBuildResult *post = buildWithHalo(*cc, *haloPost);
				int postCount = 0;
				const int postR = maxNibble(*post, vBlockR, 12 * 16, 16 * 16,
											38 * 16, 43 * 16, 12 * 16, 16 * 16, &postCount);
				CHECK(postCount > 0, "rgb diag: corner window has geometry post-arrival");
				CHECK(postR > 0,
					  "rgb diag: diagonal lava lights C's corner after the arrival rebuild");
				if (std::getenv("FT_RGB_DEBUG"))
					std::cerr << "[rgb] diag corner preR=" << preR
							  << " postR=" << postR << std::endl;
				pool.release(post);
			}
		}

		// (C) Opaque arrival: D carries a solid STONE corner band and NO
		// emitter - the arriving BLOCKERS alone must change the BFS result
		// in C. C's lava reaches a sealed corner pocket ONLY by detouring
		// through the NE halo region; D's arrival seals that detour.
		{
			ChunkPool chunkPool(8);
			ChunkManager manager(&gen, nullptr, &chunkPool);
			Chunk *cc = chunkPool.acquire(glm::vec3(0.0f));
			Chunk *dc = chunkPool.acquire(glm::vec3(float(CHUNK_SIZE), 0.0f, float(CHUNK_SIZE)));
			CHECK(cc != nullptr && dc != nullptr, "rgb block: pool acquisition");
			if (cc && dc)
			{
				// Tuned scene coordinates (C-local) and the load-bearing
				// light path:
				//   stone shell x 11..15, z 11..15, y 38..42
				//   lava room   x 12..13, z 14..15, y 39..41, LAVA (12,40,14)
				//   pocket      x 15, z 13, y 39..41 - sealed in-chunk:
				//   (14,y,13), (15,y,12) and (15,y,14) are all shell stone.
				// Light exits north at (12..13,y,16), crosses the NE halo
				// hinge (16,y,16) [D-local (0,y,0)], runs back west along
				// the east strip (16,y,15..13) and enters at (15,y,13):
				// Manhattan path length 10 into (15,40,13). North-strip ->
				// east-strip adjacency ONLY exists through (15,y,16)-
				// (16,y,16), so D's stone corner (D-local 0..3, 38..42,
				// 0..3 = halo 16..19, 38..42, 16..19) overwrites the hinge
				// and every alternative route is >= 18 steps (dark).
				ChunkManagerProbe::registerChunk(manager, glm::ivec3(0, 0, 0), cc);
				CHECK(manager.prepareAndGenerateChunk(cc, gen), "rgb block: generate C");
				for (int x = 11; x <= 15; ++x)
					for (int z = 11; z <= 15; ++z)
						for (int y = 38; y <= 42; ++y)
							cc->setVoxel(x, y, z, STONE);
				for (int x = 12; x <= 13; ++x)
					for (int z = 14; z <= 15; ++z)
						for (int y = 39; y <= 41; ++y)
							cc->setVoxel(x, y, z, AIR);
				cc->setVoxel(12, 40, 14, LAVA);
				for (int y = 39; y <= 41; ++y)
					cc->setVoxel(15, y, 13, AIR); // the sealed corner pocket

				CHECK(cc->generateMesh(), "rgb block: C meshed");
				cc->takeDirtySections();

				// Pre-arrival: NE halo null -> the whole ring is AIR and the
				// detour lights the pocket.
				auto haloPre = std::make_unique<ChunkLightHalo>();
				fillHalo(*haloPre, cc, nullptr, nullptr, nullptr, nullptr,
						 nullptr, nullptr, nullptr, nullptr);
				MeshBuildResult *pre = buildWithHalo(*cc, *haloPre);
				int preCount = 0;
				const int preR = maxNibble(*pre, vBlockR, 15 * 16, 15 * 16,
										   38 * 16, 42 * 16, 13 * 16, 14 * 16, &preCount);
				CHECK(preCount > 0, "rgb block: pocket window has geometry pre-arrival");
				CHECK(preR > 0,
					  "rgb block: pocket lit pre-arrival only through the NE halo detour");
				pool.release(pre);

				// D: generated terrain + stone corner band, NO emitters.
				CHECK(dc->prepareVoxelStorageForGeneration(), "rgb block: prepare D");
				dc->generateTerrain(gen);
				for (int x = 0; x <= 3; ++x)
					for (int z = 0; z <= 3; ++z)
						for (int y = 38; y <= 42; ++y)
							dc->setVoxel(x, y, z, STONE);
				ChunkManagerProbe::registerChunk(manager, glm::ivec3(1, 0, 1), dc);
				CHECK(!dc->isInTransit(), "rgb block: D not in transit at the arrival");
				cc->takeDirtySections();
				ChunkManagerProbe::injectCompletedGenChunk(manager, dc);
				manager.processFinishedJobs();

				CHECK(cc->dirtySections() != 0,
					  "rgb block: BLOCKER-only arrival dirties the center chunk");
				CHECK(cc->getState() == ChunkState::GENERATED,
					  "rgb block: dirtied center chunk re-armed GENERATED");

				// Rebuild with D in the ring: the detour is sealed.
				auto haloPost = std::make_unique<ChunkLightHalo>();
				fillHalo(*haloPost, cc, nullptr, nullptr, nullptr, nullptr,
						 nullptr, nullptr, nullptr, dc);
				MeshBuildResult *post = buildWithHalo(*cc, *haloPost);
				int postCount = 0;
				const int postR = maxNibble(*post, vBlockR, 15 * 16, 15 * 16,
											38 * 16, 42 * 16, 13 * 16, 14 * 16, &postCount);
				CHECK(postCount > 0, "rgb block: pocket window has geometry post-arrival");
				CHECK(postR < preR,
					  "rgb block: sealing the detour strictly darkens the pocket");
				if (std::getenv("FT_RGB_DEBUG"))
					std::cerr << "[rgb] blocker-arrival pocket preR=" << preR
							  << " postR=" << postR << std::endl;
				pool.release(post);
			}
		}

		// (D) Border-edit remesh cost: keeps the refill-excluded build
		// timing of section 25f and adds the END-TO-END remesh (halo
		// refill + buildMesh), which is what an edit actually costs.
		{
			Chunk a(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
			Chunk b(glm::vec3(float(CHUNK_SIZE), 0.0f, 0.0f), ChunkState::UNLOADED,
					nullptr, nullptr, &pool);
			generatePair(a, b);
			carveTunnel(a, b);
			auto haloA = std::make_unique<ChunkLightHalo>();
			fillHalo(*haloA, &a, nullptr, &b, nullptr, nullptr,
					 nullptr, nullptr, nullptr, nullptr);
			using Clock = std::chrono::steady_clock;
			const auto t0 = Clock::now();
			MeshBuildResult *r1 = buildWithHalo(a, *haloA);
			const double beforeMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
			a.setVoxel(15, 40, 8, LAVA);
			haloA->resetToAir();
			const auto t1 = Clock::now();
			fillLightHaloFromNeighbors(*haloA, &a, nullptr, &b, nullptr, nullptr,
									   nullptr, nullptr, nullptr, nullptr);
			const double refillMs = std::chrono::duration<double, std::milli>(Clock::now() - t1).count();
			const auto t2 = Clock::now();
			MeshBuildResult *r2 = buildWithHalo(a, *haloA);
			const double afterMs = std::chrono::duration<double, std::milli>(Clock::now() - t2).count();
			CHECK(totalOpaqueVertices(*r1) > 0 && totalOpaqueVertices(*r2) > 0,
				  "rgb perf: both border builds produced geometry");
			if (std::getenv("FT_RGB_DEBUG"))
				std::cerr << "[rgb perf] border-edit remesh: before=" << beforeMs
						  << " ms after=" << afterMs
						  << " ms endtoend=" << (refillMs + afterMs)
						  << " ms (halo snapshot included)" << std::endl;
			pool.release(r1);
			pool.release(r2);
		}
	}

	// 27) Concurrent batch dispatch sees stable neighbors (issue #141 review
	// round 3): halo readability in fillLightHaloFromNeighbors is the
	// voxel-backing contract ONLY (state != UNLOADED). A neighbor in transit
	// for a MESH job has stable, immutable voxels (edits targeting it are
	// deferred) and MUST be read: two adjacent chunks dispatched in one
	// meshPendingChunks batch would otherwise give the second halo snapshot
	// an AIR view of the first - an asymmetric seam no future edit or
	// arrival would ever invalidate. ChunkManager::meshPendingChunks also
	// skips a dispatch (retry later) when the halo pool allocation fails
	// instead of building without a halo.
	{
		MeshResultPool pool;

		// Pool-first member order: the manager (which drains its jobs in its
		// destructor) is destroyed before the pool it borrows chunks from.
		// Static member carveTunnel because a local class's member functions
		// cannot name block-scope lambdas (MSVC C2326).
		struct BatchWorld
		{
			// Sealed 3x3 air gallery along +x straddling the A|B border (the
			// section 25/26 scene): center line y=40, z 7..9, A-local x 8..15
			// continuing into B-local x 0..8. The stone shell overrides the
			// seed content, so every asserted cell below is deterministic.
			static void carveTunnel(Chunk &a, Chunk &b)
			{
				for (int x = 7; x <= 15; ++x)
					for (int z = 6; z <= 10; ++z)
						for (int y = 38; y <= 42; ++y)
							a.setVoxel(x, y, z, STONE);
				for (int x = 0; x <= 9; ++x)
					for (int z = 6; z <= 10; ++z)
						for (int y = 38; y <= 42; ++y)
							b.setVoxel(x, y, z, STONE);
				for (int x = 8; x <= 15; ++x)
					for (int z = 7; z <= 9; ++z)
						for (int y = 39; y <= 41; ++y)
							a.setVoxel(x, y, z, AIR);
				for (int x = 0; x <= 8; ++x)
					for (int z = 7; z <= 9; ++z)
						for (int y = 39; y <= 41; ++y)
							b.setVoxel(x, y, z, AIR);
			}

			ChunkPool chunkPool;
			ChunkManager manager;
			Chunk *a{nullptr};
			Chunk *b{nullptr};
			BatchWorld(TerrainGenerator &genRef, ThreadPool &tp)
				: chunkPool(8), manager(&genRef, &tp, &chunkPool)
			{
				a = chunkPool.acquire(glm::vec3(0.0f));
				b = chunkPool.acquire(glm::vec3(float(CHUNK_SIZE), 0.0f, 0.0f));
				if (!a || !b)
				{
					a = b = nullptr;
					return;
				}
				// m_chunks keys are CHUNK INDICES, not world positions
				// (section 25b): world x 0/16 -> indices 0/+1.
				ChunkManagerProbe::registerChunk(manager, glm::ivec3(0, 0, 0), a);
				ChunkManagerProbe::registerChunk(manager, glm::ivec3(1, 0, 0), b);
				if (!manager.prepareAndGenerateChunk(a, genRef) ||
					!manager.prepareAndGenerateChunk(b, genRef))
				{
					a = b = nullptr; // failed prepare releases back to the pool
					return;
				}
				carveTunnel(*a, *b);
				a->setVoxel(14, 40, 8, LAVA);
			}
		};

		// (A) Minimal targeted check - the exact review scenario: B
		// snapshots its halo while A's mesh job owns A (in transit).
		{
			Chunk a(glm::vec3(0.0f), ChunkState::UNLOADED, nullptr, nullptr, &pool);
			Chunk b(glm::vec3(float(CHUNK_SIZE), 0.0f, 0.0f), ChunkState::UNLOADED,
					nullptr, nullptr, &pool);
			const bool ok = a.prepareVoxelStorageForGeneration() &&
							b.prepareVoxelStorageForGeneration();
			CHECK(ok, "rgb batch: prepared the transit-scene pair");
			if (ok)
			{
				a.generateTerrain(gen);
				b.generateTerrain(gen);
				BatchWorld::carveTunnel(a, b);
				a.setVoxel(14, 40, 8, LAVA); // A-local border-near column (B-relative x = -2)

				auto haloB = std::make_unique<ChunkLightHalo>();
				haloB->resetToAir();
				a.setInTransit(true); // A's mesh job in flight: voxels stable, edits deferred
				fillLightHaloFromNeighbors(*haloB, &b, &a, nullptr, nullptr, nullptr,
										   nullptr, nullptr, nullptr, nullptr);
				const bool bSeesLava = haloB->voxelAt(-2, 40, 8) == static_cast<uint8_t>(LAVA);
				const bool bSeesWall = haloB->voxelAt(-2, 38, 8) == static_cast<uint8_t>(STONE);
				const size_t bEmissives = haloB->emissives.size();
				CHECK(bSeesLava,
					  "rgb batch: B's halo carries A's border LAVA while A is in transit (not AIR)");
				CHECK(!haloB->emissives.empty(),
					  "rgb batch: B's halo records A's emissives while A is in transit");
				CHECK(bSeesWall,
					  "rgb batch: B's halo carries A's tunnel wall STONE while A is in transit");
				// Clearing the flag changes nothing: the neighbor stays
				// readable and an identical refill is possible.
				a.setInTransit(false);
				haloB->resetToAir();
				fillLightHaloFromNeighbors(*haloB, &b, &a, nullptr, nullptr, nullptr,
										   nullptr, nullptr, nullptr, nullptr);
				CHECK(haloB->voxelAt(-2, 40, 8) == static_cast<uint8_t>(LAVA) &&
						  haloB->voxelAt(-2, 38, 8) == static_cast<uint8_t>(STONE) &&
						  !haloB->emissives.empty(),
					  "rgb batch: clearing in-transit keeps the neighbor readable (identical refill)");
				if (std::getenv("FT_RGB_DEBUG"))
					std::cerr << "[rgb] batch transit: bHaloLava=" << bSeesLava
					          << " bHaloWall=" << bSeesWall
					          << " bHaloEmissives=" << bEmissives << std::endl;
			}
		}

		// (B) Real scheduler batch: ONE meshPendingChunks call dispatches
		// both adjacent chunks (budget 2). Halos are filled synchronously at
		// dispatch on the calling thread, so the post-dispatch halo
		// inspection below is deterministic; the worker builds then run
		// concurrently. The scene is rebuilt twice with reversed dispatch
		// orders (camera nearer A, then nearer B) and the PUBLISHED worker
		// payloads must match byte for byte: batch order must not decide
		// who sees whom.
		{
			ThreadPool pool2(2);
			RenderSettings settings; // defaults: both chunks far inside the full-quality band
			constexpr int kTunYLo = 39 * 16, kTunYHi = 42 * 16;
			constexpr int kTunZLo = 7 * 16, kTunZHi = 10 * 16;
			// Max block-light nibble among opaque vertices inside a quantized
			// position window (section 25 helper); *count receives vertices.
			auto maxNibble = [](const MeshBuildResult &r, uint32_t (*nibble)(const Vertex &),
								int xLo, int xHi, int yLo, int yHi, int zLo, int zHi,
								int *count = nullptr) -> int
			{
				int best = 0;
				if (count)
					*count = 0;
				for (const SectionMeshPayload &s : r.sections)
					for (const Vertex &v : s.opaqueVertices)
					{
						const uint32_t qx = vQuantX(v);
						const uint32_t qy = vQuantY(v);
						const uint32_t qz = vQuantZ(v);
						if (qx < xLo || qx > xHi || qy < yLo || qy > yHi || qz < zLo || qz > zHi)
							continue;
						if (count)
							++*count;
						best = std::max<int>(best, static_cast<int>(nibble(v)));
					}
				return best;
			};

			// Order 1: A is nearer the dispatch camera -> A dispatches first
			// (queue is distance-sorted; camOffset x = 4-8 = -4: A distSq 16,
			// B 400). Order 2: B is nearer -> B dispatches first, so A's halo
			// is snapshotted while B is ALREADY in transit (the round-3 case;
			// in order 1 it is B who sees an in-transit neighbor).
			BatchWorld w1(gen, pool2);
			const Camera camA(glm::vec3(4.f, 100.f, 8.f));
			BatchWorld w2(gen, pool2);
			const Camera camB(glm::vec3(20.f, 100.f, 8.f));

			const bool worldsOk = w1.a && w1.b && w2.a && w2.b;
			CHECK(worldsOk, "rgb batch: both scheduler worlds generated");
			if (worldsOk)
			{
				// Pin both workers with LOW-priority spinners BEFORE the
				// dispatch: near chunks enqueue as HIGH, so priority order
				// guarantees no mesh job can start (let alone finish and
				// detach its halo) while the gate is closed - the inspection
				// is race-free by construction. The 2 s deadlines only guard
				// against a hang if a worker never arrives.
				std::atomic<int> gate{0};
				std::atomic<int> gated{0};
				auto gateTask = [&gate, &gated]
				{
					gated.fetch_add(1, std::memory_order_release);
					const auto deadline =
						std::chrono::steady_clock::now() + std::chrono::seconds(2);
					while (gate.load(std::memory_order_acquire) == 0 &&
						   std::chrono::steady_clock::now() < deadline)
						std::this_thread::yield();
				};
				pool2.enqueue(TaskPriority::Low, gateTask);
				pool2.enqueue(TaskPriority::Low, gateTask);
				const auto pinDeadline =
					std::chrono::steady_clock::now() + std::chrono::seconds(2);
				while (gated.load(std::memory_order_acquire) < 2 &&
					   std::chrono::steady_clock::now() < pinDeadline)
					std::this_thread::yield();
				CHECK(gated.load() == 2,
					  "rgb batch: both workers pinned before the batch dispatch");

				auto inspectBatch = [&](BatchWorld &w, const Camera &cam, const char *label)
				{
					const std::string prefix = std::string("rgb batch ") + label;
					w.manager.meshPendingChunks(cam, settings, 2); // ONE batch, both chunks
					CHECK(w.manager.pendingMeshJobs() == 2,
						  prefix + ": both jobs dispatched in one batch");
					CHECK(w.a->isInTransit() && w.b->isInTransit(),
						  prefix + ": batch dispatch flagged both chunks in transit");
					ChunkLightHalo *ha = w.a->lightHalo();
					ChunkLightHalo *hb = w.b->lightHalo();
					CHECK(ha != nullptr && hb != nullptr,
						  prefix + ": dispatch attached a halo to each chunk");
					if (ha && hb)
					{
						// Each halo saw its neighbor: B carries A's LAVA
						// column + emissives across the seam (x = -2), A
						// carries B's carved wall/gallery in the +x ring
						// (x = 17).
						const bool bSeesLava =
							hb->voxelAt(-2, 40, 8) == static_cast<uint8_t>(LAVA);
						const bool bHasEmissives = !hb->emissives.empty();
						const bool aSeesB =
							ha->voxelAt(17, 38, 8) == static_cast<uint8_t>(STONE) &&
							ha->voxelAt(17, 40, 8) == static_cast<uint8_t>(AIR);
						CHECK(bSeesLava, prefix + ": B's batch halo sees the LAVA in A across the seam");
						CHECK(bHasEmissives,
							  prefix + ": B's batch halo recorded A's emissives");
						CHECK(aSeesB, prefix + ": A's batch halo sees B's tunnel wall/gallery (not AIR)");
						if (std::getenv("FT_RGB_DEBUG"))
							std::cerr << "[rgb] batch " << label << ": aHaloSeesB=" << aSeesB
							          << " bHaloLava=" << bSeesLava
							          << " bHaloEmissives=" << hb->emissives.size()
							          << std::endl;
					}
				};
				inspectBatch(w1, camA, "A-first");
				inspectBatch(w2, camB, "B-first");
				gate.store(1, std::memory_order_release); // workers pick up the queued HIGH jobs

				auto drain = [](ChunkManager &m)
				{
					while (m.pendingMeshJobs() > 0)
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
						m.processFinishedJobs();
					}
					m.processFinishedJobs();
				};
				drain(w1.manager);
				drain(w2.manager);
				CHECK(w1.a->getState() == ChunkState::MESHED &&
						  w1.b->getState() == ChunkState::MESHED,
					  "rgb batch A-first: drained batch settles both chunks MESHED");
				CHECK(w2.a->getState() == ChunkState::MESHED &&
						  w2.b->getState() == ChunkState::MESHED,
					  "rgb batch B-first: drained batch settles both chunks MESHED");
				CHECK(w1.a->lightHalo() == nullptr && w1.b->lightHalo() == nullptr &&
						  w2.a->lightHalo() == nullptr && w2.b->lightHalo() == nullptr,
					  "rgb batch: workers detached every borrowed halo after publish");

				// Determinism on the PUBLISHED worker payloads (held until
				// GPU upload, which never runs in tests): each must equal a
				// manual rebuild with a freshly filled halo (the settled ring
				// holds the same stable voxels the dispatch snapshot saw -
				// any published seam would diverge here), and the two
				// dispatch orders must match byte for byte.
				MeshBuildResult *pubA1 = ChunkStateProbe::pendingResult(*w1.a);
				MeshBuildResult *pubB1 = ChunkStateProbe::pendingResult(*w1.b);
				MeshBuildResult *pubA2 = ChunkStateProbe::pendingResult(*w2.a);
				MeshBuildResult *pubB2 = ChunkStateProbe::pendingResult(*w2.b);
				CHECK(pubA1 && pubB1 && pubA2 && pubB2,
					  "rgb batch: every drained job published its payload");
				if (pubA1 && pubB1 && pubA2 && pubB2)
				{
					// Manual reference rebuild (section 25 pattern): pool
					// acquire + buildMesh with a freshly filled halo. A is
					// the west chunk, B the east one.
					auto canonical = [&](Chunk &chunk, Chunk &neighbor,
										 bool neighborEast) -> MeshBuildResult *
					{
						auto halo = std::make_unique<ChunkLightHalo>();
						halo->resetToAir();
						if (neighborEast)
							fillLightHaloFromNeighbors(*halo, &chunk, nullptr, &neighbor,
													   nullptr, nullptr, nullptr, nullptr,
													   nullptr, nullptr);
						else
							fillLightHaloFromNeighbors(*halo, &chunk, &neighbor, nullptr,
													   nullptr, nullptr, nullptr, nullptr,
													   nullptr, nullptr);
						chunk.setLightHalo(halo.get());
						MeshBuildResult *r = pool.acquire();
						chunk.buildMesh(*r, chunk.meshGeneration(), chunk.meshRevision());
						pool.finishBuild(r);
						chunk.setLightHalo(nullptr);
						return r;
					};
					auto sectionsEqual = [](const MeshBuildResult *x, const MeshBuildResult *y)
					{
						if (!x || !y || x->sections.size() != y->sections.size())
							return false;
						for (size_t s = 0; s < x->sections.size(); ++s)
							if (!(x->sections[s] == y->sections[s]))
								return false;
						return true;
					};
					MeshBuildResult *refA1 = canonical(*w1.a, *w1.b, true);
					MeshBuildResult *refB1 = canonical(*w1.b, *w1.a, false);
					MeshBuildResult *refA2 = canonical(*w2.a, *w2.b, true);
					MeshBuildResult *refB2 = canonical(*w2.b, *w2.a, false);
					CHECK(sectionsEqual(pubA1, refA1),
						  "rgb batch A-first: published A equals the canonical fresh-halo rebuild");
					CHECK(sectionsEqual(pubB1, refB1),
						  "rgb batch A-first: published B equals the canonical fresh-halo rebuild");
					CHECK(sectionsEqual(pubA2, refA2),
						  "rgb batch B-first: published A equals the canonical fresh-halo rebuild");
					CHECK(sectionsEqual(pubB2, refB2),
						  "rgb batch B-first: published B equals the canonical fresh-halo rebuild");
					CHECK(sectionsEqual(pubA1, pubA2),
						  "rgb batch: reversed dispatch order yields byte-identical published A payloads");
					CHECK(sectionsEqual(pubB1, pubB2),
						  "rgb batch: reversed dispatch order yields byte-identical published B payloads");
					// Non-vacuous: the concurrent batch really carries the
					// crossing lava light in B's border geometry.
					const int borderR = maxNibble(*pubB1, vBlockR, 0, 16, kTunYLo, kTunYHi,
												  kTunZLo, kTunZHi);
					CHECK(borderR > 0,
						  "rgb batch: B's published mesh carries A's lava light across the border");
					if (std::getenv("FT_RGB_DEBUG"))
						std::cerr << "[rgb] batch order-swap: canonA1="
						          << sectionsEqual(pubA1, refA1)
						          << " canonB1=" << sectionsEqual(pubB1, refB1)
						          << " canonA2=" << sectionsEqual(pubA2, refA2)
						          << " canonB2=" << sectionsEqual(pubB2, refB2)
						          << " sameA=" << sectionsEqual(pubA1, pubA2)
						          << " sameB=" << sectionsEqual(pubB1, pubB2)
						          << " bBorderR=" << borderR << std::endl;
					pool.release(refA1);
					pool.release(refB1);
					pool.release(refA2);
					pool.release(refB2);
				}
			}
		}
	}

	// Physics reads canonical, published voxels independently of mesh readiness.
	{
		ChunkPool pool(32);
		TerrainGenerator generator(42);
		ChunkManager manager(&generator, nullptr, &pool);
		Camera cam({0.f, 100.f, 0.f});
		RenderSettings settings;
		manager.updateStreaming(cam, settings);
		manager.processChunkLoading(32);
		Chunk *chunk = manager.getChunkAtWorldPos({4.f, 220.f, 4.f});
		Chunk *west = manager.getChunkAtWorldPos({-1.f, 220.f, 4.f});
		CHECK(chunk && west, "physics chunks loaded");
		if (chunk && west)
		{
			{
				ChunkCollisionView view(manager);
				CHECK(!view.sample({4, 220, 4}).available, "unprepared is unknown, not air");
				CHECK(!view.sample({10000, 220, 4}).available, "missing chunk is unknown");
				CHECK(view.sample({4, -1, 4}).solid, "solid lower world boundary");
				CHECK(!view.sample({4, WORLD_HEIGHT, 4}).solid, "open sky above world");
			}
			CHECK(chunk->prepareVoxelStorageForGeneration(), "prepare collision chunk");
			{
				ChunkCollisionView view(manager);
				CHECK(!view.sample({4, 220, 4}).available, "prepared stale backing remains unknown");
			}
			// Actual generation runs concurrently with repeated query views.
			chunk->setInTransit(true);
			std::thread generation([&] { chunk->generateTerrain(generator); });
			for (int i = 0; i < 1000; ++i)
			{
				ChunkCollisionView view(manager);
				const auto sample = view.sample({4, 0, 4});
				CHECK(!sample.available || sample.solid, "generation never exposes stale bedrock bytes");
			}
			generation.join();
			chunk->setInTransit(false);
			CHECK(manager.prepareAndGenerateChunk(west, generator), "generate negative-coordinate chunk");
			chunk->setVoxel(4, 220, 4, AIR);
			west->setVoxel(15, 220, 4, GLASS);
			{
				ChunkCollisionView view(manager);
				CHECK(view.sample({4, 220, 4}).available, "generated before mesh is collidable");
				CHECK(view.sample({-1, 220, 4}).solid, "negative floor mapping and glass solidity");
			}
			chunk->setInTransit(true); // read-only remesh: existing voxels stay queryable
			CHECK(manager.placeVoxel({4.f, 220.f, 4.f}, STONE), "queue physical obstacle");
			{
				ChunkCollisionView view(manager);
				CHECK(view.sample({4, 220, 4}).solid, "pending placement blocks before apply");
				CHECK(view.sample({5, 220, 4}).available, "remeshing does not turn chunk unknown");
				physics::Body body; body.position = {2, 220, 4.5};
				physics::QueryStats stats;
				physics::move(view, body, {5, 0, 0}, stats);
				CHECK(body.bounds().max.x <= 4, "body cannot enter a pending solid");
			}
			CHECK(manager.deleteVoxel({4.f, 220.f, 4.f}), "queue logical deletion");
			{
				ChunkCollisionView view(manager);
				CHECK(!view.sample({4, 220, 4}).solid, "coalesced pending deletion removes collision");
			}
			CHECK(manager.placeVoxel({4.f, 220.f, 4.f}, STONE), "replace pending deletion");
			chunk->setInTransit(false);
			manager.processFinishedJobs();
			{
				ChunkCollisionView view(manager);
				CHECK(view.sample({4, 220, 4}).solid, "applied collision matches logical state");
			}
			chunk->setVoxel(4, 220, 4, AIR);
			chunk->setInTransit(true);
			CHECK(manager.placeVoxel({4.f, 220.f, 4.f}, STONE), "queue before recycle");
			chunk->setInTransit(false);
			chunk->reset(chunk->getPosition(), Chunk::ResetMode::ForGeneration);
			CHECK(chunk->prepareVoxelStorageForGeneration(), "prepare new incarnation");
			chunk->generateTerrain(generator);
			chunk->setVoxel(4, 220, 4, AIR);
			{
				ChunkCollisionView view(manager);
				CHECK(!view.sample({4, 220, 4}).solid, "stale queued solid ignored after recycling");
			}
			manager.processFinishedJobs();
		}
		CHECK(physics::blockCell(OAK_LEAVES).solid, "foliage remains a solid cube");
		CHECK(physics::blockCell(ICE).solid, "ice remains solid");
		CHECK(physics::blockCell(KELP).medium == physics::Medium::Water, "kelp preserves aquatic medium");
		CHECK(physics::blockCell(KELP_TOP).medium == physics::Medium::Water, "kelp top preserves aquatic medium");
		CHECK(physics::blockCell(LAVA).medium == physics::Medium::Lava, "lava is a separate medium");
	}

	// --- Issue #128: Dynamic entity local voxel lighting sampling ----------
	{
		ChunkPool pool(32);
		TerrainGenerator generator(42);
		ChunkManager manager(&generator, nullptr, &pool);

		auto *chunk = pool.acquire(glm::vec3(0.0f));
		chunk->prepareVoxelStorageForGeneration();
		chunk->generateTerrain(generator);
		ChunkManagerProbe::registerChunk(manager, glm::ivec3(0, 0, 0), chunk);
		chunk->setLocalLightCacheWanted(true);

		// Before meshing, lightStorage is not published yet: sampleVoxelLight fallback
		const auto preMeshLight = manager.sampleVoxelLight({8, 100, 8});
		CHECK(preMeshLight.skylight == 0.0f && preMeshLight.blockRgb == glm::vec3(0.0f),
		      "unmeshed chunk returns zero fallback");

		// Build and publish mesh so chunk->hasLightStorage() becomes true
		CHECK(chunk->generateMesh(), "generate mesh with light storage");
		CHECK(chunk->hasLightStorage(), "chunk has published light storage");

		// 1. Point queries on meshed chunk
		const auto skyLight = manager.sampleVoxelLight({8, 200, 8});
		CHECK(skyLight.skylight == 1.0f, "open sky has full skylight (1.0)");

		// Out of world height queries:
		const auto aboveWorld = manager.sampleVoxelLight({8, 260, 8});
		CHECK(aboveWorld.skylight == 1.0f && aboveWorld.blockRgb == glm::vec3(0.0f),
		      "above world height has full skylight");

		const auto belowWorld = manager.sampleVoxelLight({8, -5, 8});
		CHECK(belowWorld.skylight == 0.0f && belowWorld.blockRgb == glm::vec3(0.0f),
		      "below world height returns darkness");

		// Unloaded chunk coordinates
		const auto missingChunk = manager.sampleVoxelLight({1000, 100, 1000});
		CHECK(missingChunk.skylight == 0.0f && missingChunk.blockRgb == glm::vec3(0.0f),
		      "missing chunk coordinate returns darkness");

		// 2. Smoothed light queries (trilinear interpolation)
		const auto smoothCenter = manager.sampleSmoothedLight(glm::vec3(8.5f, 200.5f, 8.5f));
		CHECK(smoothCenter.skylight == 1.0f, "smoothed light at center matches open sky");

		// Carve out a cave pocket and place a lava block
		for (int z = 4; z <= 12; ++z)
			for (int x = 4; x <= 12; ++x)
				chunk->setVoxel(x, 150, z, STONE);
		chunk->setVoxel(8, 140, 8, LAVA);
		CHECK(chunk->generateMesh(), "remesh with lava inside enclosed space");

		const auto lavaLight = manager.sampleVoxelLight({8, 141, 8});
		CHECK(lavaLight.blockRgb.r > 0.5f, "block light near lava has high red component");

		// Test ChunkCollisionView and ChunkMobWorld adapters (separate scopes to avoid nested shared_lock)
		lighting::LocalVoxelLight viewLight;
		{
			ChunkCollisionView view(manager);
			viewLight = view.sampleLight(glm::vec3(8.5f, 141.5f, 8.5f));
			CHECK(viewLight.blockRgb.r > 0.5f, "ChunkCollisionView samples smoothed light");
		}
		{
			ChunkMobWorld mobWorld(manager, generator);
			const auto mobLight = mobWorld.sampleLight(glm::vec3(8.5f, 141.5f, 8.5f));
			CHECK(mobLight.blockRgb.r == viewLight.blockRgb.r, "ChunkMobWorld delegates to view");
		}

		// Trilinear continuity: step along a line from (8.5, 141.5, 8.5) to (8.5, 145.5, 8.5)
		float prevRed = 2.0f;
		for (float y = 141.5f; y <= 145.5f; y += 0.25f)
		{
			const auto s = manager.sampleSmoothedLight(glm::vec3(8.5f, y, 8.5f));
			CHECK(s.blockRgb.r <= prevRed + 1e-4f, "block light decreases monotonically away from lava");
			prevRed = s.blockRgb.r;
		}
	}

	// 29. Cross-chunk light propagation, boundary continuity, source removal and overlapping RGB (issue #128 review)
	{
		ThreadPool tp(2);
		TerrainGenerator gen(1337);
		ChunkPool chunkPool(8);
		ChunkManager mgr(&gen, &tp, &chunkPool);
		Chunk *ca = chunkPool.acquire(glm::vec3(0.0f));
		Chunk *cb = chunkPool.acquire(glm::vec3(float(CHUNK_SIZE), 0.0f, 0.0f));
		CHECK(ca != nullptr && cb != nullptr, "acquired chunks ca and cb");
		if (ca && cb)
		{
			ChunkManagerProbe::registerChunk(mgr, glm::ivec3(0, 0, 0), ca);
			ChunkManagerProbe::registerChunk(mgr, glm::ivec3(1, 0, 0), cb);
			CHECK(mgr.prepareAndGenerateChunk(ca, gen), "generated ca");
			CHECK(mgr.prepareAndGenerateChunk(cb, gen), "generated cb");

			// Carve gallery across seam from x=8 to x=24, y=40, z=8
			for (int x = 7; x <= 15; ++x)
				for (int z = 6; z <= 10; ++z)
					for (int y = 38; y <= 42; ++y)
						ca->setVoxel(x, y, z, STONE);
			for (int x = 0; x <= 9; ++x)
				for (int z = 6; z <= 10; ++z)
					for (int y = 38; y <= 42; ++y)
						cb->setVoxel(x, y, z, STONE);
			for (int x = 8; x <= 15; ++x)
				for (int z = 7; z <= 9; ++z)
					for (int y = 39; y <= 41; ++y)
						ca->setVoxel(x, y, z, AIR);
			for (int x = 0; x <= 8; ++x)
				for (int z = 7; z <= 9; ++z)
					for (int y = 39; y <= 41; ++y)
						cb->setVoxel(x, y, z, AIR);

			// Place LAVA in Chunk A near the border at x=14, y=40, z=8
			ca->setVoxel(14, 40, 8, LAVA);

			// Mesh both chunks through the scheduler so halos are populated
			RenderSettings rs;
			const Camera cam(glm::vec3(8.0f, 40.0f, 8.0f));
			mgr.meshPendingChunks(cam, rs, 2);
			while (mgr.pendingMeshJobs() > 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				mgr.processFinishedJobs();
			}
			mgr.processFinishedJobs();

			CHECK(ca->hasLightStorage(), "ca has committed light storage");
			CHECK(cb->hasLightStorage(), "cb has committed light storage");

			// Cross-chunk sampling across boundary x = 16.0
			// LAVA is at x=14. Sampling steps along x from 14.5 to 18.5 at y=40.5, z=8.5
			const auto atLava  = mgr.sampleSmoothedLight(glm::vec3(14.5f, 40.5f, 8.5f));
			const auto at15_25 = mgr.sampleSmoothedLight(glm::vec3(15.25f, 40.5f, 8.5f));
			const auto at15_50 = mgr.sampleSmoothedLight(glm::vec3(15.50f, 40.5f, 8.5f));
			const auto at15_75 = mgr.sampleSmoothedLight(glm::vec3(15.75f, 40.5f, 8.5f));
			const auto at16_00 = mgr.sampleSmoothedLight(glm::vec3(16.00f, 40.5f, 8.5f)); // on chunk seam
			const auto at16_25 = mgr.sampleSmoothedLight(glm::vec3(16.25f, 40.5f, 8.5f));
			const auto at16_50 = mgr.sampleSmoothedLight(glm::vec3(16.50f, 40.5f, 8.5f));
			const auto at17_50 = mgr.sampleSmoothedLight(glm::vec3(17.50f, 40.5f, 8.5f));

			CHECK(atLava.blockRgb.r > 0.5f, "near lava red light is strong");
			CHECK(at16_00.blockRgb.r > 0.2f, "cross-chunk seam has propagated red light");
			CHECK(at16_50.blockRgb.r > 0.1f, "red light propagated across seam into chunk B");

			// Verify monotonic decay across boundary
			CHECK(at15_25.blockRgb.r <= atLava.blockRgb.r + 1e-4f, "decay 14.5 -> 15.25");
			CHECK(at15_50.blockRgb.r <= at15_25.blockRgb.r + 1e-4f, "decay 15.25 -> 15.50");
			CHECK(at15_50.blockRgb.r <= at15_50.blockRgb.r + 1e-4f, "decay 15.50 -> 15.75");
			CHECK(at16_00.blockRgb.r <= at15_75.blockRgb.r + 1e-4f, "decay 15.75 -> 16.00 (cross seam)");
			CHECK(at16_25.blockRgb.r <= at16_00.blockRgb.r + 1e-4f, "decay 16.00 -> 16.25 (cross seam)");
			CHECK(at16_50.blockRgb.r <= at16_25.blockRgb.r + 1e-4f, "decay 16.25 -> 16.50");
			CHECK(at17_50.blockRgb.r <= at16_50.blockRgb.r + 1e-4f, "decay 16.50 -> 17.50");

			// Continuous transition: step deltas across seam are small (no pop)
			CHECK(std::abs(at16_00.blockRgb.r - at15_75.blockRgb.r) < 0.25f, "seam step continuity left");
			CHECK(std::abs(at16_25.blockRgb.r - at16_00.blockRgb.r) < 0.25f, "seam step continuity right");

			// Test source removal: replace LAVA with AIR and remesh
			ca->setVoxel(14, 40, 8, AIR);
			ca->setState(ChunkState::GENERATED);
			cb->setState(ChunkState::GENERATED);
			mgr.meshPendingChunks(cam, rs, 2);
			while (mgr.pendingMeshJobs() > 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				mgr.processFinishedJobs();
			}
			mgr.processFinishedJobs();

			const auto removedAtLava = mgr.sampleSmoothedLight(glm::vec3(14.5f, 40.5f, 8.5f));
			const auto removedAtSeam = mgr.sampleSmoothedLight(glm::vec3(16.00f, 40.5f, 8.5f));
			const auto removedInB    = mgr.sampleSmoothedLight(glm::vec3(16.50f, 40.5f, 8.5f));
			CHECK(removedAtLava.blockRgb == glm::vec3(0.0f), "source removal: light drops to 0 at source");
			CHECK(removedAtSeam.blockRgb == glm::vec3(0.0f), "source removal: light drops to 0 at seam");
			CHECK(removedInB.blockRgb == glm::vec3(0.0f),    "source removal: light drops to 0 in chunk B");

			// Test overlapping RGB sources across chunk boundary:
			// Place REDSTONE_ORE in Chunk A (world x=13) and LAPIS_ORE in Chunk B (world x=19, B-local x=3)
			ca->setVoxel(13, 40, 8, REDSTONE_ORE);
			cb->setVoxel(3, 40, 8, LAPIS_ORE);
			ca->setState(ChunkState::GENERATED);
			cb->setState(ChunkState::GENERATED);
			mgr.meshPendingChunks(cam, rs, 2);
			while (mgr.pendingMeshJobs() > 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				mgr.processFinishedJobs();
			}
			mgr.processFinishedJobs();

			const auto seamOverlap = mgr.sampleSmoothedLight(glm::vec3(16.00f, 40.5f, 8.5f));
			CHECK(seamOverlap.blockRgb.r > 0.0f, "overlapping RGB: red component from chunk A is present at seam");
			CHECK(seamOverlap.blockRgb.b > 0.0f, "overlapping RGB: blue component from chunk B is present at seam");
		}
	}

	// 30. Empty chunk light cache (issue #128 review)
	{
		ChunkPool chunkPool(4);
		TerrainGenerator gen(1337);
		Chunk *emptyChunk = chunkPool.acquire(glm::vec3(0.0f));
		CHECK(emptyChunk != nullptr, "acquired chunk for empty-chunk light test");
		if (emptyChunk)
		{
			CHECK(emptyChunk->prepareVoxelStorageForGeneration(), "prepared storage for empty chunk");
			emptyChunk->generateTerrain(gen);
			// Carve all voxels in the chunk to AIR
			for (int z = 0; z < CHUNK_SIZE; ++z)
				for (int y = 0; y < CHUNK_HEIGHT; ++y)
					for (int x = 0; x < CHUNK_SIZE; ++x)
						emptyChunk->setVoxel(x, y, z, AIR);

			emptyChunk->setLocalLightCacheWanted(true);
			CHECK(emptyChunk->generateMesh(), "generateMesh succeeds on empty chunk");
			CHECK(emptyChunk->getOpaqueIndexCount() == 0, "empty chunk has 0 opaque indices");
			CHECK(emptyChunk->getWaterIndexCount() == 0, "empty chunk has 0 water indices");
			CHECK(emptyChunk->hasLightStorage(), "empty chunk has populated light storage when wanted");

			// In full air under the open sky, sky light is 1.0f and block light is 0.0f
			const auto sampleMid = emptyChunk->sampleLight(8, 40, 8);
			CHECK(std::abs(sampleMid.skylight - 1.0f) < 1e-4f, "empty chunk sky light is 1.0 everywhere");
			CHECK(sampleMid.blockRgb == glm::vec3(0.0f), "empty chunk block light is 0");

			chunkPool.release(emptyChunk);
		}
	}

	// 31. LOD chunk + entity light cache lifecycle (issue #128 review)
	{
		ThreadPool tp(2);
		TerrainGenerator gen(42);
		ChunkPool chunkPool(8);
		ChunkManager mgr(&gen, &tp, &chunkPool);

		Chunk *chunk = chunkPool.acquire(glm::vec3(64.0f, 0.0f, 0.0f));
		CHECK(chunk != nullptr, "acquired chunk for LOD light test");
		if (chunk)
		{
			ChunkManagerProbe::registerChunk(mgr, glm::ivec3(4, 0, 0), chunk);
			CHECK(mgr.prepareAndGenerateChunk(chunk, gen), "generated chunk for LOD light test");

			RenderSettings rs;
			rs.minRenderDistance = 16; // lodThresh = 32m, so at ~52m it will build an LOD mesh
			rs.maxRenderDistance = 256;

			// Camera at (20.0f, 40.0f, 8.0f): chunk distance is ~52m (> 32m LOD threshold, < 128m light radius)
			const Camera nearCam(glm::vec3(20.0f, 40.0f, 8.0f));
			mgr.meshPendingChunks(nearCam, rs, 1);
			while (mgr.pendingMeshJobs() > 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				mgr.processFinishedJobs();
			}
			mgr.processFinishedJobs();

			CHECK(chunk->isLODMesh(), "chunk was built as LOD mesh");
			CHECK(chunk->hasLightStorage(), "LOD chunk within 128m radius has light storage");

			// Move camera far away (> 128m)
			const Camera farCam(glm::vec3(300.0f, 40.0f, 8.0f));
			mgr.updateVisibility(farCam, 800, 600, rs);

			CHECK(!chunk->localLightCacheWanted(), "chunk outside 128m does not want light cache");
			CHECK(!chunk->hasLightStorage(), "light storage released when chunk moved outside 128m");
		}
	}

	// 32. ChunkLightPool memory trimming (issue #128 review)
	{
		ChunkLightPool pool;
		CHECK(pool.capacity() == 0, "initial pool capacity is 0");

		std::vector<ChunkLightStorage *> blocks;
		blocks.reserve(50);
		for (int i = 0; i < 50; ++i)
			blocks.push_back(pool.acquire());

		CHECK(pool.activeCount() == 50, "pool has 50 active blocks");
		CHECK(pool.capacity() >= 50, "pool capacity >= 50");
		CHECK(pool.freeCount() == 0, "pool free count is 0 while all acquired");

		for (auto *b : blocks)
			pool.release(b);

		CHECK(pool.activeCount() == 0, "pool active count is 0 after release");
		CHECK(pool.freeCount() >= 50, "pool free count >= 50");

		// Trim down to 32 free blocks
		pool.trim(32);
		CHECK(pool.capacity() == 32, "pool capacity trimmed to 32");
		CHECK(pool.freeCount() == 32, "pool free count trimmed to 32");
		CHECK(pool.activeCount() == 0, "active count remains 0");

		// Trim down to 0
		pool.trim(0);
		CHECK(pool.capacity() == 0, "pool capacity trimmed to 0");
		CHECK(pool.freeCount() == 0, "pool free count trimmed to 0");
	}

	// 33. Shutdown drain with unconsumed completed mesh jobs (issue #128 review)
	{
		TerrainGenerator gen(999);
		ChunkPool chunkPool(4);
		{
			ChunkManager mgr(&gen, nullptr, &chunkPool);
			Chunk *chunk = chunkPool.acquire(glm::vec3(0.0f));
			CHECK(chunk != nullptr, "acquired chunk for shutdown test");
			ChunkManagerProbe::registerChunk(mgr, glm::ivec3(0, 0, 0), chunk);

			auto *result = chunkPool.meshResultPool().acquire();
			CHECK(result != nullptr, "acquired mesh result");
			result->beginBuild(chunk, chunk->meshGeneration(), chunk->meshRevision());
			result->lightPool = &chunkPool.lightPool();
			result->lightStorage = chunkPool.lightPool().acquire();
			result->lightCacheAction = LightCacheAction::Replace;

			CHECK(chunkPool.meshResultPool().stats().active >= 1, "mesh result active");
			CHECK(chunkPool.lightPool().activeCount() >= 1, "light block active");

			// Inject into manager without calling processFinishedJobs()
			ChunkManagerProbe::injectCompletedMeshJob(mgr, chunk, result);

			// Destruction of mgr here must drain m_completedMeshJobs, release the
			// result back to meshResultPool, and release the lightStorage back to lightPool.
		}

		CHECK(chunkPool.meshResultPool().stats().active == 0, "shutdown cleanly returned mesh result");
		CHECK(chunkPool.lightPool().activeCount() == 0, "shutdown cleanly returned light storage");
	}

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "PASS: chunk lifecycle - moves carry full state, "
			  << "recycled generateTerrain matches owning path, capacities stable, "
			  << "streaming dispatch matches brute-force oracle\n";
	return 0;
}
