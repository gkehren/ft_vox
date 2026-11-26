// CPU-only intent validation for the native multiplayer screens (issue #306,
// corrections F02/F03/F04). Links only the pure PlayerMultiplayer translation
// unit: no SDL, Vulkan, ImGui or Engine host. eligibleAdministration pins the
// administration re-validation contract (identity-stable targeting, kick
// online requirement) and evaluateMultiplayerRequest pins the typed outcome
// of every refused request class.
#include <Engine/PlayerMultiplayer.hpp>

#include "TestMain.hpp"

#include <memory>

namespace
{
localcontrol::Reply ownerReply()
{
	localcontrol::Reply reply;
	reply.world = 7;
	reply.epoch = 3;
	reply.role = localcontrol::Role::Owner;
	reply.allowlist = false;
	reply.players = {
		{sim::PlayerId{1}, localcontrol::Role::Owner, false, false, false, 0},
		{sim::PlayerId{2}, localcontrol::Role::Player, false, false, false, 0},
		{sim::PlayerId{4}, localcontrol::Role::Admin, false, false, true, 0},
	};
	return reply;
}

gameui::AdministrationIntent intent(localcontrol::Operation operation,
	std::optional<localcontrol::Player> subject = std::nullopt, std::string profile = {})
{
	return gameui::AdministrationIntent{9, 7, 3, localcontrol::Role::Owner, operation,
		std::move(subject), false, std::move(profile)};
}

std::shared_ptr<const monitoring::Status> monitorWith(sim::PlayerId id, monitoring::SessionState state)
{
	auto status = std::make_shared<monitoring::Status>();
	status->transport.count = 1;
	status->transport.players[0] = monitoring::Player{id, state};
	return status;
}

gameui::MultiplayerRequest request(gameui::MultiplayerAction action, uint64_t launch = 9,
	uint64_t epoch = 5)
{
	gameui::MultiplayerRequest request;
	request.action = action;
	request.launch = launch;
	request.sessionEpoch = epoch;
	return request;
}

hosting::Snapshot hostSnapshot(hosting::State state, uint64_t launch = 9)
{
	hosting::Snapshot snapshot;
	snapshot.state = state;
	snapshot.launch = launch;
	return snapshot;
}

gameui::MultiplayerContext clearContext()
{
	return gameui::MultiplayerContext{5, false, false, true, false};
}
}

int main()
{
	using enum gameui::MultiplayerOutcome;
	using enum gameui::MultiplayerAction;
	using enum localcontrol::Operation;
	const auto reply = ownerReply();
	const localcontrol::Player guest{sim::PlayerId{2}, localcontrol::Role::Player, false, false, false, 0};
	const localcontrol::Player delegate{sim::PlayerId{4}, localcontrol::Role::Admin, false, false, true, 0};
	const auto liveGuest = monitorWith(sim::PlayerId{2}, monitoring::SessionState::Connected);

	// --- eligibleAdministration: identity-stable re-validation (F01/F04) ---
	CHECK(gameui::eligibleAdministration(intent(Kick, guest), 9, reply, liveGuest.get()),
		"a live connected target with a matching row is eligible");
	CHECK(gameui::eligibleAdministration(intent(Ban, guest), 9, reply, nullptr),
		"ban does not require a live monitor sample");
	CHECK(gameui::eligibleAdministration(intent(List), 9, reply, nullptr),
		"the refresh operation is untargeted and offline");
	CHECK(gameui::eligibleAdministration(intent(GrantAdmin, guest, "owner-native"), 9, reply, nullptr),
		"grant admin may carry the delegation profile");
	CHECK(!gameui::eligibleAdministration(intent(Kick, guest), 8, reply, liveGuest.get()),
		"a stale launch generation is refused");
	CHECK(!gameui::eligibleAdministration(
		gameui::AdministrationIntent{9, 8, 3, localcontrol::Role::Owner, Kick, guest, false, {}},
		9, reply, liveGuest.get()), "a stale world identity is refused");
	CHECK(!gameui::eligibleAdministration(
		gameui::AdministrationIntent{9, 7, 4, localcontrol::Role::Owner, Kick, guest, false, {}},
		9, reply, liveGuest.get()), "a stale permission epoch is refused");
	CHECK(!gameui::eligibleAdministration(
		gameui::AdministrationIntent{9, 7, 3, localcontrol::Role::Admin, GrantAdmin, guest, false, {}},
		9, reply, nullptr), "a role beyond the live permission is refused");
	CHECK(!gameui::eligibleAdministration(intent(Kick), 9, reply, liveGuest.get()),
		"a targeted operation must carry its subject");
	CHECK(!gameui::eligibleAdministration(intent(List, guest), 9, reply, nullptr),
		"an untargeted operation must not carry a subject");
	CHECK(!gameui::eligibleAdministration(intent(Kick, guest, "owner-native"), 9, reply, liveGuest.get()),
		"only grant admin may carry a profile");
	CHECK(!gameui::eligibleAdministration(
		intent(Kick, localcontrol::Player{sim::PlayerId{2}, localcontrol::Role::Admin, false, false, false, 0}),
		9, reply, liveGuest.get()), "a subject row that changed role is refused");
	CHECK(!gameui::eligibleAdministration(
		intent(Kick, localcontrol::Player{sim::PlayerId{2}, localcontrol::Role::Player, true, false, false, 0}),
		9, reply, liveGuest.get()), "a subject row that changed ban state is refused");
	CHECK(!gameui::eligibleAdministration(
		intent(Kick, localcontrol::Player{sim::PlayerId{2}, localcontrol::Role::Player, false, false, false, 60}),
		9, reply, liveGuest.get()), "a subject row that changed kick state is refused");
	CHECK(!gameui::eligibleAdministration(
		intent(Kick, localcontrol::Player{sim::PlayerId{3}, localcontrol::Role::Player, false, false, false, 0}),
		9, reply, liveGuest.get()), "an unknown subject is refused");
	CHECK(!gameui::eligibleAdministration(intent(Kick, localcontrol::Player{sim::PlayerId{1},
		localcontrol::Role::Owner, false, false, false, 0}), 9, reply, liveGuest.get()),
		"the protected owner row cannot be targeted");
	CHECK(!gameui::eligibleAdministration(
		gameui::AdministrationIntent{9, 7, 3, localcontrol::Role::Admin, GrantAdmin, delegate, false, {}},
		9, reply, nullptr), "an admin role cannot act on another admin row");

	// --- Kick semantics: a fresh connected roster entry is mandatory (F02) ---
	CHECK(!gameui::eligibleAdministration(intent(Kick, guest), 9, reply, nullptr),
		"stale monitoring prevents kicking");
	CHECK(!gameui::eligibleAdministration(intent(Kick, guest), 9, reply,
		monitorWith(sim::PlayerId{2}, monitoring::SessionState::Disconnecting).get()),
		"a target that is leaving cannot be kicked");
	CHECK(!gameui::eligibleAdministration(intent(Kick, guest), 9, reply,
		monitorWith(sim::PlayerId{2}, monitoring::SessionState::Syncing).get()),
		"a target that is syncing cannot be kicked");
	CHECK(!gameui::eligibleAdministration(intent(Kick, guest), 9, reply,
		monitorWith(sim::PlayerId{5}, monitoring::SessionState::Connected).get()),
		"a roster without the target cannot confirm the kick");
	CHECK(gameui::eligibleAdministration(intent(Kick, guest), 9, reply, liveGuest.get()),
		"a kick targets the persistent identity while it is connected");

	// --- Allowlist toggles mirror the current state (F04) ---
	CHECK(gameui::eligibleAdministration(intent(EnableAllowlist), 9, reply, nullptr),
		"enabling the disabled allowlist is eligible");
	CHECK(!gameui::eligibleAdministration(intent(DisableAllowlist), 9, reply, nullptr),
		"disabling a disabled allowlist is a no-op refusal");
	auto allowlisted = reply;
	allowlisted.allowlist = true;
	CHECK(gameui::eligibleAdministration(
		gameui::AdministrationIntent{9, 7, 3, localcontrol::Role::Owner, DisableAllowlist,
			std::nullopt, true, {}}, 9, allowlisted, nullptr),
		"disabling the enabled allowlist is eligible");
	CHECK(!gameui::eligibleAdministration(
		gameui::AdministrationIntent{9, 7, 3, localcontrol::Role::Owner, EnableAllowlist,
			std::nullopt, false, {}}, 9, allowlisted, nullptr),
		"enabling an enabled allowlist is a no-op refusal");

	// --- evaluateMultiplayerRequest: every refusal is typed (F03) ---
	CHECK(gameui::evaluateMultiplayerRequest(request(Connect), clearContext(), hostSnapshot(hosting::State::Idle))
		== Submitted, "a clear connect is submitted");
	CHECK(gameui::evaluateMultiplayerRequest(request(Connect, 9, 4), clearContext(), hostSnapshot(hosting::State::Idle))
		== StaleSession, "an old session epoch is a stale request");
	CHECK(gameui::evaluateMultiplayerRequest(request(Reconnect), gameui::MultiplayerContext{5, true, false, true, false},
		hostSnapshot(hosting::State::Idle)) == Busy, "a session switch refuses new actions");
	CHECK(gameui::evaluateMultiplayerRequest(request(Disconnect), gameui::MultiplayerContext{5, false, true, true, false},
		hostSnapshot(hosting::State::Idle)) == Busy, "a pending join draft refuses new actions");
	CHECK(gameui::evaluateMultiplayerRequest(request(Favorite), gameui::MultiplayerContext{5, false, false, false, false},
		hostSnapshot(hosting::State::Idle)) == Busy, "unreadable preferences refuse profile actions");
	CHECK(gameui::evaluateMultiplayerRequest(request(Connect), gameui::MultiplayerContext{5, false, false, true, true},
		hostSnapshot(hosting::State::Idle)) == Busy, "an in-flight join transition refuses a second connect");
	CHECK(gameui::evaluateMultiplayerRequest(request(SelectProfile), clearContext(), hostSnapshot(hosting::State::Idle))
		== Submitted && gameui::evaluateMultiplayerRequest(request(CreateProfile), clearContext(),
			hostSnapshot(hosting::State::Idle)) == Submitted &&
		gameui::evaluateMultiplayerRequest(request(Favorite), clearContext(), hostSnapshot(hosting::State::Idle))
			== Submitted && gameui::evaluateMultiplayerRequest(request(Reconnect), clearContext(),
			hostSnapshot(hosting::State::Idle)) == Submitted,
		"join-side actions submit on a clear context");
	CHECK(gameui::evaluateMultiplayerRequest(request(StartHost, 8), clearContext(), hostSnapshot(hosting::State::Idle))
		== ServerChanged, "an action from a previous server launch is refused");
	CHECK(gameui::evaluateMultiplayerRequest(request(StartHost), clearContext(), hostSnapshot(hosting::State::Starting))
		== InvalidState, "starting twice is an invalid state");
	CHECK(gameui::evaluateMultiplayerRequest(request(StartHost), clearContext(), hostSnapshot(hosting::State::Idle))
		== Submitted, "a fresh host start is submitted");
	CHECK(gameui::evaluateMultiplayerRequest(request(JoinHost), clearContext(), hostSnapshot(hosting::State::Starting))
		== InvalidState, "joining before ready is an invalid state");
	CHECK(gameui::evaluateMultiplayerRequest(request(JoinHost), gameui::MultiplayerContext{5, false, false, true, true},
		hostSnapshot(hosting::State::Ready)) == Busy, "joining during a transition waits");
	CHECK(gameui::evaluateMultiplayerRequest(request(JoinHost), clearContext(), hostSnapshot(hosting::State::Ready))
		== Submitted, "joining a ready server is submitted");
	CHECK(gameui::evaluateMultiplayerRequest(request(StopHost), clearContext(), hostSnapshot(hosting::State::Stopping))
		== InvalidState, "stopping twice is an invalid state");
	CHECK(gameui::evaluateMultiplayerRequest(request(StopHost), clearContext(), hostSnapshot(hosting::State::Ready))
		== Submitted, "stopping a live server is submitted");
	CHECK(gameui::evaluateMultiplayerRequest(request(ForceStop), clearContext(),
		hostSnapshot(hosting::State::Stopping)) == InvalidState, "force stop requires a delayed shutdown");
	hosting::Snapshot delayed = hostSnapshot(hosting::State::Stopping);
	delayed.delayed = true;
	CHECK(gameui::evaluateMultiplayerRequest(request(ForceStop), clearContext(), delayed)
		== Submitted, "force stop after a delayed shutdown is submitted");

	// --- Administration requests revalidate the channel and reply (F03) ---
	hosting::Snapshot administered = hostSnapshot(hosting::State::Ready);
	CHECK(gameui::evaluateMultiplayerRequest(request(Administer), clearContext(), administered)
		== PermissionChanged, "without a verified reply an administration intent is refused");
	administered.administration = std::make_shared<const localcontrol::Reply>(ownerReply());
	administered.adminPending = true;
	CHECK(gameui::evaluateMultiplayerRequest(request(Administer), clearContext(), administered)
		== Busy, "an in-flight administration request blocks the next one");
	administered.adminPending = false;
	administered.adminChannelFailed = true;
	CHECK(gameui::evaluateMultiplayerRequest(request(Administer), clearContext(), administered)
		== InvalidState, "a failed local control channel is an invalid state");
	administered.adminChannelFailed = false;
	CHECK(gameui::evaluateMultiplayerRequest(request(Administer), clearContext(), administered)
		== Submitted, "a verified administration channel submits");
	CHECK(gameui::evaluateMultiplayerRequest(request(Administer, 8), clearContext(), administered)
		== ServerChanged, "an administration intent from an old launch is refused");

	// --- Feedback lifetime is scoped to its generation (F13/F15) ---
	const gameui::MultiplayerFeedback clientRefused{gameui::MultiplayerOutcome::Busy,
		gameui::MultiplayerAction::Connect, 9, 5};
	CHECK(gameui::multiplayerNoticeValid(clientRefused, 5, 9, 9),
		"client connection feedback lives for its session generation");
	CHECK(gameui::multiplayerNoticeValid(clientRefused, 5, 12345, 12345),
		"an unrelated hosting change does not discard client feedback");
	CHECK(!gameui::multiplayerNoticeValid(clientRefused, 6, 9, 9),
		"a new session generation drops the old refusal");
	const gameui::MultiplayerFeedback serverBusy{gameui::MultiplayerOutcome::Busy,
		gameui::MultiplayerAction::StopHost, 9, 5};
	CHECK(gameui::multiplayerNoticeValid(serverBusy, 5, 9, 0),
		"server-scoped feedback names its launch");
	CHECK(!gameui::multiplayerNoticeValid(serverBusy, 5, 10, 10),
		"a server restart drops refusals from the previous launch");
	const gameui::MultiplayerFeedback serverMoved{gameui::MultiplayerOutcome::ServerChanged,
		gameui::MultiplayerAction::Administer, 9, 5};
	CHECK(gameui::multiplayerNoticeValid(serverMoved, 5, 10, 10),
		"a ServerChanged verdict is reported against the new launch");
	CHECK(!gameui::multiplayerNoticeValid(serverMoved, 5, 11, 10),
		"the next launch change clears the ServerChanged verdict");
	CHECK(gameui::serverScoped(gameui::MultiplayerAction::StartHost) &&
		gameui::serverScoped(gameui::MultiplayerAction::JoinHost) &&
		gameui::serverScoped(gameui::MultiplayerAction::StopHost) &&
		gameui::serverScoped(gameui::MultiplayerAction::ForceStop) &&
		gameui::serverScoped(gameui::MultiplayerAction::Administer) &&
		!gameui::serverScoped(gameui::MultiplayerAction::Connect) &&
		!gameui::serverScoped(gameui::MultiplayerAction::Favorite),
		"only hosted and administrative actions are server-scoped");

	// --- Cross-world administrative selection scope (F12) ---
	CHECK(gameui::adminSelectionCompatible(9, 7, true, 9, 7, true),
		"a roster refresh preserves the selection context");
	CHECK(!gameui::adminSelectionCompatible(10, 7, true, 9, 7, true),
		"a server relaunch invalidates the selection");
	CHECK(!gameui::adminSelectionCompatible(9, 8, true, 9, 7, true),
		"the same numeric PlayerId in another world is a different player");
	CHECK(!gameui::adminSelectionCompatible(9, 7, false, 9, 7, true),
		"losing the verified authorization invalidates the selection");
	CHECK(!gameui::adminSelectionCompatible(9, 7, true, 9, 7, false),
		"recovering the authorization never restores an obsolete selection context");
	return testmain::exitCode();
}
