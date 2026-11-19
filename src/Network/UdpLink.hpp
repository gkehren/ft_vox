#pragma once

#include "UdpSocket.hpp"

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace net::udp
{

using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
using namespace std::chrono_literals;

inline constexpr auto kJoinTimeout = 5s;
inline constexpr auto kIdleTimeout = 10s;
inline constexpr auto kHeartbeatInterval = 1s;
inline constexpr auto kHelloInterval = 500ms;
inline constexpr auto kReliableRetry = 150ms;
inline constexpr auto kReliableTimeout = 5s;
inline constexpr unsigned kReadBatch = 128;

inline wire::Kind kindOf(const wire::Packet &packet)
{
	return std::visit([](const auto &value) { return std::decay_t<decltype(value)>::kind; }, packet.payload);
}

struct Bucket
{
	bool take(Time now, double amount, double rate, double burst)
	{
		if (!m_started)
		{
			m_tokens = burst;
			m_started = true;
			m_last = now;
		}
		if (now > m_last)
		{
			m_tokens = std::min(burst, m_tokens + rate * std::chrono::duration<double>(now - m_last).count());
			m_last = now;
		}
		if (m_tokens < amount)
			return false;
		m_tokens -= amount;
		return true;
	}

private:
	Time m_last{};
	double m_tokens{};
	bool m_started{};
};

struct Traffic
{
	bool take(Time now, size_t bytes, bool global = false)
	{
		return frames.take(now, 1, global ? 2048 : 128, global ? 256 : 64) &&
			bandwidth.take(now, static_cast<double>(bytes), global ? 2097152 : 131072, global ? 262144 : 65536);
	}

	Bucket frames, bandwidth;
};

struct Rate
{
	bool takeKind(Time now, wire::Kind kind)
	{
		const auto &policy = *wire::policy(kind);
		return families[static_cast<size_t>(kind) - 1].take(now, 1, policy.messagesPerSecond, policy.burst);
	}

	Traffic traffic;
	std::array<Bucket, std::variant_size_v<wire::Payload>> families;
};

struct Sender
{
	Socket &socket;
	Address target;
	wire::Endpoint endpoint;
	Rate &peer;
	Traffic &global;
	uint64_t *sentBytes{};

	bool send(Time now, wire::Kind kind, const wire::Datagram &frame)
	{
		const auto bytes = frame.view();
		if (!peer.takeKind(now, kind) || !peer.traffic.take(now, bytes.size()) ||
		    !global.take(now, bytes.size(), true) || !socket.send(target, bytes))
			return false;
		if (sentBytes)
			*sentBytes += bytes.size();
		return true;
	}

	bool send(Time now, const wire::Packet &packet)
	{
		wire::Datagram frame;
		if (wire::encode(endpoint, packet, frame) != wire::CodecStatus::Ok)
			throw std::logic_error("invalid outbound UDP packet");
		return send(now, kindOf(packet), frame);
	}
};

// B3 uses stop-and-wait, a one-frame subset of the reliable window.
struct Link
{
	Link(wire::SessionScope scope, Time now)
		: scope(scope), incoming(scope), lastReceived(now), nextHeartbeat(now) {}

	void queue(wire::Endpoint endpoint, wire::Payload payload, Time now)
	{
		const wire::Packet packet{scope, reliableSent + 1, std::move(payload)};
		Pending frame{.datagram = {}, .kind = kindOf(packet), .since = now, .retry = now};
		if (pending || wire::encode(endpoint, packet, frame.datagram) != wire::CodecStatus::Ok)
			throw std::logic_error("invalid reliable enqueue");
		pending = frame;
	}

	bool acknowledge(uint64_t through)
	{
		if (!pending || !pending->sent || through != reliableSent + 1)
			return false;
		pending.reset();
		++reliableSent;
		return true;
	}

	void flushAck(Sender sender, Time now)
	{
		if (ack && incoming.reliableThrough() != 0 &&
		    sender.send(now, {scope, 0, wire::Ack{incoming.reliableThrough()}}))
			ack = false;
	}

	bool sendFresh(Sender sender, Time now, wire::Payload payload)
	{
		if (freshSent == wire::kMaxSequence || !sender.send(now, {scope, freshSent + 1, std::move(payload)}))
			return false;
		++freshSent;
		nextHeartbeat = now + kHeartbeatInterval;
		return true;
	}

	bool flush(Sender sender, Time now)
	{
		flushAck(sender, now);
		const bool sent = pending && now >= pending->retry && sender.send(now, pending->kind, pending->datagram);
		if (sent)
		{
			pending->sent = true;
			pending->retry = now + kReliableRetry;
			nextHeartbeat = now + kHeartbeatInterval;
		}
		if (now >= nextHeartbeat)
			sendFresh(sender, now, wire::Heartbeat{});
		return sent;
	}

	bool expired(Time now) const
	{
		return now - lastReceived >= kIdleTimeout || (pending && now - pending->since >= kReliableTimeout) ||
			freshSent == wire::kMaxSequence || reliableSent == wire::kMaxSequence;
	}

	struct Pending
	{
		wire::Datagram datagram;
		wire::Kind kind;
		Time since, retry;
		bool sent{};
	};

	wire::SessionScope scope;
	wire::ReceiveWindow incoming;
	Time lastReceived, nextHeartbeat;
	std::optional<Pending> pending;
	uint64_t freshSent{}, reliableSent{};
	bool ack{};
};

} // namespace net::udp
