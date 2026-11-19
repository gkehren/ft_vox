#include <Server/ServerHost.hpp>
#include <Server/TickSchedule.hpp>
#include <World/WorldCollisionView.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "TestMain.hpp"

namespace server
{
struct ServerHostProbe
{
	static sim::GameSimulation &simulation(ServerHost &host) { return *host.m_sim; }
};
}

namespace
{
namespace fs = std::filesystem;
using namespace std::chrono_literals;

void testSchedule()
{
	using server::TickSchedule;
	const auto start = TickSchedule::Clock::time_point{} + 2s;
	TickSchedule schedule(start, 4);
	CHECK_EXPR(schedule.takeDue(start).run == 0);
	CHECK_EXPR(schedule.takeDue(schedule.nextDeadline() - TickSchedule::Clock::duration{1}).run == 0);
	const auto first = schedule.takeDue(schedule.nextDeadline());
	CHECK_EXPR(first.run == 1 && first.dropped == 0);
	const auto stalled = schedule.takeDue(start + 1s);
	CHECK_EXPR(stalled.run == 4 && stalled.dropped == 55);
	CHECK_EXPR(schedule.takeDue(start + 1s).run == 0);
	const auto recovered = schedule.takeDue(schedule.nextDeadline());
	CHECK_EXPR(recovered.run == 1 && recovered.dropped == 0);
	const auto suspended = schedule.takeDue(start + 24h);
	CHECK_EXPR(suspended.run == 4 && suspended.dropped == 86400u * 60u - 61u - 4u);

	TickSchedule regular(start, 4);
	for (int i = 0; i < 60; ++i)
	{
		const auto tick = regular.takeDue(regular.nextDeadline());
		CHECK_EXPR(tick.run == 1 && tick.dropped == 0);
	}
	CHECK_EXPR(regular.nextDeadline() == start + 1s + std::chrono::ceil<TickSchedule::Clock::duration>(
		TickSchedule::Tick{1}));
}

TextureType sample(const VoxelWorld &world, glm::ivec3 cell)
{
	TextureType type = AIR;
	CHECK_EXPR(WorldCollisionView(world).sampleWithType(cell, &type).available);
	return type;
}

std::string readBytes(const fs::path &path)
{
	std::ifstream file(path, std::ios::binary);
	CHECK_EXPR(file.is_open());
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void testHost(const fs::path &root)
{
	server::HostConfig config;
	config.world = {.seed = 19, .worldName = "shared", .savesRoot = root};
	config.streaming.generations = 0; // strand edits deliberately at shutdown
	server::ServerHost first(config);
	std::string error;
	const auto step = server::TerrainTiming::QuiescePerTick;
	if (!first.start(error))
	{
		CHECK(false, error);
		return;
	}
	CHECK_EXPR(!first.restoredPlayer());
	auto &sim = server::ServerHostProbe::simulation(first);
	CHECK_EXPR(sim.grantItems(items::ItemId::Dirt, 3).result == items::ItemResult::Ok);
	CHECK_EXPR(sim.dropFromSlot(0, 1, {0.0, 0.0, 1.0}) == items::ItemResult::Ok);
	for (int i = 0; i < 4; ++i)
		CHECK(first.step(step, error), error);
	CHECK_EXPR(first.ticks() == 4);
	CHECK_EXPR(sim.playerSnapshot().metrics.steps == 2);
	CHECK_EXPR(sim.itemEntities().entities().size() == 1);
	if (!sim.itemEntities().entities().empty())
		CHECK_EXPR(sim.itemEntities().entities().front().ageTicks == 4);
	const WorldChunk *deferred = nullptr;
	for (const auto *chunk : sim.world()->activeChunksSnapshot())
		if (!chunk->isVoxelBackingReadable())
		{
			deferred = chunk;
			break;
		}
	CHECK_EXPR(deferred != nullptr);
	if (!deferred)
		return;
	const glm::ivec3 remote{deferred->coord().x * CHUNK_SIZE + 8, 250,
		deferred->coord().z * CHUNK_SIZE + 8};
	CHECK_EXPR(sim.placeVoxel(glm::vec3(remote) + 0.5f, GLASS).accepted);
	CHECK_EXPR(sim.world()->hasPendingLogicalEdits());
	CHECK(first.stop(error), error);
	CHECK_EXPR(first.metrics().pendingGeneration == 0);
	CHECK_EXPR(first.metrics().checkpointGeneration != 0);

	config.world.seed = 0; // use the seed stored at cold start
	config.streaming.generations = 8;
	server::ServerHost reopened(config);
	if (!reopened.start(error))
	{
		CHECK(false, error);
		return;
	}
	CHECK_EXPR(reopened.restoredPlayer() && reopened.metrics().seed == 19);
	auto &restored = server::ServerHostProbe::simulation(reopened);
	CHECK_EXPR(restored.inventory().slots()[0].count == 2);
	CHECK_EXPR(restored.itemIdHighWater() == 1);
	for (int i = 0; i < 4; ++i)
		CHECK(reopened.step(step, error), error);
	CHECK_EXPR(sample(*restored.world(), remote) == GLASS);

	// Force a real SaveService write failure after a checkpoint already
	// exists. Both the explicit barrier and teardown fallback must refuse
	// to publish a generation containing only part of the accepted state.
	const auto manifest = worldsave::checkpointManifestPath(root / "shared");
	const std::string before = readBytes(manifest);
	const glm::ivec3 dirty{8, 250, 8};
	CHECK_EXPR(restored.placeVoxel(glm::vec3(dirty) + 0.5f, STONE).accepted);
	restored.worldPersistence()->setWriteTmpFnForTests(
		[](const auto &, int32_t, int32_t, const auto &, auto &) { return worldsave::SaveStatus::IoError; });
	CHECK_EXPR(!reopened.stop(error));
	CHECK_EXPR(error == "save barrier or checkpoint commit failed");
	CHECK_EXPR(readBytes(manifest) == before);

	server::ServerHost recovered(config);
	error.clear();
	if (!recovered.start(error))
	{
		CHECK(false, error);
		return;
	}
	CHECK_EXPR(sample(*server::ServerHostProbe::simulation(recovered).world(), dirty) == AIR);
	CHECK(recovered.stop(error), error);
}
} // namespace

int main()
{
	testSchedule();
	const auto temp = fs::temp_directory_path() /
		("ft_vox_server_host_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	testHost(temp);
	std::error_code error;
	fs::remove_all(temp, error);
	CHECK_EXPR(!error);
	return testmain::exitCode();
}
