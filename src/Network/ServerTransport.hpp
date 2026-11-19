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

inline constexpr uint64_t kInputLeadTicks = 6;
inline constexpr uint64_t kSnapshotIntervalTicks = 3;

constexpr bool withinInputWindow(uint64_t intended, uint64_t completed)
{
	return intended > completed && intended - completed <= kInputLeadTicks;
}

struct InputSample
{
	uint64_t sequence{};
	wire::Input input;
};

// One slot per eligible tick. Zero sequence marks a vacant slot.
using InputQueue = std::array<InputSample, kInputLeadTicks>;

struct SessionIntents
{
	InputQueue inputs{};
	std::optional<wire::Action> action;
};

class SessionOwner
{
public:
	virtual ~SessionOwner() = default;
	virtual SessionAdmission joinRemote() = 0;
	virtual bool activateRemote(sim::PlayerId player) = 0;
	virtual void leaveRemote(sim::PlayerId player) = 0;
	// Every admitted action gets a result. Inputs are acknowledged via snapshots.
	virtual std::optional<wire::ActionResult> receiveRemote(sim::PlayerId player, const SessionIntents &intents) = 0;
	virtual wire::PlayerSnapshot snapshotRemote(sim::PlayerId player) const = 0;
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

	// Call before stepping the next simulation tick. Publish snapshots of the
	// completed state and optionally discard mailbox input after a host stall.
	void serviceOwner(SessionOwner &owner, uint64_t completedTick, bool discardInputs = false);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace net::udp
