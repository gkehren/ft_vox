#pragma once

#include "Engine.hpp"

#include <Network/ClientWorld.hpp>

struct Engine::RemoteSession
{
	using Clock = net::udp::ClientTransport::Clock;
	struct SnapshotClock
	{
		uint64_t tick{};
		Clock::time_point arrived{};

		template<class Snapshot>
		void observe(const net::udp::SnapshotHistory<Snapshot> &history, Clock::time_point now)
		{
			if (history.current && (arrived == Clock::time_point{} || history.current->tick != tick))
			{
				tick = history.current->tick;
				arrived = now;
			}
		}
		double alpha(uint64_t previousTick, Clock::time_point now) const;
		bool fresh(Clock::time_point now) const;
	};

	RemoteSession(net::udp::Address address, std::optional<uint32_t> expected);
	const net::wire::PlayerState *localPlayer() const;
	bool ready() const;
	void submit(net::wire::Action action);
	void submitSlots(net::wire::ActionKind operation, int slot, int other = 0);
	const char *connectionStatus() const;
	void disconnect() noexcept;

	net::udp::ClientTransport transport;
	const std::optional<uint32_t> expectedSeed;
	std::optional<net::wire::SessionScope> scope;
	std::optional<net::wire::Welcome> welcome;
	std::unique_ptr<net::udp::ClientWorld> projection;
	uint64_t renderEpoch{};
	SnapshotClock players, items, mobs;
	uint64_t lastInputTick{};
	bool joinRequested{}, leaveRequested{}, seedMismatch{};
	bool actionPending{};
	const char *lastConnectionStatus{};
	const char *feedback{};
	Clock::time_point feedbackUntil{};
};
