#pragma once

#include "Engine.hpp"

#include <Network/ClientWorld.hpp>
#include <Network/ClientTickScheduler.hpp>
#include <Network/ClientMovement.hpp>

#include <fstream>

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

	RemoteSession(net::udp::Address address, std::optional<uint32_t> expected,
		const net::wire::PlayerKey &key, std::string namedProfile = {});
	void startMotionTrace(uint64_t &nextFile) noexcept;
	const net::wire::PlayerState *localPlayer() const;
	bool ready() const;
	bool canReconnect() const;
	void submit(net::wire::Action action);
	void submitSlots(net::wire::ActionKind operation, int slot, int other = 0);
	ui::MultiplayerStatus connectionStatus(Clock::time_point now) const;
	const char *actionStatus(Clock::time_point now) const;
	void disconnect() noexcept;

	net::udp::ClientTransport transport;
	const net::udp::Address address;
	const std::string endpoint, namedProfile;
	const std::optional<uint32_t> expectedSeed;
	std::optional<net::wire::SessionScope> scope;
	std::optional<net::wire::Welcome> welcome;
	std::unique_ptr<net::udp::ClientWorld> projection;
	SnapshotClock players, items, mobs;
	net::udp::ClientTickScheduler inputTiming;
	net::udp::ClientPrediction prediction;
	net::udp::RemotePlayers remotePlayers;
	net::udp::ClientWorld::TerrainDependencies terrain;
	struct MotionTrace
	{
		std::ofstream file;
		Clock::time_point start, lastFlush, cameraAt{};
		net::wire::Input controls{};
		uint64_t firstInputTick{}, lastInputTick{}, lastInputSequence{};
		unsigned sent{}, failed{}, frames{};
	};
	std::optional<MotionTrace> motionTrace;
	bool hasCameraPose{};
	net::udp::ClientTraffic lastTraffic;
	Clock::time_point trafficAt{}, firstPlayerAt{};
	std::optional<Clock::time_point> roundTripAt;
	std::optional<ui::NetworkStats::Traffic> trafficRates;
	Clock::time_point jumpQueuedAt{};
	bool joinRequested{}, leaveRequested{}, seedMismatch{};
	bool actionPending{};
	const char *lastConnectionStatus{};
	const char *feedback{};
	Clock::time_point feedbackUntil{};
};
