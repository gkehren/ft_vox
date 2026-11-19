#include <Network/ClientTransport.hpp>
#include <Server/ServerHost.hpp>
#include <World/WorldCollisionView.hpp>

#include <filesystem>
#include <future>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace server
{
struct ServerHostProbe
{
	static sim::GameSimulation &simulation(ServerHost &host) { return *host.m_sim; }
	static ThreadPool &workers(ServerHost &host) { return *host.m_workers; }
	static void generationBudget(ServerHost &host, unsigned budget) { host.m_config.streaming.generations = budget; }
};
}

namespace
{
namespace fs = std::filesystem;
using namespace net;
using namespace std::chrono_literals;
using Time = udp::ServerTransport::Clock::time_point;
const auto loopback = udp::parseAddress("127.0.0.1", 0);

void require(bool success, const std::string &message)
{
	if (!success)
		throw std::runtime_error(message);
}

// One real IPv4 socket per peer. The client and server only see the relay's
// endpoint, so all traffic still exercises the production codec and pumps.
struct Relay
{
	Relay(udp::Address server, std::chrono::milliseconds delay)
		: server(server), delay(delay) {}

	void send(const wire::Packet &packet)
	{
		wire::Datagram encoded;
		require(wire::encode(wire::Endpoint::Server, packet, encoded) == wire::CodecStatus::Ok, "encode relay packet");
		require(socket.send(client, encoded.view()), "inject relay packet");
	}

	void sendToServer(const wire::Packet &packet, wire::Endpoint encodedAs = wire::Endpoint::Client)
	{
		wire::Datagram encoded;
		require(wire::encode(encodedAs, packet, encoded) == wire::CodecStatus::Ok, "encode client replay");
		require(socket.send(server, encoded.view()), "inject client replay");
	}

	void pump(Time now)
	{
		udp::Received frame;
		while (socket.receive(frame))
		{
			const bool fromServer = frame.from == server;
			if (!fromServer)
				client = frame.from;
			const auto to = fromServer ? client : server;
			const auto bytes = std::span(frame.bytes).first(frame.size);
			wire::Packet packet;
			require(wire::decode(fromServer ? wire::Endpoint::Server : wire::Endpoint::Client, bytes, packet) ==
				wire::CodecStatus::Ok, "relay saw malformed datagram");
			if (const auto *resync = std::get_if<wire::Resync>(&packet.payload); resync && !fromServer)
				lastResync = resync->reason;
			if (std::holds_alternative<wire::Action>(packet.payload))
				lastAction = packet;
			if (std::holds_alternative<wire::ActionResult>(packet.payload))
			{
				if (dropAllActionResults || std::exchange(dropActionResult, false))
				{
					droppedActionResult = packet;
					continue;
				}
				lastActionResult = packet;
			}
			if (std::holds_alternative<wire::Inventory>(packet.payload))
			{
				if (std::exchange(dropInventory, false))
				{
					droppedInventory = packet;
					continue;
				}
				lastInventory = packet;
			}
			if (const auto *snapshot = std::get_if<wire::ItemSnapshot>(&packet.payload))
			{
				itemBytes = std::max(itemBytes, frame.size);
				if (snapshot->count == 0 && std::exchange(dropItemSnapshot, false))
				{
					droppedItemSnapshot = packet;
					continue;
				}
				lastItemSnapshot = packet;
			}
			if (std::holds_alternative<wire::MobSnapshot>(packet.payload))
			{
				mobBytes = std::max(mobBytes, frame.size);
				lastMobSnapshot = packet;
			}
			if (const auto *ack = std::get_if<wire::Ack>(&packet.payload);
			    fromServer && ack && lastAction && packet.scope == lastAction->scope &&
			    ack->through == lastAction->sequence && std::exchange(dropActionAck, false))
				continue;
			if (fromServer && (std::holds_alternative<wire::ChunkFragment>(packet.payload) ||
			    std::holds_alternative<wire::WorldEdit>(packet.payload) ||
			    std::holds_alternative<wire::ChunkRemove>(packet.payload)))
			{
				traffic.bytes += frame.size;
				++traffic.frames;
				if (!sequences.emplace(packet.scope.sessionId, packet.sequence).second)
					++traffic.retransmits;
			}
			if (auto *chunk = std::get_if<wire::ChunkFragment>(&packet.payload))
			{
				if (chunk->index == 0)
					lastBegin = chunk->baseline;
				if (chunk->index == 2 && std::exchange(dropFragment, false))
					continue;
				if (chunk->index == 3 && std::exchange(duplicateFragment, false))
					pending.emplace(now + delay + 150ms, std::pair{to, frame});
				if (chunk->index + 1u == wire::kFragmentsPerChunk && std::exchange(corruptFragment, false))
				{
					chunk->materials[0] = chunk->materials[0] == wire::kAirMaterial ? static_cast<uint8_t>(STONE) : wire::kAirMaterial;
					wire::Datagram corrupted;
					require(wire::encode(wire::Endpoint::Server, packet, corrupted) == wire::CodecStatus::Ok,
						"expected checksum-only corruption");
					std::ranges::copy(corrupted.view(), frame.bytes.begin());
				}
			}
			if (std::holds_alternative<wire::WorldEdit>(packet.payload))
			{
				if (std::exchange(dropEdit, false))
				{
					oldEdit = packet;
					continue;
				}
				if (std::exchange(duplicateEdit, false))
					pending.emplace(now + delay + 150ms, std::pair{to, frame});
			}
			pending.emplace(now + delay, std::pair{to, frame});
		}
		while (!pending.empty() && pending.begin()->first <= now)
		{
			const auto &[to, packet] = pending.begin()->second;
			require(socket.send(to, std::span(packet.bytes).first(packet.size)), "relay send failed");
			pending.erase(pending.begin());
		}
	}

	udp::Socket socket{loopback};
	udp::Address server, client{};
	std::chrono::milliseconds delay;
	bool dropFragment{}, duplicateFragment{}, corruptFragment{}, dropEdit{}, duplicateEdit{};
	bool dropActionResult{}, dropAllActionResults{}, dropActionAck{};
	bool dropInventory{}, dropItemSnapshot{};
	std::optional<wire::ChunkBaseline> lastBegin;
	std::optional<wire::ResyncReason> lastResync;
	std::optional<wire::Packet> oldEdit;
	std::optional<wire::Packet> lastAction, lastActionResult, droppedActionResult;
	std::optional<wire::Packet> lastInventory, droppedInventory, lastItemSnapshot, droppedItemSnapshot, lastMobSnapshot;
	size_t itemBytes{}, mobBytes{};
	udp::WorldTraffic traffic;
	std::set<std::pair<uint64_t, uint64_t>> sequences;
	std::multimap<Time, std::pair<udp::Address, udp::Received>> pending;
};

using Coord = wire::ChunkCoord;
constexpr Coord spawn{0, 0}, distant{-20, 0};
constexpr glm::dvec3 spawnFeet{8.5, 245.0, 8.5}, farFeet{-311.5, 245.0, 8.5};

bool matches(const udp::ClientTransport &client, const VoxelWorld &world, Coord center)
{
	if (client.chunks().size() != 5)
		return false;
	for (const auto offset : VoxelWorld::playerInterestOffsets(1))
	{
		const Coord coord{center.x + offset.x, center.z + offset.z};
		const auto found = client.chunks().find(coord);
		const glm::ivec3 coreCoord{coord.x, 0, coord.z};
		const auto version = world.chunkContentVersion(coreCoord);
		if (found == client.chunks().end() || !version ||
		    found->second.version != wire::ChunkVersion{version->generation, version->revision})
			return false;
		const auto *chunk = world.getChunk(coreCoord);
		std::array<uint8_t, wire::kChunkBytes> expected;
		for (int y = 0; y < CHUNK_HEIGHT; ++y)
			for (int z = 0; z < CHUNK_SIZE; ++z)
				for (int x = 0; x < CHUNK_SIZE; ++x)
				{
					const auto material = chunk->getVoxel(x, y, z).type;
					expected[WorldChunk::index(x, y, z)] = material == AIR ? wire::kAirMaterial : material;
				}
		if (wire::chunkChecksum(expected) != wire::chunkChecksum(found->second.materials))
			return false;
	}
	return true;
}

struct TerrainGate
{
	explicit TerrainGate(ThreadPool &pool) : task(pool.enqueue([gate = release.get_future()] { gate.wait(); })) {}
	~TerrainGate() { open(); }
	void open()
	{
		if (task.valid())
		{
			release.set_value();
			task.get();
		}
	}
	std::promise<void> release;
	std::future<void> task;
};

void testReplication(const fs::path &root)
{
	server::HostConfig config;
	config.world = {.seed = 19, .worldName = "replication", .savesRoot = root};
	config.workers = 1;
	config.interestRadiusChunks = 1;
	server::ServerHost host(config);
	std::string error;
	require(host.start(error), error);
	auto &sim = server::ServerHostProbe::simulation(host);
	auto &world = *sim.world();
	auto server = std::make_unique<udp::ServerTransport>(loopback, 2, host.sessionWorld());
	Relay clean(server->address(), 0ms), impaired(server->address(), 10ms);
	impaired.dropFragment = impaired.duplicateFragment = true;
	impaired.dropEdit = impaired.duplicateEdit = true;
	udp::ClientTransport first(clean.socket.address()), second(impaired.socket.address());
	Time now{}, nextTick{};
	const auto pump = [&](server::TerrainTiming timing) {
		clean.pump(now);
		impaired.pump(now);
		server->poll(now);
		if (now >= nextTick)
		{
			server->serviceOwner(host, host.ticks());
			require(host.step(timing, error), error);
			nextTick += 16667us;
		}
		server->poll(now);
		clean.pump(now);
		impaired.pump(now);
		first.poll(now);
		second.poll(now);
		now += 5ms;
	};
	const auto until = [&](auto predicate, auto timeout, const char *failure,
		server::TerrainTiming timing = server::TerrainTiming::QuiescePerTick) {
		const auto deadline = now + timeout;
		while (!predicate() && now < deadline)
			pump(timing);
		require(predicate(), failure);
	};

	struct Measurement
	{
		std::optional<Time> welcome, first, playable;
		udp::WorldTraffic traffic;
		void observe(const udp::ClientTransport &client, const Relay &relay, Time now)
		{
			if (!welcome && client.welcome()) welcome = now;
			if (!first && client.chunks().contains(spawn)) first = now;
			if (!playable && client.chunks().size() == 5)
			{
				playable = now;
				traffic = relay.traffic;
			}
		}
		void report(const char *label) const
		{
			require(welcome && first && playable, "missing spawn timing");
			std::cout << label << " first_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(*first - *welcome).count()
				<< " neighborhood_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(*playable - *welcome).count()
				<< " world_bytes=" << traffic.bytes << " frames=" << traffic.frames << " retries=" << traffic.retransmits << '\n';
		}
	};
	Measurement cleanTime, impairedTime;
	first.connect(now);
	second.connect(now);
	std::optional<uint64_t> editedAt;
	bool teleported = false, returned = false;
	const auto joinDeadline = now + 12s;
	while ((!cleanTime.playable || !impairedTime.playable) && now < joinDeadline)
	{
		pump(server::TerrainTiming::QuiescePerTick);
		cleanTime.observe(first, clean, now);
		impairedTime.observe(second, impaired, now);
		if (!teleported && clean.lastBegin && first.welcome())
		{
			teleported = true;
			sim.setPlayerPosition(first.welcome()->player, {648.5, 245.0, 8.5});
		}
		if (!editedAt && impaired.lastBegin && impaired.lastBegin->coord == spawn)
		{
			require(second.chunks().empty(), "partial baseline became visible");
			require(impaired.lastBegin->version.revision == 0, "unexpected starting revision");
			// Edge edit exercises border mirrors. The second edit makes the
			// delayed replay arrive after a newer delta has already landed.
			require(sim.placeVoxel({0.5f, 250.5f, 8.5f}, GLASS).accepted, "edge edit during assembly");
			require(sim.placeVoxel({1.5f, 250.5f, 8.5f}, STONE).accepted, "second edit during assembly");
			editedAt = host.ticks();
		}
		if (editedAt && teleported && !returned && host.ticks() > *editedAt)
		{
			sim.setPlayerPosition(first.welcome()->player, spawnFeet);
			returned = true;
		}
	}
	cleanTime.report("loopback");
	impairedTime.report("loopback 10ms each way with loss and reordered duplicates");
	const auto &fragmentBudget = *wire::policy(wire::Kind::ChunkFragment);
	const auto minimumInstallTime = 1000000us * static_cast<int>(wire::kFragmentsPerChunk - fragmentBudget.burst) /
		fragmentBudget.messagesPerSecond;
	require(*cleanTime.first - *cleanTime.welcome >= std::chrono::ceil<std::chrono::milliseconds>(minimumInstallTime),
		"initial baseline bypassed fragment pacing");
	require(editedAt && returned && impaired.oldEdit && impairedTime.traffic.retransmits >= 2,
		"faults and teleport did not hit in-flight world updates");
	require(matches(first, world, spawn) && matches(second, world, spawn), "overlapping spawn digests differ");
	require(second.chunks().at(spawn).version.revision == 2 && second.chunks().at({-1, 0}).version.revision == 0,
		"render mirrors leaked into wire revisions");
	const auto aggregate = server->worldTraffic();
	require(aggregate.bytes >= cleanTime.traffic.bytes + impairedTime.traffic.bytes &&
		aggregate.retransmits >= impairedTime.traffic.retransmits, "world traffic counters lost sends");
	const auto peerTraffic = server->worldTraffic(second.welcome()->player);
	require(peerTraffic && peerTraffic->frames == impaired.traffic.frames && peerTraffic->bytes == impaired.traffic.bytes &&
		peerTraffic->retransmits == impaired.traffic.retransmits, "per-player world counters lost sends");

	// Capture while the published chunk has a pending target write. The
	// backing still has AIR here, so the new image must include the overlay.
	auto *published = world.getChunk({0, 0, 0});
	published->setInTransit(true);
	const auto pendingEdit = sim.placeVoxel({2.5f, 250.5f, 8.5f}, GLASS);
	std::array<uint8_t, wire::kChunkBytes> captured;
	ChunkContentVersion capturedVersion;
	const auto watermark = world.captureChunkContent({0, 0, 0}, captured, capturedVersion);
	published->setInTransit(false);
	require(pendingEdit.accepted && watermark == pendingEdit.editId + 1 && capturedVersion.revision == 3 &&
		captured[WorldChunk::index(2, 250, 8)] == GLASS && published->getVoxel(2, 250, 8).type == AIR,
		"capture omitted a pending overlay on published backing");

	const auto secondId = second.welcome()->player;
	sim.setPlayerPosition(secondId, farFeet);
	server::ServerHostProbe::generationBudget(host, 0);
	until([&] { return second.chunks().empty() && world.getChunk({distant.x, 0, distant.z}); }, 2s,
		"old interest did not unload");
	require(!world.chunkContentVersion({distant.x, 0, distant.z}), "terrain unexpectedly published");
	const glm::vec3 deferred{-319.5f, 250.5f, 8.5f};
	require(sim.placeVoxel(deferred, STONE).accepted && sim.deleteVoxel(deferred).accepted &&
		sim.placeVoxel(deferred, GLASS).accepted && world.hasPendingLogicalEdits(), "defer/coalesce edits on unreadable chunk");

	const auto firstScope = *first.scope();
	first.disconnect(now);
	until([&] { return first.status() == udp::ClientStatus::Disconnected; }, 2s, "first client could not leave");
	TerrainGate gate(server::ServerHostProbe::workers(host));
	server::ServerHostProbe::generationBudget(host, 8);
	first.connect(now);
	until([&] { return first.welcome() && sim.playerPositioned(first.welcome()->player) && world.pendingGenJobs() > 0; }, 2s,
		"no join during worker-owned generation", server::TerrainTiming::Background);
	require(first.scope() != firstScope && second.chunks().empty() && world.hasPendingLogicalEdits(),
		"unpublished backing leaked during join");
	gate.open();
	until([&] { return matches(first, world, spawn) && matches(second, world, distant); }, 12s,
		"disjoint interest digests differ after deferred publication");
	const auto farVersion = second.chunks().at(distant).version;
	require(farVersion.revision == 3 && second.chunks().at(distant).materials[WorldChunk::index(0, 250, 8)] == GLASS,
		"deferred logical edit sequence missing from baseline");
	const auto firstId = first.welcome()->player;
	sim.setPlayerPosition(firstId, farFeet);
	until([&] { return matches(first, world, distant); }, 8s, "first client did not converge after teleport");
	require(first.chunks().at(distant).version == farVersion, "shared chunk was generated twice");

	sim.setPlayerPosition(firstId, spawnFeet);
	sim.setPlayerPosition(secondId, spawnFeet);
	until([&] { return matches(first, world, spawn) && matches(second, world, spawn); }, 12s, "return to spawn failed");
	require(!world.getChunk({distant.x, 0, distant.z}), "unobserved chunk remained resident");
	sim.setPlayerPosition(secondId, farFeet);
	until([&] { return matches(second, world, distant); }, 12s, "re-entered region did not converge");
	require(second.chunks().at(distant).version.generation > farVersion.generation &&
		second.chunks().at(distant).materials[WorldChunk::index(0, 250, 8)] == GLASS, "new incarnation lost durable override");

	// The 65th pending delta must restart the projection. The trusted
	// authority accepts every edit even though the consumer fell behind.
	const auto congested = *first.scope();
	for (size_t i = 0; i <= wire::kReliableWindow; ++i)
	{
		const glm::vec3 cell{5.5f, 250.5f, 8.5f};
		require((i % 2 == 0 ? sim.placeVoxel(cell, STONE) : sim.deleteVoxel(cell)).accepted, "burst edit rejected");
	}
	until([&] { return first.scope() && first.scope() != congested && matches(first, world, spawn); }, 12s,
		"server resync after delta backlog did not converge");
	require(clean.lastResync == wire::ResyncReason::Baseline && !sim.hasPlayer(firstId), "old actor survived resync");

	const auto corruptScope = *second.scope();
	impaired.corruptFragment = true;
	second.requestWorldResync(now);
	require(second.chunks().empty(), "explicit resync retained stale chunks");
	until([&] { return impaired.lastResync == wire::ResyncReason::Checksum && second.scope() &&
		second.scope() != corruptScope && matches(second, world, spawn); }, 18s, "checksum resync did not recover");
	require(server->worldTraffic().resyncs >= 3, "missing resync counters");

	// Restart the UDP authority and replay an old scoped edit from the same
	// endpoint while the new reliable cursor still expects early frames.
	const auto oldEpoch = first.scope()->epoch;
	const auto address = server->address();
	server->stop(host);
	server.reset();
	require(host.stop(error), error);
	require(host.start(error), error);
	server = std::make_unique<udp::ServerTransport>(address, 2, host.sessionWorld());
	until([&] { return first.status() == udp::ClientStatus::TimedOut && second.status() == udp::ClientStatus::TimedOut; },
		12s, "old epoch did not time out after server restart");
	require(first.chunks().empty() && second.chunks().empty(), "departed epoch retained chunks");
	first.connect(now);
	second.connect(now);
	until([&] { return first.welcome() && second.welcome(); }, 2s, "restart did not admit clients");
	require(second.scope()->epoch != oldEpoch, "server reused its wire epoch");
	impaired.send(*impaired.oldEdit);
	auto wrongEpoch = *impaired.oldEdit;
	wrongEpoch.scope = {second.scope()->sessionId, oldEpoch};
	wrongEpoch.sequence = 1;
	impaired.send(wrongEpoch);
	const auto &restarted = *server::ServerHostProbe::simulation(host).world();
	until([&] { return matches(first, restarted, spawn) && matches(second, restarted, spawn); }, 12s,
		"stale scope disrupted new baselines after restart");
	const auto currentScope = *second.scope();
	const auto version = second.chunks().at(spawn).version;
	const auto last = std::prev(impaired.sequences.upper_bound({currentScope.sessionId, wire::kMaxSequence}));
	require(last->first == currentScope.sessionId && version.generation > 1, "missing scoped version fixture");
	const wire::WorldEdit gap{restarted.nextEditId(), {0, 8, 250}, {version.generation, version.revision + 1}, wire::kAirMaterial};
	const wire::Packet gapPacket{currentScope, last->second + 2, gap};
	impaired.send(gapPacket); // future reliable frame must not consume the gap
	second.poll(now);
	require(matches(second, restarted, spawn), "future reliable frame changed projection");
	impaired.send({currentScope, last->second + 1,
		wire::WorldEdit{gap.editId, gap.cell, {version.generation - 1, version.revision}, gap.material}});
	second.poll(now);
	require(matches(second, restarted, spawn), "stale generation changed projection");
	impaired.send(gapPacket);
	second.poll(now);
	require(second.status() == udp::ClientStatus::Resyncing && second.chunks().empty(), "revision gap did not invalidate scope");
	until([&] { return second.scope() && second.scope() != currentScope && matches(second, restarted, spawn); }, 12s,
		"revision gap did not recover");
	server->stop(host);
	require(host.stop(error), error);
}

void testVoxelActions(const fs::path &root)
{
	using Code = wire::ActionCode;
	using Kind = wire::ActionKind;
	server::HostConfig config;
	config.world = {.seed = 19, .worldName = "voxel_actions", .savesRoot = root};
	config.workers = 0;
	config.interestRadiusChunks = 1;
	server::ServerHost host(config);
	std::string error;
	require(host.start(error), error);
	const auto simulation = [&]() -> sim::GameSimulation & { return server::ServerHostProbe::simulation(host); };
	const auto world = [&]() -> VoxelWorld & { return *simulation().world(); };
	// Keep the voxel-action fixture's drops around through the checkpoint
	// assertions. EntityReplication exercises the live pickup timing.
	const_cast<items::ItemEntitySystem &>(simulation().itemEntities()).settings.pickupDelayTicks = 10000;
	const auto seedBlock = [&](int x, int y, int z, TextureType type) {
		require(simulation().placeVoxel({x + 0.5f, y + 0.5f, z + 0.5f}, type).accepted, "seed action fixture");
	};
	for (int z = 5; z <= 10; ++z)
		seedBlock(8, 244, z, STONE);
	seedBlock(8, 246, 9, STONE);
	seedBlock(8, 246, 10, BEDROCK);

	auto server = std::make_unique<udp::ServerTransport>(loopback, 2, host.sessionWorld());
	Relay clean(server->address(), 0ms), impaired(server->address(), 0ms);
	udp::ClientTransport first(clean.socket.address()), second(impaired.socket.address());
	Time now{}, nextTick{};
	const auto pump = [&] {
		clean.pump(now);
		impaired.pump(now);
		server->poll(now);
		if (now >= nextTick)
		{
			server->serviceOwner(host, host.ticks());
			require(host.step(server::TerrainTiming::Background, error), error);
			nextTick += 16667us;
		}
		server->poll(now);
		clean.pump(now);
		impaired.pump(now);
		first.poll(now);
		second.poll(now);
		now += 5ms;
	};
	const auto until = [&](auto predicate, const char *failure, std::chrono::seconds timeout = 8s) {
		const auto deadline = now + timeout;
		do
		{
			if (predicate())
				return;
			pump();
		}
		while (now < deadline);
		throw std::runtime_error(failure);
	};
	const auto ready = [&](const udp::ClientTransport &client) {
		return client.welcome() && simulation().playerPositioned(client.welcome()->player);
	};
	const auto settle = [&](udp::ClientTransport &client) {
		std::optional<wire::ActionResult> result;
		until([&] { return (result = client.takeActionResult()).has_value(); }, "missing action result");
		return *result;
	};
	const auto submit = [&](udp::ClientTransport &client, wire::Action action) {
		std::optional<uint64_t> id;
		until([&] { return (id = client.sendAction(action, now)).has_value(); }, "action slot failed to open");
		const auto result = settle(client);
		require(result.actionId == *id && result.operation == action.operation, "action result did not match request");
		return result;
	};
	const auto inventory = [&](sim::PlayerId player) {
		sim::InventorySnapshot result;
		require(simulation().inventorySnapshot(player, result), "missing actor inventory");
		return result;
	};

	first.connect(now);
	until([&] { return ready(first); }, "first actor could not join");
	second.connect(now);
	until([&] { return ready(second); }, "second actor could not join");
	const auto secondId = second.welcome()->player;
	const auto oldFirst = first.welcome()->player;
	first.disconnect(now);
	until([&] { return first.status() == udp::ClientStatus::Disconnected; }, "first actor could not leave");
	first.connect(now);
	until([&] { return ready(first); }, "first actor could not reuse its slot");
	const auto firstId = first.welcome()->player;
	require(secondId < firstId && !simulation().hasPlayer(oldFirst), "fixture did not reverse actor and slot order");
	constexpr glm::dvec3 firstFeet{8.5, 245.0, 5.5}, secondFeet{8.5, 245.0, 6.5};
	simulation().setPlayerPosition(firstId, firstFeet);
	simulation().setPlayerPosition(secondId, secondFeet);
	for (const auto player : {firstId, secondId})
		require(simulation().grantItems(player, items::ItemId::Glass, 1).result == items::ItemResult::Ok,
			"grant action fixture glass");
	const auto startingInventory = inventory(firstId);
	until([&] { return matches(first, world(), spawn) && matches(second, world(), spawn); }, "spawn projections diverged");

	constexpr wire::Cell target{8, 9, 246}, support{8, 10, 246};
	const auto version = [&] { return first.chunks().at(spawn).version; };
	wire::Action dig{.actionId = 1, .operation = Kind::Break, .target = target, .expected = version()};
	const auto initialRevision = world().nextEditId();
	const auto initialItemId = simulation().itemIdHighWater();
	// An invalid ID or a reliable gap must leave the exact next slot free.
	wire::Datagram malformed;
	require(wire::encode(wire::Endpoint::Client, {*first.scope(), 1, dig}, malformed) == wire::CodecStatus::Ok,
		"could not encode action prefix");
	std::vector<uint8_t> bytes(malformed.view().begin(), malformed.view().end());
	for (const auto byte : {uint8_t{0}, uint8_t{255}})
	{
		std::fill_n(bytes.begin() + wire::kHeaderBytes, sizeof(uint64_t), byte);
		require(clean.socket.send(server->address(), bytes), "inject malformed action ID");
	}
	auto gap = dig;
	gap.actionId = 2;
	clean.sendToServer({*first.scope(), 1, gap});
	clean.sendToServer({*first.scope(), 2, dig});
	for (int i = 0; i < 5; ++i)
		pump();
	require(world().nextEditId() == initialRevision && simulation().itemIdHighWater() == initialItemId,
		"malformed or reordered action mutated gameplay");

	impaired.dropActionResult = true;
	clean.dropEdit = true;
	require(first.sendAction(dig, now) == 1 && second.sendAction(dig, now) == 1 && !second.sendAction(dig, now),
		"clients did not reserve exactly one action each");
	const auto lostBreak = settle(first);
	const auto wonBreak = settle(second);
	require(wonBreak.code == Code::Accepted && wonBreak.editId == initialRevision && wonBreak.nextEditId == initialRevision + 1 &&
		lostBreak.code == Code::StaleRevision && lostBreak.editId == 0 && lostBreak.nextEditId == wonBreak.nextEditId &&
		lostBreak.tick == wonBreak.tick, "simultaneous break failed to resolve by actor ID and version");
	require(impaired.droppedActionResult && impaired.lastActionResult == impaired.droppedActionResult && clean.oldEdit,
		"lost result or edit did not exercise the retry path");
	until([&] { return matches(first, world(), spawn) && matches(second, world(), spawn); }, "digests diverged after break loss");
	require(simulation().itemIdHighWater() == initialItemId + 1 && host.metrics().items == 1 &&
		simulation().itemEntities().entities().front().stack == items::ItemStack{items::ItemId::Cobblestone, 1},
		"competing breaks did not create exactly one registry drop");

	// Replay with both the original sequence and the next sequence.
	const auto oldDig = *impaired.lastAction;
	impaired.sendToServer(oldDig);
	impaired.sendToServer({oldDig.scope, oldDig.sequence + 1, std::get<wire::Action>(oldDig.payload)});
	const auto beforeRejections = world().nextEditId();
	const auto reject = [&](udp::ClientTransport &client, wire::Action action, Code code) {
		const auto revision = world().nextEditId();
		const auto itemId = simulation().itemIdHighWater();
		const auto before = inventory(client.welcome()->player);
		const auto result = submit(client, action);
		require(result.code == code && result.editId == 0 && result.nextEditId == revision &&
			world().nextEditId() == revision && simulation().itemIdHighWater() == itemId &&
			inventory(client.welcome()->player) == before, "rejection changed gameplay or returned the wrong reason");
	};
	auto invalid = dig;
	invalid.expected = version();
	invalid.target.x = 400;
	reject(second, invalid, Code::OutOfReach);
	invalid.target = support;
	reject(first, invalid, Code::Forbidden);
	invalid.yawTurns = 0.5f;
	reject(first, invalid, Code::InvalidTarget);
	invalid.yawTurns = 0.f;
	invalid.expected.revision = dig.expected.revision;
	reject(first, invalid, Code::StaleRevision);
	reject(first, wire::Action{.operation = Kind::DropStack, .slot = 1}, Code::InventoryRejected);
	require(world().nextEditId() == beforeRejections, "replayed break advanced the edit watermark");

	wire::Action place{.operation = Kind::Place, .target = target, .expected = version()};
	simulation().setPlayerPosition(firstId, {8.5, 245.0, 9.5});
	reject(second, place, Code::Occupied);
	simulation().setPlayerPosition(firstId, firstFeet);

	// An accepted edit in transit changes the effective version and client
	// projection before the backing store can be written.
	{
		struct Transit
		{
			WorldChunk &chunk;
			explicit Transit(WorldChunk &chunk) : chunk(chunk) { chunk.setInTransit(true); }
			~Transit() { chunk.setInTransit(false); }
		} transit(*world().getChunk({0, 0, 0}));
		require(first.sendAction(place, now) && second.sendAction(place, now), "could not queue simultaneous placement");
		const auto lostPlace = settle(first);
		const auto wonPlace = settle(second);
		require(wonPlace.code == Code::Accepted && wonPlace.editId == beforeRejections && wonPlace.nextEditId == beforeRejections + 1 &&
			lostPlace.code == Code::StaleRevision && lostPlace.tick == wonPlace.tick && lostPlace.editId == 0,
			"simultaneous placement failed to resolve by actor ID and version");
		until([&] { return first.chunks().at(spawn).version.revision == place.expected.revision + 1 &&
			second.chunks().at(spawn).version.revision == place.expected.revision + 1; }, "deferred placement delta not delivered");
		require(world().hasPendingLogicalEdits() && transit.chunk.getVoxel(8, 246, 9).type == AIR &&
			first.chunks().at(spawn).materials[WorldChunk::index(8, 246, 9)] == GLASS &&
			inventory(firstId) == startingInventory && inventory(secondId).slots[0].isEmpty() && host.metrics().items == 1,
			"deferred placement duplicated a resource or missed the logical overlay");
		invalid = dig;
		invalid.target = support;
		invalid.expected = version();
		reject(first, invalid, Code::InvalidTarget); // pending glass occludes the bedrock
	}
	until([&] { return matches(first, world(), spawn) && matches(second, world(), spawn); }, "published placement diverged");
	place.expected = version();
	reject(second, place, Code::Occupied);
	place.target = {8, 8, 246};
	reject(second, place, Code::InventoryRejected);

	// An unpublished chunk first rejects the intent, then allows a new one
	// once the authority has generated and replicated its contents.
	server::ServerHostProbe::generationBudget(host, 0);
	simulation().setPlayerPosition(secondId, farFeet);
	constexpr wire::Cell distantBlock{-312, 10, 246};
	until([&] { return world().getChunk({distant.x, 0, distant.z}); }, "missing unpublished chunk fixture");
	auto farDig = dig;
	farDig.target = distantBlock;
	reject(second, farDig, Code::TerrainUnavailable);
	server::ServerHostProbe::generationBudget(host, 8);
	until([&] { return world().chunkContentVersion({distant.x, 0, distant.z}).has_value(); }, "distant terrain did not generate");
	seedBlock(-312, 244, 8, STONE);
	seedBlock(-312, 246, 10, STONE);
	seedBlock(-312, 246, 11, BEDROCK);
	simulation().setPlayerPosition(secondId, farFeet);
	until([&] { return matches(second, world(), distant); }, "distant preparation did not replicate");
	farDig.expected = second.chunks().at(distant).version;
	const auto beforeLeave = world().nextEditId();
	const auto oldScope = *second.scope();
	impaired.dropAllActionResults = impaired.dropActionAck = true;
	require(second.sendAction(farDig, now).has_value(), "could not queue action before leaving");
	until([&] { return world().nextEditId() == beforeLeave + 1; }, "action before leaving never committed");
	require(!impaired.dropActionAck && !second.takeActionResult(), "fixture did not lose action admission ACK and result");
	const auto oldFarDig = *impaired.lastAction;
	second.disconnect(now);
	until([&] { return second.status() == udp::ClientStatus::Disconnected && !world().getChunk({distant.x, 0, distant.z}); },
		"disconnect behind an unacknowledged action did not retire actor and unload terrain");
	impaired.dropAllActionResults = false;
	second.connect(now);
	until([&] { return ready(second); }, "actor did not reconnect");
	require(second.scope() != oldScope && !second.takeActionResult() && !simulation().hasPlayer(secondId),
		"reconnect retained the old actor or its result");
	simulation().setPlayerPosition(second.welcome()->player, farFeet);
	until([&] { return matches(second, world(), distant); }, "unload and reconnect did not converge");
	const auto &reloaded = second.chunks().at(distant);
	require(reloaded.version.generation > farDig.expected.generation &&
		reloaded.materials[WorldChunk::index(8, 246, 10)] == wire::kAirMaterial &&
		host.metrics().items == 2 && simulation().itemIdHighWater() == initialItemId + 2,
		"refetch lost the committed action or duplicated its drop");
	impaired.sendToServer(oldFarDig);
	farDig.expected = reloaded.version;
	farDig.target.z = 11;
	const auto current = submit(second, farDig);
	require(current.actionId == 1 && current.code == Code::Forbidden && world().nextEditId() == beforeLeave + 1,
		"departed session replay perturbed new action scope");

	const auto address = server->address();
	server->stop(host);
	server.reset();
	require(host.stop(error), error);
	require(host.start(error), error);
	server = std::make_unique<udp::ServerTransport>(address, 2, host.sessionWorld());
	until([&] { return first.status() == udp::ClientStatus::TimedOut && second.status() == udp::ClientStatus::TimedOut; },
		"old actions survived server restart", 12s);
	first.connect(now);
	second.connect(now);
	until([&] { return ready(first) && ready(second); }, "clients did not join new epoch");
	impaired.sendToServer(oldFarDig);
	auto wrongEpoch = oldFarDig;
	wrongEpoch.scope.sessionId = second.scope()->sessionId;
	wrongEpoch.sequence = 1;
	wrongEpoch.payload = farDig;
	impaired.sendToServer(wrongEpoch);
	simulation().setPlayerPosition(second.welcome()->player, farFeet);
	until([&] { return matches(first, world(), spawn) && matches(second, world(), distant); },
		"post-checkpoint projections diverged");
	farDig.expected = second.chunks().at(distant).version;
	const auto newEpoch = submit(second, farDig);
	require(second.scope()->epoch != oldScope.epoch && newEpoch.actionId == 1 && newEpoch.code == Code::Forbidden &&
		simulation().itemIdHighWater() == initialItemId + 2 && host.metrics().items == 2 &&
		first.chunks().at(spawn).materials[WorldChunk::index(8, 246, 9)] == GLASS &&
		second.chunks().at(distant).materials[WorldChunk::index(8, 246, 10)] == wire::kAirMaterial,
		"restart lost committed voxel/item effects or accepted an old epoch");
	server->stop(host);
	require(host.stop(error), error);
}

void testIncompatibleGenerator()
{
	udp::Socket server(loopback);
	udp::ClientTransport client(server.address());
	const Time now{};
	client.connect(now);
	client.poll(now);
	udp::Received raw;
	require(server.receive(raw), "missing discovery hello");
	wire::Packet packet;
	require(wire::decode(wire::Endpoint::Client, std::span(raw.bytes).first(raw.size), packet) == wire::CodecStatus::Ok,
		"invalid discovery hello");
	const auto hello = std::get<wire::Hello>(packet.payload);
	const wire::Welcome wrong{hello.nonce, 1, sim::PlayerId{2}, 42, TerrainGenerator::kGeneratorVersion + 1,
		wire::localCompatibility(), 0, 1};
	wire::Datagram bytes;
	require(wire::encode(wire::Endpoint::Server, {{1, 1}, 0, wrong}, bytes) == wire::CodecStatus::Ok &&
		server.send(raw.from, bytes.view()), "could not send mismatched Welcome");
	client.poll(now);
	require(client.status() == udp::ClientStatus::Rejected && client.rejection() == wire::RejectReason::Content &&
		!client.scope() && client.chunks().empty(), "incompatible generator installed a scope");
}

void testEntityReplication(const fs::path &root)
{
	using Kind = wire::ActionKind;
	using Code = wire::ActionCode;
	using Item = items::ItemId;
	server::HostConfig config;
	config.world = {.seed = 19, .worldName = "entities", .savesRoot = root};
	config.workers = 0;
	config.interestRadiusChunks = 1;
	server::ServerHost host(config);
	std::string error;
	require(host.start(error), error);
	auto &sim = server::ServerHostProbe::simulation(host);
	auto &items = const_cast<items::ItemEntitySystem &>(sim.itemEntities());
	auto &mobs = const_cast<entities::MobSystem &>(sim.mobs());
	mobs.settings.spawnChance = 0.0;
	for (int z = 3; z <= 14; ++z)
		for (int x = 6; x <= 13; ++x)
			require(sim.placeVoxel({x + 0.5f, 244.5f, z + 0.5f}, STONE).accepted, "seed entity fixture platform");
	udp::ServerTransport transport(loopback, 2, host.sessionWorld());
	Relay clean(transport.address(), 0ms), impaired(transport.address(), 15ms);
	udp::ClientTransport first(clean.socket.address()), second(impaired.socket.address());
	Time now{}, nextTick{};
	const auto pump = [&] {
		clean.pump(now);
		impaired.pump(now);
		transport.poll(now);
		if (now >= nextTick)
		{
			transport.serviceOwner(host, host.ticks());
			require(host.step(server::TerrainTiming::QuiescePerTick, error), error);
			nextTick += 16667us;
		}
		transport.poll(now);
		clean.pump(now);
		impaired.pump(now);
		first.poll(now);
		second.poll(now);
		now += 5ms;
	};
	const auto until = [&](auto predicate, const char *failure) {
		const auto deadline = now + 8s;
		do
		{
			if (predicate())
				return;
			pump();
		}
		while (now < deadline);
		throw std::runtime_error(failure);
	};
	const auto itemRecord = [](const udp::ClientTransport &client, uint64_t id) -> const wire::ItemState * {
		const auto &current = client.itemSnapshots().current;
		if (!current)
			return nullptr;
		const auto active = std::span(current->items).first(current->count);
		const auto found = std::ranges::find(active, id, &wire::ItemState::id);
		return found == active.end() ? nullptr : &*found;
	};
	const auto seesMob = [](const udp::ClientTransport &client, uint64_t id) {
		const auto &current = client.mobSnapshots().current;
		return current && std::ranges::any_of(std::span(current->mobs).first(current->count),
			[id](const auto &mob) { return mob.id == id; });
	};
	const auto action = [&](udp::ClientTransport &client, Kind operation, uint8_t slot, uint8_t other = 0) {
		std::optional<uint64_t> id;
		until([&] { id = client.sendAction({.operation = operation, .slot = slot, .otherSlot = other}, now);
			return id.has_value(); }, "inventory action slot failed to open");
		std::optional<wire::ActionResult> result;
		until([&] { result = client.takeActionResult(); return result.has_value(); }, "missing inventory result");
		require(result->actionId == *id && result->operation == operation && result->code == Code::Accepted &&
			result->editId == 0, "unexpected inventory result");
	};
	const auto count = [](const udp::ClientTransport &client, Item item) {
		const auto inventory = client.inventory();
		require(inventory.has_value(), "missing client inventory");
		unsigned total = 0;
		for (const auto stack : inventory->slots)
			if (stack.item == item)
				total += stack.count;
		return total;
	};
	first.connect(now);
	until([&] { return first.inventory().has_value(); }, "missing initial inventory");
	second.connect(now);
	until([&] { return second.inventory().has_value(); }, "missing second initial inventory");
	const auto firstId = first.welcome()->player;
	const auto secondId = second.welcome()->player;
	constexpr glm::dvec3 firstFeet{8.5, 245, 4.5}, secondFeet{8.5, 245, 13.5};
	sim.setPlayerPosition(farFeet);
	sim.setPlayerPosition(firstId, firstFeet);
	sim.setPlayerPosition(secondId, secondFeet);
	require(firstId < secondId && first.inventory()->revision == 1 && second.inventory()->revision == 1,
		"initial per-owner revisions or pickup order differ");
	require(sim.grantItems(firstId, Item::Dirt, 5).remainder.isEmpty() &&
		sim.grantItems(secondId, Item::Glass, 1).remainder.isEmpty(), "seed private inventories");
	until([&] { return count(first, Item::Dirt) == 5 && count(second, Item::Glass) == 1; }, "owner inventories diverged");
	require(count(first, Item::Glass) == 0 && count(second, Item::Dirt) == 0, "owner inventory leaked");
	const auto oldInventory = *clean.lastInventory;
	const auto secondStarting = *second.inventory();

	// Valid server-only payloads sent in the wrong direction, and a valid
	// intent carrying another peer's scope, cannot consume the next action.
	wire::Inventory forged{host.ticks(), 100, 0, {}};
	forged.slots[0] = {Item::Glass, 64};
	clean.sendToServer({*first.scope(), 1, forged}, wire::Endpoint::Server);
	clean.sendToServer({*second.scope(), 1, wire::Action{1, Kind::DropStack}});
	clean.dropInventory = true;
	action(first, Kind::Select, 2);
	require(first.inventory()->selected == 2 && clean.droppedInventory == clean.lastInventory &&
		clean.lastActionResult->sequence > clean.lastInventory->sequence, "result overtook lost inventory");
	require(*second.inventory() == secondStarting && count(first, Item::Glass) == 0 &&
		sim.itemIdHighWater() == 0, "forged inventory intent mutated authority");
	action(first, Kind::SplitHalf, 0, 1);
	require(first.inventory()->slots[0].count == 2 && first.inventory()->slots[1].count == 3, "split did not converge");
	action(first, Kind::Move, 1, 2);
	action(first, Kind::Swap, 0, 2);
	require(first.inventory()->slots[0].count == 3 && first.inventory()->slots[2].count == 2, "move or swap diverged");
	action(first, Kind::DropOne, 0);
	require(first.inventory()->slots[0].count == 2 && sim.itemIdHighWater() == 1, "drop one did not settle");
	action(first, Kind::DropStack, 2);
	const uint64_t contested = sim.itemIdHighWater();
	require(contested == 2 && first.inventory()->slots[2].isEmpty(), "drop stack did not settle");

	// Let the drops settle before moving both players into pickup range
	// in the same server tick.
	sim.setPlayerPosition(firstId, {12.5, 245, 4.5});
	until([&] { return itemRecord(first, contested) && itemRecord(second, contested) &&
		items.entities().back().pickupDelay == 0; }, "drop failed to replicate within interest");
	const auto spawnPacket = *clean.lastItemSnapshot;
	const auto contestedFeet = items.entities().back().body.position;
	sim.setPlayerPosition(firstId, contestedFeet);
	sim.setPlayerPosition(secondId, contestedFeet);
	clean.dropItemSnapshot = true;
	until([&] { return items.entities().empty() && count(first, Item::Dirt) == 5; },
		"same-tick contenders duplicated or lost dirt");
	until([&] { return !itemRecord(first, contested) && !itemRecord(second, contested); }, "pickup did not remove drop");
	require(clean.droppedItemSnapshot && count(second, Item::Dirt) == 0 && *second.inventory() == secondStarting,
		"higher PlayerId received a contested item");
	const auto removalPacket = *clean.lastItemSnapshot;
	clean.send(removalPacket);
	clean.send(spawnPacket);
	clean.send(spawnPacket);
	clean.send(oldInventory);
	first.poll(now);
	require(!itemRecord(first, contested) && count(first, Item::Dirt) == 5 &&
		first.inventory()->selected == 2, "reordered or duplicate packets rolled back the projection");

	// Exhaust the first inventory's room for dirt during pickup, so the
	// same live entity has a new stack count on both observers.
	const auto maxDirt = static_cast<uint16_t>(items::kInventorySize * 64 - 2);
	require(sim.grantItems(firstId, Item::Dirt, maxDirt - 5).remainder.isEmpty(), "fill pickup fixture");
	sim.setPlayerPosition(firstId, firstFeet);
	sim.setPlayerPosition(secondId, secondFeet);
	const auto remainderId = items.spawn({Item::Dirt, 6}, firstFeet, {}, 12);
	until([&] { const auto *item = itemRecord(first, remainderId); return item && item->stack.count == 6; },
		"spawn count was missing");
	const auto fullPacket = *clean.lastItemSnapshot;
	until([&] {
		const auto *a = itemRecord(first, remainderId);
		const auto *b = itemRecord(second, remainderId);
		return a && b && a->stack.count == 4 && b->stack.count == 4 && count(first, Item::Dirt) == maxDirt + 2;
	}, "partial pickup failed to converge");
	const auto updatePacket = *clean.lastItemSnapshot;
	clean.send(fullPacket);
	first.poll(now);
	require(itemRecord(first, remainderId) && itemRecord(first, remainderId)->stack.count == 4, "old count overwrote partial pickup");
	sim.setPlayerPosition(secondId, firstFeet);
	until([&] { return count(second, Item::Dirt) == 4 && !itemRecord(first, remainderId) && !itemRecord(second, remainderId); },
		"second pickup did not settle the remainder");
	clean.send(updatePacket);
	first.poll(now);
	require(!itemRecord(first, remainderId), "stale count resurrected a picked-up item");

	const glm::dvec3 mobFeet{12.5, 245, 9.5};
	require(mobs.add(entities::MobSpecies::Sheep, mobFeet, 301, 1, WorldCollisionView(*sim.world())), "seed passive mob");
	const auto mobId = mobs.mobs().front().replicationId;
	const auto nearItem = items.spawn({Item::Dirt, 1}, mobFeet, {}, 10000);
	constexpr auto extra = wire::kMaxItemsPerSnapshot + 4;
	for (size_t i = 0; i < extra; ++i)
		require(items.spawn({Item::Stone, 1}, {12.5, 245, 13.5}, {}, 10000) > nearItem, "seed item overflow");
	until([&] { return seesMob(first, mobId) && seesMob(second, mobId) && itemRecord(second, nearItem) &&
		second.itemSnapshots().current->count == wire::kMaxItemsPerSnapshot; }, "nearby bounded projection missing");
	require(impaired.itemBytes == 1193 && impaired.mobBytes == 72, "unexpected measured snapshot size");
	const auto priorEntityTick = first.itemSnapshots().current->tick;
	clean.sendToServer({*first.scope(), 1, wire::ItemSnapshot{host.ticks()}}, wire::Endpoint::Server);
	clean.sendToServer({*first.scope(), 1, wire::MobSnapshot{host.ticks()}}, wire::Endpoint::Server);
	until([&] { return first.itemSnapshots().current->tick > priorEntityTick; }, "missing subsequent authority snapshot");
	require(itemRecord(first, nearItem) && seesMob(first, mobId), "client forged an entity removal");
	const auto oldMob = *impaired.lastMobSnapshot;
	sim.setPlayerPosition(secondId, farFeet);
	until([&] { return second.itemSnapshots().current->count == 0 && !seesMob(second, mobId); }, "interest did not remove entities");
	impaired.send(oldMob);
	second.poll(now);
	require(seesMob(first, mobId) && !seesMob(second, mobId), "old interest resurrected a mob");
	sim.setPlayerPosition(secondId, firstFeet);
	until([&] { return seesMob(second, mobId) && itemRecord(second, nearItem); }, "interest re-entry changed entity identities");
	require(second.mobSnapshots().previous &&
		second.mobSnapshots().previous->tick < second.mobSnapshots().current->tick, "interpolation history missing");

	const auto beforeExpiry = *clean.lastItemSnapshot;
	items.settings.despawnAgeTicks = 1;
	until([&] { return first.itemSnapshots().current->count == 0; }, "age expiry did not remove projected items");
	clean.send(beforeExpiry);
	first.poll(now);
	require(first.itemSnapshots().current->count == 0, "pre-expiry snapshot resurrected a despawned item");
	items.settings.despawnAgeTicks = 18000;
	const auto highWater = items.spawn({Item::Glass, 1}, mobFeet, {}, 10000);
	const auto priorScope = *first.scope();
	first.disconnect(now);
	until([&] { return first.status() == udp::ClientStatus::Disconnected; }, "disconnect did not settle");
	require(!first.inventory() && !first.itemSnapshots().current && !first.mobSnapshots().current,
		"retired epoch left stale UI state");
	first.connect(now);
	until([&] { return first.inventory() && itemRecord(first, highWater); }, "reconnect did not converge");
	clean.send(oldInventory);
	clean.send(beforeExpiry);
	first.poll(now);
	require(first.scope() != priorScope && first.inventory()->revision == 1 && count(first, Item::Dirt) == 0 &&
		first.itemSnapshots().current->idHighWater == highWater, "old scope rolled back reconnect");
	std::cout << "EntityReplication loopback: max item datagram " << impaired.itemBytes
		<< " bytes, one-mob datagram " << impaired.mobBytes << " bytes\n";
	transport.stop(host);
	require(host.stop(error), error);
	server::ServerHost restarted(config);
	require(restarted.start(error), error);
	auto &restored = server::ServerHostProbe::simulation(restarted);
	require(restored.itemIdHighWater() == highWater && restored.itemEntities().size() == 1 &&
		restored.itemEntities().entities().front().id == highWater, "checkpoint lost shared item IDs after expiry and reconnect");
	require(restored.grantItems(Item::Stone, 1).remainder.isEmpty() &&
		restored.dropFromSlot(0, 1, {0, 1, 0}) == items::ItemResult::Ok &&
		restored.itemIdHighWater() == highWater + 1, "checkpoint reused an item ID");
	require(restarted.stop(error), error);
}

void testThreadedDelivery(const fs::path &root)
{
	server::HostConfig config;
	config.world = {.seed = 19, .worldName = "threaded", .savesRoot = root};
	config.workers = 0;
	config.interestRadiusChunks = 1;
	server::ServerHost host(config);
	std::string error;
	require(host.start(error), error);
	udp::ServerTransport server(loopback, 1, host.sessionWorld());
	udp::ClientTransport client(server.address());
	server.start();
	const auto start = udp::ServerTransport::Clock::now();
	client.connect(start);
	while (!client.chunks().contains(spawn) && udp::ServerTransport::Clock::now() - start < 8s)
	{
		client.poll(udp::ServerTransport::Clock::now());
		server.rethrowIoError();
		server.serviceOwner(host, host.ticks());
		require(host.step(server::TerrainTiming::Background, error), error);
		std::this_thread::sleep_for(5ms);
	}
	std::optional<wire::ActionResult> result;
	if (client.chunks().contains(spawn))
	{
		const auto actionStart = udp::ServerTransport::Clock::now();
		const wire::Action action{.operation = wire::ActionKind::Break, .target = {400, 8, 80},
			.expected = client.chunks().at(spawn).version};
		if (client.sendAction(action, actionStart))
		{
			while (!(result = client.takeActionResult()) && udp::ServerTransport::Clock::now() - actionStart < 4s)
			{
				client.poll(udp::ServerTransport::Clock::now());
				server.rethrowIoError();
				server.serviceOwner(host, host.ticks());
				require(host.step(server::TerrainTiming::Background, error), error);
				std::this_thread::sleep_for(5ms);
			}
		}
	}
	server.stop(host);
	server.rethrowIoError();
	require(client.chunks().contains(spawn) && server.worldTraffic().frames >= wire::kFragmentsPerChunk,
		"worker did not deliver a complete baseline from the owner mailbox");
	require(result && result->actionId == 1 && result->code == wire::ActionCode::OutOfReach && result->editId == 0,
		"worker did not deliver the owner's action result");
	require(host.stop(error), error);
}
} // namespace

int main(int argc, char **argv)
{
	const auto root = fs::temp_directory_path() / ("ft_vox_replication_" + std::to_string(udp::randomId()));
	int result = 0;
	try
	{
		if (argc == 2 && std::string_view(argv[1]) == "--actions")
			testVoxelActions(root);
		else if (argc == 2 && std::string_view(argv[1]) == "--entities")
			testEntityReplication(root);
		else
		{
			testIncompatibleGenerator();
			testReplication(root);
			testThreadedDelivery(root);
		}
	}
	catch (const std::exception &error)
	{
		std::cerr << error.what() << '\n';
		result = 1;
	}
	std::error_code cleanup;
	fs::remove_all(root, cleanup);
	return result || cleanup;
}
