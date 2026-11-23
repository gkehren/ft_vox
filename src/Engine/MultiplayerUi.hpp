#pragma once

#include <Network/ClientTransport.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ui
{
enum class MultiplayerState
{
	Disconnected,
	Connecting,
	Rejected,
	SyncingTerrain,
	Connected,
	Degraded,
	Resyncing,
	TimedOut,
	Leaving
};

struct MultiplayerStatus
{
	MultiplayerState state;
	const char *label;
	const char *detail = nullptr;
};

inline MultiplayerStatus multiplayerStatus(net::udp::ClientStatus status,
	std::optional<net::wire::RejectReason> rejection, bool seedMismatch,
	bool terrainReady, bool timingDegraded)
{
	using enum net::udp::ClientStatus;
	using State = MultiplayerState;
	// Let an in-flight departure finish before offering a retry.
	if (status == Leaving)
		return {State::Leaving, "Leaving"};
	if (seedMismatch)
		return {State::Rejected, "Rejected", "World seed mismatch"};
	switch (status)
	{
	case Connecting: return {State::Connecting, "Connecting"};
	case Connected:
		if (timingDegraded)
			return {State::Degraded, "Degraded network", "Player snapshots or input acknowledgments are delayed."};
		if (!terrainReady)
			return {State::SyncingTerrain, "Joining / syncing terrain", "Waiting for synchronized player terrain and inventory."};
		return {State::Connected, "Connected"};
	case Resyncing: return {State::Resyncing, "Resyncing", "Rejoining to refresh the world replica."};
	case TimedOut: return {State::TimedOut, "Timed out", "The server did not respond in time. Check the endpoint or try Reconnect."};
	case Rejected:
		using enum net::wire::RejectReason;
		switch (rejection.value_or(Unavailable))
		{
		case Version: return {State::Rejected, "Rejected", "Protocol version mismatch. Use matching client and server builds."};
		case Content: return {State::Rejected, "Rejected", "World generator or content mismatch"};
		case Full: return {State::Rejected, "Rejected", "Server is full"};
		case Unavailable: return {State::Rejected, "Rejected", "Server refused the connection"};
		case IdentityInUse: return {State::Rejected, "Rejected", "Player profile is already connected"};
		case ProfileLimit: return {State::Rejected, "Rejected", "Server has reached its saved player limit"};
		}
		return {State::Rejected, "Rejected"};
	case Leaving:
	case Disconnected: break;
	}
	return {State::Disconnected, "Disconnected"};
}

struct NetworkStats
{
	using Seconds = std::chrono::duration<double>;

	struct Ping
	{
		Seconds roundTrip, sampleAge;
	};
	struct Snapshot
	{
		Seconds age, timeSinceFirst;
		std::optional<double> missingFraction;
	};
	struct Traffic
	{
		double receiveKiB, sendKiB;
	};
	struct Prediction
	{
		size_t pendingInputs;
		uint64_t corrections, hardResets;
		double lastCorrection, maxCorrection;
	};

	std::optional<Ping> ping;
	std::optional<Snapshot> snapshot;
	std::optional<Traffic> traffic;
	std::optional<Prediction> prediction;

	std::optional<double> correctionsPerSecond() const
	{
		if (!prediction || !snapshot)
			return std::nullopt;
		return double(prediction->corrections) / std::max(snapshot->timeSinceFirst.count(), 1.0);
	}
};

inline bool hasLiveNetworkStats(MultiplayerState state)
{
	using enum MultiplayerState;
	return state == SyncingTerrain || state == Connected || state == Degraded;
}
} // namespace ui
