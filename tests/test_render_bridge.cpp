// RenderWorldBridge acceptance (issue #207, Epic #204 Phase C).
//
// Proves the simulation/render ownership boundary the phase introduces:
//   * the CPU world runs with NO render bridge registered (headless);
//   * the bridge is a DERIVED projection that references the host-owned world
//     — a single source of authoritative voxel truth, never a duplicate;
//   * the world outlives the bridge (detach) and a fresh bridge re-attaches to
//     the live world, projecting its current residency;
//   * mesh publication validates sim::ChunkIdentity (stale results discarded);
//   * border edits register atomic geometric commit groups (PR #178 contract).
//
// CPU-only: no Vulkan device is created. Mesh BUILD + PUBLISH is CPU work; the
// GPU upload stage is deliberately never driven here (it is covered by
// test_mesh_upload and the visual harness against a real device).
#include <Renderer/RenderWorldBridge.hpp>
#include <Chunk/Chunk.hpp>
#include <Chunk/ChunkPool.hpp>
#include <Chunk/ChunkMeshResult.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <World/VoxelWorld.hpp>
#include <World/WorldCollisionView.hpp>
#include <Camera/Camera.hpp>
#include <Engine/Profiler.hpp>
#include <Core/ThreadPool.hpp>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
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

// Friend probe declared in RenderWorldBridge.hpp / VoxelWorld.hpp: exposes the
// commit-group set and the completed-job injection seam without public API.
struct RenderBridgeProbe
{
	static size_t commitGroups(const RenderWorldBridge &b) { return b.m_commitGroups.size(); }
	static void injectCompletedMeshJob(RenderWorldBridge &b, Chunk *c, MeshBuildResult *r)
	{
		std::lock_guard<std::mutex> lk(b.m_completedJobsMutex);
		b.m_completedMeshJobs.push_back({c, r});
	}
};

// Drain async gen/mesh/light jobs to completion, mirroring the engine's
// per-frame publish pass.
static void drainBridge(RenderWorldBridge &b)
{
	int spins = 0;
	while ((b.pendingGenJobs() > 0 || b.pendingMeshJobs() > 0 || b.pendingLightJobs() > 0) &&
		   spins++ < 30000)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		b.processFinishedJobs();
	}
	b.processFinishedJobs();
}

static RenderSettings streamSettings()
{
	RenderSettings s;
	s.minRenderDistance = 32;
	s.maxRenderDistance = 64;
	return s;
}

// Stream + load + async-generate the footprint around center (no GPU).
static void populate(RenderWorldBridge &b, const glm::vec3 &center)
{
	Camera cam(center);
	const RenderSettings s = streamSettings();
	for (int tick = 0; tick < 8; ++tick)
	{
		b.updateStreaming(cam, s);
		b.processChunkLoading(64);
		b.generatePendingVoxels(cam, s, 64);
		b.processFinishedJobs();
	}
	drainBridge(b);
}

// Dispatch + publish CPU meshes for every GENERATED chunk (no GPU upload).
static void meshAll(RenderWorldBridge &b, const glm::vec3 &center)
{
	Camera cam(center);
	const RenderSettings s = streamSettings();
	for (int tick = 0; tick < 8; ++tick)
	{
		b.meshPendingChunks(cam, s, 64);
		b.processFinishedJobs();
	}
	drainBridge(b);
}

int main()
{
	// --- 1. Headless world, NO bridge registered -----------------------------
	{
		TerrainGenerator gen(42);
		ThreadPool tp(2);
		VoxelWorld world(&gen, &tp);
		WorldStreamSettings ws;
		ws.minRenderDistance = 32;
		ws.maxRenderDistance = 64;
		world.updateStreaming({0.f, 100.f, 0.f}, {0.f, 1.f}, ws);
		world.processChunkLoading(64);
		world.generatePendingVoxels({0.f, 100.f, 0.f}, 64.f * 64.f, 64);
		int spins = 0;
		while (world.asyncBusy() && spins++ < 30000)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			world.pump();
		}
		world.pump();
		CHECK(world.chunkCount() > 0, "headless world loads chunks with no bridge");
		const bool accepted = world.placeVoxel({8.5f, 200.5f, 8.5f}, STONE).accepted;
		world.pump();
		CHECK(accepted, "headless edit accepted with no bridge");
		CHECK(world.isVoxelActive({8.5f, 200.5f, 8.5f}), "headless edit applied with no bridge");
		CHECK(!world.hasPendingLogicalEdits(), "no stranded edits in headless world");
	}

	// --- 2. Bridge attach: derived projection, single source of truth --------
	{
		TerrainGenerator gen(42);
		ThreadPool tp(2);
		ChunkPool pool(256);
		VoxelWorld world(&gen, &tp);
		RenderWorldBridge bridge(&world, &tp, &pool);
		CHECK(&bridge.world() == &world, "bridge references the host world (no duplicate truth)");
		populate(bridge, {0.f, 100.f, 0.f});
		CHECK(bridge.chunkCount() == world.chunkCount(), "render handles mirror world residency");
		CHECK(bridge.getChunk({0, 0, 0}) != nullptr, "render handle bound at origin");
		const bool ok = bridge.placeVoxel({8.5f, 200.5f, 8.5f}, STONE);
		bridge.processFinishedJobs();
		CHECK(ok, "bridge edit accepted");
		CHECK(world.isVoxelActive({8.5f, 200.5f, 8.5f}), "bridge edit is authoritative in the world");
		meshAll(bridge, {0.f, 100.f, 0.f});
		Chunk *c = bridge.getChunk({0, 0, 0});
		CHECK(c && c->getState() == ChunkState::MESHED, "bridge CPU mesh published");
		CHECK(c && c->hasPendingMeshResult(), "mesh result parked awaiting GPU upload");
	}

	// --- 3. Mesh publish validates sim::ChunkIdentity (stale discarded) ------
	{
		TerrainGenerator gen(7);
		ThreadPool tp(2);
		ChunkPool pool(128);
		VoxelWorld world(&gen, &tp);
		RenderWorldBridge bridge(&world, &tp, &pool);
		populate(bridge, {0.f, 100.f, 0.f});
		meshAll(bridge, {0.f, 100.f, 0.f});
		Chunk *c = bridge.getChunk({0, 0, 0});
		CHECK(c && c->getState() == ChunkState::MESHED, "mesh published before stale injection");
		if (c)
		{
			const size_t activeBefore = c->getMeshResultPool()->activeCount();
			MeshBuildResult *stale = c->getMeshResultPool()->acquire();
			// Wrong incarnation: a recycled-generation stamp must be refused.
			stale->beginBuild(c, c->meshGeneration() + 999, c->meshRevision());
			RenderBridgeProbe::injectCompletedMeshJob(bridge, c, stale);
			bridge.processFinishedJobs();
			CHECK(c->getMeshResultPool()->activeCount() == activeBefore, "stale result returned to its pool");
			CHECK(c->hasPendingMeshResult(), "committed pending mesh intact after stale rejection");
			CHECK(c->getState() == ChunkState::MESHED, "stale publish leaves chunk MESHED");
		}
	}

	// --- 4. Border edit registers an atomic geometric commit group -----------
	{
		TerrainGenerator gen(42);
		ThreadPool tp(2);
		ChunkPool pool(256);
		VoxelWorld world(&gen, &tp);
		RenderWorldBridge bridge(&world, &tp, &pool);
		populate(bridge, {0.f, 100.f, 0.f});
		meshAll(bridge, {0.f, 100.f, 0.f});
		const size_t groupsBefore = RenderBridgeProbe::commitGroups(bridge);
		// x = CHUNK_SIZE-1 mirrors into the +x neighbor: geometry couples.
		const bool ok = bridge.placeVoxel({15.5f, 200.5f, 8.5f}, STONE);
		CHECK(ok, "border edit accepted");
		CHECK(RenderBridgeProbe::commitGroups(bridge) == groupsBefore + 1,
			  "border edit registers an atomic commit group");
	}

	// --- 5. Detach: world outlives bridge; re-attach projects the live world -
	{
		TerrainGenerator gen(42);
		ThreadPool tp(2);
		ChunkPool pool(256);
		VoxelWorld world(&gen, &tp);
		{
			RenderWorldBridge bridge(&world, &tp, &pool);
			populate(bridge, {0.f, 100.f, 0.f});
			meshAll(bridge, {0.f, 100.f, 0.f});
			CHECK(world.chunkCount() > 0, "world populated before detach");
		} // ~RenderWorldBridge: releases handles, never owns the world.
		CHECK(world.chunkCount() > 0, "world residency survives bridge destruction");
		const bool ok = world.placeVoxel({8.5f, 200.5f, 8.5f}, STONE).accepted;
		world.pump();
		CHECK(ok && world.isVoxelActive({8.5f, 200.5f, 8.5f}), "world edits after bridge detach");

		RenderWorldBridge bridge2(&world, &tp, &pool);
		CHECK(bridge2.chunkCount() == world.chunkCount(), "reattached bridge binds existing residents");
		CHECK(bridge2.getChunk({0, 0, 0}) != nullptr, "reattached handle at origin");
		CHECK(bridge2.placeVoxel({8.5f, 201.5f, 8.5f}, STONE), "reattached edit accepted");
		meshAll(bridge2, {0.f, 100.f, 0.f});
		Chunk *c = bridge2.getChunk({0, 0, 0});
		CHECK(c && c->getState() == ChunkState::MESHED && c->hasPendingMeshResult(),
			  "reattached bridge rebuilds the derived mesh");
	}

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed (" << g_checks << " run)\n";
		return 1;
	}
	std::cout << "PASS: render bridge boundary (" << g_checks << " checks)" << std::endl;
	return 0;
}
