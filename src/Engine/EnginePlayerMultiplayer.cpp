#include <Engine/EngineMultiplayer.hpp>
#include <Engine/EngineMultiplayerActions.hpp>
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

void Engine::pumpPlayerMultiplayer()
{
	if (!m_playerUi)
		return;
	// Gameplay boundary (correction F14): without an active multiplayer
	// screen there is no presentation to update — no action adapters and no
	// MultiplayerView are built during normal gameplay, and no profile,
	// world or endpoint collection is copied. A request queued before the
	// player left the flow is an intentional cancellation, consumed without
	// feedback. Hosting supervision, auto-join and the remote session
	// advance in their own ticks, independent of UI visibility; the first
	// pump after re-entering a multiplayer screen rebuilds current data.
	if (!gameui::multiplayerScreen(m_playerUi->screens().top()))
	{
		m_playerUi->takeMultiplayerRequest();
		return;
	}
	auto intent = m_playerUi->takeMultiplayerRequest();
	// Feedback lifetime (corrections F03/F13): scoped to its session
	// generation and, for server-scoped actions, to the server launch the
	// refusal was reported against — an old result never masks the current
	// server state.
	if (m_multiplayerNotice && !gameui::multiplayerNoticeValid(*m_multiplayerNotice,
			m_playerMultiplayerEpoch, m_hostSnapshot.launch, m_multiplayerNoticeServer))
		m_multiplayerNotice.reset();

	const auto actions = multiplayerActions();
	const auto remote = remoteSessionView();
	if (intent)
	{
		// A newly evaluated request supersedes the previous feedback (F08).
		m_multiplayerNotice.reset();
		const auto host = m_hostServer.snapshot();
		gameui::MultiplayerContext context;
		context.sessionEpoch = m_playerMultiplayerEpoch;
		context.switchingSession = actions.switchingSession;
		context.joinPending = m_joinRequest.has_value();
		context.joinPreferencesLoaded = actions.joinPreferencesLoaded;
		context.joinTransitionInProgress = gameui::joinTransitionInProgress(remote.multiplayerStatus);
		auto outcome = gameui::evaluateMultiplayerRequest(*intent, context, host);
		// The Engine adapters stay authoritative: a missing adapter means the
		// action cannot run in this state at all (correction F03).
		const auto hasAdapter = [&actions](gameui::MultiplayerAction action) {
			switch (action)
			{
			case gameui::MultiplayerAction::Connect: return bool(actions.connectServer);
			case gameui::MultiplayerAction::Reconnect: return bool(actions.joinServer);
			case gameui::MultiplayerAction::Disconnect: return bool(actions.leaveServer);
			case gameui::MultiplayerAction::ReturnToSolo: return bool(actions.returnToSolo);
			case gameui::MultiplayerAction::SelectProfile: return bool(actions.selectJoinProfile);
			case gameui::MultiplayerAction::CreateProfile: return bool(actions.createJoinProfile);
			case gameui::MultiplayerAction::Favorite: return bool(actions.toggleFavorite);
			case gameui::MultiplayerAction::StartHost: return bool(actions.startHostedServer);
			case gameui::MultiplayerAction::JoinHost: return bool(actions.joinHostedServer);
			case gameui::MultiplayerAction::StopHost: return bool(actions.stopHostedServer);
			case gameui::MultiplayerAction::ForceStop: return bool(actions.forceStopHostedServer);
			case gameui::MultiplayerAction::Administer: return bool(actions.administerHostedServer);
			}
			return false;
		};
		if (outcome == gameui::MultiplayerOutcome::Submitted && !hasAdapter(intent->action))
			outcome = gameui::MultiplayerOutcome::ActionUnavailable;
		// An administration intent captured at confirmation time is re-checked
		// against the live verified reply before dispatch (corrections
		// F02/F03): a roster change, a role loss or a permission epoch bump
		// fails the action instead of acting on stale state.
		if (outcome == gameui::MultiplayerOutcome::Submitted && intent->action == gameui::MultiplayerAction::Administer &&
			(!intent->administration || !host.administration ||
				!gameui::eligibleAdministration(*intent->administration, host.launch,
					*host.administration, freshMonitor(host))))
			outcome = gameui::MultiplayerOutcome::PermissionChanged;
		if (outcome == gameui::MultiplayerOutcome::Submitted)
		{
			using enum gameui::MultiplayerAction;
			switch (intent->action)
			{
			case Connect: actions.connectServer(std::move(intent->join)); break;
			case Reconnect: actions.joinServer(); break;
			case Disconnect: actions.leaveServer(); break;
			case ReturnToSolo: actions.returnToSolo(); break;
			case SelectProfile: actions.selectJoinProfile(intent->profile); break;
			case CreateProfile: actions.createJoinProfile(intent->profile); break;
			case Favorite: actions.toggleFavorite(intent->join.endpoint); break;
			case StartHost:
				// A hosting validation failure keeps its precise option-level
				// message on m_hostError (the Host screen shows it); the typed
				// notice is reserved for refused intents.
				m_hostError = hosting::validate(intent->options);
				if (m_hostError.empty())
					actions.startHostedServer(std::move(intent->options), intent->autoJoin, intent->profile);
				break;
			case JoinHost: actions.joinHostedServer(intent->profile); break;
			case StopHost: actions.stopHostedServer(); break;
			case ForceStop: actions.forceStopHostedServer(); break;
			case Administer:
			{
				const auto &action = *intent->administration;
				m_hostError = actions.administerHostedServer(action.launch, action.operation,
					action.subject ? action.subject->id : sim::PlayerId{}, action.profile);
				break;
			}
			}
		}
		else
		{
			m_multiplayerNotice = gameui::MultiplayerFeedback{outcome, intent->action,
				intent->launch, m_playerMultiplayerEpoch};
			// The launch this refusal is reported against (correction F13):
			// a ServerChanged verdict describes the move to the CURRENT
			// launch and clears when that launch moves again; other
			// server-scoped refusals name their own launch and drop when it
			// is gone.
			m_multiplayerNoticeServer = m_hostSnapshot.launch;
		}
	}

	gameui::MultiplayerView view;
	view.preferences = m_joinPreferences;
	view.profiles = m_joinProfiles;
	view.worlds = m_hostWorlds;
	view.sessionEpoch = m_playerMultiplayerEpoch;
	view.pending = actions.switchingSession || m_joinRequest.has_value() ||
		(m_remote && (m_remote->joinRequested || m_remote->leaveRequested));
	view.endpoint = remote.multiplayerEndpoint;
	view.sessionProfile = remote.multiplayerProfile;
	view.connection = remote.multiplayerStatus;
	view.network = remote.networkStats;
	view.reconnect = bool(actions.joinServer);
	view.disconnect = bool(actions.leaveServer);
	view.returnToSolo = bool(actions.returnToSolo);
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
	view.notice = m_multiplayerNotice;

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
