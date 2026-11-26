#include <Engine/PlayerMultiplayer.hpp>

#include <algorithm>

namespace gameui
{
bool eligibleAdministration(const AdministrationIntent &intent, uint64_t launch,
	const localcontrol::Reply &state, const monitoring::Status *monitor)
{
	using enum localcontrol::Operation;
	if (launch != intent.launch || state.world != intent.world || state.epoch != intent.epoch ||
		state.role != intent.role || !localcontrol::permitted(state.role, intent.operation) ||
		(localcontrol::targeted(intent.operation) != intent.subject.has_value()) ||
		(!intent.profile.empty() && intent.operation != GrantAdmin))
		return false;
	if (intent.operation == EnableAllowlist || intent.operation == DisableAllowlist)
		return state.allowlist == intent.allowlist &&
			state.allowlist == (intent.operation == DisableAllowlist);
	if (!intent.subject)
		return true;
	const auto &expected = *intent.subject;
	const auto row = std::ranges::lower_bound(state.players, expected.id.value, {}, [](const auto &p) { return p.id.value; });
	if (row == state.players.end() || row->id != expected.id || row->role != expected.role ||
		row->banned != expected.banned || row->allowed != expected.allowed ||
		row->localAdmin != expected.localAdmin || row->kickedUntil != expected.kickedUntil ||
		!localcontrol::permitted(state.role, intent.operation, row->role))
		return false;
	return intent.operation != Kick || (monitor && std::ranges::any_of(monitor->transport.roster(), [&](const auto &p) {
		return p.id == row->id && p.state == monitoring::SessionState::Connected;
	}));
}

MultiplayerOutcome evaluateMultiplayerRequest(const MultiplayerRequest &request,
	const MultiplayerContext &context, const hosting::Snapshot &host)
{
	using enum MultiplayerOutcome;
	using enum MultiplayerAction;
	if (request.sessionEpoch != context.sessionEpoch)
		return StaleSession;
	if (context.switchingSession || context.joinPending || !context.joinPreferencesLoaded)
		return Busy;
	switch (request.action)
	{
	case StartHost: case JoinHost: case StopHost: case ForceStop: case Administer:
		if (request.launch != host.launch)
			return ServerChanged;
		break;
	default: break;
	}
	switch (request.action)
	{
	case Connect:
		return context.joinTransitionInProgress ? Busy : Submitted;
	case Reconnect: case Disconnect: case ReturnToSolo:
	case SelectProfile: case CreateProfile: case Favorite:
		return Submitted;
	case StartHost:
		return host.active() ? InvalidState : Submitted;
	case JoinHost:
		if (host.state != hosting::State::Ready)
			return InvalidState;
		return context.joinTransitionInProgress ? Busy : Submitted;
	case StopHost:
		return host.state == hosting::State::Starting || host.state == hosting::State::Ready
			? Submitted : InvalidState;
	case ForceStop:
		return host.state == hosting::State::Stopping && host.delayed ? Submitted : InvalidState;
	case Administer:
		if (host.state != hosting::State::Ready || !host.administration)
			return PermissionChanged;
		if (host.adminPending)
			return Busy;
		return host.adminChannelFailed ? InvalidState : Submitted;
	}
	return Submitted;
}
} // namespace gameui
