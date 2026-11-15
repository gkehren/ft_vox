// test_voxel_world — CPU-only authoritative world acceptance (issue #206,
// Epic #204 Phase B). Links ONLY ft_vox_core: no SDL, no Vulkan device, no
// ImGui, no render chunk handles. Proves the issue's acceptance list:
//
//   * construct / generate / sample / edit / unload / reload / persist a
//     world through the CPU API without a graphics runtime;
//   * per-seed generated voxel fixtures byte-identical (same platform);
//   * border edits / corners, deferred + coalesced edits, negative coords,
//     unknown terrain, safe chunk recycling, out-of-order generation
//     completion, stale revision rejection;
//   * save restart, sparse edit normalization, identity guards, close
//     failure / retry, concurrent edits during pending async I/O;
//   * the exact ChunkCollisionView collision semantics now carried by
//     WorldCollisionView (boundaries, unknown cells, pending-edit overlay).
//
// Style mirrors the other headless suites: a CHECK macro, plain main().

#include <World/VoxelWorld.hpp>
#include <World/WorldCollisionView.hpp>
#include <World/WorldPersistence.hpp>
#include <Core/SimContracts.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Core/ThreadPool.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static int g_fails = 0;
static int g_checks = 0;
#define CHECK(cond, msg)                                                       \
	do                                                                         \
	{                                                                          \
		++g_checks;                                                            \
		if (!(cond))                                                           \
		{                                                                      \
			std::cerr << "FAIL: " << msg << " (" << __LINE__ << ")\n";         \
			++g_fails;                                                         \
		}                                                                      \
	} while (0)

// Friend probe declared in VoxelWorld.hpp: deterministic injection seams.
struct VoxelWorldProbe
{
	static size_t pendingEdits(const VoxelWorld &w) { return w.m_pendingEdits.size(); }
	static void injectGenCompletion(VoxelWorld &w, WorldChunk *c, uint64_t generation)
	{
		std::lock_guard<std::mutex> lk(w.m_completedJobsMutex);
		w.m_completedGenerationChunks.push_back({c, generation});
	}
	static void forceRecordState(WorldChunk *c, ChunkState s) { c->setState(s); }
};

namespace
{
std::filesystem::path makeTempDir(const char *tag)
{
	const auto base = std::filesystem::temp_directory_path() /
					  ("ft_vox_world_" + std::string(tag) + "_" +
					   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(base);
	return base;
}

struct TempRoot
{
	std::filesystem::path path;
	explicit TempRoot(const char *tag) : path(makeTempDir(tag)) {}
	~TempRoot()
	{
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
	}
};

glm::vec3 chunkOrigin(int cx, int cz)
{
	return {static_cast<float>(cx * CHUNK_SIZE), 0.0f, static_cast<float>(cz * CHUNK_SIZE)};
}

// Synchronously create + generate one record at (cx, cz) through the public
// bootstrap contract (no thread pool needed).
WorldChunk *makeGeneratedChunk(VoxelWorld &world, TerrainGenerator &gen, int cx, int cz)
{
	const glm::ivec3 pos(cx, 0, cz);
	if (WorldChunk *existing = world.getChunk(pos))
		return existing;
	// The bootstrap path is the public synchronous create+generate contract.
	const auto created = world.generateInitialAreaVoxels(chunkOrigin(cx, cz) + glm::vec3(8.f, 0.f, 8.f), 0);
	(void)created;
	return world.getChunk(pos);
}

void pumpUntilIdle(VoxelWorld &world)
{
	for (int i = 0; i < 2000 && world.asyncBusy(); ++i)
	{
		world.pump();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	world.pump();
}
} // namespace

// ---------------------------------------------------------------------------
// 1. Headless lifecycle: construct, generate, sample, edit, unload, reload.
// ---------------------------------------------------------------------------
static void testHeadlessLifecycle()
{
	TerrainGenerator gen(1337);
	VoxelWorld world(&gen, nullptr);

	const auto created = world.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 1);
	CHECK(created.size() == 9, "bootstrap creates the 3x3 footprint");
	CHECK(world.chunkCount() == 9, "all bootstrap records resident");

	WorldChunk *center = world.getChunk({0, 0, 0});
	CHECK(center != nullptr, "center record resident");
	CHECK(center->isVoxelBackingReadable(), "generated backing is readable");
	CHECK(center->getState() == ChunkState::GENERATED, "generated state");
	CHECK(center->hasVoxelStorage(), "voxel storage attached");

	// Sample through the CPU collision view: solid terrain below, sky above.
	{
		WorldCollisionView view(world);
		int solid = 0;
		for (int y = 0; y < 128; ++y)
			if (view.sample({8, y, 8}).solid)
				++solid;
		CHECK(solid > 0, "bootstrap terrain has solid voxels");
		const auto sky = view.sample({8, WORLD_HEIGHT - 1, 8});
		CHECK(sky.available && !sky.solid, "top of world is open sky");
	}

	// Edit through the CPU API.
	const auto edit = world.placeVoxel({8.5f, 200.5f, 8.5f}, STONE);
	CHECK(edit.accepted, "headless edit accepted");
	CHECK(center->getVoxel(8, 200, 8).type == static_cast<uint8_t>(STONE), "edit landed");
	CHECK(world.deleteVoxel({8.5f, 200.5f, 8.5f}).accepted, "headless delete accepted");
	CHECK(center->getVoxel(8, 200, 8).type == static_cast<uint8_t>(AIR), "delete landed");

	// Unload by streaming far away, then reload and verify determinism.
	std::vector<Voxel> before(center->voxelStorage()->voxels.begin(),
							  center->voxelStorage()->voxels.end());
	WorldStreamSettings settings;
	settings.maxRenderDistance = 64;
	settings.minRenderDistance = 32;
	world.updateStreaming(glm::vec3(100000.f, 100.f, 100000.f), {0.f, 1.f}, settings);
	CHECK(world.chunkCount() == 0, "far streaming center unloads everything");
	{
		WorldCollisionView view(world);
		CHECK(!view.sample({8, 64, 8}).available, "unloaded terrain is UNKNOWN, not solid");
	}
	world.updateStreaming(glm::vec3(8.f, 100.f, 8.f), {0.f, 1.f}, settings);
	world.processChunkLoading(64);
	WorldChunk *reloaded = world.getChunk({0, 0, 0});
	CHECK(reloaded != nullptr, "record reloaded at the same coordinate");
	CHECK(reloaded->getState() == ChunkState::UNLOADED, "reloaded record awaits generation");
	CHECK(reloaded->prepareVoxelStorageForGeneration(), "reload prepare");
	reloaded->generateTerrain(gen);
	CHECK(std::memcmp(before.data(), reloaded->voxelStorage()->voxels.data(),
					  before.size() * sizeof(Voxel)) == 0,
		  "reloaded generation is byte-identical (deterministic worldgen)");
}

// ---------------------------------------------------------------------------
// 2. Per-seed byte identity across two independent worlds.
// ---------------------------------------------------------------------------
static void testPerSeedByteIdentity()
{
	TerrainGenerator genA(42), genB(42);
	VoxelWorld a(&genA, nullptr), b(&genB, nullptr);
	a.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 1);
	b.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 1);
	for (int cx = -1; cx <= 1; ++cx)
		for (int cz = -1; cz <= 1; ++cz)
		{
			const WorldChunk *ra = a.getChunk({cx, 0, cz});
			const WorldChunk *rb = b.getChunk({cx, 0, cz});
			CHECK(ra && rb, "fixture chunk resident in both worlds");
			if (!ra || !rb)
				continue;
			CHECK(std::memcmp(ra->voxelStorage()->voxels.data(),
							  rb->voxelStorage()->voxels.data(), CHUNK_VOLUME) == 0,
				  "same-seed voxel backing byte-identical");
			CHECK(ra->biomeTypes == rb->biomeTypes, "same-seed biomes identical");
			CHECK(ra->heightMap == rb->heightMap, "same-seed height map identical");
			CHECK(ra->biomeGrassColors == rb->biomeGrassColors, "same-seed grass tints identical");
			CHECK(ra->biomeFoliageColors == rb->biomeFoliageColors, "same-seed foliage tints identical");
			CHECK(std::memcmp(ra->borders(), rb->borders(), sizeof(ChunkNeighborBorders)) == 0,
				  "same-seed border shell byte-identical");
		}
}

// ---------------------------------------------------------------------------
// 3. Logical edits: mirrors, corners, deferral, coalescing, negative coords,
//    stale incarnations, unknown terrain.
// ---------------------------------------------------------------------------
static void testEditSemantics()
{
	TerrainGenerator gen(2468);
	VoxelWorld world(&gen, nullptr);
	world.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 1);
	WorldChunk *origin = world.getChunk({0, 0, 0});
	WorldChunk *east = world.getChunk({1, 0, 0});
	WorldChunk *west = world.getChunk({-1, 0, 0});
	WorldChunk *south = world.getChunk({0, 0, -1});
	CHECK(origin && east && west && south, "edit fixture resident");

	// Find a guaranteed-air cell closer to the surface so the effective-state
	// no-op gate is exercised against AIR.
	int airY = -1;
	for (int y = WORLD_HEIGHT - 1; y >= 0; --y)
		if (origin->getVoxel(8, y, 8).type == static_cast<uint8_t>(AIR))
		{
			airY = y;
			break;
		}
	CHECK(airY > 1, "fixture has an air column top");

	// (a) Interior place/delete + logical no-op refusal.
	CHECK(world.placeVoxel({8.5f, airY + 0.5f, 8.5f}, STONE).accepted, "interior place");
	CHECK(!world.placeVoxel({8.5f, airY + 0.5f, 8.5f}, DIRT).accepted,
		  "place onto solid is a logical no-op");
	CHECK(world.deleteVoxel({8.5f, airY + 0.5f, 8.5f}).accepted, "interior delete");
	CHECK(!world.deleteVoxel({8.5f, airY + 0.5f, 8.5f}).accepted,
		  "delete of AIR is a logical no-op");

	// (b) Border edit mirrors into the neighbor shell (x = 0 -> west's x=16).
	const glm::vec3 borderPos(0.0f, airY + 0.5f, 8.5f);
	CHECK(world.placeVoxel(borderPos, BRICKS).accepted, "border place");
	CHECK(origin->getVoxel(0, airY, 8).type == static_cast<uint8_t>(BRICKS), "target written");
	CHECK(west->sampleForMeshing(CHUNK_SIZE, airY, 8) == BRICKS, "west mirror written");
	CHECK(world.deleteVoxel(borderPos).accepted, "border delete");
	CHECK(west->sampleForMeshing(CHUNK_SIZE, airY, 8) == AIR, "mirror deleted");

	// (c) Corner edit mirrors BOTH adjacent neighbors, never the diagonal.
	const glm::vec3 cornerPos(0.0f, airY + 0.5f, 0.0f);
	CHECK(world.placeVoxel(cornerPos, GLASS).accepted, "corner place");
	CHECK(west->sampleForMeshing(CHUNK_SIZE, airY, 0) == GLASS, "corner west mirror");
	CHECK(south->sampleForMeshing(0, airY, CHUNK_SIZE) == GLASS, "corner south mirror");
	CHECK(world.deleteVoxel(cornerPos).accepted, "corner delete");

	// (d) Deferred + coalesced edits while the target is in transit.
	origin->setInTransit(true);
	const glm::vec3 p(8.5f, airY + 0.5f, 8.5f);
	CHECK(world.placeVoxel(p, STONE).accepted, "place pending");
	CHECK(world.deleteVoxel(p).accepted, "delete pending");
	CHECK(world.placeVoxel(p, DIRT).accepted, "re-place pending");
	CHECK(VoxelWorldProbe::pendingEdits(world) == 1, "target edits coalesce to one entry");
	origin->setInTransit(false);
	world.pump();
	CHECK(origin->getVoxel(8, airY, 8).type == static_cast<uint8_t>(DIRT),
		  "coalesced final state lands");
	CHECK(VoxelWorldProbe::pendingEdits(world) == 0, "queue drained");
	world.deleteVoxel(p);

	// (e) UNLOADED backing defers edits (issue #115): a fresh record answers
	// logically empty and queues until generation lands.
	WorldChunk *fresh = makeGeneratedChunk(world, gen, 5, 5);
	CHECK(fresh != nullptr, "fresh record created");
	fresh->reset(chunkOrigin(5, 5), WorldChunk::ResetMode::ForGeneration);
	CHECK(fresh->getState() == ChunkState::UNLOADED, "fresh record UNLOADED");
	const glm::vec3 freshPos(5.0f * CHUNK_SIZE + 8.5f, 100.5f, 5.0f * CHUNK_SIZE + 8.5f);
	CHECK(world.placeVoxel(freshPos, STONE).accepted, "edit accepted against UNLOADED");
	CHECK(VoxelWorldProbe::pendingEdits(world) == 1, "UNLOADED edit deferred");
	{
		WorldCollisionView view(world);
		CHECK(!view.sample({5 * CHUNK_SIZE + 8, 100, 5 * CHUNK_SIZE + 8}).available,
			  "UNLOADED backing is UNKNOWN even with a pending edit");
	}
	CHECK(fresh->prepareVoxelStorageForGeneration(), "fresh prepare");
	fresh->generateTerrain(gen);
	world.pump();
	CHECK(fresh->getVoxel(8, 100, 8).type == static_cast<uint8_t>(STONE),
		  "deferred edit lands after generation");

	// (f) Negative coordinates: floor division maps into the right record.
	const glm::vec3 negPos(-0.5f, airY + 0.5f, -0.5f); // chunk (-1,-1), local (15,15)
	WorldChunk *neg = makeGeneratedChunk(world, gen, -1, -1);
	CHECK(neg != nullptr, "negative-coordinate record created");
	CHECK(world.placeVoxel(negPos, COBBLESTONE).accepted, "negative-coord edit accepted");
	CHECK(neg->getVoxel(15, airY, 15).type == static_cast<uint8_t>(COBBLESTONE),
		  "negative world coord lands at local 15,15 of chunk (-1,-1)");
	CHECK(VoxelWorld::worldToChunkCoord({-16.0f, 0.f, -16.0f}) == glm::ivec3(-1, 0, -1),
		  "exact negative boundary maps to chunk -1");
	CHECK(VoxelWorld::worldToChunkCoord({-16.01f, 0.f, -16.01f}) == glm::ivec3(-2, 0, -2),
		  "past the negative boundary maps to chunk -2");
	world.deleteVoxel(negPos);

	// (g) A queued edit never lands on a recycled incarnation.
	origin->setInTransit(true);
	CHECK(world.placeVoxel(p, STONE).accepted, "edit queued for recycle test");
	origin->reset(origin->getPosition()); // same pointer, new incarnation
	origin->setInTransit(false);
	world.pump();
	CHECK(origin->getVoxel(8, airY, 8).type == static_cast<uint8_t>(AIR),
		  "stale queued edit dropped after recycle");
	CHECK(VoxelWorldProbe::pendingEdits(world) == 0, "stale entry removed from the queue");
}

// ---------------------------------------------------------------------------
// 4. Out-of-order generation completion + stale revision rejection.
// ---------------------------------------------------------------------------
static void testStalePublication()
{
	TerrainGenerator gen(777);
	ThreadPool threads(2);
	VoxelWorld world(&gen, &threads);
	world.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 0);
	WorldChunk *c = world.getChunk({0, 0, 0});
	CHECK(c != nullptr, "stale fixture resident");

	// A completion stamped for a retired incarnation must not publish.
	const uint64_t liveGen = c->generation();
	VoxelWorldProbe::injectGenCompletion(world, c, liveGen + 999);
	world.publishFinishedGeneration();
	CHECK(c->isInTransit() == false, "stale completion does not disturb the record");

	// Reset bumps the incarnation; a completion for the old one is ignored
	// even if the record is idle again.
	c->reset(c->getPosition(), WorldChunk::ResetMode::ForGeneration);
	CHECK(c->generation() == liveGen + 1, "reset bumps the generation");
	VoxelWorldProbe::injectGenCompletion(world, c, liveGen);
	world.publishFinishedGeneration();
	CHECK(c->getState() == ChunkState::UNLOADED, "stale-generation completion never publishes");

	// The identity contract: revision moves on every content change, and the
	// sim::ChunkIdentity value carries both dimensions plus the coordinate.
	CHECK(c->prepareVoxelStorageForGeneration(), "stale fixture prepare");
	c->generateTerrain(gen);
	const sim::ChunkIdentity id0 = c->identity();
	CHECK(id0.chunkX == 0 && id0.chunkZ == 0, "identity carries the coordinate");
	CHECK(id0.generation == c->generation() && id0.revision == c->revision(),
		  "identity mirrors the record counters");
	c->setVoxel(1, 100, 1, STONE);
	CHECK(c->revision() == id0.revision + 1, "edit bumps the revision");
	CHECK(c->identity().generation == id0.generation, "edit keeps the incarnation");
}

// ---------------------------------------------------------------------------
// 5. Async generation through the worker pool + safe unload/recycle.
// ---------------------------------------------------------------------------
static void testAsyncGenerationAndRecycle()
{
	TerrainGenerator gen(31337);
	ThreadPool threads(2);
	VoxelWorld world(&gen, &threads);
	WorldStreamSettings settings;
	settings.maxRenderDistance = 96;
	settings.minRenderDistance = 48;
	world.updateStreaming(glm::vec3(8.f, 100.f, 8.f), {0.f, 1.f}, settings);
	world.processChunkLoading(64);
	CHECK(world.chunkCount() > 0, "async fixture loaded records");
	world.generatePendingVoxels(glm::vec3(8.f, 100.f, 8.f), 96.f * 96.f, 64);
	pumpUntilIdle(world);
	CHECK(world.pendingGenJobs() == 0, "all generation jobs finished");

	WorldChunk *c = world.getChunk({0, 0, 0});
	CHECK(c && c->isVoxelBackingReadable(), "async generation published readable backing");
	if (!c)
		return;
	const uint64_t genBefore = c->generation();

	// Edit, then unload: the edit must be captured for persistence-free
	// recycling without leaking into the next incarnation.
	CHECK(world.placeVoxel({8.5f, 200.5f, 8.5f}, STONE).accepted, "async fixture edit");
	world.updateStreaming(glm::vec3(100000.f, 100.f, 100000.f), {0.f, 1.f}, settings);
	CHECK(world.getChunk({0, 0, 0}) == nullptr, "record unloaded");

	// Reload: a recycled record is a NEW incarnation with clean state.
	world.updateStreaming(glm::vec3(8.f, 100.f, 8.f), {0.f, 1.f}, settings);
	world.processChunkLoading(64);
	WorldChunk *re = world.getChunk({0, 0, 0});
	CHECK(re != nullptr, "record reloaded");
	CHECK(re->generation() != genBefore, "recycled record has a new incarnation");
	CHECK(re->getState() == ChunkState::UNLOADED, "recycled record is not readable");
	CHECK(!re->hasPersistentEdits(), "recycled record carries no stale edits");
	{
		WorldCollisionView view(world);
		CHECK(!view.sample({8, 200, 8}).available, "recycled backing is UNKNOWN before generation");
	}
	world.generatePendingVoxels(glm::vec3(8.f, 100.f, 8.f), 96.f * 96.f, 64);
	pumpUntilIdle(world);
	CHECK(re->isVoxelBackingReadable(), "recycled record regenerated");
	CHECK(re->getVoxel(8, 200, 8).type == static_cast<uint8_t>(AIR),
		  "no stale edit resurrected in the recycled incarnation");
}

// ---------------------------------------------------------------------------
// 6. Collision semantics: the exact ChunkCollisionView contract.
// ---------------------------------------------------------------------------
static void testCollisionSemantics()
{
	TerrainGenerator gen(99);
	VoxelWorld world(&gen, nullptr);
	world.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 0);

	// Scoped views: the shared lock is held for the view's lifetime, so no
	// world mutation may run inside a view scope (the historical contract).
	{
		WorldCollisionView view(world);
		// Vertical boundaries: solid floor, open sky.
		CHECK(view.sample({8, -1, 8}).solid && view.sample({8, -1, 8}).available,
			  "below world is solid boundary");
		CHECK(view.sample({8, WORLD_HEIGHT, 8}).available && !view.sample({8, WORLD_HEIGHT, 8}).solid,
			  "above world is open sky");
		// Horizontal envelope (SHRT_MIN/SHRT_MAX): solid, never unknown.
		CHECK(view.sample({SHRT_MIN - 1, 64, 8}).solid, "far -X envelope is solid");
		CHECK(view.sample({SHRT_MAX, 64, 8}).solid, "far +X envelope is solid");
		CHECK(view.sample({8, 64, SHRT_MIN - 1}).solid, "far -Z envelope is solid");
		CHECK(view.sample({8, 64, SHRT_MAX}).solid, "far +Z envelope is solid");
		// Unknown vs loaded.
		CHECK(!view.sample({10000, 64, 10000}).available, "unloaded chunk is unknown");
	}

	// Effective pending edits overlay the stored voxel (non-mirror only).
	WorldChunk *c = world.getChunk({0, 0, 0});
	int airY = -1;
	for (int y = WORLD_HEIGHT - 1; y >= 0; --y)
		if (c->getVoxel(8, y, 8).type == static_cast<uint8_t>(AIR))
		{
			airY = y;
			break;
		}
	c->setInTransit(true);
	CHECK(world.placeVoxel({8.5f, airY + 0.5f, 8.5f}, STONE).accepted, "pending overlay place");
	{
		WorldCollisionView pendingView(world);
		const auto cell = pendingView.sample({8, airY, 8});
		CHECK(cell.available && cell.solid, "pending edit visible through the collision view");
	}
	c->setInTransit(false);
	world.pump();
	world.deleteVoxel({8.5f, airY + 0.5f, 8.5f});

	// Water medium + surface scan.
	CHECK(world.placeVoxel({8.5f, airY + 0.5f, 8.5f}, WATER).accepted, "water placed");
	{
		WorldCollisionView waterView(world);
		const auto cell = waterView.sample({8, airY, 8});
		CHECK(cell.available && !cell.solid && cell.medium == physics::Medium::Water,
			  "water cell is non-solid water medium");
		CHECK(waterView.waterSurfaceAbove({8, airY, 8}) == static_cast<float>(airY + 1),
			  "water surface is the first non-water plane");
		CHECK(waterView.waterSurfaceAbove({8, airY + 1, 8}) == kUnknownUnderwaterSurfaceY,
			  "non-water eye reports the unknown sentinel");
	}
	world.deleteVoxel({8.5f, airY + 0.5f, 8.5f});

	// sampleWithType agrees with sample and reports the block type.
	{
		WorldCollisionView view(world);
		TextureType type = AIR;
		const auto cell = view.sampleWithType({8, 0, 8}, &type);
		CHECK(cell.available && cell.solid && type == BEDROCK, "bedrock floor type reported");
	}
}

// ---------------------------------------------------------------------------
// 7. Persistence: restart, sparse normalization, guards, failure/retry,
//    concurrent edits during async I/O, stranded-edit close refusal.
// ---------------------------------------------------------------------------
static void testPersistenceLifecycle()
{
	TempRoot root("persist");
	const std::string savesRoot = (root.path / "saves").string();
	TerrainGenerator gen(42);
	ThreadPool threads(2);

	// Session 1: open, edit, flush, close.
	{
		VoxelWorld world(&gen, &threads);
		std::string err;
		CHECK(world.openWorld(savesRoot, "alpha", err), "openWorld creates the save");
		CHECK(world.isWorldOpen(), "world open");
		world.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 1);
		CHECK(world.placeVoxel({8.5f, 200.5f, 8.5f}, BRICKS).accepted, "persisted edit");
		CHECK(world.placeVoxel({9.5f, 200.5f, 9.5f}, GLASS).accepted, "second persisted edit");
		CHECK(world.flushWorld(), "flush succeeds");
		CHECK(world.closeWorld(), "close succeeds");
		CHECK(!world.isWorldOpen(), "world closed");
	}

	// Session 2: reopen, regenerate, overrides re-applied deterministically.
	{
		VoxelWorld world(&gen, &threads);
		std::string err;
		CHECK(world.openWorld(savesRoot, "alpha", err), "reopen the save");
		world.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 1);
		WorldChunk *c = world.getChunk({0, 0, 0});
		CHECK(c->getVoxel(8, 200, 8).type == static_cast<uint8_t>(BRICKS),
			  "edit A survives restart");
		CHECK(c->getVoxel(9, 200, 9).type == static_cast<uint8_t>(GLASS),
			  "edit B survives restart");
		CHECK(world.closeWorld(), "second close succeeds");
	}

	// Sparse normalization: reverting to the procedural value drops the
	// override from the persisted file (the worker diffs against the base).
	{
		VoxelWorld world(&gen, &threads);
		std::string err;
		CHECK(world.openWorld(savesRoot, "alpha", err), "reopen for normalization");
		world.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 1);
		WorldChunk *c = world.getChunk({0, 0, 0});
		// Capture the procedural base by regenerating a scratch record.
		TerrainGenerator scratchGen(42);
		WorldChunk scratch(chunkOrigin(0, 0), nullptr, nullptr);
		CHECK(scratch.prepareVoxelStorageForGeneration(), "scratch prepare");
		scratch.generateTerrain(scratchGen);
		const auto base8 = scratch.getVoxel(8, 200, 8).type;
		const auto base9 = scratch.getVoxel(9, 200, 9).type;
		c->setVoxel(8, 200, 8, static_cast<TextureType>(base8));
		c->setVoxel(9, 200, 9, static_cast<TextureType>(base9));
		CHECK(world.flushWorld(), "normalization flush");
		CHECK(!world.worldPersistence()->hasOverrides(0, 0),
			  "reverted chunk has no persisted overrides");
		CHECK(world.closeWorld(), "normalization close");
		const auto chunkFile = std::filesystem::path(savesRoot) / "alpha" / "chunks" / "0_0.chunk";
		CHECK(!std::filesystem::exists(chunkFile), "reverted chunk file removed");
	}

	// Identity guards: seed mismatch refuses the open (never overwrites).
	{
		TerrainGenerator otherGen(43);
		VoxelWorld world(&otherGen, nullptr);
		std::string err;
		CHECK(!world.openWorld(savesRoot, "alpha", err), "seed mismatch refused");
		CHECK(err.find("seed") != std::string::npos, "seed mismatch error mentions the seed");
		CHECK(!world.isWorldOpen(), "refused open leaves the world closed");
	}

	// Close failure / retry: a failed flush keeps persistence OPEN and usable.
	{
		VoxelWorld world(&gen, &threads);
		std::string err;
		CHECK(world.openWorld(savesRoot, "retry", err), "open retry world");
		world.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 0);
		CHECK(world.placeVoxel({8.5f, 200.5f, 8.5f}, STONE).accepted, "retry edit");
		std::atomic<bool> failWrites{true};
		world.worldPersistence()->setWriteTmpFnForTests(
			[&failWrites](const std::filesystem::path &dir, int32_t cx, int32_t cz,
						  const std::vector<worldsave::ChunkEdit> &overrides,
						  std::filesystem::path &outTmp) -> worldsave::SaveStatus {
				if (failWrites.load())
					return worldsave::SaveStatus::IoError;
				// Success path must still produce the tmp sidecar: the
				// service's authoritative commit renames it into place.
				return worldsave::writeChunkFileTmp(dir, cx, cz, overrides, outTmp);
			});
		CHECK(!world.closeWorld(), "close refuses on flush failure");
		CHECK(world.isWorldOpen(), "failed close keeps persistence alive");
		failWrites.store(false);
		CHECK(world.closeWorld(), "retry close succeeds");
		CHECK(!world.isWorldOpen(), "retry close tears down");
		// The edit must be durable after the retry.
		VoxelWorld verify(&gen, nullptr);
		CHECK(verify.openWorld(savesRoot, "retry", err), "reopen retry world");
		verify.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 0);
		CHECK(verify.getChunk({0, 0, 0})->getVoxel(8, 200, 8).type == static_cast<uint8_t>(STONE),
			  "edit durable after failed-then-retried close");
		CHECK(verify.closeWorld(), "verify close");
	}

	// Concurrent edits during pending async I/O: an edit lands while the
	// first capture is blocked at the commit gate; the next capture must
	// supersede it so the NEWEST value is what lands on disk.
	{
		VoxelWorld world(&gen, &threads);
		std::string err;
		CHECK(world.openWorld(savesRoot, "concurrent", err), "open concurrent world");
		world.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 0);
		CHECK(world.placeVoxel({8.5f, 200.5f, 8.5f}, STONE).accepted, "first capture");

		std::mutex gateMx;
		std::condition_variable gateCv;
		bool atCommit = false;
		bool release = false;
		world.worldPersistence()->setBeforeCommitFnForTests(
			[&](int32_t, int32_t, uint64_t) {
				std::unique_lock<std::mutex> lk(gateMx);
				atCommit = true;
				gateCv.notify_all();
				gateCv.wait(lk, [&] { return release; });
			});
		// The flush barrier waits for the gated worker, so run it off-thread.
		std::thread flusher([&world] { world.flushWorld(); });
		{
			std::unique_lock<std::mutex> lk(gateMx);
			gateCv.wait_for(lk, std::chrono::seconds(5), [&] { return atCommit; });
		}
		CHECK(atCommit, "worker reached the commit gate");
		// Newer edit while the first request is still in flight (delete +
		// re-place: a direct place onto the solid STONE is a logical no-op).
		CHECK(world.deleteVoxel({8.5f, 200.5f, 8.5f}).accepted, "newer delete during I/O");
		CHECK(world.placeVoxel({8.5f, 200.5f, 8.5f}, DIRT).accepted, "newer edit during I/O");
		{
			std::lock_guard<std::mutex> lk(gateMx);
			release = true;
		}
		gateCv.notify_all();
		flusher.join();
		// The retry sweep captures the newer desired state and persists it.
		CHECK(world.flushWorld(), "final flush after gate release");
		CHECK(world.closeWorld(), "concurrent close");
		VoxelWorld verify(&gen, nullptr);
		CHECK(verify.openWorld(savesRoot, "concurrent", err), "reopen concurrent world");
		verify.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 0);
		CHECK(verify.getChunk({0, 0, 0})->getVoxel(8, 200, 8).type == static_cast<uint8_t>(DIRT),
			  "newest edit wins over the in-flight capture");
		CHECK(verify.closeWorld(), "verify close");
	}

	// Stranded edits: close refuses while accepted edits sit on a record that
	// never generated; generating it lets the close succeed (no silent loss).
	{
		VoxelWorld world(&gen, nullptr); // no thread pool: nothing auto-generates
		std::string err;
		CHECK(world.openWorld(savesRoot, "stranded", err), "open stranded world");
		world.processChunkLoading(0); // no-op
		// Create a record without generating it via the bootstrap, then reset
		// it back to UNLOADED while keeping it resident.
		world.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 0);
		WorldChunk *c = world.getChunk({0, 0, 0});
		c->reset(c->getPosition(), WorldChunk::ResetMode::ForGeneration);
		CHECK(c->getState() == ChunkState::UNLOADED, "stranded record UNLOADED");
		CHECK(world.placeVoxel({8.5f, 200.5f, 8.5f}, STONE).accepted, "stranded edit accepted");
		CHECK(world.hasPendingLogicalEdits(), "edit stranded outside authoritative state");
		CHECK(!world.closeWorld(), "close refuses with stranded edits");
		CHECK(world.isWorldOpen(), "refused close keeps persistence alive");
		// Resolve: generate the record, land the edit, close again.
		CHECK(c->prepareVoxelStorageForGeneration(), "stranded prepare");
		c->generateTerrain(gen);
		world.pump();
		CHECK(!world.hasPendingLogicalEdits(), "stranded edit landed");
		CHECK(world.closeWorld(), "close succeeds after resolution");
		VoxelWorld verify(&gen, nullptr);
		CHECK(verify.openWorld(savesRoot, "stranded", err), "reopen stranded world");
		verify.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 0);
		CHECK(verify.getChunk({0, 0, 0})->getVoxel(8, 200, 8).type == static_cast<uint8_t>(STONE),
			  "stranded edit durable after resolved close");
		CHECK(verify.closeWorld(), "verify close");
	}
}

// ---------------------------------------------------------------------------
// 8. Unload captures edits for persistence (recycle-safe capture).
// ---------------------------------------------------------------------------
static void testUnloadCapture()
{
	TempRoot root("unload");
	const std::string savesRoot = (root.path / "saves").string();
	TerrainGenerator gen(555);
	VoxelWorld world(&gen, nullptr);
	std::string err;
	CHECK(world.openWorld(savesRoot, "unload", err), "open unload world");
	world.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 1);
	CHECK(world.placeVoxel({8.5f, 200.5f, 8.5f}, LAVA).accepted, "unload-capture edit");
	WorldStreamSettings settings;
	settings.maxRenderDistance = 64;
	settings.minRenderDistance = 32;
	world.updateStreaming(glm::vec3(100000.f, 100.f, 100000.f), {0.f, 1.f}, settings);
	CHECK(world.chunkCount() == 0, "all records unloaded");
	CHECK(world.flushWorld(), "flush after unload");
	CHECK(world.closeWorld(), "close after unload");

	VoxelWorld verify(&gen, nullptr);
	CHECK(verify.openWorld(savesRoot, "unload", err), "reopen unload world");
	verify.generateInitialAreaVoxels(glm::vec3(8.f, 100.f, 8.f), 1);
	CHECK(verify.getChunk({0, 0, 0})->getVoxel(8, 200, 8).type == static_cast<uint8_t>(LAVA),
		  "edit captured at unload survives recycle + restart");
	CHECK(verify.closeWorld(), "verify close");
}

int main()
{
	std::cout.setf(std::ios::unitbuf);
	std::cerr.setf(std::ios::unitbuf);

	testHeadlessLifecycle();
	testPerSeedByteIdentity();
	testEditSemantics();
	testStalePublication();
	testAsyncGenerationAndRecycle();
	testCollisionSemantics();
	testPersistenceLifecycle();
	testUnloadCapture();

	if (g_fails == 0)
	{
		std::cout << "PASS: voxel world — headless lifecycle, determinism, edits, collision, "
					 "persistence (" << g_checks << " checks)\n";
		return 0;
	}
	std::cerr << g_fails << " check(s) failed (" << g_checks << " total)\n";
	return 1;
}
