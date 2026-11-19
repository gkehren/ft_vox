#pragma once

#include "UdpSocket.hpp"

#include <memory>

namespace net::udp
{

struct SessionWorld
{
	uint32_t seed, generatorVersion;
};

struct SessionAdmission
{
	sim::PlayerId player;
	uint64_t tick, nextEditId;
};

struct SessionIntents
{
	std::optional<wire::Input> input;
	std::optional<wire::Action> action;
};

class SessionOwner
{
public:
	virtual ~SessionOwner() = default;
	virtual SessionAdmission joinRemote() = 0;
	virtual void leaveRemote(sim::PlayerId player) = 0;
	// Every admitted action gets a result. An input-only batch needs no reply.
	virtual std::optional<wire::ActionResult> receiveRemote(sim::PlayerId player, const SessionIntents &intents) = 0;
};

class ServerTransport
{
public:
	using Clock = std::chrono::steady_clock;
	ServerTransport(Address bind, size_t remoteCap, SessionWorld world);
	~ServerTransport();
	Address address() const;

	// Production runs poll on its own worker. Tests may pump the same IO
	// implementation with an injected monotonic clock instead of starting it.
	void start();
	void poll(Clock::time_point now);
	void rethrowIoError();
	void stop(SessionOwner &owner);

	// Call on the simulation owner at a tick boundary.
	void serviceOwner(SessionOwner &owner);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace net::udp
