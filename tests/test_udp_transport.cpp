#include <Network/ClientTransport.hpp>
#include <Network/ServerTransport.hpp>

#include <Chunk/TerrainGenerator.hpp>

#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

namespace
{
using namespace net;
using namespace std::chrono_literals;
using Clock = udp::ServerTransport::Clock;
const auto loopback = udp::parseAddress("127.0.0.1", 0);
constexpr udp::SessionWorld world{42, TerrainGenerator::kGeneratorVersion};

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

struct Owner : udp::SessionOwner
{
	udp::SessionAdmission joinRemote() override
	{
		const sim::PlayerId player{nextPlayer++};
		players.emplace(player, 0);
		return {player, ticks, 1};
	}

	void leaveRemote(sim::PlayerId player) override
	{
		require(players.erase(player) == 1, "removed wrong player");
	}

	std::optional<wire::ActionResult> receiveRemote(sim::PlayerId player, const udp::SessionIntents &work) override
	{
		auto &count = players.at(player);
		if (work.input)
			inputTick = work.input->intendedTick;
		if (!work.action)
			return std::nullopt;
		++count;
		return wire::ActionResult{work.action->actionId, ticks, 1, 0,
			work.action->operation, wire::ActionCode::Unavailable};
	}

	std::map<sim::PlayerId, unsigned> players;
	uint32_t nextPlayer{2};
	uint64_t inputTick{}, ticks{};
};

struct Loop
{
	explicit Loop(size_t cap) : server(loopback, cap, world) {}

	void step(Clock::duration elapsed = 0ms)
	{
		now += elapsed;
		server.poll(now);
		owner.ticks++;
		server.serviceOwner(owner);
		server.poll(now);
	}

	Owner owner;
	udp::ServerTransport server;
	Clock::time_point now{};
};

void send(udp::Socket &client, udp::Address address, const wire::Packet &packet)
{
	wire::Datagram bytes;
	require(wire::encode(wire::Endpoint::Client, packet, bytes) == wire::CodecStatus::Ok,
		"could not encode test packet");
	require(client.send(address, bytes.view()), "could not send test packet");
}

std::optional<wire::Packet> receive(udp::Socket &client, wire::Kind kind)
{
	udp::Received frame;
	while (client.receive(frame))
	{
		wire::Packet packet;
		require(wire::decode(wire::Endpoint::Server, {frame.bytes.data(), frame.size}, packet) ==
			wire::CodecStatus::Ok, "malformed server response");
		if (std::visit([](const auto &msg) { return msg.kind; }, packet.payload) == kind)
			return packet;
	}
	return std::nullopt;
}

void testLifecycle()
{
	Loop loop(2);
	const auto address = loop.server.address();
	require(address.port != 0, "port zero did not produce a bound port");
	udp::ClientTransport first(address), second(address), excess(address);
	first.connect(loop.now);
	second.connect(loop.now);
	first.poll(loop.now);
	second.poll(loop.now);
	loop.step();
	first.poll(loop.now);
	second.poll(loop.now);
	require(first.status() == udp::ClientStatus::Connected && second.status() == udp::ClientStatus::Connected,
		"two-client join failed");
	const auto welcome = *first.welcome();
	const auto firstScope = *first.scope();
	const auto secondPlayer = second.welcome()->player;
	require(welcome.seed == world.seed && welcome.generatorVersion == world.generatorVersion &&
		welcome.content == wire::localCompatibility() && welcome.nextEditId == 1 && welcome.worldId != 0,
		"incomplete welcome");
	require(welcome.player != secondPlayer && firstScope.sessionId != second.scope()->sessionId &&
		firstScope.epoch == second.scope()->epoch && welcome.worldId == second.welcome()->worldId,
		"session and world identities have incorrect scopes");
	loop.step(1s);
	first.poll(loop.now);
	second.poll(loop.now);
	require(first.heartbeatsReceived() && second.heartbeatsReceived(), "heartbeat exchange failed");

	excess.connect(loop.now);
	excess.poll(loop.now);
	loop.step();
	excess.poll(loop.now);
	require(excess.status() == udp::ClientStatus::Rejected && excess.rejection() == wire::RejectReason::Full &&
		loop.owner.players.size() == 2, "cap exceeded");

	first.disconnect(loop.now);
	first.poll(loop.now);
	loop.step();
	first.poll(loop.now);
	require(first.status() == udp::ClientStatus::Disconnected && !first.scope() &&
		loop.owner.players.size() == 1 && !loop.owner.players.contains(welcome.player), "explicit leave leaked a player");

	// Skip polls for the second client. Advance both ends using injected
	// time so expiry and rejoin need no real-time sleep.
	loop.step(11s);
	loop.step();
	for (unsigned i = 0; i < 8; ++i)
		second.poll(loop.now);
	require(second.status() == udp::ClientStatus::TimedOut && loop.owner.players.empty(),
		"idle timeout leaked a player");

	first.connect(loop.now);
	second.connect(loop.now);
	first.poll(loop.now);
	second.poll(loop.now);
	loop.step();
	first.poll(loop.now);
	second.poll(loop.now);
	require(first.status() == udp::ClientStatus::Connected && second.status() == udp::ClientStatus::Connected &&
		welcome.player < first.welcome()->player && secondPlayer < second.welcome()->player &&
		first.scope()->sessionId != firstScope.sessionId && first.scope()->epoch == firstScope.epoch,
		"rejoin must establish fresh sessions in the same world");
	loop.server.stop(loop.owner);
	require(loop.owner.players.empty(), "shutdown leaked players");
}

void testAdmission()
{
	Loop loop(2);
	const auto address = loop.server.address();
	udp::Socket client(loopback), foreign(loopback);
	const wire::Packet hello{{}, 0, wire::Hello{1, wire::kVersion, wire::kVersion, wire::localCompatibility()}};
	send(client, address, hello);
	loop.step();
	const auto joined = receive(client, wire::Kind::Welcome);
	require(joined.has_value(), "raw welcome missing");
	const auto scope = joined->scope;
	const auto player = std::get<wire::Welcome>(joined->payload).player;
	send(client, address, hello); // Model a lost Welcome.
	loop.step();
	require(receive(client, wire::Kind::Welcome) == joined && loop.owner.players.size() == 1,
		"duplicate Hello allocated a new player");

	const auto action = wire::Action{.actionId = 1, .operation = wire::ActionKind::Select};
	send(foreign, address, {scope, 1, action});
	send(client, address, {{scope.sessionId == 1 ? 2u : 1u, scope.epoch}, 1, action});
	send(client, address, {{scope.sessionId, scope.epoch == 1 ? 2u : 1u}, 1, action});
	send(client, address, {scope, 2, action}); // reliable gap
	const wire::Input input{1, 0, 0, 0};
	wire::Datagram valid;
	require(wire::encode(wire::Endpoint::Client, {scope, 1, action}, valid) == wire::CodecStatus::Ok,
		"encode forged action prefix");
	std::array<uint8_t, 84> forged{};
	std::ranges::copy(valid.view(), forged.begin());
	forged[9] += 4; // Widen the advertised payload and append a claimed player ID.
	forged.back() = static_cast<uint8_t>(player.value);
	require(client.send(address, forged), "send action with forged identity");
	require(wire::encode(wire::Endpoint::Client, {scope, 900, input}, valid) == wire::CodecStatus::Ok,
		"encode oversize prefix");
	std::array<uint8_t, 4000> oversized{};
	std::ranges::copy(valid.view(), oversized.begin());
	require(client.send(address, oversized), "send oversized frame");
	require(client.send(address, valid.view().first(valid.view().size() - 1)), "send truncated frame");
	loop.step();
	require(loop.owner.players.at(player) == 0 && loop.owner.inputTick == 0, "invalid frame reached simulation owner");

	send(client, address, {scope, 1, wire::Heartbeat{}});
	for (uint64_t sequence = 2; sequence < 102; ++sequence)
		send(client, address, {scope, sequence, wire::Input{sequence, 0, 0, 0}});
	loop.step();
	require(loop.owner.inputTick == 9, "input burst exceeded per-family budget or failed to coalesce");
	loop.step(1s);
	send(client, address, {scope, 4, wire::Input{99, 0, 0, 0}});
	send(client, address, {scope, 1, action});
	send(client, address, {scope, 1, action});
	auto secondAction = action;
	secondAction.actionId = 2;
	send(client, address, {scope, 2, secondAction}); // one pending action/result per peer
	loop.step();
	const auto result = receive(client, wire::Kind::ActionResult);
	require(result && result->sequence == 1 && result->scope == scope &&
		std::get<wire::ActionResult>(result->payload).code == wire::ActionCode::Unavailable &&
		loop.owner.players.at(player) == 1 && loop.owner.inputTick == 9, "replay, gap or backpressure failure");
	loop.step(200ms);
	require(receive(client, wire::Kind::ActionResult) == result, "unacknowledged result must retry unchanged");
	send(client, address, {scope, 0, wire::Ack{2}}); // acknowledges unsent data
	send(client, address, {scope, 2, secondAction});
	loop.step();
	require(loop.owner.players.at(player) == 1, "unsent ACK released backpressure");
	send(client, address, {scope, 0, wire::Ack{1}});
	send(client, address, {scope, 2, action}); // reused action ID with a fresh sequence
	send(client, address, {scope, 2, secondAction});
	loop.step();
	require(loop.owner.players.at(player) == 2, "valid retry failed after ACK freed the only result slot");

	// Leave the second action result unacknowledged. A different client
	// continues to join and exchange heartbeats while the slow peer expires.
	udp::ClientTransport other(address);
	other.connect(loop.now);
	other.poll(loop.now);
	loop.step();
	other.poll(loop.now);
	require(other.status() == udp::ClientStatus::Connected, "slow peer blocked another join");
	const auto before = loop.owner.ticks;
	for (int i = 0; i < 6; ++i)
	{
		loop.step(1s);
		other.poll(loop.now);
	}
	require(loop.owner.ticks == before + 6 && other.heartbeatsReceived() >= 3 &&
		loop.owner.players.size() == 1 && !loop.owner.players.contains(player),
		"stalled reliable send blocked ticks or another client");

	// Reuse the same UDP endpoint, then try an authenticated message from
	// the former generation. It must leave the new player untouched.
	send(client, address, {{}, 0, wire::Hello{2, wire::kVersion, wire::kVersion, wire::localCompatibility()}});
	loop.step();
	const auto rejoined = receive(client, wire::Kind::Welcome);
	require(rejoined && rejoined->scope.sessionId != scope.sessionId, "same-endpoint rejoin failed");
	send(client, address, hello); // delayed Hello from the former generation
	send(client, address, {scope, 3, wire::Disconnect{wire::DisconnectReason::Leaving}});
	loop.step();
	require(loop.owner.players.size() == 2 && !receive(client, wire::Kind::Welcome),
		"stale generation replaced or closed the new session");
	loop.step(5s); // no scoped packet ever confirmed the replacement
	require(loop.owner.players.size() == 1, "unconfirmed join survived its deadline");
	loop.server.stop(loop.owner);
}

void testThreadedShutdown()
{
	Owner owner;
	udp::ServerTransport server(loopback, 1, world);
	udp::ClientTransport client(server.address());
	server.start();
	auto now = Clock::now();
	client.connect(now);
	const auto deadline = now + 3s;
	while (!client.heartbeatsReceived() && now < deadline)
	{
		server.rethrowIoError();
		server.serviceOwner(owner);
		client.poll(now);
		std::this_thread::sleep_for(1ms);
		now = Clock::now();
	}
	require(client.heartbeatsReceived() && owner.players.size() == 1, "threaded join/heartbeat timed out");
	server.stop(owner);
	server.rethrowIoError();
	client.poll(Clock::now());
	require(owner.players.empty() && client.status() == udp::ClientStatus::Disconnected,
		"threaded shutdown failed to send final control or retire players");
}
} // namespace

int main()
{
	try
	{
		testLifecycle();
		testAdmission();
		testThreadedShutdown();
	}
	catch (const std::exception &error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
