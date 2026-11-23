#include <Engine/Engine.hpp>

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

void Engine::populateHostingUi(GameUIFrame &frame)
{
	frame.hostedServer = &m_hostSnapshot;
	frame.hostWorlds = m_hostWorlds;
	frame.hostError = m_hostError;
	frame.exitAfterHostedStop = m_exitAfterHostedStop;
	frame.startHostedServer = [this](hosting::Options options, bool autoJoin, std::string_view name) {
		m_hostError.clear();
		std::optional<std::string> autoProfile;
		if (autoJoin)
		{
			auto profile = loadNamedProfile(name, m_hostError);
			if (!profile)
				return;
			autoProfile = std::move(profile->name);
		}
		if (m_hostServer.start(std::move(options), m_hostExecutable, m_hostSavesRoot))
			m_hostAutoProfile = std::move(autoProfile);
	};
	frame.stopHostedServer = [this] {
		m_hostAutoProfile.reset();
		m_hostServer.stop();
	};
	frame.forceStopHostedServer = [this] { m_hostServer.forceStop(); };
	frame.joinHostedServer = [this](std::string_view name) {
		m_hostError.clear();
		const auto server = m_hostServer.snapshot();
		if (server.state != hosting::State::Ready || switchingSession())
			return;
		if (auto profile = loadNamedProfile(name, m_hostError))
			queueHostedJoin(server, profile->name);
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
		m_hostSnapshot = std::move(*next);
	}
	if (m_hostSnapshot.state == hosting::State::Ready && m_hostAutoProfile &&
		!m_joinRequest && !switchingSession() && !m_exitAfterHostedStop)
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
	if (gameUi)
		gameUi->showHostPanel();
	if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0)
		SDL_RestoreWindow(window);
	mouseCaptured = false;
	SDL_SetWindowRelativeMouseMode(window, false);
}
