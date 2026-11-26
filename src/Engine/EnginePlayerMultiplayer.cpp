#include <Engine/EngineMultiplayer.hpp>
#include <Engine/GameUiSample.hpp>

#include <algorithm>

namespace
{
const monitoring::Status *freshMonitor(const hosting::Snapshot &host)
{
	return host.state == hosting::State::Ready && host.status &&
		monitoring::fresh(host.status->sampledMs, monitoring::monotonicMs()) ? host.status.get() : nullptr;
}

std::string playerHostMessage(const hosting::Snapshot &host)
{
	using enum hosting::State;
	switch (host.state)
	{
	case Idle: return "Server stopped";
	case Starting: return host.delayed ? "Startup is taking longer than expected." : "Starting the server...";
	case Ready: return "Server ready";
	case Stopping: return host.delayed ? "Checkpoint is taking longer than expected." : "Stopping and saving the server...";
	case Stopped: return "Server stopped. World checkpoint committed.";
	case Failed: break;
	}
	// Supervisor output can contain paths. Summarize known classes without
	// copying child logs or exception text into the player surface.
	const auto &message = host.message;
	if (message.find("bind") != std::string::npos || message.find("10048") != std::string::npos ||
		message.find("Address already in use") != std::string::npos)
		return "Could not bind. Check the private address or choose another port.";
	if (message.find("force stopped") != std::string::npos)
		return "Server was force stopped. Changes since the last checkpoint may be lost.";
	if (message.find("owner") != std::string::npos || message.find("credential") != std::string::npos)
		return "Local owner verification failed. Check the saved owner profile or use documented offline recovery.";
	if (message.find("checkpoint") != std::string::npos || message.find("error=persistence") != std::string::npos)
		return "Server stopped without confirming a checkpoint. Reopen the world to recover the last committed save.";
	return "Server failed. Check the world, server installation and port, then try again.";
}
}

bool gameui::eligibleAdministration(const AdministrationIntent &intent, uint64_t launch,
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

void Engine::pumpPlayerMultiplayer()
{
	if (!m_playerUi)
		return;
	auto intent = m_playerUi->takeMultiplayerRequest();
	if (!gameui::multiplayerScreen(m_playerUi->screens().top()))
		return;

	GameUIFrame frame{};
	populateJoinUi(frame);
	if (m_remote)
		populateRemoteUi(frame);
	if (intent && intent->sessionEpoch == m_playerMultiplayerEpoch &&
		!frame.switchingSession && !m_joinRequest && frame.joinPreferences)
	{
		using enum gameui::MultiplayerAction;
		const auto host = m_hostServer.snapshot();
		const auto sameLaunch = intent->launch == host.launch;
		switch (intent->action)
		{
		case Connect:
			if (!gameui::joinTransitionInProgress(frame.multiplayerStatus))
				frame.connectServer(std::move(intent->join));
			break;
		case Reconnect: if (frame.joinServer) frame.joinServer(); break;
		case Disconnect: if (frame.leaveServer) frame.leaveServer(); break;
		case ReturnToSolo: if (frame.returnToSolo) frame.returnToSolo(); break;
		case SelectProfile: frame.selectJoinProfile(intent->profile); break;
		case CreateProfile: frame.createJoinProfile(intent->profile); break;
		case Favorite: frame.toggleFavorite(intent->join.endpoint); break;
		case StartHost:
			if (sameLaunch && !host.active())
			{
				m_hostError = hosting::validate(intent->options);
				if (m_hostError.empty())
					frame.startHostedServer(std::move(intent->options), intent->autoJoin, intent->profile);
			}
			break;
		case JoinHost:
			if (sameLaunch && host.state == hosting::State::Ready &&
				!gameui::joinTransitionInProgress(frame.multiplayerStatus))
				frame.joinHostedServer(intent->profile);
			break;
		case StopHost:
			if (sameLaunch && (host.state == hosting::State::Starting || host.state == hosting::State::Ready))
				frame.stopHostedServer();
			break;
		case ForceStop:
			if (sameLaunch && host.state == hosting::State::Stopping && host.delayed)
				frame.forceStopHostedServer();
			break;
		case Administer:
			if (!intent->administration || host.state != hosting::State::Ready || host.adminPending ||
				host.adminChannelFailed || !host.administration || !gameui::eligibleAdministration(
					*intent->administration, host.launch, *host.administration, freshMonitor(host)))
				m_hostError = "Permissions, player state or server changed. Refresh and review the action again.";
			else
			{
				const auto &request = *intent->administration;
				m_hostError = frame.administerHostedServer(request.launch, request.operation,
					request.subject ? request.subject->id : sim::PlayerId{}, request.profile);
			}
			break;
		}
	}

	gameui::MultiplayerView view;
	view.preferences = m_joinPreferences;
	view.profiles = m_joinProfiles;
	view.worlds = m_hostWorlds;
	view.sessionEpoch = m_playerMultiplayerEpoch;
	view.pending = frame.switchingSession || m_joinRequest.has_value() ||
		(m_remote && (m_remote->joinRequested || m_remote->leaveRequested));
	view.endpoint = frame.multiplayerEndpoint;
	view.sessionProfile = frame.multiplayerProfile;
	view.connection = frame.multiplayerStatus;
	view.network = frame.networkStats;
	view.reconnect = bool(frame.joinServer);
	view.disconnect = bool(frame.leaveServer);
	view.returnToSolo = bool(frame.returnToSolo);
	view.playable = m_remote && m_remote->ready();
	view.joinError = m_joinError;
	view.preferencesError = m_joinPreferencesError;
	view.hostError = m_hostError;
	const auto &latest = m_hostSnapshot;
	view.hostState = latest.state;
	view.hostWorld = latest.world;
	view.hostMessage = playerHostMessage(latest);
	if (latest.state == hosting::State::Ready)
		view.hostEndpoint = latest.address + ":" + std::to_string(latest.port);
	view.launch = latest.launch;
	view.checkpoint = latest.checkpoint;
	view.delayed = latest.delayed;
	view.adminPending = latest.adminPending;
	view.adminChannelFailed = latest.adminChannelFailed;
	view.adminOperation = latest.adminOperation;
	view.adminResult = latest.adminResult;
	if (freshMonitor(latest))
		view.monitor = latest.status;
	if (latest.state == hosting::State::Ready && !latest.adminChannelFailed)
		view.administration = latest.administration;

	if (m_remote && m_remote->scope && m_remote->players.fresh(RemoteSession::Clock::now()))
	{
		const auto &snapshot = m_remote->transport.playerSnapshots().current;
		if (m_remote->welcome)
			view.self = m_remote->welcome->player;
		if (snapshot)
			for (size_t i = 0; i < snapshot->count; ++i)
				view.nearby.push_back(snapshot->players[i].player);
	}
	m_playerUi->setMultiplayerView(std::move(view));
}
