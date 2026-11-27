#include <Engine/Engine.hpp>
#include <Engine/EngineMultiplayerActions.hpp>
#include <Engine/GameUiSample.hpp>

void Engine::loadHosting()
{
	try
	{
		if (const char *base = SDL_GetBasePath())
		{
			m_hostExecutable = std::filesystem::path(reinterpret_cast<const char8_t *>(base)) /
#ifdef _WIN32
				"ft_vox_server.exe";
#else
				"ft_vox_server";
#endif
		}
		m_hostWorlds = hosting::listWorlds(m_hostSavesRoot);
	}
	catch (const std::exception &)
	{
		m_hostError = "Could not open the hosted saves folder.";
	}
}

void Engine::populateHostingActions(MultiplayerActions &frame)
{
	frame.startHostedServer = [this](hosting::Options options, bool autoJoin, std::string_view name) {
		if (blocksNetworkSessionRequests())
			return;
		m_hostError.clear();
		auto profile = loadNamedProfile(name, m_hostError);
		if (!profile)
			return;
		if (m_hostServer.start(std::move(options), m_hostExecutable, m_hostSavesRoot,
				hosting::OwnerProfile{profile->key, m_savesRoot / ".client-profiles" / "named" / profile->name / "owner-control.key"}))
			m_hostAutoProfile = autoJoin ? std::optional{std::move(profile->name)} : std::nullopt;
	};
	frame.stopHostedServer = [this] {
		m_hostAutoProfile.reset();
		m_hostServer.stop();
	};
	frame.forceStopHostedServer = [this] { m_hostServer.forceStop(); };
	frame.joinHostedServer = [this](std::string_view name) {
		m_hostError.clear();
		const auto server = m_hostServer.snapshot();
		if (server.state != hosting::State::Ready || switchingSession() || blocksNetworkSessionRequests())
			return;
		if (auto profile = loadNamedProfile(name, m_hostError))
			queueHostedJoin(server, profile->name);
	};
	frame.administerHostedServer = [this](uint64_t launch, localcontrol::Operation operation,
		sim::PlayerId target, std::string_view name) -> std::string {
		hosting::AdminAction action{operation, target, std::nullopt};
		if (!name.empty())
		{
			std::string error;
			const auto profile = loadNamedProfile(name, error);
			if (!profile)
				return error;
			action.delegate = hosting::OwnerProfile{profile->key,
				m_savesRoot / ".client-profiles" / "named" / profile->name / "owner-control.key"};
		}
		if (!m_hostServer.administer(launch, std::move(action)))
			return "The server is busy or this administration session is unavailable.";
		return {};
	};
}

void Engine::queueHostedJoin(const hosting::Snapshot &server, const std::string &profile)
{
	// Loopback is the default. For an explicit private-network bind, the
	// local client targets that same interface on this machine.
	m_joinRequest = client::JoinRequest{server.address + ":" + std::to_string(server.port), profile, {}};
}

void Engine::tickHosting()
{
	if (auto next = m_hostServer.snapshotIfChanged(m_hostRevision))
	{
		if (next->state == hosting::State::Ready && m_hostSnapshot.state != hosting::State::Ready)
		{
			const auto position = std::ranges::lower_bound(m_hostWorlds, next->world);
			if (position == m_hostWorlds.end() || *position != next->world)
			{
				m_hostWorlds.insert(position, next->world);
				if (m_hostWorlds.size() > 64)
					m_hostWorlds.pop_back();
			}
		}
		// A supervisor state transition supersedes the previous operation
		// error (correction F15): an old hosting error can never mask the new
		// state — failures surface through the summarized host message.
		if (next->state != m_hostSnapshot.state)
			m_hostError.clear();
		m_hostSnapshot = std::move(*next);
	}
	if (m_hostSnapshot.state == hosting::State::Ready && m_hostAutoProfile &&
		!m_joinRequest && !switchingSession() && !m_exitAfterHostedStop && !blocksNetworkSessionRequests())
	{
		queueHostedJoin(m_hostSnapshot, *m_hostAutoProfile);
		m_hostAutoProfile.reset();
	}
	if (!m_hostSnapshot.active())
	{
		m_hostAutoProfile.reset();
		if (m_exitAfterHostedStop)
		{
			m_exitAfterHostedStop = false;
			if (m_hostSnapshot.state == hosting::State::Stopped)
				running = false;
		}
	}
}

void Engine::requestExit()
{
	if (m_playerUi && !m_uiSpike && !m_inspectionView && !m_benchmark.isActive())
	{
		if (!m_playerSessionRequest || m_playerSessionRequest->action != gameui::SessionAction::Quit)
			m_deferredQuit = true;
		return;
	}
	if (m_exitAfterHostedStop)
		return;
	if (!m_hostServer.snapshot().active())
	{
		running = false;
		return;
	}
	m_exitAfterHostedStop = true;
	m_hostAutoProfile.reset();
	m_hostServer.stop();
	if (m_playerUi && !m_uiSpike)
		m_playerUi->resetScreens();
	setInventoryOpen(false);
	if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0)
		SDL_RestoreWindow(window);
	setMouseCaptured(false);
}
