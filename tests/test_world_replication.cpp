#include <Network/ClientTransport.hpp>
#include <Server/ServerHost.hpp>

#include <filesystem>
#include <future>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

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
	std::optional<wire::ChunkBaseline> lastBegin;
	std::optional<wire::ResyncReason> lastResync;
	std::optional<wire::Packet> oldEdit;
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
	server.stop(host);
	server.rethrowIoError();
	require(client.chunks().contains(spawn) && server.worldTraffic().frames >= wire::kFragmentsPerChunk,
		"worker did not deliver a complete baseline from the owner mailbox");
	require(host.stop(error), error);
}
} // namespace

int main()
{
	const auto root = fs::temp_directory_path() / ("ft_vox_replication_" + std::to_string(udp::randomId()));
	int result = 0;
	try
	{
		testIncompatibleGenerator();
		testReplication(root);
		testThreadedDelivery(root);
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
