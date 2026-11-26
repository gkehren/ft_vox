#include <Engine/Engine.hpp>
#include <Engine/EngineMultiplayer.hpp>
#include <Engine/GameUiSample.hpp>

#include <algorithm>
#include <charconv>
#include <utility>

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

void Engine::queuePlayerSession(gameui::SessionRequest request)
{
	if (m_playerSessionRequest || switchingSession() || m_playerUi->screens().top() == Screen::SessionError)
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
	if (gameUi)
		gameUi->cancelDeveloperDialogs();
	setInventoryOpen(false);
	m_joinRequest.reset();
	m_hostAutoProfile.reset();
	m_playerSessionRequest = std::move(request);
	m_sessionProgressPresented = false;
	m_playerUi->openScreen(Screen::Loading);
	m_playerUi->setSessionStatus(progress);
	syncUiInput();
}

void Engine::failPlayerSession(std::string message)
{
	m_failedPlayerSession = std::exchange(m_playerSessionRequest, std::nullopt);
	m_playerUi->finishSessionScreen();
	m_playerUi->openScreen(Screen::SessionError);
	m_playerUi->setSessionStatus(std::move(message), !m_hostExitFailed, false, m_hostExitFailed);
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
		case Multiplayer: case Settings:
			m_playerUi->openScreen(request->action == Multiplayer ? Screen::Multiplayer : Screen::Settings);
			setDeveloperWorkspaceVisible(true);
			if (request->action == Multiplayer)
				gameUi->showHostPanel();
			else
				gameUi->showGraphicsPanel();
			break;
		case Retry: case Cancel: case ConfirmFailedQuit:
			if (!m_failedPlayerSession)
				break;
			m_playerUi->finishSessionScreen();
			if (request->action == Retry)
				queuePlayerSession(std::move(*m_failedPlayerSession));
			else if (request->action == ConfirmFailedQuit && m_hostExitFailed)
				running = false;
			else if (!sim && !m_remote && m_hostExitFailed)
				showMainMenu();
			m_failedPlayerSession.reset();
			m_hostExitFailed = false;
			break;
		case ForceStop:
			if (m_waitingForHostExit)
				m_hostServer.forceStop();
			break;
		default: queuePlayerSession(std::move(*request)); break;
		}
	}
	if (!m_playerSessionRequest)
		return;
	const auto &request = *m_playerSessionRequest;
	const auto action = request.action;
	if (m_waitingForHostExit)
	{
		if (m_hostSnapshot.active())
		{
			m_playerUi->setSessionStatus(m_hostSnapshot.delayed ? m_hostSnapshot.message :
				"Waiting for the hosted server checkpoint...", false, m_hostSnapshot.delayed);
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
			failPlayerSession(std::move(result.error));
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
		if (!m_sessionProgressPresented &&
			!(SDL_GetWindowFlags(window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)))
			return;
		if (action == Open || action == Create)
		{
			int seed = 0;
			if (!request.seed.empty())
			{
				const auto *end = request.seed.data() + request.seed.size();
				const auto parsed = std::from_chars(request.seed.data(), end, seed);
				if (parsed.ec != std::errc{} || parsed.ptr != end || seed <= 0)
				{
					failPlayerSession("Enter a whole seed from 1 to 2147483647, or leave it blank.");
					return;
				}
			}
			if (!(action == Create ? createWorldName(request.name) : safeWorldName(request.name)))
			{
				failPlayerSession("Choose a valid world name.");
				return;
			}
			m_soloWork = std::async(std::launch::async,
				[root = m_savesRoot, name = request.name, seed, action, pool = threadPool.get()]() -> SoloWorkResult {
				try
				{
					const auto path = root / name;
					if (action == Create)
					{
						fs::create_directories(root);
						if (!fs::create_directory(path))
							return {{}, {}, "That world name is already in use. Choose another name."};
					}
					else if (!directWorldDirectory(root, path) ||
						!fs::is_regular_file(fs::symlink_status(path / "world.meta")))
						return {{}, {}, "The selected world is missing or its path is unsafe. Refresh the list."};
					auto world = std::make_unique<sim::GameSimulation>(pool);
					sim::GameSimulation::Config config{seed, name, root};
					config.existingWorldOnly = action == Open;
					const auto center = world->initialize(config, {0., 100., 0.});
					if (!world->isWorldOpen())
						return {{}, {}, world->openWorldError()};
					world->bootstrapTerrain(center);
					return {std::move(world), center, {}};
				}
				catch (const std::exception &error)
				{
					return {{}, {}, error.what()};
				}
			});
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
