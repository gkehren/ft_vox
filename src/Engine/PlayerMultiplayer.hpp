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

// Explicit result of a drained multiplayer request (correction F03). Every
// refusal names WHY the request was refused so the surface can show
// actionable feedback instead of silently discarding the player's intent.
enum class MultiplayerOutcome
{
	Submitted,   // dispatched through the Engine adapters
	Cancelled,   // the player navigated away first: intentional, never an error
	StaleSession,// the session epoch moved: the request describes an old session
	ServerChanged,// the hosted server relaunched: the request targets the old launch
	Busy,        // a session transition or join draft is still finishing
	InvalidState,// the action does not apply to the current server state
	PermissionChanged,// permissions, roster or player state moved under the request
	ActionUnavailable // the Engine adapter for that action is not present
};

/// Operation-scoped feedback for the active session generation: the surface
/// drops it once the epoch or launch moves, so an old result can never mask a
/// newer session (correction F03).
struct MultiplayerFeedback
{
	MultiplayerOutcome outcome{};
	MultiplayerAction action{};
	uint64_t launch{}, sessionEpoch{};
};

/// Live host-side context a drained request is judged against. Filled by the
/// Engine from its frame state; kept pure so the decision is CPU-testable.
struct MultiplayerContext
{
	uint64_t sessionEpoch{};
	bool switchingSession{};
	bool joinPending{};
	bool joinPreferencesLoaded{};
	bool joinTransitionInProgress{};
};

/// Decide whether the Engine may dispatch `request` through its adapters
/// given `context` and the live hosted server snapshot `host`. `Submitted`
/// does NOT mean the action succeeds: the Engine adapters stay authoritative
/// (hosted validation, administration eligibility, checkpoint refusals).
MultiplayerOutcome evaluateMultiplayerRequest(const MultiplayerRequest &request,
	const MultiplayerContext &context, const hosting::Snapshot &host);

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
	/// Last refused request of this session generation (correction F03).
	std::optional<MultiplayerFeedback> notice;
};

inline bool joinTransitionInProgress(const std::optional<ui::MultiplayerStatus> &status)
{
	if (!status)
		return false;
	using enum ui::MultiplayerState;
	return status->state == Connecting || status->state == SyncingTerrain ||
		status->state == Resyncing || status->state == Leaving;
}

/// Eligibility of an administration intent against the live verified reply
/// (correction F02/F03). The intent captures the launch, world, permission
/// epoch, operation and subject row at CONFIRMATION time; this re-checks all
/// of them against the current state before the Engine dispatches, so a
/// roster change, a role loss or a permission epoch bump fails the action
/// instead of acting on stale state.
///
/// Kick semantics (correction F02): a kick intentionally targets the
/// PERSISTENT player identity, not one network session. The server stores
/// `kickedUntil` on the persistent access record (WorldRoles.cpp), refuses a
/// rejoin while it is in the future and refuses to kick a target that is not
/// online — so this check additionally requires a fresh monitoring sample
/// with the target Connected. A kicked player cannot return for
/// localcontrol::kKickSeconds; that lockout is the documented behavior the
/// confirmation wording describes.
bool eligibleAdministration(const AdministrationIntent &intent, uint64_t launch,
	const localcontrol::Reply &state, const monitoring::Status *monitor);
} // namespace gameui
