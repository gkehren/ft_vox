#include "EngineMultiplayer.hpp"

#include <Engine/ClientProfile.hpp>
#include <Engine/EngineMultiplayerActions.hpp>
#include <Engine/GameUiSample.hpp>

#include <cassert>
#include <utility>

void Engine::loadJoinPreferences()
{
	try
	{
		const auto root = m_savesRoot / ".client-profiles";
		if (!m_joinPreferences.load(root / "join-settings.txt"))
			m_joinPreferencesError = "Could not read saved multiplayer preferences.";
		m_joinProfiles = client::listProfiles(root / "named");
	}
	catch (const std::exception &)
	{
		m_joinPreferencesError = "Could not read saved multiplayer preferences or profiles.";
	}
}

void Engine::persistJoinPreferences()
{
	try
	{
		if (m_joinPreferences.save(m_savesRoot / ".client-profiles" / "join-settings.txt"))
		{
			m_joinPreferencesError.clear();
			return;
		}
	}
	catch (const std::exception &) {}
	m_joinPreferencesError = "Could not save multiplayer preferences. Changes apply for this run.";
}

MultiplayerActions Engine::multiplayerActions()
{
	MultiplayerActions frame;
	if (m_benchmark.isActive())
		return frame;

	frame.joinPreferencesLoaded = true;
	populateHostingActions(frame);
	frame.switchingSession = switchingSession() || blocksNetworkSessionRequests();
	frame.connectServer = [this](client::JoinRequest request) {
		if (!switchingSession() && !blocksNetworkSessionRequests())
			m_joinRequest = std::move(request);
	};
	if (m_remote && !frame.switchingSession)
		frame.returnToSolo = [this] {
			if (!switchingSession() && !blocksNetworkSessionRequests())
			{
				m_joinError.clear();
				m_returnToSolo = true;
			}
		};
	frame.selectJoinProfile = [this](std::string_view name) {
		m_joinError.clear();
		m_joinPreferences.selectedProfile = name;
		persistJoinPreferences();
	};
	frame.createJoinProfile = [this](std::string_view name) {
		m_joinError.clear();
		const auto canonical = client::profileName(name);
		if (!canonical)
		{
			m_joinError = "Use 1 to 32 letters, digits, dashes or underscores. Start with a letter or digit and avoid reserved names.";
			return;
		}
		const auto directory = m_savesRoot / ".client-profiles" / "named";
		try
		{
			client::playerKey(directory / *canonical, client::ProfileAccess::Create);
			m_joinPreferences.selectedProfile = *canonical;
			m_joinProfiles = client::listProfiles(directory);
			persistJoinPreferences();
		}
		catch (const client::ProfileCreationError &error)
		{
			m_joinError = error.what();
		}
		catch (const std::exception &)
		{
			m_joinError = "Could not create that profile. Choose a new name or check the local profile folder.";
		}
	};
	frame.toggleFavorite = [this](std::string_view endpoint) {
		m_joinError.clear();
		try
		{
			if (const auto address = client::parseEndpoint(endpoint))
			{
				m_joinPreferences.toggleFavorite(*address);
				persistJoinPreferences();
				return;
			}
			m_joinError = "Enter a numeric IPv4 address and a port from 1 to 65535.";
		}
		catch (const std::exception &)
		{
			m_joinError = "Could not validate the server endpoint.";
		}
	};
	if (m_remote && !frame.switchingSession)
	{
		if (m_remote->canReconnect())
			frame.joinServer = [this] {
				if (m_remote && !switchingSession() && !blocksNetworkSessionRequests())
					m_remote->joinRequested = true;
			};
		else if (m_remote->transport.status() != net::udp::ClientStatus::Leaving)
			frame.leaveServer = [this] {
				if (m_remote && !switchingSession() && !blocksNetworkSessionRequests())
					m_remote->leaveRequested = true;
			};
	}
	return frame;
}

std::optional<Engine::NamedProfile> Engine::loadNamedProfile(std::string_view name, std::string &error) const
{
	auto canonical = client::profileName(name);
	if (!canonical)
	{
		error = "Select or create a valid named profile before connecting.";
		return std::nullopt;
	}
	try
	{
		const auto profile = m_savesRoot / ".client-profiles" / "named" / *canonical;
		auto key = client::playerKey(profile, client::ProfileAccess::Load);
		return NamedProfile{std::move(*canonical), key};
	}
	catch (const std::exception &)
	{
		error = "Could not read that profile's key. Select an existing profile, restore its key, or create a new profile.";
		return std::nullopt;
	}
}

void Engine::prepareRemoteJoin(const client::JoinRequest &request)
{
	m_joinError.clear();
	const auto target = client::validateJoin(request.endpoint, request.expectedSeed, m_joinError);
	if (!target)
		return;
	const auto profile = loadNamedProfile(request.profile, m_joinError);
	if (!profile)
		return;
	// Construct the new transport before disturbing the current world. It
	// has no handshake in flight until startRemote and the next poll.
	m_nextRemote = std::make_unique<RemoteSession>(target->address, target->expectedSeed, profile->key, profile->name);
}

void Engine::applySessionRequests()
{
	// Finish an already-queued network switch before a deferred quit.
	if (m_playerSessionRequest || m_soloWork.valid() || m_failedPlayerSession)
		return;
	if (auto request = std::exchange(m_joinRequest, std::nullopt); request && !switchingSession())
	{
		try
		{
			if (!m_benchmark.isActive())
				prepareRemoteJoin(*request);
		}
		catch (const std::exception &)
		{
			m_joinError = "Could not prepare the connection. Check your network settings and try again.";
		}
	}
	if (!switchingSession() || (m_remote && !m_remote->canReconnect()))
		return;

	if (std::exchange(m_returnToSolo, false))
	{
		resumeSolo();
		resetFrameClock();
		return;
	}
	auto remote = std::move(m_nextRemote);
	if (sim && sim->isWorldOpen())
	{
		assert(renderBridge);
		vkContext->waitIdle();
		renderBridge->drainAsyncJobsForPersistence();
		if (!sim->savePlayerState(camera.getYaw(), camera.getPitch()))
		{
			m_joinError = "Could not save the solo world. Resolve the save error before joining.";
			return;
		}
	}
	try
	{
		startRemote(std::move(remote));
		resetFrameClock();
	}
	catch (const std::exception &)
	{
		m_joinError = "Could not start the connection. Check your network settings and try again.";
	}
}

void Engine::retireWorldProjection()
{
	resetWorldProjection();
	if (!m_remote)
	{
		worldRenderer->overlays().setPlayers({});
		jumpPressed = false;
	}
	if (!renderBridge)
		return;
	// Workers and render handles still reference the old backing. Also
	// finish all submitted draws before the bridge returns arena ranges.
	vkContext->waitIdle();
	renderBridge.reset();
	resourceRetire.flush();
}

void Engine::startRemote(std::unique_ptr<RemoteSession> remote)
{
	// Headroom for replicated chunks plus handles in deferred retirement.
	constexpr size_t remoteCapacity = 2 * net::wire::kMaxReplicatedChunks;
	if (!chunkPool)
		chunkPool = std::make_unique<ChunkPool>(remoteCapacity);
	else
		chunkPool->ensureCapacity(remoteCapacity, remoteCapacity);
	if (sim)
	{
		retireWorldProjection();
		sim->suspendPlayer();
		m_suspendedSolo.emplace(SuspendedSolo{std::move(sim), camera, paused, shaderParams.dayTime});
		paused = false;
	}
	else if (!m_remote)
		retireWorldProjection();
	try
	{
		// Start the join timeout after the old projection has drained.
		if (m_remote && m_remote->address == remote->address)
			remote->transport.reuseDepartedSocket(m_remote->transport);
		remote->transport.connect(RemoteSession::Clock::now());
	}
	catch (...)
	{
		if (!m_remote && m_suspendedSolo)
			resumeSolo();
		else if (!m_remote && m_useMainMenu)
			showMainMenu();
		throw;
	}
	// Replacing the session closes its trace before opening the next one.
	m_remote = std::move(remote);
	m_remote->startMotionTrace(m_nextMotionTraceFile);
	camera.setMode(CameraMode::PERSPECTIVE);
	camera.setPosition({0.f, 100.f, 0.f});
	camera.setYawPitch(-90.f, 0.f);
	if (!m_remote->namedProfile.empty())
	{
		m_joinPreferences.selectedProfile = m_remote->namedProfile;
		m_joinPreferences.recordRecent(m_remote->address);
		persistJoinPreferences();
	}
}

void Engine::resumeSolo()
{
	retireWorldProjection();
	if (m_suspendedSolo)
	{
		createRenderBridge(m_suspendedSolo->simulation->world(), m_suspendedSolo->simulation->worldGenerationId());
		sim = std::move(m_suspendedSolo->simulation);
		camera = m_suspendedSolo->camera;
		paused = m_suspendedSolo->paused;
		shaderParams.dayTime = m_suspendedSolo->dayTime;
		m_suspendedSolo.reset();
		m_remote.reset();
		if (m_playerUi && gameui::multiplayerScreen(m_playerUi->screens().top()))
			m_playerUi->resetScreens();
		if (showDemoPlayers)
			worldRenderer->overlays().setPlayers(demoPlayers);
		// Paused streaming does not dispatch meshes. Restore the nearby
		// view now so returning to a paused solo world still shows terrain.
		if (paused)
			renderBridge->meshResidentArea(camera.getPosition(), sim::GameSimulation::kBootstrapRadiusChunks,
				vkContext->getAllocator(), *immediate, worldRenderer->arenas());
	}
	else if (m_useMainMenu)
	{
		m_remote.reset();
		showMainMenu();
	}
	else
	{
		m_remote.reset();
		camera.setPosition({0.f, 100.f, 0.f});
		camera.setYawPitch(-90.f, 0.f);
		initializeNoiseGenerator(0);
	}
}
