#include <Sim/GameSimulation.hpp>
#include <World/WorldCollisionView.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <limits>
#include <utility>
#include <vector>

#include "TestMain.hpp"

struct VoxelWorldProbe
{
	static void injectCompletion(VoxelWorld &world, WorldChunk *chunk, uint64_t generation)
	{
		std::lock_guard lock(world.m_completedJobsMutex);
		world.m_completedGenerationChunks.push_back({chunk, generation, true});
	}
};

namespace
{
using sim::GameSimulation;
using sim::PlayerId;

constexpr glm::ivec3 origin{0, 0, 0};
constexpr glm::ivec3 neighbor{1, 0, 0};
constexpr glm::ivec3 farAway{80, 0, -90};
constexpr glm::vec3 borderVoxel{15.5f, 250.5f, 8.5f};
constexpr glm::ivec3 solidCell{8, 0, 8};
constexpr glm::ivec3 editCell{8, 250, 8};

glm::dvec3 feetAt(glm::ivec3 chunk)
{
	return {chunk.x * CHUNK_SIZE + 8.0, 250.0, chunk.z * CHUNK_SIZE + 8.0};
}

void observe(GameSimulation &sim, PlayerId id, glm::ivec3 chunk, int radius = 1)
{
	sim.setPlayerPosition(id, feetAt(chunk));
	CHECK_EXPR(sim.setPlayerInterestRadius(id, radius));
}

bool readable(const VoxelWorld &world, glm::ivec3 coord)
{
	return WorldCollisionView(world).sample(
		{coord.x * CHUNK_SIZE + solidCell.x, solidCell.y, coord.z * CHUNK_SIZE + solidCell.z}).available;
}

glm::vec3 cellCenter(glm::ivec3 cell)
{
	return glm::vec3(cell) + 0.5f;
}

void checkTerrain(const VoxelWorld &world, glm::ivec3 cell, TextureType expected)
{
	TextureType type = AIR;
	{
		WorldCollisionView view(world);
		const auto sampled = view.sample(cell);
		const auto typed = view.sampleWithType(cell, &type);
		const auto expectedCell = physics::blockCell(expected);
		CHECK_EXPR(sampled.available && typed.available);
		CHECK_EXPR(sampled.solid == expectedCell.solid && typed.solid == expectedCell.solid);
		CHECK_EXPR(sampled.medium == expectedCell.medium && typed.medium == expectedCell.medium);
		CHECK_EXPR(type == expected);
	}
	CHECK_EXPR(world.isVoxelActive(cellCenter(cell)) == (expected != AIR));
}

void checkUnknown(const VoxelWorld &world, glm::ivec3 cell)
{
	{
		WorldCollisionView view(world);
		CHECK_EXPR(!view.sample(cell).available);
		CHECK_EXPR(!view.sampleWithType(cell, nullptr).available);
	}
	CHECK_EXPR(!world.isVoxelActive(cellCenter(cell)));
}

void asyncTerrainPublicationAndRecycling()
{
	ThreadPool pool(1);
	GameSimulation sim(&pool);
	sim.initialize({.seed = 42, .worldName = {}}, {});
	const auto local = sim.localPlayerId();
	auto &world = *sim.world();
	observe(sim, local, origin);
	CHECK_EXPR(sim.advancePlayerStreaming({1, 0, 0}));
	auto *record = world.getChunk(origin);
	CHECK_EXPR(record != nullptr);
	if (!record)
		return;
	const auto originalGeneration = record->generation();
	checkUnknown(world, solidCell);

	std::promise<void> unblockFirst;
	auto firstReady = unblockFirst.get_future();
	auto firstBlocker = pool.enqueue(TaskPriority::High, [&] { firstReady.wait(); });
	CHECK_EXPR(sim.advancePlayerStreaming({0, 1, 0}));
	CHECK_EXPR(record->isInTransit());
	checkUnknown(world, solidCell);
	unblockFirst.set_value();
	firstBlocker.get();
	pool.enqueue(TaskPriority::Low, [] {}).get();
	CHECK_EXPR(record->isVoxelBackingReadable() && record->isInTransit());
	checkUnknown(world, solidCell);
	sim.pumpWorld();
	CHECK_EXPR(!record->isInTransit());
	checkTerrain(world, solidCell, BEDROCK);
	world.takeEvents();

	observe(sim, local, farAway);
	CHECK_EXPR(sim.advancePlayerStreaming({1, 0, 1}));
	CHECK_EXPR(!world.getChunk(origin));
	auto *recycled = world.getChunk(farAway);
	CHECK_EXPR(recycled == record && recycled->generation() != originalGeneration);
	if (recycled != record)
		return;
	const glm::ivec3 recycledCell = solidCell + glm::ivec3(farAway.x * CHUNK_SIZE, 0, farAway.z * CHUNK_SIZE);
	checkUnknown(world, solidCell);
	checkUnknown(world, recycledCell);

	std::promise<void> unblockSecond;
	auto secondReady = unblockSecond.get_future();
	auto secondBlocker = pool.enqueue(TaskPriority::High, [&] { secondReady.wait(); });
	CHECK_EXPR(sim.advancePlayerStreaming({0, 1, 0}));
	VoxelWorldProbe::injectCompletion(world, recycled, originalGeneration);
	world.publishFinishedGeneration();
	CHECK_EXPR(recycled->isInTransit());
	checkUnknown(world, recycledCell);
	CHECK_EXPR(std::ranges::none_of(world.takeEvents(), [](const WorldEvent &event) {
		return event.kind == WorldEventKind::Generated;
	}));
	unblockSecond.set_value();
	secondBlocker.get();
	pool.enqueue(TaskPriority::Low, [] {}).get();
	checkUnknown(world, recycledCell);
	sim.pumpWorld();
	checkTerrain(world, recycledCell, BEDROCK);
}

void publishedTerrainDuringWorkerLease()
{
	GameSimulation sim(nullptr);
	sim.initialize({.seed = 42, .worldName = {}}, {});
	const auto local = sim.localPlayerId();
	auto &world = *sim.world();
	CHECK_EXPR(world.generateInitialAreaVoxels(glm::vec3(feetAt(origin)), 0).size() == 1);
	observe(sim, local, origin);
	CHECK_EXPR(sim.advancePlayerStreaming({0, 0, 0}));
	auto *record = world.getChunk(origin);
	CHECK_EXPR(record != nullptr);
	if (!record)
		return;
	checkTerrain(world, solidCell, BEDROCK);
	checkTerrain(world, editCell, AIR);
	world.takeEvents();

	record->setInTransit(true); // Mesh/light can hold a lease on already published terrain.
	checkTerrain(world, solidCell, BEDROCK);
	checkTerrain(world, editCell, AIR);
	CHECK_EXPR(!world.placeVoxel(cellCenter(solidCell), STONE).accepted);
	CHECK_EXPR(world.placeVoxel(cellCenter(editCell), STONE).accepted);
	CHECK_EXPR(!world.placeVoxel(cellCenter(editCell), STONE).accepted);
	CHECK_EXPR(world.hasPendingLogicalEdits());
	checkTerrain(world, editCell, STONE);
	CHECK_EXPR(record->getVoxel(editCell.x, editCell.y, editCell.z).type == AIR);
	CHECK_EXPR(world.takeEvents().empty());
	CHECK_EXPR(world.playerStreamingStats().transientOverrideVoxels == 0);

	CHECK_EXPR(sim.setPlayerInterestRadius(local, 0));
	CHECK_EXPR(sim.advancePlayerStreaming({0, 0, 1}));
	CHECK_EXPR(world.getChunk(origin) == record);
	CHECK_EXPR(world.hasPendingLogicalEdits());
	checkTerrain(world, editCell, STONE);

	record->setInTransit(false);
	sim.pumpWorld();
	sim.pumpWorld();
	CHECK_EXPR(!world.hasPendingLogicalEdits());
	checkTerrain(world, solidCell, BEDROCK);
	checkTerrain(world, editCell, STONE);
	CHECK_EXPR(record->getVoxel(editCell.x, editCell.y, editCell.z).type == STONE);
	const auto events = world.takeEvents();
	CHECK_EXPR(events.size() == 1);
	CHECK_EXPR(events.size() == 1 && events[0].kind == WorldEventKind::EditApplied &&
			   !events[0].borderNeighbor && events[0].coord == origin);
	CHECK_EXPR(world.playerStreamingStats().transientOverrideChunks == 1);
	CHECK_EXPR(world.playerStreamingStats().transientOverrideVoxels == 1);

	CHECK_EXPR(world.deleteVoxel(cellCenter(editCell)).accepted);
	CHECK_EXPR(world.playerStreamingStats().transientOverrideVoxels == 1);
	CHECK_EXPR(sim.advancePlayerStreaming({0, 0, 1}));
	CHECK_EXPR(!world.getChunk(origin));
	CHECK_EXPR(world.playerStreamingStats().transientOverrideChunks == 1);
	CHECK_EXPR(world.playerStreamingStats().transientOverrideVoxels == 1);
	CHECK_EXPR(sim.reload(43));
	CHECK_EXPR(sim.world()->playerStreamingStats().transientOverrideChunks == 0);
	CHECK_EXPR(sim.world()->playerStreamingStats().transientOverrideVoxels == 0);
}

void distantRegionsAndValidation()
{
	GameSimulation sim(nullptr);
	const auto local = sim.localPlayerId();
	const auto peer = sim.createPlayer();
	CHECK_EXPR(!sim.advancePlayerStreaming());
	sim.initialize({.seed = 42, .worldName = {}}, {});
	observe(sim, local, origin);
	CHECK_EXPR(!sim.setPlayerInterestRadius(local, -1));
	CHECK_EXPR(!sim.setPlayerInterestRadius(local, sim::kMaxPlayerInterestRadiusChunks + 1));
	CHECK_EXPR(!sim.setPlayerInterestRadius({}, 1));
	CHECK_EXPR(sim.setPlayerInterestRadius(peer, 1));
	CHECK_EXPR(sim.advancePlayerStreaming({0, 0, 0}));
	CHECK_EXPR(sim.world()->playerStreamingStats().observers == 1);

	observe(sim, peer, farAway);
	CHECK_EXPR(sim.advancePlayerStreaming({1, 1, 1}));
	CHECK_EXPR(readable(*sim.world(), origin));
	CHECK_EXPR(!sim.world()->getChunk(farAway));
	CHECK_EXPR(sim.advancePlayerStreaming({1, 1, 1}));
	CHECK_EXPR(readable(*sim.world(), farAway));
	CHECK_EXPR(sim.world()->playerStreamingStats().desiredChunks == 10);
	CHECK_EXPR(sim.world()->playerStreamingStats().waitingForRecord == 8);
	CHECK_EXPR(sim.advancePlayerStreaming({32, 32, 32}));
	const auto *keep = sim.world()->getChunk(origin);
	CHECK_EXPR(sim.removePlayer(peer));
	CHECK_EXPR(!sim.setPlayerInterestRadius(peer, 1));
	CHECK_EXPR(sim.advancePlayerStreaming({0, 0, 2}));
	CHECK_EXPR(sim.world()->playerStreamingStats().evicted == 2);
	CHECK_EXPR(sim.world()->playerStreamingStats().outsideInterest == 3);
	CHECK_EXPR(sim.advancePlayerStreaming({0, 0, 32}));
	CHECK_EXPR(sim.world()->chunkCount() == 5);
	CHECK_EXPR(sim.world()->getChunk(origin) == keep && readable(*sim.world(), origin));
	CHECK_EXPR(!sim.world()->getChunk(farAway));

	for (double badX : {32767.0, std::numeric_limits<double>::infinity(),
						std::numeric_limits<double>::quiet_NaN()})
	{
		sim.setPlayerPosition(local, {badX, 0, 0});
		CHECK_EXPR(sim.advancePlayerStreaming({0, 0, 32}));
		CHECK_EXPR(sim.world()->playerStreamingStats().observers == 0);
	}
	const auto epoch = sim.worldGenerationId();
	CHECK_EXPR(sim.reload(43));
	CHECK_EXPR(sim.worldGenerationId() == epoch + 1);
	CHECK_EXPR(sim.world()->playerStreamingStats().desiredChunks == 0);
	observe(sim, local, {-2048, 0, -2048});
	CHECK_EXPR(sim.advancePlayerStreaming({32, 32, 32}));
	CHECK_EXPR(sim.world()->playerStreamingStats().desiredChunks == 3);
}

using Dispatch = std::vector<std::pair<WorldEventKind, glm::ivec3>>;

Dispatch crossingSequence(ThreadPool *pool)
{
	GameSimulation sim(pool);
	sim.initialize({.seed = 42, .worldName = {}}, {});
	const auto local = sim.localPlayerId();
	const auto peer = sim.createPlayer();
	observe(sim, local, origin);
	observe(sim, peer, origin);
	auto &world = *sim.world();
	Dispatch dispatch;
	const auto tick = [&](PlayerStreamingBudget budget) {
		CHECK_EXPR(sim.advancePlayerStreaming(budget));
		if (pool)
		{
			// Single worker. The low-priority barrier follows the normal
			// terrain work regardless of where it was queued.
			pool->enqueue(TaskPriority::Low, [] {}).get();
			sim.pumpWorld();
		}
		for (const auto &event : world.takeEvents())
			if (event.kind != WorldEventKind::Generated)
				dispatch.emplace_back(event.kind, event.coord);
	};
	tick({32, 32, 32});
	CHECK_EXPR(world.playerStreamingStats().desiredChunks == 5);
	CHECK_EXPR(world.playerStreamingStats().loaded == 5);
	CHECK_EXPR(world.playerStreamingStats().generationStarts == 5);
	CHECK_EXPR(dispatch.size() == 10);
	const auto *stable = world.getChunk(origin);

	observe(sim, peer, neighbor);
	for (int i = 0; i < 3; ++i)
		tick({1, 1, 1});
	CHECK_EXPR(world.chunkCount() == 8);
	const auto beforeCross = dispatch.size();
	observe(sim, local, neighbor);
	observe(sim, peer, origin);
	tick({32, 32, 32});
	CHECK_EXPR(dispatch.size() == beforeCross);
	CHECK_EXPR(sim.removePlayer(peer));
	tick({32, 32, 32});
	CHECK_EXPR(world.chunkCount() == 5);
	CHECK_EXPR(world.getChunk(origin) == stable && readable(world, origin));
	return dispatch;
}

void strandedBorderEdit(const GameSimulation::Config &config)
{
	GameSimulation sim(nullptr);
	sim.initialize(config, {});
	const auto local = sim.localPlayerId();
	const auto peer = sim.createPlayer();
	observe(sim, local, origin);
	observe(sim, peer, neighbor);
	auto &world = *sim.world();
	CHECK_EXPR(sim.advancePlayerStreaming({2, 0, 0}));
	CHECK_EXPR(!WorldCollisionView(world).sample({15, 250, 8}).available);
	const auto edit = world.placeVoxel(borderVoxel, STONE);
	CHECK_EXPR(edit.accepted && edit.geometricMembers.size() == 2);
	CHECK_EXPR(world.hasPendingLogicalEdits());

	CHECK_EXPR(sim.setPlayerInterestRadius(local, 0));
	CHECK_EXPR(sim.setPlayerInterestRadius(peer, 0));
	CHECK_EXPR(sim.advancePlayerStreaming({8, 0, 8}));
	CHECK_EXPR(world.playerStreamingStats().pinnedEdits == 1);
	CHECK_EXPR(world.getChunk(origin) != nullptr);
	CHECK_EXPR(!world.getChunk(neighbor)); // retire the original mirror record
	observe(sim, peer, {2, 0, 0}); // re-register its coordinate outside the target's interest
	for (int i = 0; i < 8; ++i)
		CHECK_EXPR(sim.advancePlayerStreaming({8, 1, 8}));
	CHECK_EXPR(!world.hasPendingLogicalEdits());
	CHECK_EXPR(!world.getChunk(origin));
	auto *mirror = world.getChunk(neighbor);
	CHECK_EXPR(mirror && mirror->sampleForMeshing(-1, 250, 8) == STONE);
	CHECK_EXPR(!WorldCollisionView(world).sample({15, 250, 8}).available);
	const auto events = world.takeEvents();
	const auto canonical = std::ranges::find_if(events, [](const WorldEvent &event) {
		return event.kind == WorldEventKind::EditApplied && !event.borderNeighbor;
	});
	const auto retired = std::ranges::find_if(events, [](const WorldEvent &event) {
		return event.kind == WorldEventKind::Unloaded && event.coord == origin;
	});
	CHECK_EXPR(canonical < retired && retired != events.end());

	observe(sim, local, origin);
	CHECK_EXPR(sim.advancePlayerStreaming({32, 32, 32}));
	CHECK_EXPR(world.isVoxelActive(borderVoxel));
	world.ensureShellPopulated(mirror);
	CHECK_EXPR(mirror && mirror->sampleForMeshing(-1, 250, 8) == STONE);

	if (!config.worldName.empty())
	{
		CHECK_EXPR(sim.savePlayerState(0.0f, 0.0f));
		sim.shutdown();
		GameSimulation reopened(nullptr);
		reopened.initialize(config, {});
		observe(reopened, reopened.localPlayerId(), origin);
		CHECK_EXPR(reopened.advancePlayerStreaming({32, 32, 32}));
		CHECK_EXPR(reopened.world()->isVoxelActive(borderVoxel));
	}
}

void outstandingGenerationBackpressure()
{
	ThreadPool pool(1);
	GameSimulation sim(&pool);
	sim.initialize({.seed = 42, .worldName = {}}, {});
	const auto local = sim.localPlayerId();
	const auto peer = sim.createPlayer();
	observe(sim, local, origin, sim::kMaxPlayerInterestRadiusChunks);
	observe(sim, peer, neighbor);
	auto &world = *sim.world();

	std::promise<void> releaseWorker;
	auto release = releaseWorker.get_future();
	auto blocker = pool.enqueue(TaskPriority::High, [&] { release.wait(); });
	constexpr unsigned huge = std::numeric_limits<unsigned>::max();
	CHECK_EXPR(sim.advancePlayerStreaming({huge, huge, huge}));
	auto stats = world.playerStreamingStats();
	CHECK_EXPR(stats.loaded == PlayerStreamingBudget::kMaxLoads);
	CHECK_EXPR(stats.generationStarts == PlayerStreamingBudget::kMaxGenerations);
	CHECK_EXPR(stats.outstandingGenerations == PlayerStreamingBudget::kMaxOutstandingGenerations);
	auto *original = world.getChunk(origin);
	const auto generation = original ? original->generation() : 0;
	CHECK_EXPR(original && original->isInTransit());
	CHECK_EXPR(world.placeVoxel(borderVoxel, STONE).accepted);

	observe(sim, local, farAway);
	CHECK_EXPR(sim.removePlayer(peer));
	CHECK_EXPR(sim.advancePlayerStreaming({huge, huge, huge}));
	stats = world.playerStreamingStats();
	CHECK_EXPR(stats.evicted == PlayerStreamingBudget::kMaxEvictions);
	CHECK_EXPR(stats.generationStarts == 0);
	CHECK_EXPR(stats.outstandingGenerations == PlayerStreamingBudget::kMaxOutstandingGenerations);
	CHECK_EXPR(world.getChunk(origin) == original);
	CHECK_EXPR(world.hasPendingLogicalEdits());

	// Reacquire interest before the worker publishes; this is still the
	// same incarnation. Leave the other old jobs behind to retire safely.
	observe(sim, local, origin, 1);
	releaseWorker.set_value();
	blocker.get();
	pool.enqueue(TaskPriority::Low, [] {}).get();
	CHECK_EXPR(!WorldCollisionView(world).sample({15, 250, 8}).available);
	CHECK_EXPR(!world.isVoxelActive(borderVoxel));
	CHECK_EXPR(sim.advancePlayerStreaming({huge, huge, huge}));
	CHECK_EXPR(world.getChunk(origin) == original && readable(world, origin));
	CHECK_EXPR(world.isVoxelActive(borderVoxel));
	CHECK_EXPR(!world.hasPendingLogicalEdits());
	const auto events = world.takeEvents();
	for (WorldEventKind kind : {WorldEventKind::Loaded, WorldEventKind::GenQueued,
								WorldEventKind::Generated, WorldEventKind::EditApplied})
	{
		CHECK_EXPR(std::ranges::count_if(events, [=](const WorldEvent &event) {
			return event.coord == origin && event.kind == kind && event.generation == generation;
		}) == 1);
	}
	CHECK_EXPR(std::ranges::none_of(events, [](const WorldEvent &event) {
		return event.coord == origin && event.kind == WorldEventKind::Unloaded;
	}));
	CHECK_EXPR(std::ranges::any_of(events, [&](const WorldEvent &event) {
		return event.kind == WorldEventKind::Generated && event.coord != origin &&
			   std::ranges::any_of(events, [&](const WorldEvent &retired) {
				   return retired.kind == WorldEventKind::Unloaded && retired.coord == event.coord &&
						  retired.generation == event.generation;
			   });
	}));
}
}

int main()
{
	asyncTerrainPublicationAndRecycling();
	publishedTerrainDuringWorkerLease();
	distantRegionsAndValidation();
	const auto baseline = crossingSequence(nullptr);
	CHECK_EXPR(crossingSequence(nullptr) == baseline);
	ThreadPool worker(1);
	CHECK_EXPR(crossingSequence(&worker) == baseline);
	strandedBorderEdit({.seed = 42, .worldName = {}});
	const auto temp = std::filesystem::temp_directory_path() /
		("ft_vox_player_streaming_" +
		 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	strandedBorderEdit({.seed = 42, .worldName = "players", .savesRoot = temp});
	std::error_code error;
	std::filesystem::remove_all(temp, error);
	outstandingGenerationBackpressure();
	return testmain::exitCode();
}
