#include "Engine/Engine.hpp"
#include "Engine/FramePacing.hpp"
#include "Engine/EngineMultiplayer.hpp"

#include <SDL3/SDL_vulkan.h>
#include <Vulkan/VkLoadLibrary.hpp>
#include <Renderer/MinecraftTextures.hpp>
#include <utils.hpp>
#include <Chunk/StreamHelpers.hpp>
#include <Chunk/ChunkBorders.hpp>
#include <Chunk/ChunkMeshResult.hpp>
#include <World/WorldCollisionView.hpp>
#include <Items/ItemRegistry.hpp>
#include <Engine/Profiler.hpp>
#include <Engine/Benchmark.hpp>
#include <Engine/InputRouting.hpp>
#include <Engine/GameUiSample.hpp>
#include <World/WorldPersistence.hpp>
#include <imgui/imgui.h>

#include <cstdio>
#include <cmath>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <random>
#include <algorithm>

#define FULLSCREEN 1

namespace
{
/// Sky fallback clear (issue #209): shared by every recorded frame.
constexpr VkClearColorValue kClearColor = {{0.38f, 0.58f, 0.92f, 1.0f}};

int budgetFromRate(int perSec, double dt, double &accum)
{
	accum += static_cast<double>(perSec) * dt;
	int n = static_cast<int>(accum);
	if (n > 0)
		accum -= static_cast<double>(n);
	return std::clamp(n, 0, 64);
}

/// Report name for a graphics quality preset (benchmark tracing, issue #138).
const char *graphicsQualityPresetName(GraphicsQualityPreset preset)
{
	switch (preset)
	{
	case GraphicsQualityPreset::Low: return "low";
	case GraphicsQualityPreset::Medium: return "medium";
	case GraphicsQualityPreset::High: return "high";
	case GraphicsQualityPreset::Cinematic: return "cinematic";
	}
	return "medium";
}
} // namespace

Engine::Engine(std::string resourcePackRoot)
	: camera(glm::vec3(0.0f, 90.0f, 0.0f))
	, m_resourcePackRoot(std::move(resourcePackRoot))
{
	if (!SDL_Init(SDL_INIT_VIDEO))
		throw std::runtime_error(std::string("Failed to initialize SDL: ") + SDL_GetError());
	// Developer capture is explicit. SDL auto-capture would otherwise
	// retain the window after the router quarantines a physical button.
	SDL_SetHintWithPriority(SDL_HINT_MOUSE_AUTO_CAPTURE, "0", SDL_HINT_OVERRIDE);

	// Shared loader probe (VkLoadLibrary.hpp) — same path used by test_vulkan_resources.
	if (!loadVulkanLibrary(&std::cout))
	{
		throw std::runtime_error(std::string("Failed to load Vulkan library: ") + SDL_GetError() + "\n" +
								 vulkanLibraryLoadHint());
	}

	windowWidth = 1920;
	windowHeight = 1080;

	Uint32 windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE |
						 SDL_WINDOW_HIGH_PIXEL_DENSITY;
	if (FULLSCREEN == 0)
	{
		const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
		if (mode)
		{
			windowWidth = mode->w;
			windowHeight = mode->h;
			windowFlags |= SDL_WINDOW_FULLSCREEN;
		}
	}
	else if (FULLSCREEN == 2)
	{
		windowFlags |= SDL_WINDOW_BORDERLESS;
	}

	window = SDL_CreateWindow("ft_vox (Vulkan)", windowWidth, windowHeight, windowFlags);
	if (!window)
		throw std::runtime_error(std::string("Failed to create SDL window: ") + SDL_GetError());

	SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

	// The swapchain must match the framebuffer, not the logical window size:
	// with SDL_WINDOW_HIGH_PIXEL_DENSITY the two differ on high-DPI displays
	// (macOS Retina, fractional Wayland). The run loop re-queries pixels each
	// frame for resize handling; this seeds the initial swapchain (issue #183).
	int pixelW = 0, pixelH = 0;
	SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);
	if (pixelW <= 0 || pixelH <= 0)
	{
		pixelW = windowWidth;
		pixelH = windowHeight;
	}
	// Single size semantics: windowWidth/Height track the framebuffer/swapchain
	// pixel extent (projection aspect, frustum visibility, the Overview
	// viewport readout all want device pixels). Logical SDL sizes are not
	// needed anywhere else.
	windowWidth = pixelW;
	windowHeight = pixelH;

	m_perfFrequency = SDL_GetPerformanceFrequency();
	updateDisplayRefreshRate();

	vkContext = std::make_unique<VkContext>();
	vkContext->init(window);

	swapchain = std::make_unique<VkSwapchain>();
	swapchain->init(*vkContext, static_cast<uint32_t>(pixelW), static_cast<uint32_t>(pixelH),
					renderSettings.vsyncEnabled);

	immediate = std::make_unique<ImmediateCommands>();
	immediate->init(*vkContext);

	stagingRing.init(vkContext->getAllocator(), VkFrameContext::kMaxFramesInFlight);
	resourceRetire.init(vkContext->getAllocator(), VkFrameContext::kMaxFramesInFlight);

	frameCtx = std::make_unique<VkFrameContext>();
	frameCtx->init(*vkContext);

	worldRenderer = std::make_unique<WorldRenderer>();
	worldRenderer->init(*vkContext, *swapchain, *immediate, resourceRetire, m_resourcePackRoot);

	imgui = std::make_unique<ImGuiLayer>();
	imgui->init(window, *vkContext, *swapchain);

	gameUi = std::make_unique<GameUI>();
	gameUi->init(*vkContext, *immediate);

	const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
	threadPool = std::make_unique<ThreadPool>(hw);

	mobStates.reserve(entities::MobSettings::capacity);

	renderSettings.minRenderDistance = 192;
	renderSettings.maxRenderDistance = 512;
	renderSettings.streamFrontBias = 0.30f; // may be overridden by setStreamFrontBias before run()

	renderSettings.loadPerSec = 640;
	renderSettings.genPerSec = 480;
	renderSettings.meshPerSec = 360;
	renderSettings.uploadPerSec = 520;
	renderSettings.lightCachePerSec = 96;
	renderSettings.shadowDistance = 160.f;
	renderSettings.raycastDistance = 8;

	camera.setWindow(window);
	camera.setMovementSpeed(20.f);
	setMouseCaptured(true);

	updateAutomaticAtmosphere(shaderParams, renderSettings.maxRenderDistance);

	SDL_ShowWindow(window);
	const auto initialWindowState = SDL_GetWindowFlags(window);
	windowFocused = (initialWindowState & SDL_WINDOW_INPUT_FOCUS) != 0;
	m_windowHidden = (initialWindowState & SDL_WINDOW_HIDDEN) != 0;
	m_windowMinimized = (initialWindowState & SDL_WINDOW_MINIMIZED) != 0;

	std::cout << "SDL version: " << SDL_GetVersion() << "\n";
	std::cout << "ft_vox: Vulkan — streaming procedural world\n";
	std::cout << "  WASD move · Space jump/swim up · Shift sprint/swim down · V flight\n";
	std::cout << "  Mouse look · LMB/RMB break/place · 1-9 hotbar · Q drop · B borders\n";
	std::cout << "  C free mouse · E inventory · F5 controls · F6 HUD hints · F10 VSync\n";
	std::cout << "  ThreadPool workers: " << hw << "\n";
}

Engine::~Engine()
{
	m_hostServer.stop();
	if (m_remote)
		m_remote->disconnect();
	if (vkContext)
		vkContext->waitIdle();
	if (m_playerUi)
		m_playerUi->updateTextInput(window, false);
	m_playerUi.reset();

	// Durable shutdown (issue #180, Phase 5): persist the player state, then
	// flush the save queue while the world is still alive. The simulation
	// captures the authoritative state; the host supplies the camera
	// orientation (presentation). ~VoxelWorld holds a safety net for chunk
	// edits, but only this path writes player.state, so the explicit flush
	// stays.
	// Partial-init guard (review M8): sim only exists once init() survived
	// device creation; a constructor that threw earlier must not crash the
	// destructor here.
	if (sim && sim->isWorldOpen())
		sim->savePlayerState(camera.getYaw(), camera.getPitch());

	// Reset order matters (issue #109): chunk/world teardown returns arena
	// ranges, which can retire whole arena pages into the retire queue.
	// The queue must outlive them and be flushed last. The render bridge
	// quiesces its derived jobs and releases its handles first; it never closes
	// the world's persistence (issue #207 review) — the host closes the
	// authoritative simulation exactly once, here, before it tears down.
	gameUi.reset();
	renderBridge.reset();
	m_remote.reset();
	if (sim)
	{
		sim->shutdown();
		sim.reset();
	}
	if (m_suspendedSolo)
	{
		m_suspendedSolo->simulation->shutdown();
		m_suspendedSolo.reset();
	}
	chunkPool.reset();
	threadPool.reset();

	imgui.reset();
	worldRenderer.reset();

	resourceRetire.flush();
	resourceRetire.shutdown();
	stagingRing.shutdown();
	frameCtx.reset();
	immediate.reset();
	swapchain.reset();
	vkContext.reset();

	if (window)
	{
		SDL_DestroyWindow(window);
		window = nullptr;
	}

	SDL_Vulkan_UnloadLibrary();
	SDL_Quit();
}

void Engine::resetWorldProjection()
{
	// World-generation epoch barrier (issue #209, Epic #204 Phase E). Runs
	// on every authoritative world transition BEFORE the old bridge is
	// detached: the derived lists below reference its handles, and every
	// per-world CPU snapshot must be dropped so nothing stale can feed the
	// replacement projection. The simulation bumps worldGenerationId()
	// across the transition; the new bridge is stamped with that epoch at
	// attach, and the UI biome cache revalidates against it. Async jobs are
	// quiesced by the bridge destructor against the still-live old world —
	// this barrier owns the CPU-side derived state only.
	drawList.clear();
	shadowList.clear();
	mobStates.clear();
	m_itemSnapshots.clear();
	m_itemStates.clear();
	m_itemEventsScratch.clear();
	clearGameplayInput();
	m_inputRouter.cancelPlayerUi();
	if (m_playerUi)
	{
		m_playerUi->resetScreens();
		m_inventoryPanelOpen = false;
	}
	if (worldRenderer)
	{
		// Drop the renderer's mob presentation too: setMobs is otherwise
		// only refreshed by the next Mobs frame step.
		worldRenderer->setMobs(mobStates);
		// Same discipline for dropped items (issue #229): no stale entity
		// presentation survives a world transition.
		worldRenderer->setItemEntities(m_itemStates);
		worldRenderer->overlays().setHighlight(OverlayHighlight{});
		if (m_remote)
			worldRenderer->overlays().setPlayers({});
	}
	highlight = OverlayHighlight{};
	uploadBudgetThisFrame = 0;
	// UI biome cache (issue #184): keyed on the world generation as well,
	// but reset explicitly so a refused/aborted transition cannot keep a
	// stale column sample either.
	m_uiBiomeColumn = {std::numeric_limits<int>::min(), std::numeric_limits<int>::min()};
	m_uiBiomeWorldGen = 0;
	m_uiBiome = -1;
	if (gameUi)
	{
		gameUi->invalidateBiomeMap();
		gameUi->resetInventoryInteraction();
	}
	if (m_remote)
	{
		m_inventoryPanelOpen = false;
		jumpPressed = false;
	}
}

void Engine::stepDerivedProjectionForTests(double dt)
{
	// Real production frame steps: the streaming step collects the draw/shadow
	// lists from the bootstrapped (meshed + GPU-uploaded) chunks, and the mob
	// step advances the authoritative simulation and fills mobStates.
	tickStreaming(dt);
	tickMobs(dt);
	tickItems(dt);
	// Establish a non-empty mob presentation precondition (see header): the
	// bootstrap radius is smaller than the mob spawn ring, so the real step
	// above usually produces no mobs here. The seeded state is cleared by the
	// epoch barrier on the next world transition, exactly like a real snapshot.
	if (mobStates.empty())
	{
		// Sentinel parked far below the world (playable Y is [0,256]) so it
		// cannot appear as a phantom mob if a future test renders between the
		// seed and the reload; it is not pushed to WorldRenderer here (tickMobs
		// already published the — empty — real snapshot above).
		entities::MobRenderState sentinel;
		sentinel.position = glm::vec3(0.0f, -10000.0f, 0.0f);
		mobStates.push_back(sentinel);
	}
}

void Engine::initializeNoiseGenerator(int seed_val)
{
	if (m_remote)
		throw std::logic_error("solo initialization requires a solo host");
	// Epoch barrier BEFORE detaching the old projection (issue #209): a
	// re-initialize must never leave stale derived state — or a bridge
	// referencing a destroyed world — behind. The bridge destructor quiesces
	// against the still-live old world.
	resetWorldProjection();
	renderBridge.reset();
	if (!chunkPool)
		chunkPool = std::make_unique<ChunkPool>(estimateChunkPoolCapacity(renderSettings.maxRenderDistance));
	if (!sim)
		sim = std::make_unique<sim::GameSimulation>(threadPool.get());

	// The simulation owns the authoritative transition (issue #208): seed
	// resolution (stored identity wins), world creation, persistence open and
	// the saved-player read. The returned center is the SAVED position when
	// one exists, so the bootstrap generates around it (issue #180 round 6).
	sim::GameSimulation::Config config;
	config.seed = seed_val;
	config.worldName = m_openWorldName;
	config.savesRoot = m_savesRoot;
	const glm::dvec3 bootstrapCenter = sim->initialize(config, glm::dvec3(camera.getPosition()));

	camera.setPosition(glm::vec3(bootstrapCenter));
	renderBridge = std::make_unique<RenderWorldBridge>(sim->world(), threadPool.get(),
												  chunkPool.get(), sim->worldGenerationId());

	// Synchronous terrain bootstrap: authoritative voxels + render binding,
	// meshing and upload in one step (the bridge drains the world's Loaded
	// events itself). The headless path is sim->bootstrapTerrain().
	renderBridge->generateInitialArea(glm::vec3(bootstrapCenter),
									  sim::GameSimulation::kBootstrapRadiusChunks,
									  vkContext->getAllocator(), *immediate, worldRenderer->arenas());

	// Restore the validated player state, or fall back to the surface spawn
	// around the camera position (the historical scan anchor). Camera
	// orientation is presentation: apply the saved yaw/pitch here.
	const sim::GameSimulation::SpawnOutcome spawn =
		sim->finalizePlayerSpawn(glm::dvec3(camera.getPosition()));
	camera.setPosition(glm::vec3(spawn.eye));
	if (spawn.restored)
		camera.setYawPitch(spawn.yaw, spawn.pitch);

	demoPlayers = {
		{{4.f, 80.f, 4.f}, 1},
		{{-6.f, 78.f, 8.f}, 2},
		{{10.f, 82.f, -3.f}, 3},
	};
	for (auto &p : demoPlayers)
	{
		for (int y = CHUNK_HEIGHT - 1; y > 0; --y)
		{
			if (sim->isVoxelActive(glm::vec3(p.position.x, static_cast<float>(y), p.position.z)))
			{
				p.position.y = static_cast<float>(y + 1);
				break;
			}
		}
	}
	if (showDemoPlayers && worldRenderer)
		worldRenderer->overlays().setPlayers(demoPlayers);

	std::cout << "World seed: " << sim->seed()
			  << " chunks=" << renderBridge->chunkCount()
			  << " view=" << renderSettings.maxRenderDistance << "\n";
}

void Engine::setVSync(bool enabled)
{
	renderSettings.vsyncEnabled = enabled;
	if (!swapchain || (swapchain->isVSync() == enabled && !m_pendingVSync))
		return;

	// UI is built after beginFrame() acquired an image. Defer the mode change so
	// the old swapchain remains alive through record/submit/present.
	m_pendingVSync = enabled;
	requestSwapchainRecreate();
}

void Engine::setGraphicsQualityPreset(GraphicsQualityPreset preset)
{
	// Apply immediately when the renderer exists (GUI/tool callers); CLI
	// parsing runs before init, so park it and apply at the top of run().
	if (worldRenderer)
		worldRenderer->postSettings().applyPreset(preset);
	else
		m_pendingQualityPreset = preset;
	// Tag benchmark reports with the quality tier (issue #138): the parked
	// path writes the label here too, so both apply paths stay consistent.
	// A benchmark in flight keeps the tier its measured frames rendered with.
	if (!m_benchmark.isActive())
		m_benchmark.config().qualityLabel = graphicsQualityPresetName(preset);
}

Engine::ResourcePackApplyResult Engine::applyResourcePack(const std::string &resourcePackRoot)
{
	ResourcePackApplyResult out{};
	if (!vkContext || !worldRenderer || !immediate)
	{
		out.message = "Renderer not ready.";
		out.isError = true;
		return out;
	}

	const std::string path = trimTrailingSlashes(resourcePackRoot);
	try
	{
		vkContext->waitIdle();
		const TextureAtlasLoadReport report = worldRenderer->reloadResourcePack(path);
		++m_atlasVersion; // UI icon descriptors revalidate against the new atlas
		// Eagerly drop icon views of the destroyed atlas while the device is
		// idle (issue #229 review): no orphaned view lingers until next draw.
		if (gameUi)
			gameUi->invalidateItemIcons();
		out.atlasOk = true;
		out.packHits = report.packHits;
		out.packMisses = report.packMisses;
        const auto entityReport = worldRenderer->mobTextureReport();
        out.entityHits = entityReport.hits; out.entityMisses = entityReport.misses;

		if (path.empty())
		{
			m_resourcePackRoot.clear();
			out.message = "Loaded default resource pack (default-resource-pack.zip).";
			return out;
		}

		if (report.packInvalid() && out.entityHits == 0)
		{
			// Fully invalid: use default assets and clear active pack so UI matches reality.
			m_resourcePackRoot.clear();
			out.isError = true;
			out.message =
				"Invalid resource pack: no block or mob textures found. "
				"Using default resource pack.";
			return out;
		}

		m_resourcePackRoot = path;
		if (report.packMisses > 0 || out.entityMisses > 0)
		{
			out.isWarning = true;
			out.message = "Resource pack incomplete: " + std::to_string(report.packMisses) + " of " +
						  std::to_string(report.requiredLayers) +
						  " block textures missing; mobs: " + std::to_string(out.entityHits) + " loaded, " +
                    std::to_string(out.entityMisses) + " missing. Default pack used for missing textures.";
			return out;
		}

		out.message = "Loaded resource pack.";
		return out;
	}
	catch (const std::exception &e)
	{
		std::cerr << "applyResourcePack failed: " << e.what() << "\n";
		out.isError = true;
		out.message = std::string("Failed to load resource pack: ") + e.what() +
					  " Previous atlas kept.";
		return out;
	}
}

void Engine::setInspectionView(glm::vec3 position, float yaw, float pitch, float seconds)
{
    m_inspectionView = true;
    m_inspectionSeconds = seconds;
    camera.setPosition(position);
    camera.setYawPitch(yaw, pitch);
    sim->setPlayerFlight(true, glm::dvec3(camera.getPosition()) -
                                   glm::dvec3(0, sim->playerSettings().eyeHeight, 0));
    jumpPressed = false;
    setMouseCaptured(false);
    shaderParams.dayCycleEnabled = false;
    shaderParams.dayTime = 0.35f;
    showDemoPlayers = false;
    worldRenderer->overlays().setPlayers({});
    std::cout << "Inspection camera: " << position.x << ' ' << position.y << ' ' << position.z
              << ' ' << yaw << ' ' << pitch << '\n';
}

void Engine::placeCameraOnSurface()
{
	const glm::vec3 pos = camera.getPosition();
	if (!renderBridge) return;
	// Keep the established benchmark's center/altitude byte-for-byte: changing
	// gameplay spawn must not silently change the renderer comparison workload.
	if (m_benchmark.isActive())
	{
		for (int y = CHUNK_HEIGHT - 1; y > 0; --y)
			if (sim->isVoxelActive({pos.x, float(y), pos.z}))
			{
				camera.setPosition({pos.x, float(y + 3), pos.z});
				resetPlayerAtCamera();
				return;
			}
		camera.setPosition({pos.x, 100.f, pos.z});
		resetPlayerAtCamera();
		return;
	}
	// The simulation owns the spawn scan + player reset (issue #208); the
	// camera follows the resulting eye.
	camera.setPosition(glm::vec3(sim->spawnPlayerOnSurface(glm::dvec3(pos))));
}

void Engine::resetPlayerAtCamera()
{
	sim->setPlayerPosition(glm::dvec3(camera.getPosition()) -
						   glm::dvec3(0, sim->playerSettings().eyeHeight, 0));
	jumpPressed = false;
}

void Engine::setPlayerFlight(bool enabled)
{
	if (!sim || m_benchmark.isActive()) return;
	const glm::dvec3 feet = glm::dvec3(camera.getPosition()) -
							glm::dvec3(0, sim->playerSettings().eyeHeight, 0);
	// Disabling flight validates clearance at the camera feet and refuses
	// when blocked (the simulation sets the status message).
	if (!sim->setPlayerFlight(enabled, feet))
		return;
	if (!enabled)
		camera.setMode(CameraMode::PERSPECTIVE);
	jumpPressed = false;
}

bool Engine::reloadWorld(int newSeed)
{
	if (!sim || !vkContext || !threadPool || !chunkPool || !immediate)
		return false;

	// Deliberate (issue #180): benchmark / reload worlds are transient —
	// close (and flush) any world opened for interactive play so the run can
	// never write into the user's save. Persistence stays off afterwards.
	// The close can be REFUSED (stranded logical edits - issue #180 review
	// round 3): aborting the reload keeps the bridge/world intact, so a
	// debug/benchmark action can never destroy the world after a refused
	// close. The bridge quiesces its render jobs around the world's
	// durability barrier (render jobs hold record leases).
	if (renderBridge && renderBridge->isWorldOpen())
	{
		if (!renderBridge->closeWorld())
		{
			std::cerr << "[world] reload aborted: persistent world could not be closed safely"
			          << std::endl;
			return false;
		}
	}

	// Epoch barrier (issue #209): drop every derived per-world CPU snapshot
	// BEFORE the old bridge is detached (the lists reference its handles).
	// A refused close above leaves the world AND its projection intact.
	resetWorldProjection();

	vkContext->waitIdle();
	resourceRetire.flush();
	// Carry the opt-in chunk lifecycle trace across the reload (issue #179
	// review round 2): re-apply it to the new bridge BEFORE
	// generateInitialArea() so bootstrap events are not lost — the UI-side
	// sync in updateDebugUiState only runs later in the frame.
	const bool chunkTraceEnabled =
		renderBridge && renderBridge->chunkEventTraceEnabled();
	renderBridge.reset();
	// The simulation owns the authoritative transition (issue #208). The
	// persistent world was already closed through the bridge above (or none
	// was open), so the simulation's world-side close cannot refuse here: in
	// the graphical flow the reload always proceeds. The refusal branch below
	// is a defensive headless-style guard — if it ever fires, the OLD world
	// stays authoritative and the fresh bridge is deliberately re-projected
	// onto it so the aborted reload leaves a renderable session.
	const bool reloaded = sim->reload(newSeed);
	renderBridge = std::make_unique<RenderWorldBridge>(sim->world(), threadPool.get(),
												  chunkPool.get(), sim->worldGenerationId());
	renderBridge->setChunkEventTraceEnabled(chunkTraceEnabled);
	if (!reloaded)
		return false;

	camera.setMode(CameraMode::PERSPECTIVE);
	camera.setPosition(glm::vec3(0.f, 100.f, 0.f));
	camera.setMovementSpeed(20.f);

	renderBridge->generateInitialArea(camera.getPosition(),
									  sim::GameSimulation::kBootstrapRadiusChunks,
									  vkContext->getAllocator(), *immediate, worldRenderer->arenas());
	placeCameraOnSurface();

	demoPlayers = {
		{{4.f, 80.f, 4.f}, 1},
		{{-6.f, 78.f, 8.f}, 2},
		{{10.f, 82.f, -3.f}, 3},
	};
	for (auto &p : demoPlayers)
	{
		for (int y = CHUNK_HEIGHT - 1; y > 0; --y)
		{
			if (sim->isVoxelActive(glm::vec3(p.position.x, static_cast<float>(y), p.position.z)))
			{
				p.position.y = static_cast<float>(y + 1);
				break;
			}
		}
	}
	if (showDemoPlayers && worldRenderer)
		worldRenderer->overlays().setPlayers(demoPlayers);

	GetProfiler().clearHistory();
	GetProfiler().setEnabled(true);

	// Capture-local gauges (chunks.active/deferred, staging.slice.bytes) are
	// zeroed by beginCapture() and re-published by the first measured frame.
	telemetry::registry().beginCapture();

	std::cout << "[bench] World reloaded seed=" << sim->seed()
			  << " chunks=" << renderBridge->chunkCount() << "\n";
	return true;
}

void Engine::onResize(int width, int height)
{
	if (width <= 0 || height <= 0)
		return;
	windowWidth = width;
	windowHeight = height;
	updateDisplayRefreshRate();
	requestSwapchainRecreate();
}

void Engine::requestSwapchainRecreate()
{
	m_swapchainRecreateRequested = true;
}

void Engine::recreateSwapchainIfNeeded(uint32_t width, uint32_t height)
{
	if (!swapchain || width == 0 || height == 0)
		return;

	const bool desiredVSync = m_pendingVSync.value_or(swapchain->isVSync());
	const VkExtent2D extent = swapchain->getExtent();
	if (!m_swapchainRecreateRequested && desiredVSync == swapchain->isVSync() &&
		extent.width == width && extent.height == height)
		return;

	PROFILE_SCOPE("SwapchainRecreate");
	// VkSwapchain::recreate waits for the device before destroying WSI state.
	// Every swapchain consumer is refreshed before the next image acquisition.
	swapchain->recreate(width, height, desiredVSync);
	frameCtx->onSwapchainRecreate(swapchain->getImageCount());
	worldRenderer->onSwapchainRecreate(*swapchain);
	if (m_playerUi)
		m_playerUi->renderer().setOutput(swapchain->getImageFormat(), swapchain->getColorSpace());
	if (imgui && imgui->onSwapchainRecreate(*swapchain) && gameUi)
		gameUi->onImGuiVulkanBackendRecreate();

	windowWidth = static_cast<int>(swapchain->getExtent().width);
	windowHeight = static_cast<int>(swapchain->getExtent().height);
	m_pendingVSync.reset();
	m_swapchainRecreateRequested = false;
}

void Engine::tickDayCycle(double dt)
{
	// Pause gates ONLY time progression. The derived atmosphere parameters
	// must still track dayTime and the current view distance every frame:
	// otherwise a view-distance change (or atmosphere edit) made while
	// paused would leave stale fog values until the world unpauses.
	if (!paused && !nativePause() && shaderParams.dayCycleEnabled)
	{
		shaderParams.dayTime = std::fmod(shaderParams.dayTime + static_cast<float>(dt) * shaderParams.dayCycleSpeed, 1.0f);
		if (shaderParams.dayTime < 0.f)
			shaderParams.dayTime += 1.f;
	}
	updateAutomaticAtmosphere(shaderParams, renderSettings.maxRenderDistance);
}

void Engine::tickStreaming(double dt)
{
	PROFILE_SCOPE("Streaming");
	if (!renderBridge)
		return;
	if (paused || nativePause())
	{
		{
			PROFILE_SCOPE("Visibility");
			renderBridge->updateVisibility(camera, windowWidth, windowHeight, renderSettings);
		}
		{
			PROFILE_SCOPE("CollectLists");
			renderBridge->collectDrawList(drawList);
			renderBridge->collectShadowList(shadowList, camera, renderSettings.shadowDistance);
		}
		uploadBudgetThisFrame = 0;
		return;
	}

	// Grow pool when the view-distance slider (or other settings) outgrows the
	// free list. Incremental (kChunkPoolMaxGrowPerCall per tick, converge over
	// several frames) and issued inside the measured streaming section so the
	// allocation work shares the per-frame streaming budget; pointer-stable so
	// loaded chunks stay valid.
	const double frameDt = std::min(dt, 0.05);
	const auto streamT0 = std::chrono::steady_clock::now();
	const auto streamElapsedMs = [&]() {
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - streamT0).count();
	};
	const double maxStreamMs = static_cast<double>(renderSettings.maxStreamMs);

	// Incremental pool growth (no single-frame hitch when the slider jumps):
	// each tick adds at most kChunkPoolMaxGrowPerCall slots until the estimate
	// for the view distance is reached, inside the measured streaming section
	// so the allocation work shares the per-frame streaming budget;
	// pointer-stable so loaded chunks stay valid. The PoolGrow scope exists
	// only while capacity is actually below target — below it every call is
	// guaranteed to grow — so the benchmark's PoolGrow samples are real
	// growth steps, never no-op scope timings.
	if (!m_remote && chunkPool)
	{
		const size_t poolTarget = estimateChunkPoolCapacity(renderSettings.maxRenderDistance);
		if (chunkPool->capacity() < poolTarget)
		{
			PROFILE_SCOPE("PoolGrow");
			chunkPool->ensureCapacity(poolTarget, kChunkPoolMaxGrowPerCall);
		}
	}

	{
		PROFILE_SCOPE("FinishedJobs");
		renderBridge->processFinishedJobs();
	}
	{
		PROFILE_SCOPE("DeferredRelease");
		renderBridge->processDeferredReleases();
	}
	if (!m_remote)
	{
		PROFILE_SCOPE("UpdateStreaming");
		renderBridge->updateStreaming(camera, renderSettings);
	}
	else
	{
		PROFILE_SCOPE("ReplicaPublication");
		const auto *player = m_remote->localPlayer();
		const auto observer = player ? glm::vec3(net::udp::playerFeet(*player)) : camera.getPosition();
		m_remote->projection->reconcile(m_remote->transport.chunks(), observer);
		renderBridge->publishReplicaChanges();
		updateRemoteCamera();
	}

	const int loadBudget = budgetFromRate(renderSettings.loadPerSec, frameDt, streamAccum);
	const int genBudget = budgetFromRate(renderSettings.genPerSec, frameDt, m_genAccum);
	const int meshBudget = budgetFromRate(renderSettings.meshPerSec, frameDt, m_meshAccum);
	const int lightCacheBudget = budgetFromRate(renderSettings.lightCachePerSec, frameDt, m_lightCacheAccum);
	uploadBudgetThisFrame = std::max(budgetFromRate(renderSettings.uploadPerSec, frameDt, m_uploadAccum), 1);

	// Count budgets capped by shared per-frame CPU time envelope.
	const int loadN = remainingCountBudget(std::max(loadBudget, 1), streamElapsedMs(), maxStreamMs);
	if (!m_remote && loadN > 0)
	{
		PROFILE_SCOPE("Load");
		renderBridge->processChunkLoading(loadN);
	}
	const int genN = remainingCountBudget(std::max(genBudget, 1), streamElapsedMs(), maxStreamMs);
	if (!m_remote && genN > 0)
	{
		PROFILE_SCOPE("GenDispatch");
		renderBridge->generatePendingVoxels(camera, renderSettings, genN);
	}
	int meshN = remainingCountBudget(std::max(meshBudget, 1), streamElapsedMs(), maxStreamMs);
	if (m_remote)
		meshN = std::clamp(16 - static_cast<int>(renderBridge->pendingMeshJobs()), 0, std::min(meshN, 4));
	if (meshN > 0)
	{
		PROFILE_SCOPE("MeshDispatch");
		renderBridge->meshPendingChunks(camera, renderSettings, meshN);
	}
	// Dedicated entity light-cache dispatch (issue #172): separate budget so
	// cache acquisition neither competes with nor degenerates into render
	// mesh rebuilds. Unlike meshN, no forced minimum — lightCachePerSec = 0
	// disables the dedicated path.
	int lightCacheN = remainingCountBudget(lightCacheBudget, streamElapsedMs(), maxStreamMs);
	if (m_remote)
		lightCacheN = std::clamp(8 - static_cast<int>(renderBridge->pendingLightJobs()), 0, lightCacheN);
	if (lightCacheN > 0)
	{
		PROFILE_SCOPE("LightCacheDispatch");
		renderBridge->updateEntityLightCaches(camera, renderSettings, lightCacheN);
	}
	// GPU uploads are recorded inside recordFrame (after acquire) — no waitIdle.
	if (m_remote && !m_remote->hasCameraPose)
	{
		drawList.clear();
		shadowList.clear();
	}
	else
	{
		{
			PROFILE_SCOPE("Visibility");
			renderBridge->updateVisibility(camera, windowWidth, windowHeight, renderSettings);
		}
		PROFILE_SCOPE("CollectLists");
		renderBridge->collectDrawList(drawList);
		renderBridge->collectShadowList(shadowList, camera, renderSettings.shadowDistance);
	}

	const double now = SDL_GetTicks() / 1000.0;
	const size_t n = renderBridge->chunkCount();
	if (n != m_lastLoggedChunks && now - m_lastStreamLogTime > 1.0)
	{
		std::cout << "[stream] chunks=" << n
				  << " draw=" << drawList.size()
				  << " shadow=" << shadowList.size()
				  << " q="
				  << renderBridge->pendingLoadCount() << "/"
				  << renderBridge->pendingGenJobs() << "/"
				  << renderBridge->pendingMeshJobs() << "/"
				  << renderBridge->pendingLightJobs() << "\n";
		m_lastLoggedChunks = n;
		m_lastStreamLogTime = now;
	}
}

bool Engine::raycastVoxel(glm::vec3 &outBlock, glm::vec3 &outPrevious)
{
	if (m_remote)
	{
		const auto hit = remoteTarget();
		outBlock = glm::vec3(hit.block);
		outPrevious = glm::vec3(hit.adjacent);
		return hit.hit;
	}
	// Thin forward (issue #227, Epic #223 Phase D): the targeting RULE is
	// sim::raycastVoxel — the historical march moved verbatim into the
	// simulation. The host supplies camera origin/front + raycastDistance as
	// plain values; the bridge guard keeps the historical no-projection
	// behavior (e.g. mid-reload) exactly.
	if (!renderBridge || !sim)
		return false;
	const sim::RaycastHit hit = sim->raycastVoxel(camera.getPosition(), camera.getFront(),
												  static_cast<float>(renderSettings.raycastDistance));
	if (!hit.hit)
		return false;
	outBlock = hit.block;
	outPrevious = hit.adjacent;
	return true;
}

void Engine::updateHighlight()
{
	glm::vec3 block, prev;
	if (raycastVoxel(block, prev))
	{
		highlight.active = true;
		highlight.position = block;
		highlight.color = {1.f, 0.25f, 0.15f};
	}
	else
	{
		highlight.active = false;
	}
	if (worldRenderer)
		worldRenderer->overlays().setHighlight(highlight);
}

void Engine::handleEvents()
{
	// Shortcut-routing frame built ONCE per event batch (review §4.8): the
	// ~45-field GameUIFrame with its std::function callbacks used to be
	// reconstructed for every single key event.
	GameUIFrame shortcutFrame{};
	shortcutFrame.render = &renderSettings;
	shortcutFrame.paused = m_remote ? nullptr : &paused;
	shortcutFrame.setVSync = [this](bool v) { setVSync(v); };

	SDL_Event event;
	syncUiInput();
	while (SDL_PollEvent(&event))
	{
		if (routeUiEvent(event))
			continue;

		switch (event.type)
		{
		case SDL_EVENT_QUIT:
			requestExit();
			break;
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			if (event.window.windowID == SDL_GetWindowID(window))
				requestExit();
			break;
		case SDL_EVENT_WINDOW_HIDDEN:
			m_windowHidden = true;
			[[fallthrough]];
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			windowFocused = false;
			m_inputRouter.cancelAll();
			clearGameplayInput();
			cancelDeveloperInput();
			if (sim)
				sim->suspendPlayer();
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
		{
			windowFocused = true;
			int count = 0;
			const bool *keys = SDL_GetKeyboardState(&count);
			for (int key = 1; key < count; ++key)
				m_inputRouter.reconcileKey(size_t(key), keys[key]);
			const SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
			for (unsigned button = 1; button < 32; ++button)
				m_inputRouter.reconcileButton(button, (buttons & SDL_BUTTON_MASK(button)) != 0);
			clearGameplayInput();
			SDL_GetRelativeMouseState(nullptr, nullptr);
			if (sim)
				sim->suspendPlayer();
			break;
		}
		case SDL_EVENT_WINDOW_SHOWN:
			m_windowHidden = false;
			break;
		case SDL_EVENT_WINDOW_MINIMIZED:
			m_windowMinimized = true;
			m_inputRouter.cancelAll();
			clearGameplayInput();
			cancelDeveloperInput();
			break;
		case SDL_EVENT_WINDOW_RESTORED:
		case SDL_EVENT_WINDOW_MAXIMIZED:
			m_windowMinimized = false;
			break;
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
			// Pixel semantics only (review §4.8): SDL_EVENT_WINDOW_RESIZED
			// carries LOGICAL sizes and triggered a redundant swapchain
			// recreate on DPI != 1 displays.
			onResize(event.window.data1, event.window.data2);
			break;
		case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
			// The window moved to another monitor: the pacing refresh rate
			// must follow (165 Hz -> 60 Hz or back), otherwise VSync pacing
			// keeps snapping to the old interval.
			updateDisplayRefreshRate();
			break;
		case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
			updateDisplayRefreshRate();
			break;
		case SDL_EVENT_KEY_DOWN:
		{
			// OS key-repeat filter FIRST (review §4.8): it used to sit after
			// the shortcut routing, so holding a key re-triggered F-key
			// toggles on every repeat and spam-dropped items on Q.
			if (event.key.repeat)
				break;
			const SDL_Keycode key = event.key.key;
			// Workspace visibility (issue #303 F04). GLOBAL by policy, so it also
			// fires while a developer text field owns the keyboard; the whole
			// workspace goes away, the legacy inventory keeps floating and the
			// dockspace identity survives (F01).
			if (isWorkspaceToggle(static_cast<int>(key), event.key.mod))
			{
				setDeveloperWorkspaceVisible(!m_showDeveloperUi);
				break;
			}
			const KeyRoute keyRoute = classifyKeyRoute(static_cast<int>(key), event.key.mod);

			// Input routing policy (issue #76): F-key panel/VSync shortcuts
			// are intentional globals (non-text keys); gameplay keys must
			// not fire while ImGui owns the keyboard, or typing 'p'/'c'/'b'
			// in a text field would pause the world, steal the cursor, or
			// toggle overlays.
			if (keyRoute == KeyRoute::GlobalShortcut)
			{
				// An explicit tool shortcut also brings back the workspace
				// when the game was launched without the developer shell.
				if (key != SDLK_F6 && key != SDLK_F10 && !m_benchmark.isActive())
					setDeveloperWorkspaceVisible(true);
				if (gameUi && gameUi->handleGlobalShortcut(static_cast<int>(key), shortcutFrame))
					break;
			}

			if (key == SDLK_ESCAPE)
			{
				if (m_playerUi)
					m_playerUi->openPause();
				else
					requestExit();
				break;
			}

			if (m_benchmark.isActive() || !windowFocused)
				break;
			if (key == SDLK_SPACE && !paused)
			{
				jumpPressed = true;
				if (m_remote)
					m_remote->jumpQueuedAt = RemoteSession::Clock::now();
			}
			if (key == SDLK_V && sim && !paused)
			{
				setPlayerFlight(!sim->playerFlight());
				break;
			}
			if (key == SDLK_X && sim && sim->playerFlight() && !paused)
			{
				camera.setMovementSpeed(camera.getMovementSpeed() < 100.f ? 125.f : 20.f);
				break;
			}

			if (keyRoute == KeyRoute::GameplayShortcut)
			{
				if (key == SDLK_P && m_playerUi && !m_remote)
				{
					m_playerUi->openPause();
					break;
				}
				if (gameUi->handleGameplayShortcut(static_cast<int>(key), shortcutFrame))
					break;

				// Engine-owned gameplay keys (C/B/T/1-9/Q/E) fall through here:
				// classified by the policy, executed by Engine because their
				// host state (mouseCaptured, overlays, camera) lives here;
				// the gameplay keys issue simulation commands (issue #227).
			}

			if (key == SDLK_C)
			{
				setMouseCaptured(!mouseCaptured);
				break;
			}
			if (key == SDLK_B)
			{
				showChunkBorders = !showChunkBorders;
				if (worldRenderer)
					worldRenderer->overlays().setShowChunkBorders(showChunkBorders);
				break;
			}
			// Gameplay interaction keys (issue #227, Epic #223 Phase D): the
			// host maps input to simulation commands — no gameplay rule here.
			if (key >= SDLK_1 && key <= SDLK_9)
			{
				selectHotbarSlot(static_cast<int>(key - SDLK_1));
				break;
			}
			if (key == SDLK_Q && !paused)
			{
				// Drop one item from the selected hotbar slot along the camera
				// front (host-supplied plain direction, the savePlayerState
				// presentation-input pattern).
				dropSelectedItem();
				break;
			}
			if (key == SDLK_E)
			{
				setInventoryOpen(!m_inventoryPanelOpen);
				break;
			}
			if (key == SDLK_T)
			{
				// Legacy creative block cycle: FLIGHT/creative only (issue
				// #227). Walking mode selects through the gameplay hotbar.
				if (sim && sim->playerFlight())
				{
					int next = static_cast<int>(sim->selectedBlock()) + 1;
					if (next >= static_cast<int>(COUNT))
						next = 0;
					// Skip AIR
					if (static_cast<TextureType>(next) == AIR)
						next = 1;
					sim->setSelectedBlock(static_cast<TextureType>(next));
				}
				break;
			}
			break;
		}
		case SDL_EVENT_MOUSE_MOTION:
			if (mouseCaptured)
			{
				camera.processMouseMovement(static_cast<float>(event.motion.xrel),
											static_cast<float>(event.motion.yrel));
			}
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
			if (!mouseCaptured || !renderBridge || paused)
				break;
			if (m_remote)
			{
				if (windowFocused && !m_inventoryPanelOpen &&
					(event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT))
					sendRemoteVoxelAction(event.button.button == SDL_BUTTON_RIGHT);
				break;
			}
			{
				glm::vec3 block, prev;
				if (!raycastVoxel(block, prev))
					break;
				// Interaction mapping (issue #227, Epic #223 Phase D): WALKING
				// mode issues the simulation gameplay commands (break drops
				// items, place consumes the hotbar); FLIGHT keeps the
				// creative/editor path unchanged (infinite blocks). Accepted
				// gameplay edits publish through the bridge exactly like
				// creative edits (event drain + commit groups) — no second
				// edit path.
				if (event.button.button == SDL_BUTTON_LEFT)
				{
					if (sim->playerFlight())
					{
						renderBridge->deleteVoxel(block);
					}
					else
					{
						WorldEditResult edit;
						sim->breakBlock(glm::ivec3(block), &edit);
						renderBridge->publishEditResult(edit);
					}
				}
				else if (event.button.button == SDL_BUTTON_RIGHT)
				{
					if (sim->playerFlight())
					{
						// Player-aware placement validation is authoritative gameplay
						// state (issue #209): the simulation owns the rule, the bridge
						// publishes the accepted edit (event drain + commit groups).
						const TextureType selected = sim->selectedBlock();
						if (sim->canPlaceVoxel(prev, selected))
							renderBridge->placeVoxel(prev, selected);
					}
					else
					{
						WorldEditResult edit;
						sim->placeSelectedBlock(glm::ivec3(prev), &edit);
						renderBridge->publishEditResult(edit);
					}
				}
			}
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			if (camera.getMode() == CameraMode::ISOMETRIC)
			{
				camera.addIsometricZoom(static_cast<float>(event.wheel.y) * 4.f);
			}
			else if (mouseCaptured && event.wheel.y != 0)
			{
				// Hotbar cycle (issue #227): wheel up = previous slot, down =
				// next, wrapping 0..8. Selection is inventory management, so
				// it stays available while paused (world mutations are not).
				const int delta = event.wheel.y > 0 ? -1 : 1;
				int next = selectedHotbarSlot() + delta;
				if (next < 0)
					next = items::kHotbarSize - 1;
				else if (next >= items::kHotbarSize)
					next = 0;
				selectHotbarSlot(next);
			}
			break;
		default:
			break;
		}
		syncUiInput();
	}
}

void Engine::processInput(double dt)
{
    if (m_inspectionView) return;
	// Host suspension policy (issue #209): pause/focus/benchmark freeze the
	// authoritative player; the simulation clears accumulators + buffered
	// input so nothing accumulated is replayed on resume.
	if (!m_remote && simulationSuspended())
	{
		sim->suspendPlayer();
		jumpPressed = false;
		return;
	}
	// Isometric is an inspection mode; it must never move a physical body.
	if (sim && camera.getMode() == CameraMode::ISOMETRIC && !sim->playerFlight())
		setPlayerFlight(true);
	const bool captured = !m_movementAllowed;
	static_assert(SDL_SCANCODE_COUNT <= gameui::InputRouter::kKeys);
	std::array<bool, SDL_SCANCODE_COUNT> keys{};
	if (!captured)
	{
		const bool *physical = SDL_GetKeyboardState(nullptr);
		for (const auto key : {SDL_SCANCODE_W, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D,
			SDL_SCANCODE_SPACE, SDL_SCANCODE_LSHIFT})
			keys[key] = physical[key] && m_inputRouter.gameplayKey(size_t(key));
	}
	if (sim && sim->playerFlight())
	{
		if (!captured) camera.processKeyboard(std::min(dt, 0.05), keys.data());
		resetPlayerAtCamera();
		return;
	}
	physics::PlayerInput input;
	if (!captured)
	{
		const double yaw = glm::radians(double(camera.getYaw()));
		const glm::dvec2 forward(std::cos(yaw), std::sin(yaw));
		const glm::dvec2 right(-forward.y, forward.x);
		input.move = forward * double(int(keys[SDL_SCANCODE_W]) - int(keys[SDL_SCANCODE_S])) +
			right * double(int(keys[SDL_SCANCODE_D]) - int(keys[SDL_SCANCODE_A]));
		input.sprint = keys[SDL_SCANCODE_LSHIFT];
		input.jumpPressed = jumpPressed;
		input.swimUp = keys[SDL_SCANCODE_SPACE];
		input.swimDown = keys[SDL_SCANCODE_LSHIFT];
	}
	if (m_remote)
	{
		if (captured || !m_remote->ready())
			jumpPressed = false;
		sendRemoteInput(input);
		return;
	}
	if (captured)
		sim->clearPlayerInput();
	jumpPressed = false;
	{
		PROFILE_SCOPE("Physics");
		sim->advancePlayer(dt, input, false);
		camera.setPosition(glm::vec3(sim->playerRenderEye()));
	}
}

void Engine::tickMobs(double dt)
{
	PROFILE_SCOPE("Mobs");
	if (!renderBridge || !sim->isInitialized())
		return;
	const bool disabled = !sim->mobsEnabled() || m_benchmark.isActive();
	// Authoritative fixed-step mob simulation behind the CPU facade (issue
	// #208): the scoped WorldMobView holds only the world lock; spawn/retire/
	// interest are plain sim values. Suspension is host policy (issue #209):
	// pause/focus/benchmark plus the authoritative mobs toggle.
	sim->advanceMobs(dt, glm::dvec3(camera.getPosition()),
					 renderSettings.maxRenderDistance,
					 simulationSuspended() || !sim->mobsEnabled());
	if (disabled)
	{
		mobStates.clear();
	}
	else
	{
		sim->mobSnapshots(mobStates);
		// Derived entity light: sampled from the render bridge's caches
		// AFTER the mob snapshot (presentation-side; the removed
		// ChunkMobWorld adapter used to nest the world and render locks
		// here — issue #208).
		for (auto &ms : mobStates)
		{
			const glm::vec3 samplePos = entities::mobLightSamplePosition(ms);
			const auto light = renderBridge->sampleSmoothedLight(samplePos);
			ms.localSkylight = light.skylight;
			ms.localBlockRgb = light.blockRgb;
		}
	}
	worldRenderer->setMobs(mobStates);
}

void Engine::tickItems(double dt)
{
	PROFILE_SCOPE("Items");
	if (!renderBridge || !sim->isInitialized())
		return;
	// Dropped-item simulation (issue #227 completes the #226 loop in-game):
	// the same host suspension policy as the player/mobs.
	sim->advanceItems(dt, simulationSuspended());
	// Derived presentation (issue #229, the mob snapshot pattern): id-sorted
	// snapshots -> interpolated render states. The registry resolves the
	// atlas layer (ItemId -> placeableBlock), spin/bob derive from the
	// authoritative age + interpolation fraction (presentation-only), and
	// local light is sampled from the bridge AFTER the snapshot.
	sim->itemSnapshots(m_itemSnapshots);
	const float alpha = static_cast<float>(sim->itemInterpolationAlpha());
	m_itemStates.clear();
	for (const sim::ItemEntitySnapshot &s : m_itemSnapshots)
	{
		ItemRenderState st;
		st.position = glm::vec3(glm::mix(s.previous, s.current, double(alpha)));
		const std::optional<TextureType> block = items::placeableBlockOf(s.stack.item);
		st.atlasLayer = block ? static_cast<uint32_t>(*block) : 0u;
		const float age = static_cast<float>(s.ageTicks) + alpha;
		st.rotation = age * 0.05f;
		st.bob = std::sin(age * 0.1f) * 0.05f;
		const auto light = renderBridge->sampleSmoothedLight(st.position);
		st.localSkylight = light.skylight;
		st.localBlockRgb = light.blockRgb;
		m_itemStates.push_back(st);
	}
	if (worldRenderer)
		worldRenderer->setItemEntities(m_itemStates);
	// Drain the lifecycle stream at frame cadence (the WorldEvent contract):
	// v1 presentation is snapshot-driven (no spawn/despawn effects yet), so
	// drained events are discarded — draining keeps the bounded stream
	// healthy (droppedEventCount stays 0 in real play).
	sim->takeItemEvents(m_itemEventsScratch);
	m_itemEventsScratch.clear();
}

void Engine::tickBenchmark(double dt)
{
	if (m_benchmark.isActive() && !benchmarkOwnedCamera)
	{
		benchmarkOwnedCamera = true;
		flightBeforeBenchmark = sim->playerFlight();
		sim->suspendPlayer();
		jumpPressed = false;
	}
	if (m_benchmark.phase() == BenchmarkPhase::Reloading)
	{
		const BenchmarkConfig &cfg = m_benchmark.config();
		if (cfg.biomeMapZoom > 0.0f)
			gameUi->configureBenchmarkMap(cfg.biomeMapZoom);
		if (cfg.forceVsyncOff && renderSettings.vsyncEnabled)
		{
			m_benchmark.markForceVsync(true);
			setVSync(false);
		}
		// Let the main loop apply a requested mode at its pre-acquire boundary.
		// The benchmark snapshot must describe the swapchain it actually measures.
		if (m_pendingVSync)
			return;
		m_benchmark.setSettingsSnapshot(
			renderSettings.maxRenderDistance, windowWidth, windowHeight,
			renderSettings.vsyncEnabled,
			swapchain
				? VkSwapchain::presentModeName(swapchain->getPresentMode())
				: nullptr,
			vkContext ? vkContext->getDeviceProperties().deviceName : nullptr,
			vkContext ? vkContext->hasMultiDrawIndirect() : false,
			vkContext ? vkContext->maxDrawIndirectCount() : 0,
			renderSettings.streamFrontBias);

	if (!reloadWorld(cfg.seed))
	{
		// Never continue Warmup/Running against the OLD persistent world
		// (issue #180 review round 4): a benchmark measures a fresh
		// transient world or nothing. cancel() moves the benchmark back to
		// Idle; the regular post-benchmark restore block (this tick and the
		// next) returns the player/camera/vsync to their prior state.
		std::cerr << "[benchmark] world reload failed; benchmark cancelled" << std::endl;
		m_benchmark.cancel();
		return;
	}
		m_benchmark.onWorldReady(camera.getPosition());
		// Place camera on first path point
		m_benchmark.tick(0.0, camera);
		return;
	}

	if (m_benchmark.phase() == BenchmarkPhase::Warmup ||
		m_benchmark.phase() == BenchmarkPhase::Running)
	{
		m_benchmark.tick(dt, camera);
		// Open the streaming-counter window before this frame's tickStreaming
		// so warmup work stays outside the reported counters (issue #108).
		if (m_benchmark.consumeStreamingWindowStart() && renderBridge)
			m_benchmark.beginStreamingMeasurement(renderBridge->streamingMaintenanceStats());
		// One-shot scripted view-distance switch (--benchmark-view-switch):
		// fires 25% into the measurement so the rest of the run exercises
		// the incremental ChunkPool growth the report's PoolGrow stats
		// describe. Applied before this frame's tickStreaming (tickStreaming
		// runs later in the frame), and the near range follows the slider rule.
		if (int switchTo = 0; m_benchmark.consumeViewDistanceSwitch(switchTo))
		{
			renderSettings.maxRenderDistance = switchTo;
			renderSettings.minRenderDistance =
				clampedNearRenderDistance(renderSettings.minRenderDistance, switchTo);
			std::cout << "[benchmark] view distance -> " << switchTo << " blocks" << std::endl;
		}
	}

	// Restore VSync after done/cancel if we forced it off
	if (!m_benchmark.isActive() && benchmarkOwnedCamera)
	{
		benchmarkOwnedCamera = false;
		const glm::dvec3 feet = glm::dvec3(camera.getPosition()) -
		                        glm::dvec3(0, sim->playerSettings().eyeHeight, 0);
		sim->setPlayerFlight(true, feet);
		jumpPressed = false;
		// Lifecycle restore, not the gameplay toggle (review §4.8): the
		// validated exit-flight path can REFUSE at the scripted camera
		// position and leave flight stuck on; restore the exact
		// pre-benchmark mode unconditionally. Same camera side effect as
		// Engine::setPlayerFlight's walking path.
		sim->restorePlayerFlight(flightBeforeBenchmark, feet);
		if (!flightBeforeBenchmark)
			camera.setMode(CameraMode::PERSPECTIVE);
	}
	if (!m_benchmark.isActive() && m_benchmark.needsVsyncRestore())
	{
		setVSync(m_benchmark.prevVsync());
		m_benchmark.clearVsyncRestore();
	}
}

// Publish the main-thread memory/workload gauges every frame (issue #179):
// the developer console reads them live via telemetry::sampleLive(), outside
// any benchmark capture. Pure publication — set() only writes current/peak
// gauges and touches no capture state. The render-bridge frame samples
// (issue #112 review) are main-thread coherent snapshots; the VoxelPool
// itself stays a low-level thread-safe component without telemetry in its
// hot path. capacityBytes covers blocks retained by the pool including the
// free list, i.e. the high-water reuse footprint.
void Engine::publishFrameTelemetry()
{
	auto &telemetry = telemetry::registry();
	if (!telemetry.enabled)
		return;

	// Batched publication (review §4.8): one mutex acquisition per group
	// instead of ~27 per frame — the main thread no longer contends with
	// worker mesh-sample commits on the registry mutex during bursts.
	telemetry.setBatch({
		{telemetry::ActiveChunks, renderBridge ? renderBridge->chunkCount() : 0},
		{telemetry::DeferredChunks, renderBridge ? renderBridge->deferredReleaseCount() : 0},
	});

	if (!renderBridge)
		return;

	// voxel.pool.* / border.pool.*: the AUTHORITATIVE backing pools live in
	// VoxelWorld (issue #206), owned by the simulation (issue #208) — bound
	// render handles write through world records, so ChunkPool's own
	// voxel/border pools are unused in the game path. Read the world's pools
	// so the retained-backing memory gauges (docs/workload-telemetry.md)
	// describe the live backing. These depend only on the world, not the
	// render pool, so they are published before the ChunkPool guard below
	// (issue #206 review).
	const VoxelPool &voxelPool = viewWorld()->voxelPool();
	const size_t voxelCap = voxelPool.capacity();
	// border.pool.*: retained neighbor-border blocks (issue #103/#113).
	const BorderPool &borderPool = viewWorld()->borderPool();
	const size_t borderCap = borderPool.capacity();
	telemetry.setBatch({
		{telemetry::VoxelPoolCapacity, voxelCap},
		{telemetry::VoxelPoolActive, voxelPool.activeCount()},
		{telemetry::VoxelPoolFree, voxelPool.freeCount()},
		{telemetry::VoxelPoolCapacityBytes, voxelCap * sizeof(VoxelStorage)},
		{telemetry::BorderPoolCapacity, borderCap},
		{telemetry::BorderPoolActive, borderPool.activeCount()},
		{telemetry::BorderPoolFree, borderPool.freeCount()},
		{telemetry::BorderPoolCapacityBytes, borderCap * sizeof(ChunkNeighborBorders)},
	});

	if (!renderBridge->getChunkPool())
		return;
	ChunkPool *pool = renderBridge->getChunkPool();

	// mesh.pool.* + cpu.opaque/water.*: mesh build buffers live in
	// pooled transient results, not on chunks (issue #104). The cpu.*
	// gauges describe the in-flight/pending payloads while capacityBytes
	// reflects retained pool capacity (active + free list).
	const auto meshStats = pool->meshResultPool().stats();
	// light.pool.*: retained chunk light storage blocks (issue #128).
	const size_t lightCap = pool->lightStorageCapacity();
	telemetry.setBatch({
		{telemetry::OpaqueVertexBytes, meshStats.opaqueVertexSize},
		{telemetry::OpaqueVertexCapacity, meshStats.opaqueVertexCapacity},
		{telemetry::OpaqueIndexBytes, meshStats.opaqueIndexSize},
		{telemetry::OpaqueIndexCapacity, meshStats.opaqueIndexCapacity},
		{telemetry::WaterVertexBytes, meshStats.waterVertexSize},
		{telemetry::WaterVertexCapacity, meshStats.waterVertexCapacity},
		{telemetry::WaterIndexBytes, meshStats.waterIndexSize},
		{telemetry::WaterIndexCapacity, meshStats.waterIndexCapacity},
		{telemetry::CpuMeshCapacity, meshStats.capacityBytes()},
		{telemetry::MeshPoolCapacity, meshStats.capacity},
		{telemetry::MeshPoolActive, meshStats.active},
		{telemetry::MeshPoolFree, meshStats.free},
		{telemetry::MeshPoolCapacityBytes, meshStats.capacityBytes()},
		{telemetry::LightPoolCapacity, lightCap},
		{telemetry::LightPoolActive, pool->lightStorageActive()},
		{telemetry::LightPoolFree, pool->lightStorageFree()},
		{telemetry::LightPoolCapacityBytes, lightCap * sizeof(ChunkLightStorage)},
	});
}

void Engine::sampleBenchmarkFrame()
{
	if (m_benchmark.phase() != BenchmarkPhase::Running)
		return;
	Profiler &prof = GetProfiler();
	// A reset during reload discards the interrupted frame, even at zero warmup.
	if (prof.historyCount() == 0)
		return;
	uint64_t tJobs = 0, mJobs = 0, lJobs = 0, lcJobs = 0;
	float tMs = 0.f, mMs = 0.f, lMs = 0.f, lcMs = 0.f;
	m_benchmark.sampleGpu(frameCtx->gpuProfiler().latest());
	const int wc = prof.workerSnapshotCount();
	const WorkerSnapshot *ws = prof.workerSnapshots();
	for (int i = 0; i < wc; ++i)
	{
		if (!ws[i].name)
			continue;
		m_benchmark.sampleBackgroundWork(ws[i].name, ws[i].count, ws[i].totalMs);
		if (std::strcmp(ws[i].name, "TerrainGen") == 0)
		{
			tJobs = ws[i].count;
			tMs = ws[i].totalMs;
		}
		else if (std::strcmp(ws[i].name, "MeshBuild") == 0)
		{
			mJobs = ws[i].count;
			mMs = ws[i].totalMs;
		}
		else if (std::strcmp(ws[i].name, "MeshLOD") == 0)
		{
			lJobs = ws[i].count;
			lMs = ws[i].totalMs;
		}
		else if (std::strcmp(ws[i].name, "LightCache") == 0)
		{
			lcJobs = ws[i].count;
			lcMs = ws[i].totalMs;
		}
	}

	// Main-thread memory/workload gauges are published every frame by
	// publishFrameTelemetry() (issue #179); no per-sample work needed here.
	// Named-field sample (review §4.8): designated initializers replace the
	// 23 positional arguments — a swapped float pair can no longer compile.
	m_benchmark.sampleFrame(BenchmarkFrameSample{
		.frameMs = prof.lastFrameMs(),
		.scopeStreaming = prof.lastScopeMs("Streaming"),
		.scopeAcquire = prof.lastScopeMs("Acquire"),
		.scopeRecord = prof.lastScopeMs("Record"),
		.scopeImGui = prof.lastScopeMs("DevUIBuild"),
		.scopePresent = prof.lastScopeMs("Present"),
		.scopeVisibility = prof.lastScopeMs("Visibility"),
		.scopeMeshUpload = prof.lastScopeMs("MeshUpload"),
		.scopePoolGrow = prof.lastScopeMs("PoolGrow"),
		.chunks = renderBridge ? renderBridge->chunkCount() : 0,
		.drawCount = drawList.size(),
		.pendingLoad = renderBridge ? renderBridge->pendingLoadCount() : 0,
		.pendingGen = renderBridge ? renderBridge->pendingGenJobs() : 0,
		.pendingMesh = renderBridge ? renderBridge->pendingMeshJobs() : 0,
		.pendingLight = renderBridge ? renderBridge->pendingLightJobs() : 0,
		.terrainJobs = tJobs,
		.terrainMs = tMs,
		.meshJobs = mJobs,
		.meshMs = mMs,
		.lodJobs = lJobs,
		.lodMs = lMs,
		.lightCacheJobs = lcJobs,
		.lightCacheMs = lcMs,
	});
	m_benchmark.sampleIndirectCommands(worldRenderer ? worldRenderer->lastIndirectCommandCount() : 0);
	if (renderBridge)
		m_benchmark.sampleStreamingStats(renderBridge->streamingMaintenanceStats());
}

void Engine::drawUi()
{
	// The developer workspace can be hidden at runtime, but ImGui still runs:
	// the legacy inventory may be open, and the dockspace is submitted with
	// KeepAliveOnly so the persisted docking layout survives the hidden frames
	// (issue #303 F01). Only a native player screen or an inspection/benchmark
	// view removes the developer layer entirely.
	if (!gameUi || !developerUiPresent())
		return;

	const bool prevCapture = mouseCaptured;
	bool presentedCapture = mouseCaptured;
	bool presentedPause = paused || nativePause();
	const bool prevPause = presentedPause;

	GameUIFrame f{};
	f.camera = &camera;
	// Read-only player snapshot for the UI (issue #184): biome/chunk resolved
	// here via the canonical queries so panels never touch the generator or
	// the simulation's player controller directly. The kinematic/diagnostic
	// fields come from the simulation's value snapshot (issue #208).
	const sim::PlayerSnapshot playerView = sim ? sim->playerSnapshot() : sim::PlayerSnapshot{};
	f.player.position = camera.getPosition();
	f.player.yaw = camera.getYaw();
	f.player.pitch = camera.getPitch();
	{
		const glm::ivec2 chunk = playerui::worldToChunkCoord(f.player.position.x,
															 f.player.position.z);
		f.player.chunkX = chunk.x;
		f.player.chunkZ = chunk.y;
	}
	// Biome: only the Detailed overlay displays it (issue #184 review), so
	// skip the query in every other UI state — and even then re-sample only
	// when the voxel column or the world generation changed (the player can
	// spend many frames inside one column).
	f.player.biome = -1;
	if (m_showDeveloperUi && sim && sim->terrainGenerator() && gameUi->needsPlayerBiome())
	{
		const glm::ivec2 column =
			worldToVoxelColumn(glm::vec2(f.player.position.x, f.player.position.z));
		if (column != m_uiBiomeColumn || m_uiBiomeWorldGen != sim->worldGenerationId())
		{
			m_uiBiomeColumn = column;
			m_uiBiomeWorldGen = sim->worldGenerationId();
			// Single canonical world -> voxel-column convention (floor).
			const BiomeType biome = sim->terrainGenerator()->getBiomeAt(column.x, column.y);
			m_uiBiome = (biome >= 0 && biome < BIOME_COUNT) ? static_cast<int>(biome) : -1;
		}
		f.player.biome = m_uiBiome;
	}
	f.player.flight = playerView.flight;
	f.player.grounded = playerView.grounded;
	f.player.swimming = playerView.submerged.water + playerView.submerged.lava > 0;
	f.player.waitingForTerrain = playerView.waitingForTerrain;
	f.player.speed = static_cast<float>(glm::length(playerView.velocity));
	f.player.status = playerView.status;
	f.player.physicsSteps = playerView.metrics.steps;
	f.player.queriedCells = playerView.metrics.queries.cells;
	f.player.queryIterations = playerView.metrics.queries.iterations;
	f.player.droppedSteps = playerView.metrics.droppedSteps;
	f.player.submergedWater = playerView.submerged.water > 0;
	f.player.submergedLava = playerView.submerged.lava > 0;
	// Camera view state travels through the snapshot (issue #184 review):
	// panels display these values and mutate through the commands below.
	f.player.cameraViewMode = camera.getMode() == CameraMode::ISOMETRIC
								  ? playerui::CameraViewMode::Isometric
								  : playerui::CameraViewMode::Perspective;
	f.player.cameraMovementSpeed = camera.getMovementSpeed();
	f.player.mouseSensitivity = camera.getMouseSensitivity();
	f.player.isometricZoom = camera.getIsometricZoom();
	if (sim)
	{
		f.setPlayerFlight = [this](bool enabled) { setPlayerFlight(enabled); };
		f.setCameraMode = [this](CameraMode mode) {
			if (m_benchmark.isActive()) return;
			if (mode == CameraMode::ISOMETRIC && !sim->playerFlight()) setPlayerFlight(true);
			camera.setMode(mode);
		};
	}
	f.setCameraMovementSpeed = [this](float value) { camera.setMovementSpeed(value); };
	f.setMouseSensitivity = [this](float value) { camera.setMouseSensitivity(value); };
	f.setIsometricZoom = [this](float value) { camera.setIsometricZoom(value); };
	f.chunks = renderBridge.get();
	f.pool = chunkPool.get();
	f.generator = sim ? sim->terrainGenerator() : nullptr;
	f.worldRenderer = worldRenderer.get();
	f.vk = vkContext.get();
	f.gpu = &frameCtx->gpuProfiler();
	f.frameIndex = frameCtx->currentFrameIndex();
	f.imm = immediate.get();
	f.shader = &shaderParams;
	f.render = &renderSettings;
	f.timing = &renderTiming;
	f.staging = &stagingRing;
	// Commands-only sim surface (review M6): read-only views + command
	// callbacks, the inventory discipline — no mutable pointer into
	// authoritative state crosses into the UI.
	f.highlight = &highlight;
	f.mouseCaptured = &presentedCapture;
	f.showChunkBorders = &showChunkBorders;
	f.mobCount = mobStates.size();
	f.mobVisible = worldRenderer->visibleMobs();
	populateJoinUi(f);
	if (m_remote)
		populateRemoteUi(f);
	else
	{
		f.selectedTexture = &sim->selectedBlock();
		f.setSelectedBlock = [this](TextureType type) { sim->setSelectedBlock(type); };
		f.showDemoPlayers = &showDemoPlayers;
		f.mobsEnabled = sim->mobsEnabled();
		f.setMobsEnabled = [this](bool enabled) { sim->setMobsEnabled(enabled); };
		f.paused = &presentedPause;
		sim->inventorySnapshot(f.inventory);
		f.selectHotbarSlot = [this](int slot) { selectHotbarSlot(slot); };
		f.moveStack = [this](int from, int to) { sim->moveStack(from, to); };
		f.swapStacks = [this](int a, int b) { sim->swapStacks(a, b); };
		f.splitStack = [this](int from, int to) { sim->splitStack(from, to); };
		f.dropFromSlot = [this](int slot, uint16_t count, glm::dvec3 dir) { sim->dropFromSlot(slot, count, dir); };
	}
	f.inventoryPanelOpen = &m_inventoryPanelOpen;
	f.atlasVersion = m_atlasVersion;
	if (m_playerUi)
		f.gameUiStats = m_playerUi->lastRecordedStats();
	f.seed = worldSeed();
	f.worldGenerationId = worldGenerationId();
	// World-save snapshot (issue #180, Phase 5): a plain-copy of the
	// persistence status — the UI frame never holds WorldPersistence types.
	f.worldSave.active = renderBridge && renderBridge->isWorldOpen();
	if (f.worldSave.active && renderBridge->worldPersistence())
	{
		const WorldPersistence::Status st = renderBridge->worldPersistence()->status();
		f.worldSave.worldName = st.name;
		f.worldSave.seed = st.seed;
		f.worldSave.queueDepth = st.queueDepth;
		f.worldSave.queueDepthPeak = st.queueDepthPeak;
		f.worldSave.dirtyCoordinates = st.dirtyCoordinates;
		f.worldSave.failedCoordinates = st.failedCoordinates;
		f.worldSave.avgSerializeMs = st.avgSerializeMs;
		f.worldSave.maxSerializeMs = st.maxSerializeMs;
		f.worldSave.avgWriteMs = st.avgWriteMs;
		f.worldSave.maxWriteMs = st.maxWriteMs;
		f.worldSave.rejectedBusyQueueFull = st.rejectedBusyQueueFull;
		f.worldSave.rejectedBusyCommitGate = st.rejectedBusyCommitGate;
		f.worldSave.enqueued = st.enqueued;
		f.worldSave.completed = st.completed;
		f.worldSave.superseded = st.superseded;
		f.worldSave.failed = st.failed;
		f.worldSave.deleted = st.deleted;
		f.worldSave.bytesWritten = st.bytesWritten;
		f.worldSave.lastError = st.lastError;
	}
	else if (sim && !sim->openWorldError().empty())
	{
		// A world was requested but this session runs transient: surface why.
		f.worldSave.lastError = sim->openWorldError();
	}
	f.worldSave.error = !f.worldSave.lastError.empty();
	f.fps = static_cast<float>(fps > 0.0 ? fps : ImGui::GetIO().Framerate);
	f.frameMs = static_cast<float>(deltaTime * 1000.0);
	// Hierarchical-profiler CPU frame time for the shell status strip (issue
	// #183/#179): distinct from the paced simulation delta above.
	f.cpuFrameMs = GetProfiler().lastFrameMs();
	f.drawCount = drawList.size();
	f.windowW = windowWidth;
	f.windowH = windowHeight;
	f.benchmark = m_remote || m_hostSnapshot.active() ? nullptr : &m_benchmark;
	if (swapchain)
		f.presentModeName =
			VkSwapchain::presentModeName(swapchain->getPresentMode());
	if (vkContext)
	{
		f.deviceName = vkContext->getDeviceProperties().deviceName;
		f.vkApiVersion = vkContext->getDeviceProperties().apiVersion;
		f.validation = vkContext->isValidationEnabled();
	}
	f.setVSync = [this](bool v) { setVSync(v); };
	// Graphics ▸ Display "(applying)" marker (issue #185): true until the
	// deferred swapchain recreate consumed the pending toggle.
	f.vsyncPending = m_pendingVSync.has_value();
	if (imgui)
	{
		f.uiScale = imgui->uiScale();
		f.setUiScale = [this](float scale) { imgui->requestUiScale(scale); };
	}
	f.requestExit = [this]() { requestExit(); };
	// Runtime workspace visibility (issue #303 F04): the same action the
	// Ctrl+Shift+D shortcut performs, reachable from the Developer menu.
	f.toggleDeveloperWorkspace = [this]() { setDeveloperWorkspaceVisible(!m_showDeveloperUi); };
	f.resourcePackRoot = &m_resourcePackRoot;
	f.applyResourcePack = [this](const std::string &path) -> GameUIFrame::ResourcePackUiResult {
		const ResourcePackApplyResult r = applyResourcePack(path);
		return {r.atlasOk, r.isError, r.isWarning, r.message};
	};
	if (threadPool && !m_remote)
	{
		f.submitBiomeMap = [this](BiomeMapRequest req) -> std::future<BiomeMapResult> {
			if (!m_benchmark.config().biomeMapSequential)
				return submitBiomeMap(*threadPool, std::move(req));
			// Benchmark control: the previous one-Low-job map path.
			auto promise = std::make_shared<std::promise<BiomeMapResult>>();
			auto future = promise->get_future();
			const auto start = std::chrono::steady_clock::now();
			threadPool->submit(TaskPriority::Low, [promise, req, start] {
				BiomeMapResult result;
				try { result = generateBiomeMap(req); } catch (...) {}
				result.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
				promise->set_value(std::move(result));
			});
			return future;
		};
	}

	if (m_showDeveloperUi)
		gameUi->draw(f);
	else
		gameUi->drawWithoutWorkspace(f);

	if (presentedPause != prevPause)
	{
		if (!m_playerUi)
			paused = presentedPause;
		else if (presentedPause)
		{
			setInventoryOpen(false);
			m_playerUi->openPause();
		}
		else
		{
			paused = false;
			m_playerUi->closePause();
		}
	}
	if (presentedCapture != prevCapture)
		setMouseCaptured(presentedCapture);
	if (worldRenderer)
		worldRenderer->overlays().setShowChunkBorders(showChunkBorders);

	if (worldRenderer && !m_remote)
		worldRenderer->overlays().setPlayers(showDemoPlayers ? demoPlayers : std::vector<OverlayPlayer>{});
}

void Engine::updateDisplayRefreshRate()
{
	float previous = m_displayRefreshRate;
	m_displayRefreshRate = 0.0f;
	if (!window)
		return;
	SDL_DisplayID displayId = SDL_GetDisplayForWindow(window);
	if (!displayId)
		displayId = SDL_GetPrimaryDisplay();
	if (displayId)
	{
		const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(displayId);
		if (mode && std::isfinite(mode->refresh_rate) && mode->refresh_rate > 10.0f)
			m_displayRefreshRate = mode->refresh_rate;
	}
	if (m_displayRefreshRate != previous)
		std::cout << "Display refresh rate: " << m_displayRefreshRate << " Hz\n";
}

void Engine::resetFrameClock()
{
	m_lastPerfCounter = SDL_GetPerformanceCounter();
	if (m_perfFrequency != 0)
	{
		const double now = static_cast<double>(m_lastPerfCounter) / static_cast<double>(m_perfFrequency);
		lastFrame = now;
		lastTime = now;
	}
}

void Engine::updateWindowTitle(double currentFrame)
{
	frameCount++;
	if (currentFrame - lastTime < 1.0)
		return;
	fps = frameCount;
	frameCount = 0;
	lastTime = currentFrame;
	char title[192];
	char benchTag[48]{};
	if (m_benchmark.isActive())
	{
		std::snprintf(benchTag, sizeof(benchTag), " [BENCH %.0fs]",
					  m_benchmark.remainingMeasureSec());
	}
	std::snprintf(title, sizeof(title),
				  "ft_vox — %.0f FPS | chunks %zu | draw %zu | seed %d%s%s",
				  fps,
				  renderBridge ? renderBridge->chunkCount() : 0,
				  drawList.size(), worldSeed(),
				  (paused || nativePause()) ? " [PAUSED]" : "", benchTag);
	SDL_SetWindowTitle(window, title);
}

void Engine::updateFrameUniforms(uint32_t frameIndex, int pixelW, int pixelH, double currentFrame)
{
	PROFILE_SCOPE("UpdateUBO");
	const float farPlane = computeCameraFarPlane(renderSettings.maxRenderDistance);
	// Shadow quality tier change (issue #137): recreate the shadow map
	// at the requested resolution before it feeds the frame UBO.
	if (worldRenderer->postSettings().shadowMapSize > 0 &&
		worldRenderer->activeShadowMapSize() != uint32_t(worldRenderer->postSettings().shadowMapSize))
		worldRenderer->applyShadowMapSize(uint32_t(worldRenderer->postSettings().shadowMapSize));
	// Visual immersion follows the eye, including water occupied by kelp.
	bool underwater = false;
	float underwaterSurfaceY = kUnknownUnderwaterSurfaceY;
	if (renderBridge && (!m_remote || m_remote->hasCameraPose))
	{
		WorldCollisionView world(*viewWorld());
		const glm::ivec3 eye = glm::ivec3(glm::floor(camera.getPosition()));
		underwater = world.sample(eye).medium == physics::Medium::Water;
		underwaterSurfaceY = world.waterSurfaceAbove(eye);
	}
	worldRenderer->postSettings().underwater = underwater;
	worldRenderer->postSettings().underwaterSurfaceY = underwaterSurfaceY;
	worldRenderer->updateFrameUBO(frameIndex, camera,
							static_cast<float>(pixelW), static_cast<float>(pixelH), farPlane,
							static_cast<float>(currentFrame), shaderParams,
							renderSettings.shadowCascadeFar, underwater);
}

gameui::HudView Engine::playerHudView() const
{
	using gameui::style::Status;
	gameui::HudView view;
	view.captured = mouseCaptured && windowFocused && !m_captureFailed;
	view.hints = gameUi ? gameUi->showOverlayHints() : true;
	if (switchingSession())
	{
		view.status = "Switching worlds";
		view.statusKind = Status::Pending;
		view.activity = "The previous session is closing and the next one is loading.";
		return view;
	}

	bool canAim = false;
	if (m_remote)
	{
		const auto now = RemoteSession::Clock::now();
		const auto connection = m_remote->connectionStatus(now);
		// Transport status -> HUD session row (issue #303 F07): the copy, the bar
		// and the reticle are one decision, unit-tested in
		// tests/test_game_ui_components (hudSession).
		using enum ui::MultiplayerState;
		gameui::HudSessionState hudState = gameui::HudSessionState::Pending;
		switch (connection.state)
		{
		case Connected: hudState = gameui::HudSessionState::Connected; break;
		case Degraded: hudState = gameui::HudSessionState::Degraded; break;
		case Connecting:
		case SyncingTerrain:
		case Resyncing:
		case Leaving: hudState = gameui::HudSessionState::Pending; break;
		case Disconnected:
		case TimedOut:
		case Rejected: hudState = gameui::HudSessionState::Blocked; break;
		}
		const bool synchronized = m_remote->ready();
		const auto session = gameui::hudSession(hudState, synchronized, connection.label,
			connection.detail ? connection.detail : std::string_view{});
		view.status = session.status;
		view.statusKind = session.kind;
		view.activity = session.activity;
		view.recovery = session.recovery;
		if (session.showHotbar && synchronized)
		{
			if (const auto snapshot = m_remote->transport.inventory())
				view.hotbar = gameui::makeHudHotbar(snapshot->slots, snapshot->selected, m_atlasVersion,
					m_remote->actionPending);
			canAim = view.hotbar.available && session.allowAim;
			// Pending action feedback survives a degraded-but-playable session.
			if (const char *action = m_remote->actionStatus(now))
				view.activity = action;
		}
	}
	else if (sim && renderBridge)
	{
		sim::InventorySnapshot inventory;
		sim->inventorySnapshot(inventory);
		view.hotbar = gameui::makeHudHotbar(inventory.slots, inventory.selectedSlot, m_atlasVersion, false);
		const auto player = sim->playerSnapshot();
		canAim = !player.waitingForTerrain;
		if (!canAim)
		{
			view.status = "Waiting for terrain";
			view.statusKind = Status::Pending;
			view.activity = "The surrounding chunks are still being generated.";
			view.recovery = "The hotbar returns as soon as the world is ready.";
		}
		view.creative = player.flight;
		if (view.creative && size_t(player.selectedBlock) < textureTypeString.size())
		{
			view.creativeBlock = items::itemName(items::itemForBlock(player.selectedBlock));
			if (view.creativeBlock.empty())
				view.creativeBlock = textureTypeString[size_t(player.selectedBlock)];
		}
	}
	view.aim = view.captured && canAim && camera.getMode() == CameraMode::PERSPECTIVE;
	view.target = view.aim && highlight.active;
	return view;
}

void Engine::recordFrame(uint32_t frameIndex, uint32_t imageIndex, int uploadBudget,
						 std::optional<uint64_t> gpuTag)
{
	PROFILE_SCOPE("Record");
	const bool developerOverlay = developerUiAvailable();
	const bool buildPlayerUi = m_playerUi && !m_inspectionView &&
		!m_exitAfterBenchmark && !m_benchmark.isActive();
	if (buildPlayerUi)
	{
		PROFILE_SCOPE("GameUIBuild");
		// A modal or floating window may have appeared during DevUIBuild,
		// even if SDL delivered no pointer event this frame.
		if (!m_uiSpike && m_playerUi->hasArmedPress())
		{
			float x = 0.f, y = 0.f;
			SDL_GetMouseState(&x, &y);
			if (mouseCaptured || m_benchmark.locksInput() || inputContext().devDialog ||
				developerPointerAt(x, y) != ImGuiLayer::PointerHit::PassThrough)
				cancelGameUiPointer();
		}
		gameui::Clip view{};
		if (gameUi && m_showDeveloperUi && developerOverlay)
		{
			const auto area = gameUi->shell().gameViewRect();
			const ImVec2 origin = ImGui::GetMainViewport()->Pos;
			const float x = area.pos.x - origin.x, y = area.pos.y - origin.y;
			view = {{x, y, x + area.size.x, y + area.size.y}};
		}
		std::optional<gameui::HudView> hud;
		if (!m_uiSpike && m_playerUi->hudVisible() && !m_inventoryPanelOpen && !paused &&
			m_playerUi->screens().top() == gameui::Screen::Hud &&
			!(developerOverlay && (imgui->hasOpenPopup() || gameUi->isFileDialogOpen())))
			hud = playerHudView();
		m_playerUi->prepare(window, frameIndex, swapchain->getExtent(), view,
			worldRenderer->getTextureManager(), m_atlasVersion, hud ? &*hud : nullptr);
		syncUiInput();
	}
	std::function<void(VkCommandBuffer)> devUiDraw;
	if (developerOverlay)
		devUiDraw = [&](VkCommandBuffer cmd) { imgui->recordDraw(cmd); };
	worldRenderer->recordFrame(
		frameCtx->commandBuffer(), frameIndex, imageIndex, *swapchain, drawList, shadowList, kClearColor,
		[&](VkCommandBuffer cmd) {
			if (renderBridge && uploadBudget > 0 && !paused && !nativePause())
			{
				PROFILE_SCOPE("MeshUpload");
				// Barrier keyed on COPIES RECORDED, not publications (review M2):
				// commit-group members record copies across frames before their
				// atomic publish, so a zero published count with recorded copies
				// still needs the transfer->vertex dependency.
				bool copiesRecorded = false;
				renderBridge->uploadPendingMeshes(
					vkContext->getAllocator(), stagingRing, cmd, resourceRetire,
					worldRenderer->arenas(), camera, uploadBudget, &copiesRecorded);
				if (copiesRecorded)
				{
					// Make staged mesh data visible to vertex/index fetch in later passes.
					VkMemoryBarrier barrier{};
					barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
					barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
					barrier.dstAccessMask =
						VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
					vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
										 VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier, 0, nullptr,
										 0, nullptr);
				}
			}
		},
		devUiDraw,
		// Biome-map upload AFTER the ImGui pass (issue #191 review
		// round 2): ImGui samples this texture directly, so this
		// frame's draw lists must record against the previous
		// publication; the new pixels+grid become visible to the
		// next frame's UI build. Mesh uploads stay in preRecord —
		// nothing in ImGui samples them in the same frame.
		[&](VkCommandBuffer cmd) {
			if (gameUi && gameUi->hasPendingBiomeMapUpload())
			{
				PROFILE_SCOPE("BiomeMapUpload");
				gameUi->recordPendingBiomeMapUpload(cmd, stagingRing);
			}
		}, &frameCtx->gpuProfiler(), gpuTag.value_or(m_benchmark.gpuCaptureTag()),
		buildPlayerUi ? &m_playerUi->renderer() : nullptr);
	if (buildPlayerUi)
		m_playerUi->publishRecordedStats(frameNumber, frameIndex);
}

void Engine::enableGameUi(bool standalone, bool previewStrip)
{
	auto sample = std::make_unique<GameUiSample>();
	if (standalone && !setMouseCaptured(false))
		throw std::runtime_error(std::string("UI spike could not release the cursor: ") + SDL_GetError());
	sample->init(window, *vkContext, *immediate, swapchain->getImageFormat(), swapchain->getColorSpace(), standalone);
	// The persisted policy is applied as a request: the first prepare() resolves
	// it at the frame boundary (issue #302). A store problem is surfaced where
	// the setting is edited, not silently swallowed.
	sample->setScalePolicy(m_playerUiSettings.scale);
	sample->setSettingsNotice(m_playerUiError);
	sample->setPreviewStripVisible(previewStrip);
	sample->setHudVisible(m_showPlayerHud);
	m_uiSpike = standalone;
	m_playerUi = std::move(sample);
}

// ===========================================================================
// Frame orchestration (issue #209, Epic #204 Phase E). The Engine is the
// graphical host: it owns SDL/Vulkan/ImGui and the presentation camera,
// drives the CPU simulation through plain commands, and projects the sim's
// read-only snapshots through the renderer-owned bridge. One frame runs in
// this fixed order (the numbered steps below):
//
//   1. Events       SDL poll -> UI routing + sim commands (handleEvents)
//   2. Viewport     minimized guard + deferred swapchain recreate
//   3. Acquire      VkFrameContext::beginFrame (WSI; may block on present)
//   4. Frame clock  paced dt sampled AFTER acquire so WSI stalls never feed
//                   the simulation; failed acquire / minimize resync it
//   5. Benchmark    benchmark policy; may reload the world (epoch barrier)
//   6. Simulation   SDL -> PlayerCommand -> sim->advancePlayer; the camera
//                   follows the interpolated render eye (flight: camera
//                   drives the body via sim->setPlayerPosition)
//   7. Day cycle    presentation-only ShaderParameters (pause-gated here)
//   8. Streaming    bridge: publish jobs, dispatch gen/mesh/light, visibility,
//                   draw/shadow lists, upload budget — NO GPU work here
//   9. Mobs         sim->advanceMobs + snapshots + bridge entity light ->
//                   WorldRenderer::setMobs
//   9b. Items       sim->advanceItems + id-sorted snapshots + bridge light ->
//                   WorldRenderer::setItemEntities (issues #226/#229)
//  10. Highlight    sim raycast (thin forward) -> overlay
//  11. Frame slots  retire/arena/staging beginFrame (fence already waited)
//  12. ImGui        UI build from sim/render snapshots (CPU only)
//  13. UBO          underwater sample + frame uniforms
//  14. Record       passes + mesh uploads — the ONLY GPU upload point
//  15. Present      submit + present
//  16. Telemetry    gauges, profiler end, RenderTiming, benchmark sample
//
// The simulation never sees SDL/ImGui/Vulkan; the bridge never owns sim
// state. World transitions (initializeNoiseGenerator / reloadWorld) go
// through the epoch barrier (resetWorldProjection) so no stale derived
// snapshot or async job survives a reload.
// ===========================================================================
void Engine::run()
{
	running = true;
	if (m_pendingQualityPreset)
	{
		worldRenderer->postSettings().applyPreset(*m_pendingQualityPreset);
		m_pendingQualityPreset.reset();
	}
	if (m_perfFrequency == 0)
		m_perfFrequency = SDL_GetPerformanceFrequency();
	updateDisplayRefreshRate();

	m_lastPerfCounter = SDL_GetPerformanceCounter();
	lastFrame = static_cast<double>(m_lastPerfCounter) / static_cast<double>(m_perfFrequency);
	lastTime = lastFrame;
	const double inspectionStart = lastFrame;
	if (!m_inspectionView && !m_exitAfterBenchmark)
	{
		loadPlayerUiPreferences();
		if (!m_playerUi)
			enableGameUi(false, false);
	}
	if (!m_uiSpike && !m_inspectionView && !m_exitAfterBenchmark)
	{
		loadJoinPreferences();
		loadHosting();
	}

	// First run on this machine (no imgui.ini yet): start with the default
	// developer layout instead of an empty workspace. Benchmark runs keep the
	// stock UI closed so open panels cannot perturb scores (issue #183).
	if (imgui && gameUi && !imgui->hadExistingIni() && !m_exitAfterBenchmark && !m_uiSpike)
	{
		gameUi->shell().queueDefaultLayout();
		gameUi->enableDefaultDeveloperPanels();
	}

	while (running)
	{
		GetProfiler().beginFrame();

		// (1) Events: SDL -> UI routing + simulation commands.
		{
			PROFILE_SCOPE("Events");
			handleEvents();
		}
		if (running)
			tickHosting();
		if (!running)
		{
			GetProfiler().endFrame();
			break;
		}
		applySessionRequests();

		// (2) Viewport: a minimized/hidden window skips the whole frame; the
		// clock resync AFTER the sleep keeps the skipped stretch out of the
		// next gameplay timestep (restore = rawDt of a normal frame).
		int pixelW = 0, pixelH = 0;
		SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);
		if (pixelW == 0 || pixelH == 0 ||
			(SDL_GetWindowFlags(window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)))
		{
			if (m_remote)
				pollRemote();
			GetProfiler().endFrame();
			SDL_Delay(16);
			resetFrameClock();
			continue;
		}

		recreateSwapchainIfNeeded(static_cast<uint32_t>(pixelW),
								  static_cast<uint32_t>(pixelH));

		frameCtx->gpuProfiler().syncCapture(GetProfiler().captureEpoch());
		// Inspection timeout check runs BEFORE acquisition: breaking after a
		// successful beginFrame would leave the image acquired with no
		// submit/present (Vulkan contract violation).
		const uint64_t preAcquireCounter = SDL_GetPerformanceCounter();
		const double preAcquireTime = static_cast<double>(preAcquireCounter) / static_cast<double>(m_perfFrequency);
		if (m_inspectionView && m_inspectionSeconds > 0.f &&
			preAcquireTime - inspectionStart >= m_inspectionSeconds)
		{
			GetProfiler().endFrame();
			break;
		}

		// (3) Acquire: WSI image + frame slot. A failed acquire can block on
		// presentation for a long time; that stretch is not simulated, so the
		// frame clock is re-synchronized instead of feeding it to gameplay.
		uint32_t imageIndex = 0;
		{
			PROFILE_SCOPE("Acquire");
			if (!frameCtx->beginFrame(*swapchain, imageIndex))
			{
				requestSwapchainRecreate();
				if (m_remote)
					pollRemote();
				GetProfiler().endFrame();
				resetFrameClock();
				continue;
			}
		}

		// (4) Frame clock: paced dt from the acquire-to-acquire interval.
		const uint64_t currentPerfCounter = SDL_GetPerformanceCounter();
		const double currentFrame = static_cast<double>(currentPerfCounter) / static_cast<double>(m_perfFrequency);

		const double rawDt = (m_lastPerfCounter > 0)
			? static_cast<double>(currentPerfCounter - m_lastPerfCounter) / static_cast<double>(m_perfFrequency)
			: (1.0 / 60.0);
		m_lastPerfCounter = currentPerfCounter;
		lastFrame = currentFrame;

		deltaTime = frame_pacing::computePacedDeltaTime(
			rawDt,
			swapchain ? swapchain->isVSync() : renderSettings.vsyncEnabled,
			static_cast<double>(m_displayRefreshRate));

		updateWindowTitle(currentFrame);

		// WSI can wait for another refresh. Drain snapshots after that wait
		// so the input scheduler samples the network close to its send.
		if (m_remote)
			pollRemote();

		// (5) Benchmark policy (may reload the world through the epoch
		// barrier), then (6) the authoritative player step, then (7) the
		// presentation day cycle.
		{
			PROFILE_SCOPE("Benchmark");
			if (!m_remote)
				tickBenchmark(deltaTime);
		}
		{
			PROFILE_SCOPE("Input");
			processInput(deltaTime);
		}
		{
			PROFILE_SCOPE("DayCycle");
			tickDayCycle(deltaTime);
		}
		// (8) Derived world projection: streaming, visibility, draw lists.
		tickStreaming(deltaTime);
		// (9) Mob simulation + derived presentation, (9b) dropped items,
		// (10) block highlight.
		if (m_remote)
			updateRemoteEntities();
		else
		{
			tickMobs(deltaTime);
			tickItems(deltaTime);
		}
		{
			PROFILE_SCOPE("Highlight");
			updateHighlight();
		}

		// (11) Frame slot is free (fence waited): recycle retired GPU buffers
		// + reset staging slice.
		const uint32_t frameIndex = frameCtx->frameIndex();
		resourceRetire.beginFrame(frameNumber);
		worldRenderer->arenas().beginFrame(frameNumber);
		stagingRing.beginFrame(frameIndex);

		// (12) ImGui build from sim/render snapshots (CPU only; draw later).
		if (imgui && !m_uiSpike)
		{
			PROFILE_SCOPE("DevUIBuild");
			imgui->beginFrame();
			drawUi();
			imgui->endFrame();
			syncUiInput();
		}

		// (13) Presentation uniforms (underwater sample + frame UBO).
		updateFrameUniforms(frameIndex, pixelW, pixelH, currentFrame);

		frameCtx->gpuProfiler().syncCapture(GetProfiler().captureEpoch());
		const int uploadBudget = uploadBudgetThisFrame;
		// Auto-exposure adaptation runs on the GPU with the real frame delta
		// (issue #140); clamped inside PostStack against long stalls.
		worldRenderer->setFrameDt(static_cast<float>(deltaTime));
		// (14) Record: passes + the frame's mesh uploads (no device idle).
		recordFrame(frameIndex, imageIndex, uploadBudget);

		// (15) Submit / present.
		{
			PROFILE_SCOPE("Present");
			if (!frameCtx->submitAndPresent(*swapchain, imageIndex))
				requestSwapchainRecreate();
		}

		// (16) Live memory/workload gauges for the developer console (issue
		// #179). Published AFTER this frame's record/upload work so the
		// CPU pool gauges reflect the uploads already executed and the
		// next UI refresh reads the last completed frame; the benchmark
		// sampler (below) then observes the same coherent values as
		// before the console existed. Profiled so the debug-tool
		// overhead stays visible.
		{
			PROFILE_SCOPE("TelemetryPublish");
			publishFrameTelemetry();
		}

		++frameNumber;

		GetProfiler().endFrame();
		if (m_remote && m_remote->motionTrace)
			traceRemoteMotion(rawDt);

		// Sync legacy RenderTiming from the frame we just closed (Streaming panel).
		renderTiming.frustumCulling = GetProfiler().lastScopeMs("Visibility");
		renderTiming.chunkGeneration = GetProfiler().lastScopeMs("GenDispatch");
		renderTiming.meshGeneration = GetProfiler().lastScopeMs("MeshDispatch");
		renderTiming.chunkRendering = GetProfiler().lastScopeMs("MeshUpload");
		renderTiming.uiRendering = GetProfiler().lastScopeMs("DevUIBuild") + GetProfiler().lastScopeMs("GameUIBuild");
		renderTiming.totalFrame = GetProfiler().lastFrameMs();

		sampleBenchmarkFrame();

		if (m_exitAfterBenchmark &&
			m_benchmark.phase() == BenchmarkPhase::Done)
		{
			std::cout << '\n' << m_benchmark.formatReportText();
			const std::string path =
				m_benchmark.saveReportToFile("benchmark-results");
			if (!path.empty())
				std::cout << "Benchmark saved: " << path << '\n';
			running = false;
		}
	}
}
