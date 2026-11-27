#include <Engine/Engine.hpp>
#include <Engine/EngineMultiplayer.hpp>
#include <Engine/GameUiSample.hpp>

#include <algorithm>
#include <charconv>
#include <utility>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using gameui::SessionAction;
using gameui::Screen;

namespace
{
bool safeWorldName(std::string_view name)
{
	return name.size() <= gameui::TextField::kMaxBytes && WorldPersistence::isValidWorldName(name) &&
		name.front() != '.' && name.back() != '.' && name.back() != ' ' &&
		std::ranges::none_of(name, [](unsigned char c) { return c < 32 || c == 127; });
}

bool createWorldName(std::string_view name)
{
	if (!safeWorldName(name) || name.size() > 32)
		return false;
	const auto digit = [](char c) { return c >= '0' && c <= '9'; };
	const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
	if (!std::ranges::all_of(name, [&](char c) { return alpha(c) || digit(c) || c == '_' || c == '-'; }))
		return false;
	std::string upper(name);
	for (char &c : upper)
		if (c >= 'a' && c <= 'z')
			c -= 'a' - 'A';
	return upper != "CON" && upper != "PRN" && upper != "AUX" && upper != "NUL" &&
		!(upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && digit(upper.back()));
}

bool directWorldDirectory(const fs::path &root, const fs::path &path)
{
	if (!fs::is_directory(fs::symlink_status(path)))
		return false;
	const auto resolved = fs::canonical(path);
	return resolved == fs::canonical(root) / path.filename();
}

bool removeEmptyDirectory(const fs::path &path)
{
	// Directory-only and atomic with respect to any entries arriving since
	// reservation. A replaced regular file cannot be removed by this call.
#ifdef _WIN32
	return _wrmdir(path.c_str()) == 0;
#else
	return rmdir(path.c_str()) == 0;
#endif
}

gameui::WorldCatalog listLocalWorlds(const fs::path &root)
{
	gameui::WorldCatalog catalog;
	try
	{
		if (!fs::exists(root))
			return catalog;
		size_t inspected = 0;
		for (const auto &entry : fs::directory_iterator(root))
		{
			if (++inspected > 512 || catalog.names.size() == 64)
			{
				catalog.notice = "Showing up to 64 worlds from the first 512 entries.";
				break;
			}
			const auto name = entry.path().filename().string();
			if (safeWorldName(name) && directWorldDirectory(root, entry.path()) &&
				fs::is_regular_file(fs::symlink_status(entry.path() / "world.meta")))
				catalog.names.push_back(name);
		}
		std::ranges::sort(catalog.names);
	}
	catch (const fs::filesystem_error &)
	{
		catalog.notice = "Could not read the saves folder. Check access and refresh.";
	}
	return catalog;
}
} // namespace

void Engine::showMainMenu()
{
	m_useMainMenu = true;
	m_openWorldName.clear();
	paused = false;
	demoPlayers.clear();
	m_playerUi->resetScreens(Screen::MainMenu);
	syncUiInput();
}

bool Engine::blocksNetworkSessionRequests() const
{
	return m_playerSessionRequest || m_failedPlayerSession || m_deferredQuit || m_soloWork.valid();
}

bool Engine::acceptsPlayerSession(SessionAction action) const
{
	using enum SessionAction;
	switch (action)
	{
	case Retry: return m_failedPlayerSession && m_retryablePlayerFailure;
	case Cancel: return m_failedPlayerSession.has_value();
	case ConfirmFailedQuit: return m_failedPlayerSession && m_hostExitFailed;
	case ForceStop:
		if (m_waitingForHostExit)
		{
			const auto hosted = m_hostServer.snapshot();
			return hosted.state == hosting::State::Stopping && hosted.delayed;
		}
		return false;
	default: break;
	}
	if (m_playerSessionRequest || m_soloWork.valid() || m_failedPlayerSession ||
		m_joinRequest || switchingSession() || m_exitAfterHostedStop)
		return false;
	switch (action)
	{
	case Create: case Open: return !sim && !m_remote && !m_suspendedSolo && !renderBridge;
	case Save: return sim && sim->isWorldOpen() && !m_remote;
	case Leave: return sim || m_remote;
	case Quit: return true;
	default: return false;
	}
}

void Engine::startDeferredQuit()
{
	if (!m_deferredQuit || !acceptsPlayerSession(SessionAction::Quit))
		return;
	m_deferredQuit = false;
	queuePlayerSession({SessionAction::Quit, {}, {}});
}

void Engine::queuePlayerSession(gameui::SessionRequest request)
{
	if (!acceptsPlayerSession(request.action))
		return;
	using enum SessionAction;
	const auto action = request.action;
	const char *progress = "Saving world...";
	switch (action)
	{
	case Create: case Open: progress = "Loading local world..."; break;
	case Leave: progress = m_remote ? "Leaving server..." : "Saving and leaving world..."; break;
	case Quit: progress = "Preparing to exit..."; break;
	case Save: break;
	default: return;
	}
	cancelDeveloperInput();
	if (developerUi)
		developerUi->cancelDeveloperDialogs();
	setInventoryOpen(false);
	if (action == Quit || action == Leave)
		m_hostAutoProfile.reset();
	m_playerSessionRequest = std::move(request);
	m_sessionProgressPresented = false;
	m_playerUi->openScreen(Screen::Loading);
	m_playerUi->setSessionStatus(progress);
	syncUiInput();
}

void Engine::failPlayerSession(std::string message, bool retryable)
{
	m_failedPlayerSession = std::exchange(m_playerSessionRequest, std::nullopt);
	m_retryablePlayerFailure = retryable && !m_hostExitFailed;
	m_playerUi->finishSessionScreen();
	m_playerUi->openScreen(Screen::SessionError);
	m_playerUi->setSessionStatus(std::move(message), m_retryablePlayerFailure, false, m_hostExitFailed);
	syncUiInput();
}

void Engine::completePlayerSession()
{
	const auto action = m_playerSessionRequest->action;
	m_playerSessionRequest.reset();
	m_playerUi->finishSessionScreen();
	switch (action)
	{
	case SessionAction::Create: case SessionAction::Open:
		m_playerUi->resetScreens();
		break;
	case SessionAction::Leave:
		showMainMenu();
		break;
	case SessionAction::Quit:
		running = false;
		break;
	default: break;
	}
	resetFrameClock();
	syncUiInput();
}

void Engine::pumpPlayerSession()
{
	if (!m_playerUi || m_uiSpike)
		return;
	using enum SessionAction;
	using namespace std::chrono_literals;
	m_playerUi->setSessionContext(bool(m_remote), sim && sim->isWorldOpen(), m_hostSnapshot.active());
	if (m_worldCatalog.valid() && m_worldCatalog.wait_for(0s) == std::future_status::ready)
		m_playerUi->setWorldCatalog(m_worldCatalog.get());

	if (auto request = m_playerUi->takeSessionRequest())
	{
		switch (request->action)
		{
		case List:
			if (!m_worldCatalog.valid())
			{
				m_playerUi->setWorldCatalog({{}, "Reading saved worlds..."});
				m_worldCatalog = std::async(std::launch::async, listLocalWorlds, m_savesRoot);
			}
			break;
		case Retry: case Cancel: case ConfirmFailedQuit:
		{
			if (!acceptsPlayerSession(request->action))
				break;
			auto failed = std::exchange(m_failedPlayerSession, std::nullopt);
			const bool hostedFailed = std::exchange(m_hostExitFailed, false);
			m_retryablePlayerFailure = false;
			m_playerUi->finishSessionScreen();
			if (request->action == Retry)
				queuePlayerSession(std::move(*failed));
			else
			{
				// A close recorded during a failed operation survives Retry.
				// Cancel drops it so returning to the game cannot close it.
				m_deferredQuit = false;
				if (request->action == ConfirmFailedQuit)
					running = false;
				else if (!sim && !m_remote && hostedFailed)
					showMainMenu();
			}
			break;
		}
		case ForceStop:
			if (acceptsPlayerSession(ForceStop))
				m_hostServer.forceStop();
			break;
		case Quit: requestExit(); break;
		default: queuePlayerSession(std::move(*request)); break;
		}
	}
	if (running)
		startDeferredQuit();
	if (!m_playerSessionRequest)
		return;
	const auto &request = *m_playerSessionRequest;
	const auto action = request.action;
	if (m_waitingForHostExit)
	{
		if (m_hostSnapshot.active())
		{
			const bool forceAvailable = m_hostSnapshot.state == hosting::State::Stopping && m_hostSnapshot.delayed;
			m_playerUi->setSessionStatus(forceAvailable ? m_hostSnapshot.message :
				"Waiting for the hosted server checkpoint...", false, forceAvailable);
			return;
		}
		m_waitingForHostExit = false;
		if (m_hostSnapshot.state == hosting::State::Stopped)
			completePlayerSession();
		else
		{
			m_hostExitFailed = true;
			failPlayerSession(m_hostSnapshot.message);
		}
		return;
	}

	if (m_soloWork.valid())
	{
		if (m_soloWork.wait_for(0s) != std::future_status::ready)
			return;
		auto result = m_soloWork.get();
		// The request owns the worker until completion. Publish only into the
		// empty frame-owner slot, after the previous projection was retired.
		if (sim || m_remote || m_suspendedSolo || renderBridge)
		{
			failPlayerSession("The session changed before the world operation finished.", false);
			return;
		}
		sim = std::move(result.simulation);
		if (sim)
		{
			if (action == Open || action == Create)
			{
				camera.setMode(CameraMode::PERSPECTIVE);
				camera.setYawPitch(-90.f, 0.f);
				camera.setMovementSpeed(20.f);
				finishSoloBootstrap(result.center, true);
			}
			else
			{
				renderBridge = std::make_unique<RenderWorldBridge>(sim->world(), threadPool.get(),
					chunkPool.get(), sim->worldGenerationId());
				renderBridge->meshResidentArea(camera.getPosition(), sim::GameSimulation::kBootstrapRadiusChunks,
					vkContext->getAllocator(), *immediate, worldRenderer->arenas());
			}
		}
		resetFrameClock();
		if (!result.error.empty())
		{
			failPlayerSession(std::move(result.error), result.retryable);
			return;
		}
		if (action != Quit)
		{
			completePlayerSession();
			return;
		}
	}
	else
	{
		// Give the loading screen a presentation before retiring the GPU
		// projection. Minimized windows have no swapchain frame to wait for.
		if (!m_sessionProgressPresented && !m_windowMinimized && !m_windowHidden &&
			!(SDL_GetWindowFlags(window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)))
			return;
		if (action == Open || action == Create)
		{
			if (sim || m_remote || m_suspendedSolo || renderBridge)
			{
				failPlayerSession("Leave the current session before opening a local world.", false);
				return;
			}
			int seed = 0;
			if (!request.seed.empty())
			{
				const auto *end = request.seed.data() + request.seed.size();
				const auto parsed = std::from_chars(request.seed.data(), end, seed);
				if (parsed.ec != std::errc{} || parsed.ptr != end || seed <= 0)
				{
					failPlayerSession("Enter a whole seed from 1 to 2147483647, or leave it blank.", false);
					return;
				}
			}
			if (!(action == Create ? createWorldName(request.name) : safeWorldName(request.name)))
			{
				failPlayerSession("Choose a valid world name.", false);
				return;
			}
			try
			{
				m_soloWork = std::async(std::launch::async, prepareLocalWorld, m_savesRoot, request.name,
					seed, action, threadPool.get(), m_beforeLocalWorldInitializeForTests);
			}
			catch (const std::system_error &error)
			{
				failPlayerSession(error.what());
			}
			return;
		}
		if (m_remote)
		{
			m_leavingToMenu = true;
			if (!m_remote->canReconnect())
				return;
			retireWorldProjection();
			m_remote.reset();
			m_leavingToMenu = false;
			if (m_suspendedSolo)
			{
				sim = std::move(m_suspendedSolo->simulation);
				camera = m_suspendedSolo->camera;
				shaderParams.dayTime = m_suspendedSolo->dayTime;
				m_suspendedSolo.reset();
			}
		}
		if (sim)
		{
			retireWorldProjection();
			m_soloWork = std::async(std::launch::async,
				[world = std::move(sim), yaw = camera.getYaw(), pitch = camera.getPitch(), action]() mutable -> SoloWorkResult {
				try
				{
					if (world->isWorldOpen() && (!world->savePlayerState(yaw, pitch) ||
						(action != Save && !world->world()->closeWorld())))
					{
						auto message = world->world()->worldPersistence()->status().lastError;
						if (message.empty())
							message = "The world could not finish saving. Try again or return to the game.";
						return {std::move(world), {}, std::move(message)};
					}
					return {action == Save ? std::move(world) : nullptr, {}, {}};
				}
				catch (const std::exception &error)
				{
					return {std::move(world), {}, error.what()};
				}
			});
			return;
		}
	}
	if (action == Quit && m_hostServer.snapshot().active())
	{
		m_waitingForHostExit = true;
		m_hostAutoProfile.reset();
		m_hostServer.stop();
	}
	else
		completePlayerSession();
}

Engine::SoloWorkResult Engine::prepareLocalWorld(const fs::path &root, const std::string &name, int seed,
	SessionAction action, ThreadPool *pool, const std::function<void(const fs::path &)> &beforeInitialize)
{
	fs::path resolvedRoot, path;
	bool ownsReservation = false;
	const auto failed = [&](std::string message) -> SoloWorkResult {
		if (!ownsReservation)
			return {{}, {}, std::move(message)};
		// A competing creation can only reserve the name after this directory
		// is removed. Check its location and remove only an empty directory.
		// No metadata, chunks, or unknown entries are ever removed.
		try
		{
			if (!fs::exists(fs::symlink_status(path)) ||
				(directWorldDirectory(resolvedRoot, path) && removeEmptyDirectory(path)))
				return {{}, {}, std::move(message)};
		}
		catch (const fs::filesystem_error &)
		{
			// Report the preserved path when rollback cannot be verified.
		}
		return {{}, {}, "Could not create '" + name + "'. Its folder was kept. "
			"Refresh Local worlds to open it if listed, or choose a different name. " + message, false};
	};
	try
	{
		if (action == SessionAction::Create)
			fs::create_directories(root);
		resolvedRoot = fs::canonical(root);
		path = resolvedRoot / name;
		if (action == SessionAction::Create)
		{
			ownsReservation = fs::create_directory(path);
			if (!ownsReservation)
				return {{}, {}, "That world name is already in use. Choose another name.", false};
		}
		else if (!directWorldDirectory(resolvedRoot, path) ||
			!fs::is_regular_file(fs::symlink_status(path / "world.meta")))
			return {{}, {}, "The selected world is missing or its path is unsafe. Refresh the list.", false};

		if (beforeInitialize)
			beforeInitialize(path);
		auto world = std::make_unique<sim::GameSimulation>(pool);
		sim::GameSimulation::Config config{seed, name, resolvedRoot};
		config.existingWorldOnly = action == SessionAction::Open;
		const auto center = world->initialize(config, {0., 100., 0.});
		if (!world->isWorldOpen())
			return failed(world->openWorldError());
		world->bootstrapTerrain(center);
		return {std::move(world), center, {}};
	}
	catch (const std::exception &error)
	{
		return failed(error.what());
	}
}
