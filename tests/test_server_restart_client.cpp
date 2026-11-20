#include <Network/ClientTransport.hpp>
#include <Server/ServerHost.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace server
{
struct ServerHostProbe
{
	static sim::GameSimulation &simulation(ServerHost &host) { return *host.m_sim; }
	static bool startWithInitialCheckpointHook(ServerHost &host, std::string &error, void (*hook)())
	{
		return host.start(error, hook);
	}
};
}

namespace
{
namespace udp = net::udp;
namespace wire = net::wire;
using namespace std::chrono_literals;
using Item = items::ItemId;

constexpr wire::PlayerKey kAlice{1}, kBob{2}, kUncheckpointed{3}, kOther{4};
constexpr sim::PlayerId kAliceId{2}, kBobId{3};
constexpr wire::Cell kTarget{8, 8, 229};
constexpr wire::ChunkCoord kChunk{0, 0};

void require(bool condition, std::string_view error)
{
	if (!condition)
		throw std::runtime_error(std::string(error));
}

server::HostConfig migrationConfig(const std::filesystem::path &root)
{
	server::HostConfig config;
	config.world = {.seed = 19, .worldName = "migration", .savesRoot = root};
	config.workers = 0;
	return config;
}

constexpr glm::dvec3 kMigrationFeet{8.5, 230, 8.5};
const auto kMigrationEditIndex = WorldChunk::index(8, 228, 8);

void seedMigration(const std::filesystem::path &root)
{
	const auto config = migrationConfig(root);
	sim::GameSimulation solo(nullptr);
	const auto center = solo.initialize(config.world, kMigrationFeet);
	require(solo.isWorldOpen(), solo.openWorldError());
	solo.bootstrapTerrain(center);
	solo.finalizePlayerSpawn(center);
	solo.restorePlayerFlight(true, kMigrationFeet);
	solo.setSelectedBlock(GLASS);
	require(solo.grantItems(Item::Stone, 4).remainder.isEmpty(), "grant migration fixture stone");
	require(solo.selectHotbarSlot(5) == items::ItemResult::Ok, "select migration fixture hotbar");
	require(solo.dropFromSlot(0, 1, {0, 0, 1}) == items::ItemResult::Ok, "drop migration fixture item");
	require(solo.placeVoxel({8.5f, 228.5f, 8.5f}, GLASS).accepted, "edit migration fixture chunk");
	require(solo.savePlayerState(15, 5), "commit v1 migration fixture");
}

void checkMigrationSave(const WorldPersistence &persistence, bool extraItem)
{
	PlayerPersistState player;
	worldsave::InventoryPersistState inventory;
	worldsave::ItemsPersistState items;
	require(persistence.readPlayerState(player) && player.x == kMigrationFeet.x && player.y == kMigrationFeet.y &&
		player.z == kMigrationFeet.z && player.yaw == 15 && player.pitch == 5 &&
		player.flight && player.selectedBlock == GLASS, "migration changed the solo player state");
	require(persistence.readInventoryState(inventory) && inventory.selectedSlot == 5 &&
		inventory.slots[0].itemId == static_cast<uint16_t>(Item::Stone) && inventory.slots[0].count == 3,
		"migration changed the solo inventory");
	require(persistence.readItemsState(items) && items.entities.size() == (extraItem ? 2u : 1u) &&
		items.entities.front().id == 1 && items.entities.front().itemId == static_cast<uint16_t>(Item::Stone) &&
		items.entities.front().count == 1, "migration lost or duplicated the original drop");
	const auto edits = persistence.overridesSnapshot(0, 0);
	require(edits.size() == 1 && edits.front().localIndex == kMigrationEditIndex &&
		edits.front().blockType == GLASS, "migration lost or changed the solo chunk edit");
}

void openSoloMigration(const std::filesystem::path &root, bool shouldOpen)
{
	const auto config = migrationConfig(root);
	WorldPersistence persistence;
	const auto result = persistence.openOrCreate(root, config.world.worldName, config.world.seed,
		TerrainGenerator::kGeneratorVersion);
	if (shouldOpen)
	{
		require(result.ok, result.error);
		require(!persistence.checkpointWorldId(), "solo read a multiplayer checkpoint");
		checkMigrationSave(persistence, false);
	}
	else
		require(!result.ok && result.error.find("requires the dedicated server") != std::string::npos,
			"unexpected solo open result: " + result.error);
}

void completeMigration(const std::filesystem::path &root, std::string_view mode)
{
	const bool reconnect = mode == "reconnect-migration";
	server::ServerHost host(migrationConfig(root));
	std::string error;
	if (!host.start(error))
		throw std::runtime_error(error);
	auto &sim = server::ServerHostProbe::simulation(host);
	checkMigrationSave(*sim.worldPersistence(), reconnect);
	if (mode == "publish-migration")
		return; // retain the v1 fallback directly behind the first v2 generation
	const auto player = host.joinRemote(kAlice).player;
	require(player == kAliceId && host.activateRemote(player), "migration did not retain Alice's actor ID");
	const auto *inventory = sim.inventory(player);
	if (reconnect)
	{
		require(inventory->slots()[0] == items::ItemStack{Item::Dirt, 2}, "reconnect changed Alice's inventory");
		require(host.joinRemote(kBob).player.value > player.value, "reconnect reused a reserved actor ID");
		require(sim.itemIdHighWater() > sim.itemEntities().entities().back().id, "restart reused a reserved item ID");
	}
	else
	{
		require(inventory->slots()[0].isEmpty(), "remote profile inherited legacy solo inventory");
		require(sim.grantItems(player, Item::Dirt, 3).remainder.isEmpty() &&
			sim.dropFromSlot(player, 0, 1, {0, 0, 1}) == items::ItemResult::Ok, "create Alice's durable drop");
	}
	const auto &drop = sim.itemEntities().entities().back();
	require(drop.stack == items::ItemStack{Item::Dirt, 1}, "migration changed Alice's durable drop");
	std::cout << "MIGRATION world=" << host.sessionWorld().id << " item=" << drop.id << '\n';
	if (!host.stop(error))
		throw std::runtime_error(error);
}

void seed(const std::filesystem::path &root)
{
	server::HostConfig config;
	config.world = {.seed = 19, .worldName = "restart", .savesRoot = root};
	config.workers = 0;
	{
		sim::GameSimulation solo(nullptr);
		const auto center = solo.initialize(config.world, {8, 100, 8});
		require(solo.isWorldOpen(), solo.openWorldError());
		solo.bootstrapTerrain(center);
		solo.finalizePlayerSpawn(center);
		require(solo.grantItems(Item::Stone, 2).remainder.isEmpty() && solo.savePlayerState(15, 5),
			"write a v1 solo checkpoint");
	}
	server::ServerHost host(config);
	std::string error;
	if (!host.start(error))
		throw std::runtime_error(error);
	auto &sim = server::ServerHostProbe::simulation(host);
	require(host.restoredPlayer() && sim.inventory().slots()[0] == items::ItemStack{Item::Stone, 2},
		"migration changed the legacy anchor");
	require(host.joinRemote(kAlice).player == kAliceId && host.activateRemote(kAliceId), "enroll Alice");
	require(host.joinRemote(kBob).player == kBobId && host.activateRemote(kBobId), "enroll Bob");
	require(sim.inventory(kAliceId)->slots()[0].isEmpty() && sim.inventory(kBobId)->slots()[0].isEmpty(),
		"remote profile inherited solo inventory");
	sim.restorePlayerFlight(kAliceId, true, {8.5, 230, 8.5});
	sim.restorePlayerFlight(kBobId, true, {11.5, 231, 8.5});
	require(sim.grantItems(kAliceId, Item::Dirt, 5).remainder.isEmpty(), "grant Alice's stack");
	require(sim.grantItems(kBobId, Item::Glass, 6).remainder.isEmpty(), "grant Bob's stack");
	require(sim.placeVoxel({8.5f, 228.5f, 8.5f}, STONE).accepted, "place a backing block for the network action");
	host.leaveRemote(kBobId);
	if (!host.stop(error))
		throw std::runtime_error(error);
}

struct Clients
{
	udp::Address address;
	udp::ClientTransport alice{address, kAlice};
	udp::ClientTransport bob{address, kBob};

	template<class Predicate>
	void until(Predicate ready, std::string_view message, udp::ClientTransport *extra = nullptr)
	{
		const auto limit = udp::ClientTransport::Clock::now() + 25s;
		do
		{
			const auto now = udp::ClientTransport::Clock::now();
			alice.poll(now);
			bob.poll(now);
			if (extra)
				extra->poll(now);
			if (ready())
				return;
			std::this_thread::sleep_for(2ms);
		}
		while (udp::ClientTransport::Clock::now() < limit);
		throw std::runtime_error(std::string(message));
	}

	void connect(bool bobFirst)
	{
		auto &first = bobFirst ? bob : alice;
		auto &second = bobFirst ? alice : bob;
		first.connect(udp::ClientTransport::Clock::now());
		until([&] { return first.inventory().has_value(); }, "first profile did not connect");
		second.connect(udp::ClientTransport::Clock::now());
		until([&] {
			return second.inventory() && alice.chunks().contains(kChunk) &&
				alice.latestPlayerSnapshot() && bob.latestPlayerSnapshot() &&
				alice.itemSnapshots().current && bob.itemSnapshots().current;
		}, "player, inventory, chunks and item snapshots did not arrive");
		require(alice.welcome()->player == kAliceId && bob.welcome()->player == kBobId, "actor IDs changed");
		require(alice.welcome()->worldId == bob.welcome()->worldId, "clients see different world IDs");
		checkPose(alice, kAliceId, {8.5, 230, 8.5});
		checkPose(bob, kBobId, {11.5, 231, 8.5});
	}

	static void checkPose(const udp::ClientTransport &client, sim::PlayerId id, std::array<double, 3> feet)
	{
		const auto snapshot = client.latestPlayerSnapshot().value();
		const auto end = snapshot.players.begin() + snapshot.count;
		const auto actor = std::find_if(snapshot.players.begin(), end, [&](const auto &p) { return p.player == id; });
		require(actor != end && actor->feet == feet && (actor->flags & wire::PlayerState::flight), "pose or flight changed");
	}

	void action(udp::ClientTransport &client, const wire::Action &action)
	{
		require(client.sendAction(action, udp::ClientTransport::Clock::now()).has_value(), "could not queue action");
		std::optional<wire::ActionResult> result;
		until([&] { return (result = client.takeActionResult()).has_value(); }, "action did not settle");
		require(result->code == wire::ActionCode::Accepted,
				"action rejected with code " + std::to_string(static_cast<unsigned>(result->code)));
	}

	uint8_t material() const
	{
		return alice.chunks().at(kChunk).materials[(kTarget.y * 16 + kTarget.z) * 16 + kTarget.x];
	}

	void checkInventory(uint16_t aliceCount, uint16_t bobCount, int bobSelected) const
	{
		const auto a = alice.inventory().value();
		const auto b = bob.inventory().value();
		require(a.selected == 0 && a.slots[0] == items::ItemStack{Item::Dirt, aliceCount}, "Alice inventory changed");
		require(b.selected == bobSelected && b.slots[0] == items::ItemStack{Item::Glass, bobCount},
				"Bob inventory changed");
	}

	uint64_t glassDrop()
	{
		const auto snapshot = alice.itemSnapshots().current.value();
		uint64_t id{};
		for (size_t i = 0; i < snapshot.count; ++i)
		{
			const auto &item = snapshot.items[i];
			if (item.stack == items::ItemStack{Item::Glass, 1})
			{
				require(!id, "rollback duplicated a Glass drop");
				id = item.id;
			}
		}
		return id;
	}
};

void network(std::string_view mode, uint16_t port)
{
	Clients clients{udp::parseAddress("127.0.0.1", port)};
	clients.connect(mode != "update");
	auto &alice = clients.alice;
	auto &bob = clients.bob;
	uint64_t drop{};
	uint32_t uncheckpointed{}, newActor{};
	if (mode == "update")
	{
		clients.checkInventory(5, 6, 0);
		require(clients.material() == wire::kAirMaterial, "fixture target was occupied");
		clients.action(alice, {.operation = wire::ActionKind::Place, .target = kTarget,
			.expected = alice.chunks().at(kChunk).version, .pitchTurns = -0.25f});
		clients.action(bob, {.operation = wire::ActionKind::Select, .slot = 7});
		clients.action(bob, {.operation = wire::ActionKind::DropOne});
		clients.until([&] { return (drop = clients.glassDrop()) && clients.material() == DIRT; },
			"committable block edit or drop not visible");
		clients.checkInventory(4, 5, 7);
		bob.disconnect(udp::ClientTransport::Clock::now());
		clients.until([&] {
			return bob.status() == udp::ClientStatus::Disconnected &&
				alice.latestPlayerSnapshot() && alice.latestPlayerSnapshot()->count == 1;
		}, "Bob was not retired before the server save");
	}
	else
	{
		clients.checkInventory(4, 5, 7);
		require(clients.material() == DIRT && (drop = clients.glassDrop()), "committed block or drop disappeared");
		udp::ClientTransport newProfile(clients.address, kUncheckpointed);
		newProfile.connect(udp::ClientTransport::Clock::now());
		clients.until([&] { return newProfile.inventory().has_value(); }, "uncheckpointed profile did not connect",
			&newProfile);
		require(std::ranges::all_of(newProfile.inventory()->slots, &items::ItemStack::isEmpty),
			"uncheckpointed profile received another player's inventory");
		uncheckpointed = newProfile.welcome()->player.value;
		if (mode == "crash")
		{
			clients.action(alice, {.operation = wire::ActionKind::Break, .target = kTarget,
				.expected = alice.chunks().at(kChunk).version, .pitchTurns = -0.25f});
			clients.action(bob, {.operation = wire::ActionKind::Select, .slot = 2});
			clients.until([&] { return clients.material() == wire::kAirMaterial &&
				alice.itemSnapshots().current->idHighWater > drop; }, "uncommitted edit and item ID were not observed");
		}
		else
		{
			udp::ClientTransport duplicate(clients.address, kBob);
			duplicate.connect(udp::ClientTransport::Clock::now());
			clients.until([&] { return duplicate.status() == udp::ClientStatus::Rejected; },
				"duplicate profile was not refused", &duplicate);
			require(duplicate.rejection() == wire::RejectReason::IdentityInUse, "wrong profile refusal");
			udp::ClientTransport other(clients.address, kOther);
			other.connect(udp::ClientTransport::Clock::now());
			clients.until([&] { return other.inventory().has_value(); }, "new profile did not connect", &other);
			newActor = other.welcome()->player.value;
			require(newActor > uncheckpointed &&
				std::ranges::all_of(other.inventory()->slots, &items::ItemStack::isEmpty),
				"new profile got a reused actor ID or another player's inventory");
		}
	}
	std::cout << "RESTART world=" << alice.welcome()->worldId << " epoch=" << alice.scope()->epoch
			  << " drop=" << drop << " high_water=" << alice.itemSnapshots().current->idHighWater
			  << " uncheckpointed=" << uncheckpointed << " new_actor=" << newActor << '\n';
}
}

int main(int argc, char **argv)
{
	try
	{
		require(argc == 3, "expected a fixture mode and a root path or port");
		const std::string_view mode = argv[1];
		if (mode == "seed")
			seed(argv[2]);
		else if (mode == "seed-migration")
			seedMigration(argv[2]);
		else if (mode == "open-solo" || mode == "refuse-solo")
			openSoloMigration(argv[2], mode == "open-solo");
		else if (mode == "crash-migration")
		{
			server::ServerHost host(migrationConfig(argv[2]));
			std::string error;
			const auto reached = server::ServerHostProbe::startWithInitialCheckpointHook(host, error, [] {
				std::_Exit(77);
			});
			throw std::runtime_error(reached ? "initial migration did not reach the crash seam" : error);
		}
		else if (mode == "publish-migration" || mode == "complete-migration" || mode == "reconnect-migration")
			completeMigration(argv[2], mode);
		else
			network(mode, static_cast<uint16_t>(std::stoi(argv[2])));
		return 0;
	}
	catch (const std::exception &error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
