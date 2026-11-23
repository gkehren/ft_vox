#include <Items/ItemRegistry.hpp>
#include <Network/ClientTransport.hpp>
#include <Network/ClientMovement.hpp>
#include <Network/ClientTickScheduler.hpp>
#include <Network/ClientWorld.hpp>
#include <Server/ServerHost.hpp>
#include <World/WorldCollisionView.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
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
	Relay(udp::Address server, std::chrono::milliseconds delay, udp::Address bind = loopback)
		: socket(bind), server(server), delay(delay) {}

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
				if (chunk->index == 0 && std::exchange(dropFragment, false))
					continue;
				if (chunk->index == 0 && std::exchange(duplicateFragment, false))
					pending.emplace(now + delay + 150ms, std::pair{to, frame});
				const auto encodedSize = chunk->baseline.encodedSize;
				if (chunk->index + 1u == wire::fragmentCount(encodedSize) && std::exchange(corruptFragment, false))
				{
					// The last byte is a material for either encoding.
					auto &material = chunk->data[wire::fragmentSize(chunk->index, encodedSize) - 1];
					material = material == wire::kAirMaterial ? static_cast<uint8_t>(STONE) : wire::kAirMaterial;
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
			const auto transit = profile ? profile(packet, fromServer, now) : std::optional{delay};
			if (transit)
			{
				pending.emplace(now + *transit, std::pair{to, frame});
				if (duplicateMovement && packet.sequence % 17 == 0 &&
				    (std::holds_alternative<wire::Input>(packet.payload) ||
				     std::holds_alternative<wire::PlayerSnapshot>(packet.payload)))
				{
					pending.emplace(now + *transit + 20ms, std::pair{to, frame});
					++movementDuplicates;
				}
			}
		}
		while (!pending.empty() && pending.begin()->first <= now)
		{
			const auto &[to, packet] = pending.begin()->second;
			require(socket.send(to, std::span(packet.bytes).first(packet.size)), "relay send failed");
			pending.erase(pending.begin());
		}
	}

	udp::Socket socket;
	udp::Address server, client{};
	std::chrono::milliseconds delay;
	std::function<std::optional<std::chrono::milliseconds>(const wire::Packet &, bool, Time)> profile;
	bool dropFragment{}, duplicateFragment{}, corruptFragment{}, dropEdit{}, duplicateEdit{};
	bool dropActionResult{}, dropAllActionResults{}, dropActionAck{};
	bool dropInventory{}, dropItemSnapshot{};
	bool duplicateMovement{};
	unsigned movementDuplicates{};
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
	require(clean.lastResync == wire::ResyncReason::Baseline && first.welcome()->player == firstId,
		"resync did not retain the persistent actor under a fresh scope");

	const auto corruptScope = *second.scope();
	impaired.corruptFragment = true;
	sim.setPlayerPosition(secondId, spawnFeet);
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

	// Reserve the second actor before opening any transport slots to make
	// actor ID order differ from slot order.
	const auto secondKey = udp::randomPlayerKey();
	const auto enrolled = host.joinRemote(secondKey).player;
	require(enrolled.valid() && host.activateRemote(enrolled), "could not enroll the offline fixture actor");
	host.leaveRemote(enrolled);
	auto server = std::make_unique<udp::ServerTransport>(loopback, 2, host.sessionWorld());
	Relay clean(server->address(), 0ms), impaired(server->address(), 0ms);
	udp::ClientTransport first(clean.socket.address()), second(impaired.socket.address(), secondKey);
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
	const auto firstId = first.welcome()->player;
	require(secondId == enrolled && secondId < firstId, "fixture did not reverse actor and slot order");
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
	require(second.scope() != oldScope && !second.takeActionResult() && second.welcome()->player == secondId,
		"reconnect did not clear the old scope or preserve the actor");
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
		simulation().itemIdHighWater() > initialItemId + 2 && host.metrics().items == 2 &&
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

void testAnchorPickup(const fs::path &root)
{
	server::HostConfig config;
	config.world = {.seed = 19, .worldName = "anchor_pickup", .savesRoot = root};
	config.workers = 0;
	config.interestRadiusChunks = 1;
	server::ServerHost host(config);
	std::string error;
	require(host.start(error), error);
	auto &sim = server::ServerHostProbe::simulation(host);
	auto &items = const_cast<items::ItemEntitySystem &>(sim.itemEntities());
	udp::ServerTransport transport(loopback, 2, host.sessionWorld());
	udp::ClientTransport first(transport.address()), second(transport.address());
	const std::array clients{&first, &second};
	Time now{};
	const auto until = [&](auto ready, const char *failure) {
		for (int i = 0; i < 120; ++i)
		{
			if (ready())
				return;
			for (auto *client : clients)
				client->poll(now);
			transport.poll(now);
			transport.serviceOwner(host, host.ticks());
			transport.poll(now);
			for (auto *client : clients)
				client->poll(now);
			require(host.step(server::TerrainTiming::QuiescePerTick, error), error);
			now += 17ms;
		}
		require(ready(), failure);
	};

	first.connect(now);
	until([&] { return first.inventory().has_value(); }, "first pickup contender failed to join");
	second.connect(now);
	until([&] { return second.inventory().has_value(); }, "second pickup contender failed to join");
	const auto firstId = first.welcome()->player;
	const auto secondId = second.welcome()->player;
	require(sim.localPlayerId() < firstId && firstId < secondId, "pickup fixture did not order the anchor first");
	sim::InventorySnapshot anchorBefore;
	sim.inventorySnapshot(anchorBefore);
	constexpr auto dirt = items::ItemId::Dirt;
	const auto count = [](const auto &slots) {
		unsigned total = 0;
		for (const auto stack : slots)
			if (stack.item == items::ItemId::Dirt)
				total += stack.count;
		return total;
	};
	const auto verifyPickup = [&](uint16_t dropCount, unsigned firstTotal, unsigned secondTotal) {
		const auto anchorFeet = sim.playerFeet();
		sim.setPlayerPosition(firstId, anchorFeet);
		sim.setPlayerPosition(secondId, anchorFeet);
		const auto id = items.spawn({dirt, dropCount}, anchorFeet, {}, 18);
		require(id != 0, "seed contested drop");
		const auto seesDrop = [id](const udp::ClientTransport &client) {
			const auto &snapshot = client.itemSnapshots().current;
			return snapshot && snapshot->count == 1 && snapshot->items.front().id == id;
		};
		until([&] { return seesDrop(first) && seesDrop(second); }, "pickup contenders did not see the drop");
		require(items.size() == 1 && items.entities().front().pickupDelay > 0,
			"pickup fixture missed the live drop");
		for (const auto idInRange : {sim.localPlayerId(), firstId, secondId})
			require(glm::distance(sim.playerFeet(idInRange), items.entities().front().body.position) <=
				items.settings.pickupRadius, "pickup contender is out of range");
		const auto converged = [&] {
			return first.itemSnapshots().current->count == 0 && second.itemSnapshots().current->count == 0 &&
				count(first.inventory()->slots) == firstTotal && count(second.inventory()->slots) == secondTotal;
		};
		until(converged, "remote pickup did not converge or the anchor stole the drop");
		const auto removalTick = first.itemSnapshots().current->tick;
		until([&] { return first.itemSnapshots().current->tick > removalTick; }, "missing post-pickup snapshot");
		sim::InventorySnapshot anchorAfter;
		sim.inventorySnapshot(anchorAfter);
		require(converged() && anchorAfter == anchorBefore && items.size() == 0 &&
			sim.itemIdHighWater() == id && count(sim.inventory(firstId)->slots()) == firstTotal &&
			count(sim.inventory(secondId)->slots()) == secondTotal, "pickup was duplicated or changed the anchor inventory");
	};
	verifyPickup(3, 3, 0);
	const auto capacity = static_cast<uint16_t>(items::kInventorySize * items::maxStackOf(dirt));
	require(sim.grantItems(firstId, dirt, capacity - 4).remainder.isEmpty(), "fill the first contender's inventory");
	verifyPickup(4, capacity, 3);
	transport.stop(host);
	require(host.stop(error), error);
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
	const auto savedInventory = *first.inventory();
	first.disconnect(now);
	until([&] { return first.status() == udp::ClientStatus::Disconnected; }, "disconnect did not settle");
	require(!first.inventory() && !first.itemSnapshots().current && !first.mobSnapshots().current,
		"retired epoch left stale UI state");
	first.connect(now);
	until([&] { return first.inventory() && itemRecord(first, highWater); }, "reconnect did not converge");
	clean.send(oldInventory);
	clean.send(beforeExpiry);
	first.poll(now);
	require(first.scope() != priorScope && first.welcome()->player == firstId &&
		first.inventory()->revision == 1 && first.inventory()->slots == savedInventory.slots &&
		first.inventory()->selected == savedInventory.selected &&
		first.itemSnapshots().current->idHighWater == highWater, "old scope rolled back reconnect");
	std::cout << "EntityReplication loopback: max item datagram " << impaired.itemBytes
		<< " bytes, one-mob datagram " << impaired.mobBytes << " bytes\n";
	transport.stop(host);
	require(host.stop(error), error);
	server::ServerHost restarted(config);
	require(restarted.start(error), error);
	auto &restored = server::ServerHostProbe::simulation(restarted);
	const auto reservedThrough = restored.itemIdHighWater();
	require(reservedThrough > highWater && restored.itemEntities().size() == 1 &&
		restored.itemEntities().entities().front().id == highWater, "checkpoint lost shared item IDs after expiry and reconnect");
	require(restored.grantItems(Item::Stone, 1).remainder.isEmpty() &&
		restored.dropFromSlot(0, 1, {0, 1, 0}) == items::ItemResult::Ok &&
		restored.itemIdHighWater() == reservedThrough + 1, "checkpoint reused an item ID");
	require(restarted.stop(error), error);
}

void testClientTickMath()
{
	using Scheduler = udp::ClientTickScheduler;
	Scheduler scheduler;
	const Time now{};
	require(scheduler.schedule(now).count == 0, "scheduled without a snapshot");
	scheduler.observeSnapshot(100, now);
	const auto fallback = scheduler.schedule(now);
	require(!scheduler.roundTripEstimate() && fallback.firstTick >= 102 &&
		udp::withinInputWindow(fallback.firstTick + fallback.count - 1, 100), "unmeasured fallback");
	scheduler.observeRoundTrip(-1ms);
	scheduler.observeRoundTrip(Scheduler::Seconds{std::numeric_limits<double>::infinity()});
	scheduler.observeRoundTrip(24h);
	require(!scheduler.roundTripEstimate(), "invalid RTT accepted");
	scheduler.observeRoundTrip(120ms);
	const auto delayed = scheduler.schedule(now);
	require(delayed.firstTick == 108 && delayed.count == 2 &&
		!udp::withinInputWindow(delayed.firstTick, 100) && udp::withinInputWindow(delayed.firstTick, 107),
		"used the snapshot tick instead of the expected arrival window");

	std::vector<wire::Input> sent;
	const auto emit = [&](const wire::Input &sample) { sent.push_back(sample); return true; };
	const wire::Input controls{.moveX = 1.f, .flags = wire::Input::sprint | wire::Input::jump};
	// First write succeeds and the jump write hits backpressure.
	require(!scheduler.send(now, controls, [&](auto sample) {
		return (sample.flags & wire::Input::jump) == 0 && emit(sample);
	}), "reported an unsent jump");
	require(scheduler.send(now, controls, emit) && sent.size() == 2 &&
		sent[0].intendedTick < sent[1].intendedTick && sent[0].flags == wire::Input::sprint &&
		sent[1].flags == (wire::Input::sprint | wire::Input::jump), "jump edge duplicated or lost in batch");
	require(scheduler.schedule(now).count == 0, "reused a sent tick");
	require(scheduler.schedule(now + 500ms).count == 0, "stale snapshot extrapolated through a frame stall");
	scheduler.observeSnapshot(130, now + 500ms);
	require(scheduler.schedule(now + 500ms).firstTick > 130 &&
		scheduler.schedule(now + 500ms).count <= Scheduler::kMaxBatch, "replayed backlog on frame resume");

	scheduler.reset();
	scheduler.observeRoundTrip(20ms);
	scheduler.observeRoundTrip(400ms);
	require(*scheduler.roundTripEstimate() < 40ms, "a single RTT outlier jumped the clock");
	for (int i = 0; i < 12; ++i)
		scheduler.observeRoundTrip(120ms);
	require(*scheduler.roundTripEstimate() > 110ms, "repeated measurements did not adapt");
	scheduler.observeSnapshot(40, now);
	scheduler.send(now + 100ms, {}, emit);
	scheduler.observeSnapshot(41, now + 100ms);
	require(scheduler.schedule(now + 100ms).count == 0, "backward correction reused sent ticks");
	scheduler.observeSnapshot(50, now + 150ms);
	require(scheduler.schedule(now + 150ms).firstTick > sent.back().intendedTick,
		"clock correction jammed the sent tick cursor");

	scheduler.reset();
	scheduler.observeRoundTrip(32ms);
	scheduler.observeSnapshot(100, now);
	scheduler.send(now, {}, emit);
	require(sent.back().intendedTick == 103, "unexpected initial lead");
	const auto afterAcquire = scheduler.schedule(now + 40ms);
	require(afterAcquire.firstTick == 104 && afterAcquire.count == 3,
		"late render frame skipped an input tick that could still be admitted");

	scheduler.reset();
	scheduler.observeRoundTrip(32ms);
	scheduler.observeSnapshot(100, now);
	const auto beforeLatePoll = scheduler.schedule(now + 65ms);
	scheduler.observeSnapshot(103, now + 65ms);
	const auto afterLatePoll = scheduler.schedule(now + 65ms);
	require(beforeLatePoll.firstTick + beforeLatePoll.count == afterLatePoll.firstTick + afterLatePoll.count,
		"late polling of a snapshot stalled the running input clock");

	scheduler.reset();
	require(!scheduler.roundTripEstimate() && scheduler.schedule(now).count == 0, "session timing survived reset");
	scheduler.observeSnapshot(UINT64_MAX - 2, Time::min());
	require(scheduler.schedule(Time::max()).count == 0, "extreme clock endpoints overflowed");
	require(scheduler.schedule(Time::min()).count == 0, "tick overflow wrapped");
	scheduler.observeRoundTrip(0ms);
	scheduler.send(Time::min(), {}, emit);
	require(sent.back().intendedTick == UINT64_MAX && scheduler.schedule(Time::min()).count == 0,
		"last representable tick wrapped the send cursor");
	scheduler.reset();
	scheduler.observeSnapshot(0, now);
	require(scheduler.schedule(now).firstTick < 10, "new epoch could not start from tick zero");
}

void testClientTimingSamples()
{
	udp::Socket server(loopback);
	udp::ClientTransport client(server.address());
	const Time start{};
	client.connect(start);
	client.poll(start);
	udp::Received frame;
	require(server.receive(frame), "first Hello missing");
	wire::Packet hello;
	require(wire::decode(wire::Endpoint::Client, std::span(frame.bytes).first(frame.size), hello) ==
		wire::CodecStatus::Ok, "read first Hello");
	const auto peer = frame.from;
	const auto nonce = std::get<wire::Hello>(hello.payload).nonce;
	const auto reply = [&](wire::Packet packet) {
		wire::Datagram datagram;
		require(wire::encode(wire::Endpoint::Server, packet, datagram) == wire::CodecStatus::Ok &&
			server.send(peer, datagram.view()), "write timing reply");
	};
	const wire::SessionScope scope{13, 17};
	const sim::PlayerId player{2};
	const wire::Welcome welcome{nonce, 3, player, 42, TerrainGenerator::kGeneratorVersion,
		wire::localCompatibility(), 0, 1};
	client.poll(start + 500ms); // Same nonce as the first attempt.
	require(server.receive(frame), "Hello retry missing");
	reply({scope, 0, welcome});
	client.poll(start + 520ms);
	require(client.status() == udp::ClientStatus::Connected && !client.takeRoundTripSample(),
		"ambiguous Hello provided an RTT sample");

	const wire::Action select{.operation = wire::ActionKind::Select, .slot = 1};
	require(client.sendAction(select, start + 530ms) == 1, "queue first action");
	client.poll(start + 550ms);
	reply({scope, 0, wire::Ack{1}});
	reply({scope, 1, wire::ActionResult{1, 0, 1, 0, wire::ActionKind::Select, wire::ActionCode::Accepted}});
	client.poll(start + 630ms);
	require(client.takeRoundTripSample() == 80ms && !client.takeRoundTripSample() && client.takeActionResult(),
		"action RTT did not use actual first send and matched ACK");
	require(client.sendAction(select, start + 640ms) == 2, "queue second action");
	client.poll(start + 650ms);
	client.poll(start + 800ms); // Successfully retransmitted before ACK.
	reply({scope, 0, wire::Ack{2}});
	client.poll(start + 810ms);
	require(!client.takeRoundTripSample(), "retransmitted action provided an RTT sample");

	wire::PlayerSnapshot snapshot{.tick = 50, .count = 1};
	snapshot.players[0].player = player;
	reply({scope, 1, snapshot});
	client.poll(start + 830ms);
	require(client.playerSnapshotReceivedAt() == start + 830ms, "snapshot receipt timestamp");
	require(client.sendInput({53}, start + 830ms) && client.sendInput({100}, start + 1600ms) &&
		client.inputAcknowledgmentStalled(start + 1600ms), "missing input acknowledgment diagnostic");
	auto impossible = snapshot;
	impossible.tick = 99;
	impossible.lastProcessedInput = 3; // Input 3 is due at tick 100.
	reply({scope, 99, impossible});
	impossible.lastProcessedInput = wire::kMaxSequence;
	reply({scope, 100, impossible});
	snapshot.tick = 100;
	snapshot.lastProcessedInput = 3; // Heartbeat then two inputs.
	reply({scope, 2, snapshot});
	client.poll(start + 1610ms);
	require(!client.inputAcknowledgmentStalled(start + 1610ms), "input acknowledgment did not recover");
	require(client.latestPlayerSnapshot() == snapshot, "impossible acknowledgment poisoned the fresh sequence");
	impossible.tick = 101;
	impossible.lastProcessedInput = 2;
	reply({scope, 101, impossible});
	client.poll(start + 1615ms);
	require(client.latestPlayerSnapshot() == snapshot, "regressive acknowledgment replaced authoritative movement");

	client.disconnect(start + 1620ms);
	client.poll(start + 1620ms);
	reply({scope, 0, wire::Ack{3}});
	client.poll(start + 1630ms);
	require(client.status() == udp::ClientStatus::Disconnected && !client.playerSnapshotReceivedAt() &&
		!client.takeRoundTripSample(), "timing state survived departure");
	while (server.receive(frame)) {}
	client.connect(start + 1840ms);
	client.poll(start + 1840ms);
	require(server.receive(frame) && wire::decode(wire::Endpoint::Client,
		std::span(frame.bytes).first(frame.size), hello) == wire::CodecStatus::Ok, "read new Hello");
	auto restarted = welcome;
	restarted.nonce = std::get<wire::Hello>(hello.payload).nonce;
	restarted.worldId++;
	const wire::SessionScope newScope{21, 22};
	reply({newScope, 0, restarted});
	reply({scope, 3, snapshot}); // Delayed old-epoch snapshot.
	snapshot.tick = 0;
	snapshot.lastProcessedInput = 0;
	reply({newScope, 1, snapshot});
	client.poll(start + 1860ms);
	require(client.scope() == newScope && client.takeRoundTripSample() == 20ms &&
		client.latestPlayerSnapshot() && client.latestPlayerSnapshot()->tick == 0 &&
		client.playerSnapshotReceivedAt() == start + 1860ms,
		"new session and epoch inherited the old timing stream");
}

// Keep the real server's admission and physics while measuring movement
// separately from the cost of streaming world images.
struct MovementOwner : udp::SessionOwner
{
	explicit MovementOwner(server::ServerHost &host) : host(host) {}
	udp::SessionAdmission joinRemote(const wire::PlayerKey &key) override { return host.joinRemote(key); }
	bool activateRemote(sim::PlayerId player) override { return host.activateRemote(player); }
	void leaveRemote(sim::PlayerId player) override { host.leaveRemote(player); }
	std::optional<wire::ActionResult> receiveRemote(sim::PlayerId player, const udp::SessionIntents &intents) override
	{
		return host.receiveRemote(player, intents);
	}
	wire::PlayerSnapshot snapshotRemote(sim::PlayerId player) const override { return host.snapshotRemote(player); }

	server::ServerHost &host;
};

struct FlatCollision : physics::VoxelCollisionWorld
{
	physics::Cell sample(glm::ivec3 p) const override
	{
		if (unknown && p.x >= 4)
			return {};
		return {true, p.y <= floor || (wall && p.x == 4)};
	}

	int floor = 0;
	bool wall{}, unknown{};
};

void testDelayedJoin(const fs::path &root)
{
	server::HostConfig config;
	config.world = {.seed = 19, .worldName = "timing-cold-join", .savesRoot = root};
	config.workers = 0;
	config.interestRadiusChunks = 1;
	server::ServerHost host(config);
	std::string error;
	require(host.start(error), error);
	udp::ServerTransport transport(loopback, 1, host.sessionWorld());
	Relay relay(transport.address(), 75ms);
	udp::ClientTransport client(relay.socket.address());
	const Time start{};
	client.connect(start);
	auto nextTick = start;
	for (auto now = start; now < start + wire::kChunkTransferTimeout; now += 1ms)
	{
		relay.pump(now);
		transport.poll(now);
		if (now >= nextTick)
		{
			transport.serviceOwner(host, host.ticks());
			require(host.step(server::TerrainTiming::QuiescePerTick, error), error);
			nextTick += 16667us;
		}
		transport.poll(now);
		relay.pump(now);
		client.poll(now);
		if (client.chunks().contains(spawn) && client.inventory() && client.playerSnapshots().current)
		{
			require(now - start <= 2s && !relay.lastResync, "150 ms cold join missed its 2 s budget");
			std::cout << "150 ms cold join ready in " << (now - start) / 1ms << " ms\n";
			transport.stop(host);
			require(host.stop(error), error);
			return;
		}
	}
	require(false, "150 ms cold join did not publish spawn terrain, inventory and player");
}

void testPredictionResets()
{
	FlatCollision world;
	udp::ClientPrediction prediction;
	wire::PlayerSnapshot sample{.tick = 100, .count = 1};
	auto &local = sample.players[0];
	local = {{2}, {3.6, 1.001, 0.5}, {}, wire::PlayerState::grounded, 1};
	const Time start{1s};
	prediction.observe(sample, local, start, world);
	for (uint64_t i = 1; i <= prediction.kMaxTicks; ++i)
		prediction.record(i, {100 + i, 1.f}, start, world);
	const auto predicted = prediction.feet();
	require(predicted.x > 5.0 && prediction.backlog() == prediction.kMaxTicks, "prediction did not fill its bounded history");
	prediction.record(33, {UINT64_MAX, 1.f}, start, world);
	prediction.record(wire::kMaxSequence + 1, {133, 1.f}, start, world);
	prediction.record(1, {101, -1.f}, start, world);
	require(prediction.feet() == predicted, "invalid or stale input advanced prediction");

	// The authority rejected every moving input and the local terrain
	// changed. No smoothing or replay may preserve the old speculative pose.
	world.wall = true;
	sample.tick = 132;
	prediction.observe(sample, local, start + 80ms, world);
	require(prediction.backlog() == 0 && prediction.present(start + 80ms, world) == udp::playerFeet(local) &&
		prediction.stats().hardResets == 1, "large correction retained a rejected pose");
	prediction.observe({.tick = 131, .lastProcessedInput = 32}, local, start + 81ms, world);
	require(prediction.feet() == udp::playerFeet(local), "outdated server tick replaced the anchor");
	for (unsigned i = 1; i <= 10; ++i)
		prediction.record(32 + i, {132 + i, 1.f}, start + 82ms, world);
	const auto atWall = prediction.present(start + 90ms, world);
	require(atWall && atWall->x < 3.701, "local prediction crossed a wall");

	// Teleport by less than the smoothing threshold, with an unprocessed tail.
	++sample.tick;
	++local.motionReset;
	local.feet[0] -= 0.1;
	prediction.observe(sample, local, start + 100ms, world);
	require(prediction.backlog() == 0 && prediction.present(start + 100ms, world) == udp::playerFeet(local) &&
		prediction.stats().hardResets == 2, "small teleport was smoothed or replayed");
	prediction.record(50, {140, -1.f}, start + 110ms, world);
	prediction.invalidateTerrain();
	prediction.record(51, {141, -1.f}, start + 111ms, world);
	require(prediction.backlog() == 0 && prediction.feet() == udp::playerFeet(local), "terrain reset reused pre-edit inputs");
	world.unknown = true;
	local.feet[0] = 4.5;
	++sample.tick;
	++local.motionReset;
	prediction.observe(sample, local, start + 120ms, world);
	require(!prediction.present(start + 120ms, world), "teleport presented a body inside unknown terrain");

	prediction = {}; // Scope/epoch transition clears timers, input and correction state.
	world.wall = world.unknown = false;
	sample.tick = 0;
	local = {{2}, {0.5, 1.001, 0.5}, {}, wire::PlayerState::grounded, 1};
	prediction.observe(sample, local, start + 1s, world);
	prediction.record(1, {1, 0.f, 0.f, wire::Input::jump}, start + 1s, world);
	require(prediction.feet().y > 1.1, "fresh epoch did not predict its first jump");
	prediction.expire(start + 1500ms);
	require(prediction.backlog() == 0 && prediction.feet() == udp::playerFeet(local),
		"500 ms frame stall retained speculative movement");

	prediction = {};
	sample.tick = 200;
	local.feet[0] = 3.6;
	prediction.observe(sample, local, start, world);
	for (uint64_t i = 1; i <= 7; ++i)
		prediction.record(i, {200 + i, 1.f}, start, world);
	sample.tick += 7;
	prediction.observe(sample, local, start + 50ms, world);
	const auto smoothed = prediction.present(start + 50ms, world);
	require(smoothed && smoothed->x > 3.7, "minor correction did not smooth in clear terrain");
	world.wall = true;
	require(prediction.present(start + 51ms, world) == udp::playerFeet(local),
		"minor correction offset crossed newly solid terrain");

	prediction = {};
	sample.tick = 300;
	sample.localTimers[0] = 0.1;
	local.feet = {0.5, 1.06, 0.5};
	local.velocity = {0.0f, -1.0f, 0.0f};
	local.flags = 0;
	prediction.observe(sample, local, start, world);
	prediction.record(1, {305}, start, world);
	require(prediction.feet().y > 1.1, "prediction lost an acknowledged buffered jump");
}

void testLocalPresentationCadence()
{
	FlatCollision world;
	udp::ClientPrediction prediction;
	constexpr auto kWalkSpeed = physics::PlayerSettings{}.walkSpeed;
	constexpr auto kTick = std::chrono::duration<double>{1.0 / wire::kTickRate};
	constexpr auto kFrame = 7ms;
	const Time start{1s};
	wire::PlayerSnapshot snapshot{.tick = 100, .count = 1};
	auto &player = snapshot.players[0];
	player = {{2}, {8.0, 1.001, 8.0}, {float(kWalkSpeed), 0.f, 0.f}, wire::PlayerState::grounded, 1};
	prediction.observe(snapshot, player, start, world);
	uint64_t sent = 0, acknowledged = 0;
	double leastSpeed = kWalkSpeed, mostSpeed = kWalkSpeed;
	auto previous = prediction.present(start, world);
	for (auto age = kFrame; age <= 1200ms; age += kFrame)
	{
		const auto tick = static_cast<uint64_t>(age / kTick);
		const auto now = start + age;
		if ((tick / 3) * 3 > acknowledged)
		{
			acknowledged = (tick / 3) * 3;
			snapshot.tick = 100 + acknowledged;
			snapshot.lastProcessedInput = acknowledged;
			player.feet[0] = 8.0 + double(acknowledged) * kTick.count() * kWalkSpeed;
			prediction.observe(snapshot, player, now, world);
		}
		while (sent < tick + 2)
		{
			++sent;
			prediction.record(sent, {100 + sent, 1.f}, now, world);
		}
		const auto pose = prediction.present(now, world);
		require(previous && pose, "steady local walking lost its camera pose");
		if (age >= 200ms)
		{
			const double speed = (pose->x - previous->x) / std::chrono::duration<double>(kFrame).count();
			leastSpeed = std::min(leastSpeed, speed);
			mostSpeed = std::max(mostSpeed, speed);
		}
		previous = pose;
	}
	std::cout << "LocalCamera 7ms walk expected=" << kWalkSpeed << " min=" << leastSpeed
		<< " max=" << mostSpeed << " blocks/s\n";
	require(leastSpeed >= kWalkSpeed * 0.95 && mostSpeed <= kWalkSpeed * 1.05,
		"local camera stalled or jumped between movement ticks");
}

void testRemoteInterpolation()
{
	udp::RemotePlayers players;
	const Time start{1s};
	wire::PlayerSnapshot sample{.tick = 6, .count = 2};
	sample.players[0].player = {2};
	auto &remote = sample.players[1];
	remote = {{3}, {0.0, 2.0, 0.0}};
	players.observe(sample, start);
	for (unsigned i = 1; i <= 4; ++i)
	{
		sample.tick += 3;
		remote.feet[0] = double(i);
		players.observe(sample, start + i * 50ms);
	}
	const auto between = players.present(remote.player, start + 225ms);
	require(between && std::abs(between->x - 2.5) < 1e-8,
		"remote position was not interpolated on the delayed tick clock");
	require(players.present(remote.player, start + 400ms) == udp::playerFeet(remote),
		"loss extrapolated remote movement");
	require(!players.present(remote.player, start + 500ms), "stale remote player was not hidden");
	auto stale = sample;
	sample.tick += 6;
	sample.count = 1;
	players.observe(sample, start + 300ms);
	players.observe(stale, start + 310ms);
	require(!players.present(remote.player, start + 310ms) && players.missingFraction() > 0.0,
		"late packet revived a departed player or hid a missing snapshot");
	sample.tick += 3;
	sample.count = 2;
	remote.feet[0] = 10.0;
	players.observe(sample, start + 350ms);
	require(players.present(remote.player, start + 350ms) == udp::playerFeet(remote),
		"interest re-entry interpolated across an absence");
	sample.tick += 3;
	remote.feet[0] = 10.1;
	++remote.motionReset;
	players.observe(sample, start + 400ms);
	require(players.present(remote.player, start + 400ms) == udp::playerFeet(remote),
		"remote teleport interpolated across a discontinuity");
}

void testDelayedInput(const fs::path &root, std::chrono::milliseconds delay, bool impaired, unsigned frameRate = 60)
{
	server::HostConfig config;
	config.world = {.seed = 19, .worldName = "timing-" + std::to_string(delay.count()) +
		(impaired ? "-impaired-" : "-") + std::to_string(frameRate), .savesRoot = root};
	config.workers = 0;
	config.interestRadiusChunks = 2;
	config.streaming.generations = 0;
	server::ServerHost host(config);
	MovementOwner owner(host);
	std::string error;
	require(host.start(error), error);
	auto &sim = server::ServerHostProbe::simulation(host);
	for (int x = 0; x < 16; ++x)
		for (int z = 6; z <= 10; ++z)
			require(sim.placeVoxel({x + 0.5f, 230.5f, z + 0.5f}, STONE).accepted, "prepare walking surface");

	udp::ServerTransport transport(loopback, 1, host.sessionWorld());
	Relay relay(transport.address(), delay);
	relay.duplicateMovement = impaired;
	udp::ClientTransport client(relay.socket.address());
	udp::ClientTickScheduler scheduler;
	udp::ClientPrediction prediction;
	udp::RemotePlayers remotePlayers;
	FlatCollision collision;
	collision.floor = 230;
	physics::PlayerController offline;
	std::optional<Time> offlineOnset, predictedOnset, authorityOnset;
	size_t peakBacklog = 0;
	std::optional<Time> movementStart;
	uint64_t sentTick = 0, sentSequence = 0;
	unsigned emitted = 0, delivered = 0, jumps = 0, processed = 0, stableProcessed = 0;
	std::set<uint64_t> stableSequences;
	unsigned recovered = 0;
	bool reordered = false, droppedSnapshot = false, droppedInput = false;
	Time latestDelivery{};
	relay.profile = [&](const wire::Packet &packet, bool fromServer, Time now) -> std::optional<std::chrono::milliseconds> {
		const auto age = movementStart ? now - *movementStart : 0ms;
		const auto *input = std::get_if<wire::Input>(&packet.payload);
		if (input && !fromServer)
		{
			require(input->intendedTick > sentTick && packet.sequence > sentSequence, "nonmonotonic input or Fresh sequence");
			sentTick = input->intendedTick;
			sentSequence = packet.sequence;
			++emitted;
			if ((input->flags & wire::Input::jump) != 0)
				++jumps;
			if (age >= 400ms && age < 1200ms)
				stableSequences.insert(packet.sequence);
			if (age >= 1450ms && age < 1990ms)
				require(input->moveX == 0 && input->flags == 0, "neutral controls queued a held command");
		}
		if (!impaired || !movementStart ||
		    (!input && !std::holds_alternative<wire::PlayerSnapshot>(packet.payload)))
			return delay;
		if (input && emitted % 13 == 0)
		{
			droppedInput = true;
			return std::nullopt;
		}
		if (!input && ((age >= 2090ms && age < 2480ms) || packet.sequence % 11 == 0))
		{
			droppedSnapshot = true;
			return std::nullopt;
		}
		const auto transit = std::chrono::milliseconds{20 + int(packet.sequence % 3) * 15};
		if (input)
		{
			reordered |= now + transit < latestDelivery;
			latestDelivery = now + transit;
			++delivered;
		}
		return transit;
	};

	const Time start{};
	client.connect(start);
	uint64_t lastProcessed = 0;
	auto nextServer = start;
	auto nextFrame = start;
	using Tick = std::chrono::duration<int64_t, std::ratio<1, wire::kTickRate>>;
	int64_t frameNumber = 0;
	double beginX = 0, turnX = 0, endX = 0;
	double leastViewSpeed = physics::PlayerSettings{}.walkSpeed, mostViewSpeed = leastViewSpeed;
	unsigned walkingFrames = 0;
	std::optional<glm::dvec3> previousView;
	std::optional<Time> previousFrame;
	bool jump = false, jumpStarted = false, airborne = false, neutralStopped = false;
	for (Time now = start; now < start + 6s; now += 1ms)
	{
		const auto age = movementStart ? now - *movementStart : 0ms;
		relay.pump(now);
		transport.poll(now);
		if (now >= nextServer)
		{
			transport.serviceOwner(owner, host.ticks());
			require(host.step(server::TerrainTiming::QuiescePerTick, error), error);
			nextServer = start + std::chrono::ceil<Time::duration>(Tick{static_cast<int64_t>(host.ticks())});
			if (movementStart)
			{
				const auto welcome = client.welcome();
				require(welcome && sim.hasPlayer(welcome->player), "timing fixture lost its live session");
				const auto player = welcome->player;
				const auto state = host.snapshotRemote(player);
				require(state.lastProcessedInput >= lastProcessed, "authoritative input acknowledgment ran backward");
				if (state.lastProcessedInput > lastProcessed)
				{
					++processed;
					stableProcessed += stableSequences.contains(state.lastProcessedInput);
					recovered += age > 3000ms;
					lastProcessed = state.lastProcessedInput;
				}
				sim::PlayerSnapshot local;
				require(sim.playerSnapshot(player, local), "lost authoritative player during test");
				if (!authorityOnset && local.position.x > beginX + 0.01)
					authorityOnset = now;
				airborne |= local.velocity.y > 0.5;
				if (age >= 1850ms && age < 2000ms)
				{
					turnX = local.position.x;
					neutralStopped |= std::abs(local.velocity.x) < 0.01;
				}
				if (age >= 3500ms)
					endX = local.position.x;
			}
		}
		transport.poll(now);
		relay.pump(now);
		if (now >= nextFrame)
		{
			nextFrame = start + std::chrono::ceil<Time::duration>(std::chrono::duration<double>{
				double(++frameNumber) / frameRate});
			if (impaired && age >= 2380ms && age < 2880ms)
				continue;
			client.poll(now);
			if (const auto rtt = client.takeRoundTripSample())
				scheduler.observeRoundTrip(*rtt);
			if (const auto &sample = client.playerSnapshots().current)
			{
				const auto stamp = *client.playerSnapshotReceivedAt();
				const auto &local = sample->players[0];
				require(local.player == client.welcome()->player, "timing fixture received an unexpected actor");
				scheduler.observeSnapshot(sample->tick, stamp);
				prediction.observe(*sample, local, stamp, collision);
				remotePlayers.observe(*sample, stamp);
				if (!movementStart)
				{
					movementStart = now;
					beginX = local.feet[0];
					offline.reset(udp::playerFeet(local));
				}
			}
			if (movementStart)
			{
				if (!impaired && !jumpStarted && age >= 650ms)
					jump = jumpStarted = true;
				const float move = age < 1300ms ? 1.f : age >= 2000ms && age < 3600ms ? -1.f : 0.f;
				unsigned batch = 0;
				if (scheduler.send(now, {.moveX = move, .flags = jump ? wire::Input::jump : uint8_t{0}}, [&](auto input) {
					++batch;
					const auto sequence = client.sendInput(input, now);
					if (sequence)
						prediction.record(sequence, input, now, collision);
					return sequence != 0;
				}))
					jump = false;
				prediction.expire(now);
				peakBacklog = std::max(peakBacklog, prediction.backlog());
				const double frameDt = previousFrame ? std::chrono::duration<double>(now - *previousFrame).count() :
					1.0 / frameRate;
				offline.advance(frameDt, {{move, 0.0}}, collision);
				if (!offlineOnset && offline.body.position.x > beginX + 0.01)
					offlineOnset = now;
				const auto pose = prediction.present(now, collision);
				require(pose.has_value(), "predicted view lost its safe body");
				if (previousView && age >= 400ms && age < 600ms)
				{
					++walkingFrames;
					const auto speed = (pose->x - previousView->x) / frameDt;
					leastViewSpeed = std::min(leastViewSpeed, speed);
					mostViewSpeed = std::max(mostViewSpeed, speed);
				}
				previousView = pose;
				previousFrame = now;
				if (!predictedOnset && pose->x > beginX + 0.01)
					predictedOnset = now;
				require(batch <= udp::ClientTickScheduler::kMaxBatch, "unbounded frame catch-up");
			}
		}
	}
	require(movementStart && scheduler.roundTripEstimate() &&
		*scheduler.roundTripEstimate() >= delay * 2, "unambiguous handshake timing missing");
	require(turnX > beginX + 1.5 && endX < turnX - 1.25 && neutralStopped && recovered > 20,
		"authoritative movement or neutral/recovery failed at " + config.world.worldName);
	if (impaired)
		require(reordered && droppedInput && droppedSnapshot && delivered > 50 && relay.movementDuplicates &&
			remotePlayers.missingFraction() > 0.0, "did not exercise relay impairments");
	else
	{
		require(stableSequences.size() > 35 && stableProcessed * 4 >= stableSequences.size() * 3 &&
			jumps == 1 && airborne, "stable admission or single jump failed at " + config.world.worldName);
		constexpr auto kWalkSpeed = physics::PlayerSettings{}.walkSpeed;
		require(walkingFrames >= 10 && leastViewSpeed >= kWalkSpeed * 0.95 && mostViewSpeed <= kWalkSpeed * 1.05,
			"steady camera motion failed at " + config.world.worldName);
	}
	const auto final = sim.playerFeet(client.welcome()->player);
	require(predictedOnset && authorityOnset && offlineOnset && *predictedOnset < *authorityOnset &&
		*predictedOnset <= *offlineOnset + 17ms && glm::length(final - prediction.feet()) < 0.01,
		"prediction lost responsiveness or failed to converge at " + config.world.worldName);
	const auto milliseconds = [start = *movementStart](Time now) { return (now - start) / 1ms; };
	const auto &stats = prediction.stats();
	const auto traffic = client.traffic();
	std::cout << "ClientTickScheduling " << config.world.worldName << " RTT=" <<
		std::chrono::duration_cast<std::chrono::milliseconds>(*scheduler.roundTripEstimate()).count()
		<< "ms processed=" << processed << '/' << emitted << " stable=" << stableProcessed << '/' << stableSequences.size()
		<< " neutralX=" << turnX << " finalX=" << endX
		<< " onset_ms(offline/predicted/server)=" << milliseconds(*offlineOnset) << '/' <<
		milliseconds(*predictedOnset) << '/' << milliseconds(*authorityOnset)
		<< " corrections=" << stats.corrections << " max_m=" << stats.maxCorrection
		<< " resets=" << stats.hardResets << " max_pending=" << peakBacklog
		<< " snapshot_missing=" << remotePlayers.missingFraction()
		<< " walking_view_mps(min/max)=" << leastViewSpeed << '/' << mostViewSpeed
		<< " final_error_m=" << glm::length(final - prediction.feet())
		<< " udp_bytes(rx/tx)=" << traffic.receivedBytes << '/' << traffic.sentBytes << '\n';
	transport.stop(owner);
	require(host.stop(error), error);
}

void testRenderBacking()
{
	udp::ClientWorld view;
	auto &world = view.world();
	udp::ChunkProjection images;
	constexpr wire::ChunkVersion firstVersion{7, 0};
	constexpr glm::ivec3 west{-1, 0, 0};
	constexpr glm::ivec3 east{0, 0, 0};
	constexpr glm::ivec3 westBorder{-1, 42, 2};
	const size_t index = WorldChunk::index(CHUNK_SIZE - 1, westBorder.y, westBorder.z);
	for (int x = -1; x <= 3; ++x)
	{
		auto &image = images[{x, 0}];
		image.version = firstVersion;
		image.materials.fill(wire::kAirMaterial);
	}
	auto &image = images.at({-1, 0});
	image.materials[index] = STONE;
	view.reconcile(images, {-8.f, 42.f, 8.f});
	require(world.chunkCount() == udp::ClientWorld::kPublicationsPerFrame && !world.getChunk({3, 0, 0}),
		"replica did not prioritize nearby chunks within its budget");
	auto *westRecord = world.getChunk(west);
	auto *eastRecord = world.getChunk(east);
	require(westRecord && eastRecord && westRecord->occupiedSectionMask() == (1 << 2) &&
		view.observedVersion(westBorder) == firstVersion, "baseline did not publish native voxels and wire version");
	world.ensureShellPopulated(eastRecord);
	require(eastRecord->sampleForMeshing(-1, 42, 2) == STONE, "replica seam lost the baseline border");
	const auto eastBeforeEdit = eastRecord->revision();
	require(!world.deleteVoxel(glm::vec3(westBorder)).accepted, "replica accepted a local edit");

	westRecord->setInTransit(true);
	image.version.revision++;
	image.materials[index] = wire::kAirMaterial;
	{
		const udp::ClientWorld::CollisionView collision(view, images);
		require(!collision.sample(westBorder).available, "prediction sampled stale terrain behind a render lease");
	}
	view.reconcile(images, {-8.f, 42.f, 8.f});
	require(view.observedVersion(westBorder) == firstVersion && world.isVoxelActive(glm::vec3(westBorder)),
		"replica overwrote a record leased by a worker");
	westRecord->setInTransit(false);
	view.reconcile(images, {-8.f, 42.f, 8.f});
	{
		const udp::ClientWorld::CollisionView collision(view, images);
		require(collision.sample(westBorder).available && !collision.sample(westBorder).solid,
			"prediction did not resume on the matching publication");
	}
	require(view.observedVersion(westBorder) == image.version && westRecord->occupiedSectionMask() == 0 &&
		eastRecord->revision() > eastBeforeEdit, "delta did not invalidate occupancy and neighboring work");
	world.ensureShellPopulated(eastRecord);
	require(eastRecord->sampleForMeshing(-1, 42, 2) == AIR, "replica seam retained the previous border");

	const auto beforeRetire = westRecord->identity();
	westRecord->setInTransit(true);
	images.erase({-1, 0});
	view.reconcile(images, {-8.f, 42.f, 8.f});
	{
		const WorldCollisionView collision(world);
		require(!collision.sample(westBorder).available && westRecord->isReplicaRetiring() &&
			!view.observedVersion(westBorder) && westRecord->revision() > beforeRetire.revision,
			"unloaded chunk remained queryable or valid for an old mesh publication");
	}
	auto &replacement = images[{-1, 0}];
	replacement.version = {8, 0};
	replacement.materials.fill(wire::kAirMaterial);
	replacement.materials[index] = DIRT;
	view.reconcile(images, {-8.f, 42.f, 8.f});
	require(!view.observedVersion(westBorder), "a re-entry revived a retired worker lease");
	westRecord->setInTransit(false);
	view.reconcile(images, {-8.f, 42.f, 8.f});
	require(view.observedVersion(westBorder) == replacement.version &&
		world.getChunk(west)->getVoxel(CHUNK_SIZE - 1, 42, 2).type == DIRT,
		"re-entry did not install the latest baseline");
}

// Exercise the same verified collision view and snapshot/input order as the
// graphical host. Stage edits independently of replica publication.
struct ReplicaPrediction
{
	udp::ClientWorld world;
	udp::ChunkProjection verified;
	udp::ClientWorld::TerrainDependencies terrain;
	udp::ClientPrediction prediction;
	wire::PlayerSnapshot sample{.tick = 97, .count = 1};
	Time now{1s};
	uint64_t sequence{}, intendedTick{};

	udp::ClientWorld::CollisionView collision()
	{
		return {world, verified, &terrain};
	}

	physics::Cell cell(glm::ivec3 position) { return collision().sample(position); }
	udp::ClientWorld::TerrainChanges terrainChanges() { return collision().terrainChanges(); }

	void stageFloor(wire::ChunkCoord coord)
	{
		auto &image = verified[coord];
		image.version = {1, 0};
		image.materials.fill(wire::kAirMaterial);
		std::fill_n(image.materials.begin(), CHUNK_SIZE * CHUNK_SIZE, uint8_t(STONE));
	}

	void edit(glm::ivec3 cell, uint8_t material)
	{
		const wire::ChunkCoord coord{floorDivChunk(cell.x), floorDivChunk(cell.z)};
		auto &image = verified.at(coord);
		image.materials[WorldChunk::index(cell.x - coord.x * CHUNK_SIZE, cell.y,
			cell.z - coord.z * CHUNK_SIZE)] = material;
		++image.version.revision;
	}

	void publish()
	{
		world.reconcile(verified, glm::vec3(udp::playerFeet(sample.players[0])));
	}

	void snapshot(glm::dvec3 feet, uint32_t motionReset = 1)
	{
		now += 50ms;
		sample.tick += wire::kPlayerSnapshotIntervalTicks;
		intendedTick = std::max(intendedTick, sample.tick);
		auto &local = sample.players[0];
		local = {{2}, {feet.x, feet.y, feet.z}, {}, wire::PlayerState::grounded, motionReset};
		prediction.observe(sample, local, now, collision());
	}

	void input(float moveX, unsigned ticks = 1)
	{
		const auto view = collision();
		for (unsigned i = 0; i < ticks; ++i)
			prediction.record(++sequence, {++intendedTick, moveX}, now, view);
	}

	std::optional<glm::dvec3> camera()
	{
		const auto view = collision();
		prediction.checkTerrain(view);
		return prediction.present(now, view);
	}
};

void testReplicaCameraHold(bool leased)
{
	ReplicaPrediction client;
	constexpr glm::dvec3 anchor{8.5, 1.001, 8.5};
	client.stageFloor({0, 0});
	client.publish();
	client.snapshot(anchor);
	client.input(1.f, 3);
	const auto displayed = client.camera();
	require(displayed && displayed->x > anchor.x && client.prediction.backlog() == 3,
		"camera fixture did not establish a predicted pose");

	auto *record = client.world.world().getChunk({0, 0, 0});
	require(record != nullptr, "missing standing chunk");
	record->setInTransit(leased);
	client.edit({10, 1, 8}, STONE);
	for (int frame = 0; frame < 3; ++frame)
	{
		if (leased)
			client.publish();
		if (frame == 1)
			client.edit({10, 2, 8}, STONE);
		client.snapshot(anchor);
		client.input(-1.f);
		require(!client.cell({8, 0, 8}).available &&
			client.prediction.waitingForTerrain() && client.prediction.backlog() == 0 &&
			client.prediction.feet() == anchor && client.camera() == displayed &&
			client.prediction.stats().hardResets == 1, "revision gap moved the camera or reset prediction twice");
	}
	record->setInTransit(false);
	client.publish();
	client.input(-1.f);
	require(client.cell({8, 0, 8}).solid && client.camera() == displayed &&
		client.prediction.backlog() == 0, "publication resumed prediction without a fresh snapshot");
	client.snapshot(anchor);
	client.input(1.f);
	const auto resumed = client.camera();
	require(resumed && std::abs(resumed->y - displayed->y) < 0.01 && resumed->x > anchor.x &&
		!client.prediction.waitingForTerrain() && client.prediction.backlog() == 1 &&
		client.prediction.stats().hardResets == 1, "fresh anchor failed to resume after publication");
}

void testReplicaTerrainChanges()
{
	ReplicaPrediction client;
	constexpr glm::dvec3 anchor{8.5, 1.001, 8.5};
	client.stageFloor({0, 0});
	client.stageFloor({1, 0});
	client.publish();
	client.snapshot(anchor);
	client.input(1.f, 3);
	const auto nextAnchor = client.prediction.feet();
	client.input(1.f);
	const auto before = client.camera();
	client.edit({24, 1, 8}, STONE);
	require(!client.terrainChanges().invalidated, "unqueried neighbor invalidated prediction");
	client.publish();
	client.now += 17ms;
	client.input(1.f);
	const auto after = client.camera();
	require(before && after && after->x > before->x && client.prediction.backlog() == 5 &&
		client.prediction.stats().hardResets == 0, "unrelated edit interrupted movement");
	client.snapshot(nextAnchor);
	const auto reconciled = client.camera();
	require(reconciled && client.prediction.backlog() == 2 && client.prediction.stats().hardResets == 0,
		"unrelated edit discarded the pending replay tail");

	client.edit({8, 0, 8}, wire::kAirMaterial);
	require(client.camera() == reconciled && client.prediction.backlog() == 0 &&
		!client.cell({8, 0, 8}).available, "support edit used stale physics or moved the camera");
	client.publish();
	client.snapshot(anchor);
	client.input(0.f, 6);
	const auto falling = client.camera();
	require(falling && falling->y < 0.95 && falling->y > 0.0, "prediction did not fall through removed support");

	client.edit({8, 0, 8}, STONE);
	require(client.camera() == falling, "unpublished support placement moved the camera");
	client.publish();
	require(!client.camera(), "held pose remained visible inside newly published solid terrain");
	client.snapshot(anchor);
	require(client.camera() == anchor, "camera did not follow authoritative recovery");
}

void testReplicaTerrainBoundaries()
{
	for (const int edge : {16, 0, -16})
	{
		ReplicaPrediction client;
		const glm::dvec3 anchor{edge - 0.55, 1.001, 8.5};
		client.stageFloor({floorDivChunk(edge - 1), 0});
		client.stageFloor({floorDivChunk(edge), 0});
		client.publish();
		client.snapshot(anchor);
		client.input(1.f, 18);
		const auto crossed = client.camera();
		require(crossed && crossed->x > edge, "prediction fixture did not cross the seam");
		client.edit({edge, 1, 8}, STONE);
		require(client.terrainChanges().invalidated && client.camera() == crossed &&
			client.prediction.backlog() == 0, "destination chunk was absent from the swept footprint");
		client.publish();
		require(!client.camera(), "camera projected the held body through a published obstacle");
		client.snapshot(anchor);
		client.input(1.f, 18);
		const auto stopped = client.camera();
		require(stopped && stopped->x < edge - 0.299 && client.prediction.stats().hardResets == 1,
			"re-anchored movement ignored the obstacle at the seam");
	}

	ReplicaPrediction client;
	constexpr glm::dvec3 anchor{15.45, 1.001, 8.5};
	client.stageFloor({0, 0});
	client.publish();
	client.snapshot(anchor);
	client.input(1.f, 18);
	const auto blocked = client.camera();
	require(blocked && blocked->x < 15.701, "prediction traversed unknown terrain");
	client.stageFloor({1, 0});
	require(!client.terrainChanges().invalidated, "verified baseline counted as readable before publication");
	client.publish();
	require(client.terrainChanges().invalidated && client.camera() == blocked &&
		client.prediction.backlog() == 0, "publication did not retire the history blocked by unknown terrain");
	client.snapshot(anchor);
	client.input(1.f, 12);
	const auto crossed = client.camera();
	require(crossed && crossed->x > 16.0, "prediction could not enter the verified neighbor");
}

void testReplicaTerrainDiscontinuities()
{
	ReplicaPrediction client;
	constexpr glm::dvec3 origin{8.5, 1.001, 8.5}, destination{88.5, 1.001, 8.5};
	client.stageFloor({0, 0});
	client.publish();
	client.snapshot(origin);
	require(client.camera() == origin, "initial replica pose missing");
	auto *oldBacking = client.world.world().getChunk({0, 0, 0});
	require(oldBacking != nullptr, "missing teleport origin backing");
	oldBacking->setInTransit(true);
	client.edit({8, 0, 8}, wire::kAirMaterial);
	require(client.camera() == origin && client.prediction.waitingForTerrain(), "teleport fixture was not holding a pose");
	client.snapshot(destination, 2);
	require(!client.camera() && client.prediction.waitingForTerrain(),
		"teleport reused the old visual hold at an unavailable destination");
	client.stageFloor({5, 0});
	client.publish();
	require(!client.camera(), "teleport drew a pose before its destination was re-anchored");
	client.snapshot(destination, 2);
	require(client.camera() == destination, "teleport failed to establish a new safe pose");
	oldBacking->setInTransit(false);
	client.input(1.f, 2);
	const auto newPose = client.camera();
	client.edit({88, 0, 8}, wire::kAirMaterial);
	require(newPose && client.camera() == newPose && client.prediction.waitingForTerrain(), "epoch fixture did not hold");
	client.terrain = {};
	client.prediction = {};
	client.sequence = client.intendedTick = 0;
	require(!client.camera() && !client.prediction.waitingForTerrain() &&
		!client.terrainChanges().invalidated && client.prediction.backlog() == 0,
		"retiring the session left its presentation or prediction live");
	client.sample.tick = 0;
	constexpr glm::dvec3 newEpochFeet{-80.5, 1.001, 8.5};
	client.snapshot(newEpochFeet);
	require(!client.camera(), "new epoch used an old pose during cold join");
	client.stageFloor({floorDivChunk(-81), 0});
	client.publish();
	client.snapshot(newEpochFeet);
	require(client.camera() == newEpochFeet, "new epoch inherited the old camera offset");
	client.input(1.f);
	require(client.prediction.backlog() == 1, "new epoch inherited the old input sequence");
}

void testReplicaTerrainCapacity()
{
	ReplicaPrediction client;
	client.stageFloor({0, 0});
	client.publish();
	client.snapshot({8.5, 1.001, 8.5});
	client.input(1.f);
	{
		const auto view = client.collision();
		for (size_t i = 1; i < udp::ClientWorld::TerrainDependencies::kCapacity; ++i)
			require(!view.sample({int(i * CHUNK_SIZE), 1, 8}).available, "synthetic dependency should be unknown");
		require(!view.terrainChanges().invalidated, "dependency footprint overflowed early");
		const int beyond = int(udp::ClientWorld::TerrainDependencies::kCapacity * CHUNK_SIZE);
		require(!view.sample({beyond, 1, 8}).available && view.terrainChanges().invalidated,
			"dependency overflow failed to demand conservative invalidation");
		client.prediction.checkTerrain(view);
	}
	require(client.prediction.waitingForTerrain() && client.prediction.backlog() == 0 &&
		client.prediction.stats().hardResets == 1, "overflow retained untracked prediction");
	client.snapshot({8.5, 1.001, 8.5});
	require(client.camera() && !client.prediction.waitingForTerrain() && !client.terrainChanges().invalidated,
		"a fresh bounded footprint did not recover after overflow");
}

struct ClosureLoop
{
	ClosureLoop(server::ServerHost &host, size_t cap) : host(host), transport(loopback, cap, host.sessionWorld()) {}

	server::ServerHost &host;
	udp::ServerTransport transport;
	Time now{}, nextTick{};
	std::vector<std::pair<Relay *, udp::ClientTransport *>> peers;

	void pump()
	{
		for (const auto &[relay, client] : peers)
			relay->pump(now);
		transport.poll(now);
		if (now >= nextTick)
		{
			std::string error;
			transport.serviceOwner(host, host.ticks());
			require(host.step(server::TerrainTiming::QuiescePerTick, error), error);
			nextTick += std::chrono::ceil<Time::duration>(std::chrono::duration<double>{server::kTickDt});
		}
		transport.poll(now);
		for (const auto &[relay, client] : peers)
		{
			relay->pump(now);
			client->poll(now);
		}
		now += 5ms;
	}

	template<class Predicate>
	void until(Predicate ready, const char *error, std::chrono::milliseconds timeout = 12s)
	{
		const auto deadline = now + timeout;
		while (!ready())
		{
			require(now < deadline, error);
			pump();
		}
	}

	void advance(std::chrono::milliseconds duration)
	{
		const auto deadline = now + duration;
		until([&] { return now >= deadline; }, "could not advance clock", duration);
	}

	wire::ActionResult action(udp::ClientTransport &client, wire::Action value)
	{
		const auto id = client.sendAction(value, now);
		require(id.has_value(), "could not queue closure action");
		std::optional<wire::ActionResult> result;
		until([&] { return (result = client.takeActionResult()).has_value(); }, "closure action timed out");
		require(result->actionId == *id && result->operation == value.operation &&
			result->code == wire::ActionCode::Accepted, "closure action did not apply");
		return *result;
	}
};

void measureChunkDelivery(const fs::path &root, std::chrono::milliseconds rtt, size_t clientCount, bool loss,
	bool enforceBudget = false)
{
	server::HostConfig config;
	config.world = {.seed = 19, .worldName = "chunks-" + std::to_string(rtt.count()) + "-" +
		std::to_string(clientCount) + (loss ? "-loss" : "-clean"), .savesRoot = root};
	config.workers = 0;
	config.interestRadiusChunks = 1;
	server::ServerHost host(config);
	std::string error;
	require(host.start(error), error);
	auto &sim = server::ServerHostProbe::simulation(host);
	const auto &world = *sim.world();
	ClosureLoop loop(host, clientCount);
	struct Peer
	{
		Peer(udp::Address server, std::chrono::milliseconds delay, size_t index, bool loss)
			// Distinct loopback sources model independent hosts at discovery.
			: relay(server, delay, udp::parseAddress("127.0.0." + std::to_string(index + 2), 0)),
			  client(relay.socket.address()), fragmentAttempts(index), ackAttempts(index)
		{
			if (loss)
				relay.profile = [this, delay](const wire::Packet &packet, bool fromServer, Time)
					-> std::optional<std::chrono::milliseconds> {
					// Drop one in ten transmissions independently in each
					// direction. Retries go through the same schedule.
					if ((fromServer && std::holds_alternative<wire::ChunkFragment>(packet.payload) &&
					     fragmentAttempts++ % 10 == 0) ||
					    (!fromServer && std::holds_alternative<wire::Ack>(packet.payload) &&
					     ackAttempts++ % 10 == 0))
						return std::nullopt;
					return delay;
				};
		}
		Relay relay;
		udp::ClientTransport client;
		size_t fragmentAttempts, ackAttempts;
		std::optional<std::chrono::milliseconds> firstPlayable;
	};
	std::vector<std::unique_ptr<Peer>> peers;
	for (size_t i = 0; i < clientCount; ++i)
	{
		auto &peer = *peers.emplace_back(std::make_unique<Peer>(loop.transport.address(), rtt / 2, i, loss));
		loop.peers.emplace_back(&peer.relay, &peer.client);
		peer.client.connect(loop.now);
	}
	const auto settled = [&](Coord center) {
		return std::ranges::all_of(peers, [&](const auto &peer) {
			const auto &chunks = peer->client.chunks();
			return chunks.size() == 5 && std::ranges::all_of(VoxelWorld::playerInterestOffsets(1),
				[&](const auto offset) { return chunks.contains({center.x + offset.x, center.z + offset.z}); });
		});
	};

	const auto start = loop.now;
	auto firstMin = std::chrono::milliseconds::max();
	std::chrono::milliseconds firstMax{};
	size_t peakTransfers{};
	loop.until([&] {
		peakTransfers = std::max(peakTransfers, loop.transport.worldSendQueue().baselines);
		for (const auto &peer : peers)
		{
			if (peer->firstPlayable || !peer->client.chunks().contains(spawn) ||
			    !peer->client.inventory() || !peer->client.latestPlayerSnapshot())
				continue;
			peer->firstPlayable = std::chrono::duration_cast<std::chrono::milliseconds>(loop.now - start - 5ms);
			firstMin = std::min(firstMin, *peer->firstPlayable);
			firstMax = std::max(firstMax, *peer->firstPlayable);
		}
		return settled(spawn) && std::ranges::all_of(peers, [](const auto &peer) { return peer->firstPlayable.has_value(); });
	}, "clients did not settle the spawn neighborhood", 90s);
	const auto coldMs = (loop.now - start - 5ms) / 1ms;
	const auto cold = loop.transport.worldTraffic();
	require(std::ranges::all_of(peers, [&](const auto &peer) { return matches(peer->client, world, spawn); }),
		"spawn neighborhood differs from the authority");
	constexpr Coord shifted{4, 0};
	for (const auto &peer : peers)
		sim.restorePlayerFlight(peer->client.welcome()->player, true, {72.5, 245, 8.5});
	const auto shiftStart = loop.now;
	loop.until([&] {
		peakTransfers = std::max(peakTransfers, loop.transport.worldSendQueue().baselines);
		return settled(shifted);
	}, "clients did not sustain streaming after changing interest", 90s);
	const auto shiftMs = (loop.now - shiftStart - 5ms) / 1ms;
	const auto total = loop.transport.worldTraffic();
	require(std::ranges::all_of(peers, [&](const auto &peer) {
		return matches(peer->client, world, shifted) && !peer->relay.lastResync;
	}), "shifted clients need a resync or differ from the authority");
	require(peakTransfers > 0 && peakTransfers <= std::min(clientCount, udp::kMaxConcurrentChunkTransfers),
		"cold chunk delivery exceeded its transfer slots");
	require(total.resyncs == 0 && (!loss || (cold.retransmits > 0 && total.retransmits > cold.retransmits)),
		"chunk delivery did not demonstrate retry recovery in both phases");
	std::cout << "COLD_CHUNKS rtt_ms=" << rtt.count() << " clients=" << clientCount << " loss_pct=" << (loss ? 10 : 0)
		<< " first_min_ms=" << firstMin.count() << " first_max_ms=" << firstMax.count()
		<< " neighborhood_ms=" << coldMs << " cold_bytes=" << cold.bytes << " cold_retries=" << cold.retransmits
		<< " shift_ms=" << shiftMs << " shift_bytes=" << total.bytes - cold.bytes
		<< " shift_retries=" << total.retransmits - cold.retransmits << " peak_transfers=" << peakTransfers << '\n';
	if (enforceBudget)
	{
		require(peakTransfers == udp::kMaxConcurrentChunkTransfers, "did not saturate the global transfer limit");
		require(firstMax <= 4s && coldMs <= 12'000 && shiftMs <= 14'000,
			"contended chunk delivery missed its timing budget");
		require(cold.bytes <= clientCount * 128 * 1024 && total.bytes - cold.bytes <= clientCount * 128 * 1024,
			"contended chunk delivery missed its traffic budget");
	}
	loop.transport.stop(host);
	require(host.stop(error), error);
}

struct ClosureState
{
	std::vector<std::tuple<Coord, uint64_t, uint64_t, uint64_t>> chunks;
	std::array<wire::PlayerState, 2> players;
	std::array<std::array<items::ItemStack, items::kInventorySize>, 2> inventories;
	std::array<int, 2> selected{};
	std::vector<wire::ItemState> drops;
	uint64_t nextEdit{}, itemHighWater{};
	bool operator==(const ClosureState &) const = default;
};

void verifyClosureReopen(const server::HostConfig &config, const ClosureState &saved,
	const std::array<wire::PlayerKey, 2> &profiles, uint64_t worldId, uint64_t epoch, uint64_t generation)
{
	server::ServerHost host(config);
	std::string error;
	require(host.start(error), error);
	require(host.restoredPlayer() && host.metrics().checkpointGeneration == generation,
		"closure restart did not open the committed generation");
	ClosureLoop loop(host, profiles.size());
	Relay firstRelay(loop.transport.address(), 0ms), secondRelay(loop.transport.address(), 0ms);
	udp::ClientTransport first(firstRelay.socket.address(), profiles[0]), second(secondRelay.socket.address(), profiles[1]);
	loop.peers = {{&firstRelay, &first}, {&secondRelay, &second}};
	// Rejoin in reverse order to exercise persistent actor identity.
	second.connect(loop.now);
	loop.until([&] { return second.inventory().has_value(); }, "second profile could not resume");
	first.connect(loop.now);
	loop.until([&] {
		for (const auto *client : {&first, &second})
		{
			const auto &players = client->playerSnapshots().current;
			if (!client->inventory() || !players || players->count != saved.players.size() ||
				!client->itemSnapshots().current || client->chunks().size() != saved.chunks.size())
				return false;
		}
		return true;
	}, "restarted clients did not receive all committed state");
	const std::array clients{&first, &second};
	const auto editWatermark = host.metrics().nextEditId;
	require(editWatermark > 0, "restart did not initialize an edit watermark for the new epoch");
	for (size_t i = 0; i < clients.size(); ++i)
	{
		const auto &client = *clients[i];
		require(client.welcome()->worldId == worldId && client.scope()->epoch != epoch &&
			client.scope()->epoch == first.scope()->epoch && client.welcome()->player == saved.players[i].player,
			"closure restart reused the epoch or changed the world or actor");
		require(client.welcome()->nextEditId == editWatermark, "restart advertised an inconsistent edit watermark");
		for (const auto &[coord, savedGeneration, revision, hash] : saved.chunks)
		{
			const auto chunk = client.chunks().find(coord);
			require(chunk != client.chunks().end() && wire::chunkChecksum(chunk->second.materials) == hash,
				"closure restart changed committed chunk materials");
		}
		require(client.inventory()->slots == saved.inventories[i] && client.inventory()->selected == saved.selected[i],
			"closure restart changed a private inventory");
		const auto &players = *client.playerSnapshots().current;
		for (size_t p = 0; p < saved.players.size(); ++p)
			require(players.players[p].player == saved.players[p].player &&
				players.players[p].feet == saved.players[p].feet, "closure restart changed a settled player pose");
		const auto &drops = *client.itemSnapshots().current;
		require(drops.count == saved.drops.size() && drops.idHighWater >= saved.itemHighWater,
			"closure restart changed item population or allocator watermark");
		for (size_t item = 0; item < saved.drops.size(); ++item)
			require(drops.items[item].id == saved.drops[item].id && drops.items[item].stack == saved.drops[item].stack,
				"closure restart changed a committed drop");
	}
	loop.transport.stop(host);
	require(host.stop(error), error);
}

ClosureState testClosure(const fs::path &root, unsigned workers)
{
	using Kind = wire::ActionKind;
	using Item = items::ItemId;
	server::HostConfig config;
	config.world = {.seed = 19, .worldName = "closure-" + std::to_string(workers), .savesRoot = root};
	config.workers = workers;
	config.interestRadiusChunks = 1;
	server::ServerHost host(config);
	std::string error;
	require(host.start(error), error);
	auto &sim = server::ServerHostProbe::simulation(host);
	auto &world = *sim.world();
	const auto block = [&](int x, int y, int z, TextureType material) {
		require(sim.placeVoxel({x + 0.5f, y + 0.5f, z + 0.5f}, material).accepted, "seed closure arena");
	};
	for (int z = 4; z <= 14; ++z)
	{
		for (int x = 7; x <= 11; ++x)
			block(x, 244, z, STONE);
		block(11, 245, z, STONE);
		block(11, 246, z, STONE);
	}
	constexpr wire::Cell target{8, 9, 246};
	block(target.x, target.y, target.z, STONE);
	block(8, 246, 10, BEDROCK);

	constexpr std::array profiles{wire::PlayerKey{1}, wire::PlayerKey{2}};
	const auto prepare = [&](wire::PlayerKey key, Item item, uint16_t count) {
		const auto player = host.joinRemote(key).player;
		require(player.valid() && host.activateRemote(player), "enroll closure profile");
		sim.setPlayerPosition(player, {8.5, 245, 5.5});
		require(sim.grantItems(player, item, count).remainder.isEmpty(), "seed private closure inventory");
		host.leaveRemote(player);
		return player;
	};
	const auto a = prepare(profiles[0], Item::Glass, 1);
	const auto b = prepare(profiles[1], Item::Dirt, 3);
	ClosureLoop loop(host, 2);
	Relay firstRelay(loop.transport.address(), 0ms), secondRelay(loop.transport.address(), 0ms);
	udp::ClientTransport first(firstRelay.socket.address(), profiles[0]), second(secondRelay.socket.address(), profiles[1]);
	loop.peers = {{&firstRelay, &first}, {&secondRelay, &second}};
	const auto currentTerrain = [&](const udp::ClientTransport &client) {
		const auto offsets = VoxelWorld::playerInterestOffsets(1);
		return client.chunks().size() == offsets.size() && std::ranges::all_of(offsets, [&](const auto offset) {
			const auto chunk = client.chunks().find({spawn.x + offset.x, spawn.z + offset.z});
			const auto version = world.chunkContentVersion({spawn.x + offset.x, 0, spawn.z + offset.z});
			return chunk != client.chunks().end() && version &&
				chunk->second.version == wire::ChunkVersion{version->generation, version->revision};
		});
	};
	const auto joined = [&] {
		const auto &aSnapshot = first.playerSnapshots().current;
		const auto &bSnapshot = second.playerSnapshots().current;
		return first.inventory() && second.inventory() && aSnapshot && bSnapshot && aSnapshot->count == 2 &&
			bSnapshot->count == 2 && currentTerrain(first) && currentTerrain(second);
	};
	const auto verifyTerrain = [&] {
		require(matches(first, world, spawn) && matches(second, world, spawn),
			"client chunk material hashes disagree with the authority");
	};
	first.connect(loop.now);
	second.connect(loop.now);
	loop.until(joined, "two clients did not receive shared terrain and private state");
	verifyTerrain();
	require(first.welcome()->player == a && second.welcome()->player == b && a < b &&
		first.scope()->epoch == second.scope()->epoch && first.scope()->sessionId != second.scope()->sessionId,
		"unexpected two-client identity or scope");
	require(first.inventory()->slots[0] == items::ItemStack{Item::Glass, 1} &&
		second.inventory()->slots[0] == items::ItemStack{Item::Dirt, 3}, "private inventory crossed peers");
	const auto initialEdit = world.nextEditId();
	const auto initialItem = sim.itemIdHighWater();
	const auto voxel = [&](Kind operation) {
		return wire::Action{.operation = operation, .target = target, .expected = first.chunks().at(spawn).version};
	};
	firstRelay.dropEdit = true;
	secondRelay.duplicateEdit = true;
	firstRelay.dropActionResult = true;
	require(loop.action(first, voxel(Kind::Break)).editId == initialEdit, "break assigned an unexpected edit ID");
	loop.until(joined, "break did not converge after edit loss");
	verifyTerrain();
	require(firstRelay.oldEdit && firstRelay.droppedActionResult && !secondRelay.duplicateEdit &&
		sim.itemIdHighWater() == initialItem + 1, "break fault did not exercise exactly-once delivery");
	require(loop.action(first, voxel(Kind::Place)).editId == initialEdit + 1, "placement assigned an unexpected edit ID");
	loop.until(joined, "placement did not converge");
	verifyTerrain();
	firstRelay.send(*firstRelay.oldEdit);
	loop.advance(25ms);
	const auto material = [&](const udp::ClientTransport &c) {
		return c.chunks().at(spawn).materials[WorldChunk::index(target.x, target.y, target.z)];
	};
	require(material(first) == GLASS && material(second) == GLASS && world.nextEditId() == initialEdit + 2,
		"late break overwrote placement or repeated a mutation");

	const auto amount = [](const auto &slots, Item item) {
		unsigned count = 0;
		for (const auto &slot : slots)
			if (slot.item == item)
				count += slot.count;
		return count;
	};
	const auto hasDirtDrop = [](const udp::ClientTransport &c) {
		const auto &snapshot = c.itemSnapshots().current;
		return snapshot && std::ranges::any_of(std::span(snapshot->items).first(snapshot->count),
			[](const auto &item) { return item.stack.item == Item::Dirt; });
	};
	loop.action(second, {.operation = Kind::DropOne});
	loop.until([&] { return hasDirtDrop(first) && hasDirtDrop(second); }, "drop is missing from one observer");
	const auto inFlight = *firstRelay.lastItemSnapshot;
	// Both actors start together and send the same walking intent in the same
	// tick. Their bodies can overlap. The shared pickup order is by PlayerId.
	bool contested = false;
	for (int i = 0; i < 80 && !contested; ++i)
	{
		const wire::Input walk{.intendedTick = host.ticks() + 3, .moveZ = 1.f};
		require(first.sendInput(walk, loop.now) && second.sendInput(walk, loop.now), "send contested movement");
		loop.advance(25ms);
		require(sim.playerFeet(a) == sim.playerFeet(b), "pickup contenders did not share the same trajectory");
		contested = amount(sim.inventory(a)->slots(), Item::Dirt) == 1;
	}
	require(contested && sim.itemIdHighWater() == initialItem + 2, "contested drop was lost or duplicated");
	loop.until([&] { return !hasDirtDrop(first) && !hasDirtDrop(second); }, "pickup removal did not converge");
	firstRelay.send(inFlight);
	firstRelay.send(inFlight);
	loop.advance(25ms);
	require(!hasDirtDrop(first), "an out-of-order item snapshot resurrected the drop");

	unsigned inputs{}, snapshots{}, lostInputs{}, lostSnapshots{}, reordered{};
	Time lastInputDelivery{};
	firstRelay.duplicateMovement = true;
	firstRelay.profile = [&](const wire::Packet &packet, bool fromServer, Time at) -> std::optional<std::chrono::milliseconds> {
		if (!fromServer && std::holds_alternative<wire::Input>(packet.payload))
		{
			if (++inputs % 9 == 0)
			{
				++lostInputs;
				return std::nullopt;
			}
			const auto delay = inputs % 2 ? 60ms : 5ms;
			reordered += at + delay < lastInputDelivery;
			lastInputDelivery = at + delay;
			return delay;
		}
		if (fromServer && std::holds_alternative<wire::PlayerSnapshot>(packet.payload))
		{
			++snapshots;
			if (snapshots % 7 == 0)
			{
				++lostSnapshots;
				return std::nullopt;
			}
			// A 75 ms sample arrives after the next 50 ms snapshot.
			return snapshots % 3 == 0 ? 75ms : 0ms;
		}
		return 0ms;
	};
	const auto beforeWall = sim.playerFeet(a);
	double nearWallX{};
	for (int i = 0; i < 480; ++i)
	{
		require(first.sendInput({.intendedTick = host.ticks() + udp::kInputLeadTicks - 1, .moveX = 1.f}, loop.now),
			"send impaired movement");
		loop.advance(25ms);
		if (i == 399)
			nearWallX = sim.playerFeet(a).x;
	}
	firstRelay.profile = {};
	firstRelay.duplicateMovement = false;
	loop.advance(500ms);
	const auto feet = sim.playerFeet(a);
	require(lostInputs && lostSnapshots && reordered && firstRelay.movementDuplicates,
		"movement fault profile failed to inject every fault");
	require(feet.x > beforeWall.x + 2.0 && feet.x > 10.5 && feet.x < 11.0 && feet.y >= 244.99 &&
		feet.y <= 245.01 && std::abs(feet.x - nearWallX) < 1e-6,
		"faulted movement did not reach and stop at the authoritative wall, x=" + std::to_string(feet.x) +
		" y=" + std::to_string(feet.y) + " z=" + std::to_string(feet.z) + " near=" + std::to_string(nearWallX));

	// One bad baseline exercises the full resync path. Replay a retired
	// DropOne through the same endpoint after the new session is active.
	const auto retiredScope = *second.scope();
	const auto retiredAction = *secondRelay.lastAction;
	secondRelay.corruptFragment = true;
	second.requestWorldResync(loop.now);
	loop.until([&] { return joined() && *second.scope() != retiredScope && secondRelay.lastResync ==
		wire::ResyncReason::Checksum; }, "checksum failure did not recover through a fresh session", 20s);
	require(second.welcome()->player == b && !secondRelay.corruptFragment, "resync changed the actor or skipped corruption");
	for (unsigned i = 0; i < 256; ++i)
	{
		const std::array<uint8_t, 3> truncated{0, 0, 0};
		require(secondRelay.socket.send(loop.transport.address(), truncated), "send bounded malformed burst");
	}
	secondRelay.sendToServer(retiredAction);
	loop.advance(500ms);
	require(sim.itemIdHighWater() == initialItem + 2 && world.nextEditId() == initialEdit + 2,
		"malformed burst or retired credential mutated the world");
	loop.action(second, {.operation = Kind::Select, .slot = 1});
	require(second.inventory()->selected == 1 && first.inventory()->selected == 0,
		"valid traffic failed after the burst or selection crossed peers");
	verifyTerrain();

	ClosureState state{};
	for (const auto &[coord, chunk] : first.chunks())
		state.chunks.emplace_back(coord, chunk.version.generation, chunk.version.revision, wire::chunkChecksum(chunk.materials));
	const auto authority = host.snapshotRemote(a);
	require(authority.count == 2 && first.latestPlayerSnapshot()->players == authority.players &&
		second.latestPlayerSnapshot()->players == authority.players, "settled wire poses disagree with the authority");
	std::copy_n(authority.players.begin(), state.players.size(), state.players.begin());
	const auto authoritativeItems = host.entitySnapshotsRemote(a)->items;
	for (const auto &[client, player] : {std::pair{&first, a}, std::pair{&second, b}})
	{
		const auto &snapshot = client->itemSnapshots().current;
		require(snapshot && snapshot->count == authoritativeItems.count && snapshot->idHighWater == authoritativeItems.idHighWater &&
			std::equal(snapshot->items.begin(), snapshot->items.begin() + snapshot->count, authoritativeItems.items.begin()),
			"settled drops disagree with the authority");
		const auto index = player == a ? 0 : 1;
		const auto inventory = *client->inventory();
		state.inventories[index] = inventory.slots;
		state.selected[index] = inventory.selected;
		require(inventory == host.inventoryRemote(player), "private replica disagrees with its owner");
	}
	state.drops.assign(authoritativeItems.items.begin(), authoritativeItems.items.begin() + authoritativeItems.count);
	const auto total = [&](Item item) {
		unsigned count = amount(state.inventories[0], item) + amount(state.inventories[1], item);
		for (const auto &drop : sim.itemEntities().entities())
			if (drop.stack.item == item)
				count += drop.stack.count;
		return count;
	};
	require(total(Item::Dirt) == 3 && amount(state.inventories[0], Item::Dirt) == 1 &&
		amount(state.inventories[1], Item::Dirt) == 2 && total(Item::Glass) == 0 && total(Item::Cobblestone) == 1,
		"placed glass, mined stone, or contested dirt failed conservation accounting");
	state.nextEdit = world.nextEditId();
	state.itemHighWater = sim.itemIdHighWater();
	const auto worldId = first.welcome()->worldId;
	const auto epoch = first.scope()->epoch;
	second.disconnect(loop.now);
	loop.until([&] { return second.status() == udp::ClientStatus::Disconnected &&
		first.playerSnapshots().current && first.playerSnapshots().current->count == 1; }, "departed actor remained visible");
	require(second.chunks().empty() && !second.inventory(), "departed client retained a projection");
	loop.transport.stop(host);
	require(host.stop(error), error);
	verifyClosureReopen(config, state, profiles, worldId, epoch, host.metrics().checkpointGeneration);
	return state;
}

struct ProcessUsage
{
	double cpuMs{};
	uint64_t peakRssKiB{};
};

ProcessUsage processUsage()
{
#ifdef _WIN32
	const auto process = GetCurrentProcess();
	PROCESS_MEMORY_COUNTERS memory{};
	FILETIME creation{}, exit{}, kernel{}, user{};
	require(K32GetProcessMemoryInfo(process, &memory, sizeof(memory)) &&
		GetProcessTimes(process, &creation, &exit, &kernel, &user), "read process usage");
	const auto ticks = [](FILETIME time) { return (uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime; };
	return {static_cast<double>(ticks(kernel) + ticks(user)) / 10000, memory.PeakWorkingSetSize / 1024};
#else
	rusage usage{};
	require(getrusage(RUSAGE_SELF, &usage) == 0, "read process usage");
	const auto ms = [](timeval time) { return static_cast<double>(time.tv_sec) * 1000 + time.tv_usec / 1000.0; };
	auto rss = static_cast<uint64_t>(usage.ru_maxrss);
#ifdef __APPLE__
	rss /= 1024;
#endif
	return {ms(usage.ru_utime) + ms(usage.ru_stime), rss};
#endif
}

void testMultiplayerSoak(const fs::path &root)
{
	constexpr size_t clientCount = 8;
	constexpr unsigned waves = 3;
	constexpr unsigned shifts = 3;
	const size_t interest = VoxelWorld::playerInterestOffsets(1).size();
	const auto started = udp::ServerTransport::Clock::now();
	const auto cpuStart = processUsage().cpuMs;
	server::HostConfig config;
	config.world = {.seed = 19, .worldName = "soak", .savesRoot = root};
	config.interestRadiusChunks = 1;
	config.workers = 2;
	uint64_t checkpointGeneration{};
	udp::WorldTraffic traffic;
	uint64_t baselineAllocations{}, ticks{};
	size_t peakChunks{}, peakVoxelBlocks{}, peakQueue{}, peakTransfers{}, warmVoxelBlocks{};
	{
		server::ServerHost host(config);
		std::string error;
		require(host.start(error), error);
		auto &sim = server::ServerHostProbe::simulation(host);
		ClosureLoop loop(host, clientCount);
		std::array<sim::PlayerId, clientCount> actors{};
		const auto sample = [&] {
			const auto queue = loop.transport.worldSendQueue();
			peakQueue = std::max(peakQueue, queue.pending);
			peakTransfers = std::max(peakTransfers, queue.baselines);
			peakChunks = std::max(peakChunks, host.metrics().chunks);
			peakVoxelBlocks = std::max(peakVoxelBlocks, sim.world()->voxelPool().capacity());
		};
		const auto until = [&](auto ready, const char *message) {
			loop.until([&] { sample(); return ready(); }, message, 15s);
		};
		for (unsigned wave = 0; wave < waves; ++wave)
		{
			std::vector<std::unique_ptr<Relay>> relays;
			std::vector<std::unique_ptr<udp::ClientTransport>> clients;
			for (size_t i = 0; i < clientCount; ++i)
			{
				auto relay = std::make_unique<Relay>(loop.transport.address(), 0ms);
				relays.push_back(std::move(relay));
				wire::PlayerKey key{};
				key[0] = static_cast<uint8_t>(i + 1);
				clients.push_back(std::make_unique<udp::ClientTransport>(relays.back()->socket.address(), key));
				loop.peers.emplace_back(relays.back().get(), clients.back().get());
				clients.back()->connect(loop.now);
			}
			until([&] { return std::ranges::all_of(clients, [](const auto &c) { return c->inventory().has_value(); }); },
				"soak join did not publish all private inventories");
			for (size_t i = 0; i < clientCount; ++i)
			{
				const auto player = clients[i]->welcome()->player;
				if (wave == 0)
					actors[i] = player;
				require(player == actors[i] && std::ranges::all_of(clients[i]->inventory()->slots, &items::ItemStack::isEmpty),
					"soak reconnect changed identity or inventory");
			}
			for (unsigned shift = 0; shift < shifts; ++shift)
			{
				// Move authority-owned observers far enough to force disjoint
				// interests quickly. Closure covers user-driven walking.
				std::array<Coord, clientCount> centers{};
				for (size_t i = 0; i < clientCount; ++i)
				{
					const auto x = 4 * static_cast<int>(i) + 4;
					const auto z = 4 * static_cast<int>(shift + 1);
					centers[i] = {x, z};
					sim.restorePlayerFlight(actors[i], true, {x * 16.0 + 8.5, 245, z * 16.0 + 8.5});
				}
				until([&] {
					for (size_t i = 0; i < clientCount; ++i)
					{
						const auto &chunks = clients[i]->chunks();
						if (chunks.size() != interest || !chunks.contains(centers[i]) ||
						    !std::ranges::all_of(VoxelWorld::playerInterestOffsets(1), [&](const auto offset) {
								return chunks.contains({centers[i].x + offset.x, centers[i].z + offset.z});
						    }))
							return false;
					}
					return true;
				}, "soak client retained old chunks or starved waiting for new interest");
			}
			// Start another interest change, then leave with sends still busy.
			for (size_t i = 0; i < clientCount; ++i)
				sim.restorePlayerFlight(actors[i], true, {i * 64.0 + 8.5, 245, -55.5});
			until([&] { return loop.transport.worldSendQueue().baselines > 0; }, "soak did not stage a baseline");
			for (const auto &client : clients)
				client->disconnect(loop.now);
			until([&] {
				return host.metrics().players == 1 && host.metrics().chunks <= interest &&
					loop.transport.worldSendQueue().pending == 0 &&
					std::ranges::all_of(clients, [](const auto &c) {
						return c->status() == udp::ClientStatus::Disconnected && c->chunks().empty();
					});
			}, "soak leave did not drain sends, players and chunk residency");
			loop.peers.clear();
			if (wave == 0)
				warmVoxelBlocks = peakVoxelBlocks;
		}
		const auto queue = loop.transport.worldSendQueue();
		baselineAllocations = queue.baselinesQueued;
		const auto residentBudget = (clientCount + 1) * interest + config.streaming.loads;
		require(peakChunks <= residentBudget && peakVoxelBlocks <= residentBudget &&
			peakVoxelBlocks <= warmVoxelBlocks + config.streaming.generations,
			"soak residency or voxel allocation exceeded the fixture budget");
		require(peakQueue > 0 && peakQueue <= clientCount && peakTransfers > 0 &&
			peakTransfers <= udp::kMaxConcurrentChunkTransfers && queue.baselines == 0,
			"soak world send queue violated its bounds");
		const auto expectedBaselines = clientCount * shifts * interest * waves;
		require(baselineAllocations >= expectedBaselines &&
			baselineAllocations <= expectedBaselines + clientCount * interest * 2 * waves,
			"soak baseline allocations exceeded the warmup and early-disconnect allowance");
		ticks = host.ticks();
		traffic = loop.transport.worldTraffic();
		loop.transport.stop(host);
		require(host.stop(error), error);
		checkpointGeneration = host.metrics().checkpointGeneration;
	}
	const auto checkpoint = root / "soak" / "checkpoints";
	const auto generation = checkpoint / ("generation-" + std::to_string(checkpointGeneration));
	uintmax_t checkpointBytes = fs::file_size(checkpoint / "manifest");
	for (const auto &entry : fs::recursive_directory_iterator(generation))
		if (entry.is_regular_file())
			checkpointBytes += entry.file_size();
	require(checkpointBytes > 0 && checkpointBytes < 1024 * 1024, "soak empty-world checkpoint grew beyond 1 MiB");
	server::ServerHost reopened(config);
	std::string error;
	const auto restartStarted = udp::ServerTransport::Clock::now();
	require(reopened.start(error), error);
	const auto restarted = udp::ServerTransport::Clock::now();
	require(reopened.restoredPlayer() && reopened.metrics().checkpointGeneration == checkpointGeneration,
		"soak restart did not load the committed generation");
	require(reopened.stop(error), error);
	const auto finished = udp::ServerTransport::Clock::now();
	const auto usage = processUsage();
	std::cout << "SOAK clients=" << clientCount << " waves=" << waves << " ticks=" << ticks
		<< " peak_chunks=" << peakChunks << " voxel_blocks=" << peakVoxelBlocks
		<< " voxel_bytes=" << peakVoxelBlocks * sizeof(VoxelStorage) << " peak_world_sends=" << peakQueue
		<< " peak_baselines=" << peakTransfers << " baseline_allocations=" << baselineAllocations
		<< " world_tx_bytes=" << traffic.bytes << " retransmits=" << traffic.retransmits
		<< " checkpoint_bytes=" << checkpointBytes
		<< " restart_ms=" << std::chrono::duration<double, std::milli>(restarted - restartStarted).count()
		<< " wall_ms=" << std::chrono::duration<double, std::milli>(finished - started).count()
		<< " cpu_ms=" << usage.cpuMs - cpuStart << " peak_rss_kib=" << usage.peakRssKiB << '\n';
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
	require(client.chunks().contains(spawn) && server.worldTraffic().frames > 0,
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
		{
			testAnchorPickup(root);
			testEntityReplication(root);
		}
		else if (argc == 2 && std::string_view(argv[1]) == "--input-timing")
		{
			testClientTickMath();
			testClientTimingSamples();
			testPredictionResets();
			testLocalPresentationCadence();
			testReplicaCameraHold(false);
			testReplicaCameraHold(true);
			testReplicaTerrainDiscontinuities();
			testRemoteInterpolation();
			testDelayedJoin(root);
			for (const auto delay : {0ms, 15ms, 40ms, 75ms})
				testDelayedInput(root, delay, false);
			testDelayedInput(root, 40ms, true);
			testDelayedInput(root, 0ms, false, 144);
			testDelayedInput(root, 75ms, false, 144);
		}
		else if (argc == 2 && std::string_view(argv[1]) == "--measure-chunks")
		{
			for (const auto rtt : {0ms, 30ms, 80ms, 150ms})
				for (const auto clients : {1u, 2u, 8u})
					for (const bool loss : {false, true})
						measureChunkDelivery(root, rtt, clients, loss);
		}
		else if (argc == 2 && std::string_view(argv[1]) == "--cold-chunks")
			measureChunkDelivery(root, 150ms, udp::kMaxConcurrentChunkTransfers + 2, true, true);
		else if (argc == 2 && std::string_view(argv[1]) == "--closure")
		{
			const auto sync = testClosure(root, 0);
			const auto async = testClosure(root, 2);
			require(sync == async, "repeated closure runs differed in authoritative state");
			std::cout << "CLOSURE chunks=" << sync.chunks.size() << " center_hash=" << std::get<3>(sync.chunks[2])
				<< " next_edit=" << sync.nextEdit << " items=" << sync.drops.size()
				<< " item_high_water=" << sync.itemHighWater << '\n';
		}
		else if (argc == 2 && std::string_view(argv[1]) == "--soak")
			testMultiplayerSoak(root);
		else
		{
			testRenderBacking();
			testReplicaTerrainChanges();
			testReplicaTerrainBoundaries();
			testReplicaTerrainCapacity();
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
