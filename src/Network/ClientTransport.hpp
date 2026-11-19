#pragma once

#include "UdpSocket.hpp"

#include <memory>

namespace net::udp
{

enum class ClientStatus { Disconnected, Connecting, Connected, Leaving, TimedOut, Rejected };

// Minimal v2 client IO adapter. A single caller owns the nonblocking pump
// and its connection state. A future client host can run it on its IO thread.
class ClientTransport
{
public:
	using Clock = std::chrono::steady_clock;
	explicit ClientTransport(Address server);
	~ClientTransport();
	void connect(Clock::time_point now);
	void poll(Clock::time_point now);
	void disconnect(Clock::time_point now);

	ClientStatus status() const;
	std::optional<wire::Welcome> welcome() const;
	std::optional<wire::SessionScope> scope() const;
	std::optional<wire::RejectReason> rejection() const;
	uint64_t heartbeatsReceived() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace net::udp
