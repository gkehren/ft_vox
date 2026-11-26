#pragma once

#include <Engine/MultiplayerJoin.hpp>
#include <Engine/MultiplayerUi.hpp>
#include <Hosting/ServerSupervisor.hpp>

namespace gameui
{
enum class MultiplayerAction
{
	Connect, Reconnect, Disconnect, ReturnToSolo, SelectProfile, CreateProfile, Favorite,
	StartHost, JoinHost, StopHost, ForceStop, Administer
};

struct AdministrationIntent
{
	uint64_t launch{}, world{}, epoch{};
	localcontrol::Role role{};
	localcontrol::Operation operation{};
	std::optional<localcontrol::Player> subject;
	bool allowlist{};
	std::string profile;
};

struct MultiplayerRequest
{
	MultiplayerAction action{};
	client::JoinRequest join;
	hosting::Options options;
	std::string profile;
	bool autoJoin{};
	uint64_t launch{};
	uint64_t sessionEpoch{};
	std::optional<AdministrationIntent> administration;
};

struct MultiplayerView
{
	client::JoinPreferences preferences;
	std::vector<std::string> profiles, worlds;
	std::string endpoint, sessionProfile, joinError, preferencesError, hostError;
	std::optional<ui::MultiplayerStatus> connection;
	ui::NetworkStats network;
	bool pending{}, reconnect{}, disconnect{}, returnToSolo{}, playable{};
	uint64_t sessionEpoch{};
	hosting::State hostState{};
	std::string hostWorld, hostEndpoint, hostMessage;
	uint64_t launch{}, checkpoint{};
	bool delayed{}, adminPending{}, adminChannelFailed{};
	localcontrol::Operation adminOperation{};
	std::optional<localcontrol::Result> adminResult;
	std::shared_ptr<const monitoring::Status> monitor;
	std::shared_ptr<const localcontrol::Reply> administration;
	std::vector<sim::PlayerId> nearby;
	sim::PlayerId self;
};

inline bool joinTransitionInProgress(const std::optional<ui::MultiplayerStatus> &status)
{
	if (!status)
		return false;
	using enum ui::MultiplayerState;
	return status->state == Connecting || status->state == SyncingTerrain ||
		status->state == Resyncing || status->state == Leaving;
}

bool eligibleAdministration(const AdministrationIntent &intent, uint64_t launch,
	const localcontrol::Reply &state, const monitoring::Status *monitor);
} // namespace gameui
