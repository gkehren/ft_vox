#include "ClientTransport.hpp"
#include "UdpLink.hpp"

#include <Chunk/TerrainGenerator.hpp>

#include <stdexcept>
#include <utility>

namespace net::udp
{

struct ClientTransport::Impl
{
	explicit Impl(Address server) : server(server)
	{
		if (server.port == 0 || server.ipv4 == 0)
			throw std::invalid_argument("expected a concrete server endpoint");
	}

	void finish(ClientStatus next)
	{
		status = next;
		link.reset();
		welcome.reset();
		playerSnapshot.reset();
	}

	Sender sender()
	{
		return {socket, server, wire::Endpoint::Client, outbound, globalOut};
	}

	void receive(const Received &frame, Time now)
	{
		if (frame.from != server || !inbound.traffic.take(now, frame.size) || frame.size > wire::kDatagramBytes)
			return;
		wire::Packet packet;
		if (wire::decode(wire::Endpoint::Server, {frame.bytes.data(), frame.size}, packet) != wire::CodecStatus::Ok ||
		    !inbound.takeKind(now, kindOf(packet)))
			return;
		if (status == ClientStatus::Connecting)
		{
			if (const auto *reply = std::get_if<wire::Welcome>(&packet.payload);
			    reply && reply->nonce == nonce && reply->content == wire::localCompatibility() &&
			    reply->generatorVersion == TerrainGenerator::kGeneratorVersion)
			{
				link.emplace(packet.scope, now);
				welcome = *reply;
				status = ClientStatus::Connected;
			}
			else if (const auto *reject = std::get_if<wire::Reject>(&packet.payload);
			         reject && reject->nonce == nonce)
			{
				rejection = reject->reason;
				finish(ClientStatus::Rejected);
			}
			return;
		}
		if (!link || packet.scope != link->scope)
			return;
		if (const auto *ack = std::get_if<wire::Ack>(&packet.payload))
		{
			if (status == ClientStatus::Leaving && link->acknowledge(ack->through))
				finish(ClientStatus::Disconnected);
			return;
		}
		const auto kind = kindOf(packet);
		const auto *players = std::get_if<wire::PlayerSnapshot>(&packet.payload);
		if (players && (players->tick < welcome->tick || (playerSnapshot && players->tick <= playerSnapshot->tick) ||
		                std::ranges::none_of(std::span(players->players).first(players->count),
		                     [this](const auto &player) { return player.player == welcome->player; })))
			return;
		if (!players && kind != wire::Kind::Heartbeat && kind != wire::Kind::Disconnect && kind != wire::Kind::Resync)
			return;
		if (link->incoming.admit(packet) != wire::ReceiveStatus::Accepted)
			return;
		link->lastReceived = now;
		if (players)
			playerSnapshot = *players;
		else if (kind == wire::Kind::Heartbeat)
			++heartbeats;
		else
		{
			link->ack = true;
			link->flushAck(sender(), now);
			finish(ClientStatus::Disconnected);
		}
	}

	void poll(Time now)
	{
		if ((status == ClientStatus::Connecting && now >= deadline) || (link && link->expired(now)))
			finish(ClientStatus::TimedOut);
		if (status == ClientStatus::Connecting && now >= nextHello &&
		    sender().send(now, {{}, 0, wire::Hello{nonce, wire::kVersion, wire::kVersion, wire::localCompatibility()}}))
			nextHello = now + kHelloInterval;
		Received frame;
		for (unsigned i = 0; i < kReadBatch && socket.receive(frame); ++i)
			receive(frame, now);
		if (link)
			link->flush(sender(), now);
	}

	Socket socket{{0, 0}};
	Address server;
	ClientStatus status{ClientStatus::Disconnected};
	std::optional<Link> link;
	std::optional<wire::Welcome> welcome;
	std::optional<wire::PlayerSnapshot> playerSnapshot;
	std::optional<wire::RejectReason> rejection;
	Rate inbound, outbound;
	Traffic globalOut;
	Time deadline{}, nextHello{};
	uint64_t nonce{}, heartbeats{};
};

ClientTransport::ClientTransport(Address server) : m_impl(std::make_unique<Impl>(server)) {}
ClientTransport::~ClientTransport() = default;

void ClientTransport::connect(Clock::time_point now)
{
	if (m_impl->status == ClientStatus::Connecting || m_impl->link)
		throw std::logic_error("UDP client must finish the current session before joining");
	m_impl->nonce = randomId();
	m_impl->rejection.reset();
	m_impl->heartbeats = 0;
	m_impl->deadline = now + kJoinTimeout;
	m_impl->nextHello = now;
	m_impl->status = ClientStatus::Connecting;
}

void ClientTransport::poll(Clock::time_point now) { m_impl->poll(now); }

void ClientTransport::disconnect(Clock::time_point now)
{
	if (m_impl->status == ClientStatus::Connecting)
		m_impl->finish(ClientStatus::Disconnected);
	if (m_impl->status == ClientStatus::Connected)
	{
		m_impl->link->queue(wire::Endpoint::Client, wire::Disconnect{wire::DisconnectReason::Leaving}, now);
		m_impl->status = ClientStatus::Leaving;
	}
}

ClientStatus ClientTransport::status() const { return m_impl->status; }
bool ClientTransport::sendInput(const wire::Input &input, Clock::time_point now)
{
	return m_impl->status == ClientStatus::Connected && wire::validInput(input) &&
		m_impl->link->sendFresh(m_impl->sender(), now, input);
}

std::optional<wire::Welcome> ClientTransport::welcome() const { return m_impl->welcome; }
std::optional<wire::SessionScope> ClientTransport::scope() const
{
	return m_impl->link ? std::optional{m_impl->link->scope} : std::nullopt;
}
std::optional<wire::PlayerSnapshot> ClientTransport::latestPlayerSnapshot() const { return m_impl->playerSnapshot; }
std::optional<wire::RejectReason> ClientTransport::rejection() const { return m_impl->rejection; }
uint64_t ClientTransport::heartbeatsReceived() const { return m_impl->heartbeats; }

} // namespace net::udp
