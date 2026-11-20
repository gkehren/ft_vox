#include <Network/ClientTransport.hpp>
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
	CHECK_EXPR(restored.itemIdHighWater() > 1 && restored.itemEntities().size() == 1 &&
			   restored.itemEntities().entities().front().id == 1);
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
	CHECK_EXPR(error.starts_with("save barrier or checkpoint commit failed"));
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

void testAnchorParticipation(const fs::path &root)
{
	server::HostConfig config;
	config.world = {.seed = 4, .worldName = "anchor_participation", .savesRoot = root};
	config.workers = 0;
	config.interestRadiusChunks = 2;
	server::ServerHost host(config);
	std::string error;
	if (!host.start(error))
	{
		CHECK(false, error);
		return;
	}
	auto &sim = server::ServerHostProbe::simulation(host);
	auto &items = const_cast<items::ItemEntitySystem &>(sim.itemEntities());
	auto &mobs = const_cast<entities::MobSystem &>(sim.mobs());
	const auto step = [&] { CHECK(host.step(server::TerrainTiming::QuiescePerTick, error), error); };
	const auto anchorFeet = sim.playerFeet();
	CHECK_EXPR(sim.playerPositioned(sim.localPlayerId()));
	sim::InventorySnapshot anchorBefore;
	sim.inventorySnapshot(anchorBefore);
	const auto dropId = items.spawn({items::ItemId::Dirt, 3}, anchorFeet, {}, 3);
	CHECK_EXPR(dropId != 0);

	// Seed 4 has a forest chunk just north of spawn. A grass pad covers
	// every candidate column in its group so the default-policy control
	// can witness a scan with a guaranteed spawn chance.
	for (int z = -13; z <= -4; ++z)
		for (int x = 3; x <= 12; ++x)
			CHECK_EXPR(sim.placeVoxel({x + 0.5f, 244.5f, z + 0.5f}, GRASS_TOP).accepted);
	mobs.settings.spawnMin = 10;
	mobs.settings.spawnMax = 24;
	mobs.settings.spawnChance = 1;
	mobs.settings.spawnInterval = 60;
	const glm::dvec3 anchorMobFeet{anchorFeet.x, 245.001, anchorFeet.z};
	const auto support = [&](glm::dvec3 feet) {
		return sim.placeVoxel(glm::vec3(feet) - glm::vec3(0, 0.5f, 0), STONE).accepted;
	};
	const auto addMob = [&](glm::dvec3 feet, entities::EntityId id) {
		return mobs.add(entities::MobSpecies::Sheep, feet, id, id, WorldCollisionView(*sim.world()));
	};
	CHECK_EXPR(support(anchorMobFeet) && addMob(anchorMobFeet, 1));
	step();
	CHECK_EXPR(mobs.mobs().empty());
	const auto remote = host.joinRemote().player;
	CHECK_EXPR(remote.valid() && !sim.playerPositioned(remote));
	for (int i = 0; i < 5; ++i)
		step();
	CHECK_EXPR(mobs.mobs().empty());
	CHECK_EXPR(items.size() == 1);
	if (!items.entities().empty())
	{
		const auto &drop = items.entities().front();
		CHECK_EXPR(drop.id == dropId && drop.stack.count == 3 && drop.pickupDelay == 0 &&
			drop.ageTicks == host.ticks());
		CHECK_EXPR(glm::distance(drop.body.position, sim.playerFeet()) <= items.settings.pickupRadius);
	}
	sim::InventorySnapshot anchorAfter;
	sim.inventorySnapshot(anchorAfter);
	CHECK_EXPR(anchorAfter == anchorBefore);

	// With the same terrain and no positioned remote, the simulation's
	// default policy scans immediately using the local player's position.
	sim.advancePlayerMobs(server::kTickDt, config.interestRadiusChunks * CHUNK_SIZE, false);
	CHECK_EXPR(!mobs.mobs().empty());

	// Use explicit placement to test retention with observers far apart.
	mobs.settings.spawnChance = 0;
	CHECK_EXPR(host.activateRemote(remote));
	const auto remoteFeet = anchorMobFeet + glm::dvec3(128, 0, 0);
	sim.setPlayerPosition(remote, remoteFeet);
	step(); // Publish the far player's chunk before adding a mob there.
	CHECK_EXPR(addMob(anchorMobFeet, 2));
	CHECK_EXPR(support(remoteFeet) && addMob(remoteFeet, 3));
	step();
	CHECK_EXPR(mobs.mobs().size() == 1 && mobs.mobs().front().id == 3);
	host.leaveRemote(remote);
	step();
	CHECK_EXPR(mobs.mobs().empty());
	CHECK(host.stop(error), error);
}

void testMovement(const fs::path &root)
{
	namespace udp = net::udp;
	namespace wire = net::wire;
	server::HostConfig config;
	config.world = {.seed = 19, .worldName = "movement", .savesRoot = root};
	config.workers = 0;
	config.interestRadiusChunks = 2;
	config.streaming.generations = 0; // preserve an unreadable chunk for the collision check
	server::ServerHost host(config);
	std::string error;
	if (!host.start(error))
	{
		CHECK(false, error);
		return;
	}
	auto &sim = server::ServerHostProbe::simulation(host);

	// A flat walking surface and a two-block wall inside the bootstrapped chunk.
	for (int x = 0; x < 16; ++x)
		for (int z = 6; z <= 10; ++z)
			CHECK_EXPR(sim.placeVoxel({x + 0.5f, 230.5f, z + 0.5f}, STONE).accepted);
	for (float y : {231.5f, 232.5f})
		CHECK_EXPR(sim.placeVoxel({10.5f, y, 8.5f}, STONE).accepted);

	udp::ServerTransport transport(udp::parseAddress("127.0.0.1", 0), 2, host.sessionWorld());
	udp::ClientTransport first(transport.address()), second(transport.address());
	const std::array clients{&first, &second};
	auto now = udp::ServerTransport::Clock::time_point{};
	const auto boundary = [&](bool discardInputs = false) {
		transport.poll(now);
		transport.serviceOwner(host, host.ticks(), discardInputs);
		transport.poll(now);
		for (auto *client : clients)
			client->poll(now);
	};
	for (auto *client : clients)
	{
		client->connect(now);
		client->poll(now);
	}
	boundary(); // Welcome, then the clients send confirmation heartbeats.
	if (!first.welcome() || !second.welcome())
	{
		CHECK(false, "movement clients did not join");
		return;
	}
	const std::array players{first.welcome()->player, second.welcome()->player};
	for (const auto id : players)
		CHECK_EXPR(!sim.playerPositioned(id));
	boundary(); // The tick owner now spawns both walking bodies.
	CHECK_EXPR(first.latestPlayerSnapshot() && first.latestPlayerSnapshot()->count == 2);
	const auto originalScope = *first.scope();

	std::array<physics::PlayerController, 2> reference;
	for (size_t i = 0; i < players.size(); ++i)
	{
		CHECK_EXPR(sim.playerPositioned(players[i]) && !sim.playerFlight(players[i]));
		reference[i].reset(sim.playerFeet(players[i]));
		CHECK_EXPR(reference[i].body.position.x == 8.5 && reference[i].body.position.y > 231.0);
	}

	const auto sampleFor = [](size_t index, uint64_t tick) -> std::optional<wire::Input> {
		// Model two consecutive lost samples. No retransmission or held input.
		if (tick < 3 || tick > 40 || tick == 14 || tick == 15)
			return std::nullopt;
		const uint8_t flags = index == 0 ? wire::Input::sprint : (tick == 33 ? wire::Input::jump : 0);
		return wire::Input{tick, index == 0 ? 1.f : -1.f, 0, flags};
	};
	std::array<uint64_t, 2> expectedAck{};
	bool observedJump = false;
	const auto stepAgainst = [&](const std::array<physics::PlayerInput, 2> &commands) {
		{
			WorldCollisionView world(*sim.world());
			for (size_t i = 0; i < players.size(); ++i)
				reference[i].advance(server::kTickDt, commands[i], world);
		}
		CHECK(host.step(server::TerrainTiming::QuiescePerTick, error), error);
		now += 17ms;
		for (size_t i = 0; i < players.size(); ++i)
		{
			sim::PlayerSnapshot actual;
			CHECK_EXPR(sim.playerSnapshot(players[i], actual));
			CHECK_EXPR(actual.position == reference[i].body.position && actual.velocity == reference[i].body.velocity);
			CHECK_EXPR(actual.grounded == reference[i].body.grounded &&
				actual.waitingForTerrain == reference[i].body.waitingForTerrain);
			CHECK_EXPR(!actual.flight && actual.metrics.steps == 2 && actual.metrics.droppedSteps == 0);
			CHECK_EXPR(host.snapshotRemote(players[i]).lastProcessedInput == expectedAck[i]);
			if (i == 1 && actual.velocity.y > 0)
				observedJump = true;
		}
	};
	for (uint64_t tick = 1; tick <= 45; ++tick)
	{
		std::array<physics::PlayerInput, 2> commands{};
		for (size_t i = 0; i < players.size(); ++i)
		{
			if (const auto future = sampleFor(i, tick + 2))
				CHECK_EXPR(clients[i]->sendInput(*future, now));
			if (const auto current = sampleFor(i, tick))
			{
				commands[i].move = {current->moveX, 0};
				commands[i].sprint = i == 0;
				commands[i].jumpPressed = i == 1 && tick == 33;
				expectedAck[i] = expectedAck[i] == 0 ? 2 : expectedAck[i] + 1; // heartbeat is sequence 1
			}
		}
		boundary();
		stepAgainst(commands);
	}
	boundary();
	CHECK_EXPR(observedJump);
	CHECK_EXPR(sim.playerFeet(players[0]).x > 9.5 && sim.playerFeet(players[0]).x < 10.0);
	CHECK_EXPR(sim.playerFeet(players[1]).x < 7.5);
	CHECK_EXPR(first.scope() == originalScope && first.latestPlayerSnapshot() == host.snapshotRemote(players[0]));
	CHECK_EXPR(second.latestPlayerSnapshot() == host.snapshotRemote(players[1]));
	if (const auto latest = first.latestPlayerSnapshot())
	{
		CHECK_EXPR(latest->players[0].player == players[0] && latest->players[1].player == players[1]);
		CHECK_EXPR((latest->players[0].flags & wire::PlayerState::grounded) != 0);
		CHECK_EXPR((latest->players[0].flags & wire::PlayerState::flight) == 0);
	}

	// Queue a future jump in the host and another in the IO mailbox. The
	// scheduler's stall reset discards both and resumes with two idle steps.
	CHECK_EXPR(first.sendInput({host.ticks() + 3, -1, 0, wire::Input::jump}, now));
	boundary();
	CHECK_EXPR(first.sendInput({host.ticks() + 4, -1, 0, wire::Input::jump}, now));
	transport.poll(now);
	now += 500ms;
	host.suspendPlayerInput();
	for (auto &controller : reference)
		controller.suspend();
	boundary(true);
	for (uint64_t i = 0; i < udp::kInputLeadTicks; ++i)
	{
		stepAgainst({});
		boundary();
	}

	CHECK_EXPR(first.sendInput({host.ticks() + 3, -1, 0, wire::Input::jump}, now));
	boundary();
	first.disconnect(now);
	first.poll(now);
	boundary();
	CHECK_EXPR(!sim.hasPlayer(players[0]) && !first.latestPlayerSnapshot());
	first.connect(now);
	first.poll(now);
	boundary();
	boundary();
	CHECK_EXPR(first.welcome() && first.welcome()->player == players[0]);
	CHECK_EXPR(first.scope() && first.scope()->epoch == originalScope.epoch && *first.scope() != originalScope);
	CHECK_EXPR(first.latestPlayerSnapshot() && first.latestPlayerSnapshot()->lastProcessedInput == 0);

	// An active player in an unloaded chunk stays put and publishes the
	// existing unknown-terrain state. The spawn-area peer cannot see them.
	const glm::dvec3 unknownFeet{100.5, 231.0, 8.5};
	sim.setPlayerPosition(players[1], unknownFeet);
	CHECK(host.step(server::TerrainTiming::QuiescePerTick, error), error);
	const auto waiting = host.snapshotRemote(players[1]);
	CHECK_EXPR(waiting.count == 1 && waiting.players[0].player == players[1]);
	CHECK_EXPR((waiting.players[0].flags & wire::PlayerState::waitingForTerrain) != 0);
	CHECK_EXPR(sim.playerFeet(players[1]) == unknownFeet);

	transport.stop(host);
	CHECK_EXPR(sim.playerCount() == 1);

	// A crowded snapshot reserves room for its owner even when their ID
	// sorts after every other eligible player.
	std::array<sim::PlayerId, wire::kMaxPlayersPerSnapshot + 2> crowd;
	for (auto &id : crowd)
	{
		id = host.joinRemote().player;
		CHECK_EXPR(id.valid() && host.activateRemote(id));
	}
	const auto capped = host.snapshotRemote(crowd.back());
	CHECK_EXPR(capped.count == wire::kMaxPlayersPerSnapshot);
	CHECK_EXPR(capped.players[wire::kMaxPlayersPerSnapshot - 1].player == crowd.back());
	for (size_t i = 0; i + 1 < capped.count; ++i)
		CHECK_EXPR(capped.players[i].player == crowd[i]);

	CHECK_EXPR(sim.placeVoxel({8.5f, 232.5f, 8.5f}, STONE).accepted);
	CHECK_EXPR(sim.breakBlock(crowd.front(), {8, 232, 8}) == sim::BreakResult::Ok);
	for (int i = 0; i < 40; ++i)
		CHECK(host.step(server::TerrainTiming::QuiescePerTick, error), error);
	CHECK_EXPR(sim.itemEntities().entities().empty());
	CHECK_EXPR((sim.inventory(crowd.front())->slots()[0] == items::ItemStack{items::ItemId::Cobblestone, 1}));
	for (const auto id : std::span(crowd).subspan(1))
		CHECK_EXPR(sim.inventory(id)->slots()[0].isEmpty());
	for (const auto id : crowd)
		host.leaveRemote(id);
	CHECK(host.stop(error), error);
}
} // namespace

int main()
{
	testSchedule();
	const auto temp = fs::temp_directory_path() /
		("ft_vox_server_host_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	testHost(temp);
	testAnchorParticipation(temp);
	testMovement(temp);
	std::error_code error;
	fs::remove_all(temp, error);
	CHECK_EXPR(!error);
	return testmain::exitCode();
}
