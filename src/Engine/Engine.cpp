#include "Engine/Engine.hpp"

#include <SDL3/SDL_vulkan.h>
#include <Vulkan/VkLoadLibrary.hpp>
#include <utils.hpp>
#include <Chunk/StreamHelpers.hpp>
#include <Engine/Profiler.hpp>
#include <Engine/Benchmark.hpp>
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
int budgetFromRate(int perSec, double dt, double &accum)
{
	accum += static_cast<double>(perSec) * dt;
	int n = static_cast<int>(accum);
	if (n > 0)
		accum -= static_cast<double>(n);
	return std::clamp(n, 0, 64);
}

void updateAtmosphereFromDayTime(ShaderParameters &sp)
{
	const float dayTime = sp.dayTime;
	const float sunAngle = dayTime * 6.2831853f - 1.5707963f;
	const glm::vec3 sunDir =
		glm::normalize(glm::vec3(std::cos(sunAngle), std::sin(sunAngle) * 0.85f + 0.15f, 0.35f));
	sp.sunDirection = sunDir;
	sp.lightDirection = sunDir.y > 0.05f ? sunDir : -sunDir;

	sp.dayFactor = glm::smoothstep(-0.05f, 0.25f, sunDir.y);
	sp.nightFactor = glm::smoothstep(0.05f, -0.15f, sunDir.y);
	sp.sunsetFactor = glm::clamp(1.0f - std::abs(sunDir.y) * 3.0f, 0.0f, 1.0f) * (1.0f - sp.nightFactor);

	if (sp.automaticAtmosphere)
	{
		// Day fog: deeper blue (less milky white) → warm sunset → night
		const glm::vec3 dayFog(0.42f, 0.66f, 0.95f);
		const glm::vec3 sunsetFog(0.95f, 0.48f, 0.28f);
		const glm::vec3 nightFog(0.04f, 0.06f, 0.14f);
		sp.fogColor = dayFog * sp.dayFactor + sunsetFog * sp.sunsetFactor + nightFog * sp.nightFactor;
		// Shape-readable light without wash-out or neon
		sp.ambientStrength = 0.18f + 0.08f * sp.dayFactor + 0.04f * sp.sunsetFactor;
		sp.diffuseIntensity = 0.64f + 0.30f * sp.dayFactor + 0.10f * sp.sunsetFactor;
		sp.fogDensity = 0.045f + 0.06f * sp.nightFactor;
		sp.fogStart = 300.0f;
		sp.fogEnd = 880.0f;
	}

	// Approximate sun world position for debug display.
	sp.sunPosition = sp.celestialOrbitCenter + sunDir * sp.celestialOrbitRadius;
	sp.moonPosition = sp.celestialOrbitCenter - sunDir * sp.celestialOrbitRadius;
}
} // namespace

Engine::Engine()
	: camera(glm::vec3(0.0f, 90.0f, 0.0f))
{
	if (!SDL_Init(SDL_INIT_VIDEO))
		throw std::runtime_error(std::string("Failed to initialize SDL: ") + SDL_GetError());

	// Shared loader probe (VkLoadLibrary.hpp) — same path used by test_vulkan_resources.
	if (!loadVulkanLibrary(&std::cout))
	{
		throw std::runtime_error(std::string("Failed to load Vulkan library: ") + SDL_GetError() + "\n" +
								 vulkanLibraryLoadHint());
	}

	windowWidth = 1920;
	windowHeight = 1080;

	Uint32 windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;
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

	vkContext = std::make_unique<VkContext>();
	vkContext->init(window);

	swapchain = std::make_unique<VkSwapchain>();
	swapchain->init(*vkContext, static_cast<uint32_t>(windowWidth), static_cast<uint32_t>(windowHeight),
					renderSettings.vsyncEnabled);

	immediate = std::make_unique<ImmediateCommands>();
	immediate->init(*vkContext);

	stagingRing.init(vkContext->getAllocator(), VkFrameContext::kMaxFramesInFlight);
	resourceRetire.init(vkContext->getAllocator(), VkFrameContext::kMaxFramesInFlight);

	frameCtx = std::make_unique<VkFrameContext>();
	frameCtx->init(*vkContext);

	worldRenderer = std::make_unique<WorldRenderer>();
	worldRenderer->init(*vkContext, *swapchain, *immediate);

	imgui = std::make_unique<ImGuiLayer>();
	imgui->init(window, *vkContext, *swapchain, *immediate);

	gameUi = std::make_unique<GameUI>();
	gameUi->init(*vkContext, *immediate);

	const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
	threadPool = std::make_unique<ThreadPool>(hw);

	renderSettings.minRenderDistance = 128;
	renderSettings.maxRenderDistance = 256;
	// Pool sized for unload disk (1.5× view) + headroom; grows later via ensureCapacity.
	chunkPool = std::make_unique<ChunkPool>(estimateChunkPoolCapacity(renderSettings.maxRenderDistance));

	renderSettings.loadPerSec = 120;
	renderSettings.genPerSec = 80;
	renderSettings.meshPerSec = 60;
	renderSettings.uploadPerSec = 100;
	renderSettings.shadowDistance = 160.f;
	renderSettings.raycastDistance = 8;

	camera.setWindow(window);
	camera.setMovementSpeed(20.f);
	SDL_SetWindowRelativeMouseMode(window, true);

	updateAtmosphereFromDayTime(shaderParams);

	SDL_ShowWindow(window);

	std::cout << "SDL version: " << SDL_GetVersion() << "\n";
	std::cout << "ft_vox: Vulkan — streaming procedural world\n";
	std::cout << "  WASD fly · mouse look · LMB/RMB edit · T block · B borders\n";
	std::cout << "  C free mouse · F1–F7 panels · F10 VSync · Esc quit\n";
	std::cout << "  ThreadPool workers: " << hw << "\n";
}

Engine::~Engine()
{
	if (vkContext)
		vkContext->waitIdle();

	resourceRetire.flush();
	resourceRetire.shutdown();
	stagingRing.shutdown();

	gameUi.reset();
	chunkManager.reset();
	chunkPool.reset();
	threadPool.reset();
	terrainGenerator.reset();

	imgui.reset();
	worldRenderer.reset();
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

void Engine::initializeNoiseGenerator(int seed_val)
{
	if (seed_val <= 0)
	{
		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<> dis(100000, 999999);
		seed = dis(gen);
	}
	else
	{
		seed = seed_val;
	}

	terrainGenerator = std::make_unique<TerrainGenerator>(seed);
	chunkManager = std::make_unique<ChunkManager>(terrainGenerator.get(), threadPool.get(),
												  chunkPool.get());

	chunkManager->generateInitialArea(camera.getPosition(), kBootstrapRadius,
									  vkContext->getAllocator(), *immediate);
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
			if (chunkManager->isVoxelActive(glm::vec3(p.position.x, static_cast<float>(y), p.position.z)))
			{
				p.position.y = static_cast<float>(y + 1);
				break;
			}
		}
	}
	if (showDemoPlayers && worldRenderer)
		worldRenderer->overlays().setPlayers(demoPlayers);

	std::cout << "World seed: " << seed
			  << " chunks=" << chunkManager->chunkCount()
			  << " view=" << renderSettings.maxRenderDistance << "\n";
}

void Engine::setVSync(bool enabled)
{
	renderSettings.vsyncEnabled = enabled;
	if (swapchain)
		swapchain->setVSync(enabled);
}

void Engine::placeCameraOnSurface()
{
	const glm::vec3 pos = camera.getPosition();
	for (int y = CHUNK_HEIGHT - 1; y > 0; --y)
	{
		if (chunkManager &&
			chunkManager->isVoxelActive(glm::vec3(pos.x, static_cast<float>(y), pos.z)))
		{
			camera.setPosition(glm::vec3(pos.x, static_cast<float>(y + 3), pos.z));
			return;
		}
	}
	camera.setPosition(glm::vec3(pos.x, 100.f, pos.z));
}

void Engine::reloadWorld(int newSeed)
{
	if (!vkContext || !threadPool || !chunkPool || !immediate)
		return;

	seed = newSeed > 0 ? newSeed : 42;
	drawList.clear();
	shadowList.clear();

	vkContext->waitIdle();
	resourceRetire.flush();
	chunkManager.reset();
	terrainGenerator = std::make_unique<TerrainGenerator>(seed);
	chunkManager = std::make_unique<ChunkManager>(terrainGenerator.get(), threadPool.get(),
												  chunkPool.get());

	camera.setMode(CameraMode::PERSPECTIVE);
	camera.setPosition(glm::vec3(0.f, 100.f, 0.f));
	camera.setMovementSpeed(20.f);

	chunkManager->generateInitialArea(camera.getPosition(), kBootstrapRadius,
									  vkContext->getAllocator(), *immediate);
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
			if (chunkManager->isVoxelActive(glm::vec3(p.position.x, static_cast<float>(y), p.position.z)))
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

	std::cout << "[bench] World reloaded seed=" << seed
			  << " chunks=" << chunkManager->chunkCount() << "\n";
}

void Engine::onResize(int width, int height)
{
	if (width <= 0 || height <= 0)
		return;
	windowWidth = width;
	windowHeight = height;
	framebufferResized = true;
}

void Engine::tickDayCycle(double dt)
{
	if (paused)
		return;
	if (shaderParams.dayCycleEnabled)
	{
		shaderParams.dayTime = std::fmod(shaderParams.dayTime + static_cast<float>(dt) * shaderParams.dayCycleSpeed, 1.0f);
		if (shaderParams.dayTime < 0.f)
			shaderParams.dayTime += 1.f;
	}
	updateAtmosphereFromDayTime(shaderParams);
}

void Engine::tickStreaming(double dt)
{
	PROFILE_SCOPE("Streaming");
	if (!chunkManager)
		return;
	if (paused)
	{
		{
			PROFILE_SCOPE("Visibility");
			chunkManager->updateVisibility(camera, windowWidth, windowHeight, renderSettings);
		}
		{
			PROFILE_SCOPE("CollectLists");
			chunkManager->collectDrawList(drawList);
			chunkManager->collectShadowList(shadowList, camera, renderSettings.shadowDistance);
		}
		uploadBudgetThisFrame = 0;
		return;
	}

	// Grow pool when the view-distance slider (or other settings) outgrows the free list.
	// Cheap no-op when already large enough; pointer-stable so loaded chunks stay valid.
	if (chunkPool)
		chunkPool->ensureCapacity(estimateChunkPoolCapacity(renderSettings.maxRenderDistance));

	const double frameDt = std::min(dt, 0.05);
	const auto streamT0 = std::chrono::steady_clock::now();
	const auto streamElapsedMs = [&]() {
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - streamT0).count();
	};
	const double maxStreamMs = static_cast<double>(renderSettings.maxStreamMs);

	{
		PROFILE_SCOPE("FinishedJobs");
		chunkManager->processFinishedJobs();
	}
	{
		PROFILE_SCOPE("DeferredRelease");
		chunkManager->processDeferredReleases(resourceRetire);
	}
	{
		PROFILE_SCOPE("UpdateStreaming");
		chunkManager->updateStreaming(camera, renderSettings);
	}

	static double genAccum = 0.0, meshAccum = 0.0, uploadAccum = 0.0;
	const int loadBudget = budgetFromRate(renderSettings.loadPerSec, frameDt, streamAccum);
	const int genBudget = budgetFromRate(renderSettings.genPerSec, frameDt, genAccum);
	const int meshBudget = budgetFromRate(renderSettings.meshPerSec, frameDt, meshAccum);
	uploadBudgetThisFrame = std::max(budgetFromRate(renderSettings.uploadPerSec, frameDt, uploadAccum), 1);

	// Count budgets capped by shared per-frame CPU time envelope.
	const int loadN = remainingCountBudget(std::max(loadBudget, 1), streamElapsedMs(), maxStreamMs);
	if (loadN > 0)
	{
		PROFILE_SCOPE("Load");
		chunkManager->processChunkLoading(loadN);
	}
	const int genN = remainingCountBudget(std::max(genBudget, 1), streamElapsedMs(), maxStreamMs);
	if (genN > 0)
	{
		PROFILE_SCOPE("GenDispatch");
		chunkManager->generatePendingVoxels(camera, renderSettings, genN);
	}
	const int meshN = remainingCountBudget(std::max(meshBudget, 1), streamElapsedMs(), maxStreamMs);
	if (meshN > 0)
	{
		PROFILE_SCOPE("MeshDispatch");
		chunkManager->meshPendingChunks(camera, renderSettings, meshN);
	}
	// GPU uploads are recorded inside recordFrame (after acquire) — no waitIdle.
	{
		PROFILE_SCOPE("Visibility");
		chunkManager->updateVisibility(camera, windowWidth, windowHeight, renderSettings);
	}
	{
		PROFILE_SCOPE("CollectLists");
		chunkManager->collectDrawList(drawList);
		chunkManager->collectShadowList(shadowList, camera, renderSettings.shadowDistance);
	}

	static size_t lastLoggedChunks = 0;
	static double lastLogTime = 0.0;
	const double now = SDL_GetTicks() / 1000.0;
	const size_t n = chunkManager->chunkCount();
	if (n != lastLoggedChunks && now - lastLogTime > 1.0)
	{
		std::cout << "[stream] chunks=" << n
				  << " draw=" << drawList.size()
				  << " shadow=" << shadowList.size()
				  << " q="
				  << chunkManager->pendingLoadCount() << "/"
				  << chunkManager->pendingGenJobs() << "/"
				  << chunkManager->pendingMeshJobs() << "\n";
		lastLoggedChunks = n;
		lastLogTime = now;
	}
}

bool Engine::raycastVoxel(glm::vec3 &outBlock, glm::vec3 &outPrevious)
{
	if (!chunkManager)
		return false;

	const glm::vec3 origin = camera.getPosition();
	const glm::vec3 dir = glm::normalize(camera.getFront());
	const float maxDist = static_cast<float>(renderSettings.raycastDistance);
	const float step = 0.05f;

	glm::vec3 prev = origin;
	for (float t = 0.f; t <= maxDist; t += step)
	{
		const glm::vec3 p = origin + dir * t;
		const glm::vec3 block(std::floor(p.x), std::floor(p.y), std::floor(p.z));

		if (chunkManager->isVoxelActive(block))
		{
			outBlock = block;
			outPrevious = glm::vec3(std::floor(prev.x), std::floor(prev.y), std::floor(prev.z));
			if (outPrevious == outBlock)
				outPrevious = block - glm::vec3(dir.x > 0 ? 1.f : (dir.x < 0 ? -1.f : 0.f),
												dir.y > 0 ? 1.f : (dir.y < 0 ? -1.f : 0.f),
												dir.z > 0 ? 1.f : (dir.z < 0 ? -1.f : 0.f));
			return true;
		}
		prev = p;
	}
	return false;
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
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		if (imgui)
			imgui->processEvent(event);

		switch (event.type)
		{
		case SDL_EVENT_QUIT:
			running = false;
			break;
		case SDL_EVENT_WINDOW_RESIZED:
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
			onResize(event.window.data1, event.window.data2);
			break;
		case SDL_EVENT_KEY_DOWN:
		{
			const SDL_Keycode key = event.key.key;

			// Function / panel shortcuts work even when ImGui wants keyboard.
			if (gameUi)
			{
				GameUIFrame dummy{};
				dummy.render = &renderSettings;
				dummy.paused = &paused;
				dummy.setVSync = [this](bool v) { setVSync(v); };
				if (gameUi->handleShortcut(static_cast<int>(key), dummy))
					break;
			}

			if (key == SDLK_ESCAPE)
			{
				running = false;
				break;
			}
			if (key == SDLK_C)
			{
				mouseCaptured = !mouseCaptured;
				SDL_SetWindowRelativeMouseMode(window, mouseCaptured);
				break;
			}
			if (key == SDLK_B)
			{
				showChunkBorders = !showChunkBorders;
				if (worldRenderer)
					worldRenderer->overlays().setShowChunkBorders(showChunkBorders);
				break;
			}
			if (key == SDLK_T && !(imgui && imgui->wantCaptureKeyboard()))
			{
				int next = static_cast<int>(selectedTexture) + 1;
				if (next >= static_cast<int>(COUNT))
					next = 0;
				// Skip AIR
				if (static_cast<TextureType>(next) == AIR)
					next = 1;
				selectedTexture = static_cast<TextureType>(next);
				break;
			}
			break;
		}
		case SDL_EVENT_MOUSE_MOTION:
			if (m_benchmark.locksInput())
				break;
			if (mouseCaptured && !(imgui && imgui->wantCaptureMouse()))
			{
				camera.processMouseMovement(static_cast<float>(event.motion.xrel),
											static_cast<float>(event.motion.yrel));
			}
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
			if (imgui && imgui->wantCaptureMouse())
				break;
			if (m_benchmark.locksInput())
				break;
			if (!mouseCaptured || !chunkManager || paused)
				break;
			{
				glm::vec3 block, prev;
				if (!raycastVoxel(block, prev))
					break;
				if (event.button.button == SDL_BUTTON_LEFT)
					chunkManager->deleteVoxel(block);
				else if (event.button.button == SDL_BUTTON_RIGHT)
					chunkManager->placeVoxel(prev, selectedTexture);
			}
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			if (camera.getMode() == CameraMode::ISOMETRIC &&
				!(imgui && imgui->wantCaptureMouse()))
			{
				camera.addIsometricZoom(static_cast<float>(event.wheel.y) * 4.f);
			}
			break;
		default:
			break;
		}
	}
}

void Engine::processInput(double dt)
{
	if (m_benchmark.locksInput())
		return;
	if (paused)
		return;
	if (imgui && imgui->wantCaptureKeyboard())
		return;
	const bool *keys = SDL_GetKeyboardState(nullptr);
	camera.processKeyboard(dt, keys);
}

void Engine::tickBenchmark(double dt)
{
	if (m_benchmark.phase() == BenchmarkPhase::Reloading)
	{
		const BenchmarkConfig &cfg = m_benchmark.config();
		if (cfg.forceVsyncOff)
		{
			m_benchmark.markForceVsync(renderSettings.vsyncEnabled);
			if (renderSettings.vsyncEnabled)
				setVSync(false);
		}
		m_benchmark.setSettingsSnapshot(
			renderSettings.maxRenderDistance, windowWidth, windowHeight,
			renderSettings.vsyncEnabled,
			vkContext ? vkContext->getDeviceProperties().deviceName : nullptr);

		reloadWorld(cfg.seed);
		m_benchmark.onWorldReady(camera.getPosition());
		// Place camera on first path point
		m_benchmark.tick(0.0, camera);
		return;
	}

	if (m_benchmark.phase() == BenchmarkPhase::Warmup ||
		m_benchmark.phase() == BenchmarkPhase::Running)
	{
		m_benchmark.tick(dt, camera);
	}

	// Restore VSync after done/cancel if we forced it off
	if (!m_benchmark.isActive() && m_benchmark.needsVsyncRestore())
	{
		setVSync(m_benchmark.prevVsync());
		m_benchmark.clearVsyncRestore();
	}
}

void Engine::sampleBenchmarkFrame()
{
	if (m_benchmark.phase() != BenchmarkPhase::Running)
		return;

	Profiler &prof = GetProfiler();
	uint64_t tJobs = 0, mJobs = 0, lJobs = 0;
	float tMs = 0.f, mMs = 0.f, lMs = 0.f;
	const int wc = prof.workerSnapshotCount();
	const WorkerSnapshot *ws = prof.workerSnapshots();
	for (int i = 0; i < wc; ++i)
	{
		if (!ws[i].name)
			continue;
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
	}

	m_benchmark.sampleFrame(
		prof.lastFrameMs(), prof.lastScopeMs("Streaming"), prof.lastScopeMs("Acquire"),
		prof.lastScopeMs("Record"), prof.lastScopeMs("ImGui"), prof.lastScopeMs("Present"),
		prof.lastScopeMs("Visibility"), prof.lastScopeMs("MeshUpload"),
		chunkManager ? chunkManager->chunkCount() : 0, drawList.size(),
		chunkManager ? chunkManager->pendingLoadCount() : 0,
		chunkManager ? chunkManager->pendingGenJobs() : 0,
		chunkManager ? chunkManager->pendingMeshJobs() : 0, tJobs, tMs, mJobs, mMs, lJobs, lMs);
}

void Engine::drawUi()
{
	if (!imgui || !gameUi)
		return;

	const bool prevCapture = mouseCaptured;

	GameUIFrame f{};
	f.camera = &camera;
	f.chunks = chunkManager.get();
	f.pool = chunkPool.get();
	f.generator = terrainGenerator.get();
	f.worldRenderer = worldRenderer.get();
	f.vk = vkContext.get();
	f.imm = immediate.get();
	f.shader = &shaderParams;
	f.render = &renderSettings;
	f.timing = &renderTiming;
	f.selectedTexture = &selectedTexture;
	f.highlight = &highlight;
	f.mouseCaptured = &mouseCaptured;
	f.showChunkBorders = &showChunkBorders;
	f.showDemoPlayers = &showDemoPlayers;
	f.paused = &paused;
	f.seed = seed;
	f.fps = static_cast<float>(fps > 0.0 ? fps : ImGui::GetIO().Framerate);
	f.frameMs = static_cast<float>(deltaTime * 1000.0);
	f.drawCount = drawList.size();
	f.windowW = windowWidth;
	f.windowH = windowHeight;
	f.benchmark = &m_benchmark;
	if (vkContext)
	{
		f.deviceName = vkContext->getDeviceProperties().deviceName;
		f.vkApiVersion = vkContext->getDeviceProperties().apiVersion;
		f.validation = vkContext->isValidationEnabled();
	}
	f.setVSync = [this](bool v) { setVSync(v); };

	gameUi->draw(f);

	if (mouseCaptured != prevCapture)
		SDL_SetWindowRelativeMouseMode(window, mouseCaptured);
	if (worldRenderer)
		worldRenderer->overlays().setShowChunkBorders(showChunkBorders);

	if (worldRenderer)
		worldRenderer->overlays().setPlayers(showDemoPlayers ? demoPlayers : std::vector<OverlayPlayer>{});
}

void Engine::run()
{
	running = true;
	lastFrame = SDL_GetTicks() / 1000.0;
	lastTime = lastFrame;

	const VkClearColorValue clearColor = {{0.38f, 0.58f, 0.92f, 1.0f}};

	while (running)
	{
		GetProfiler().beginFrame();

		const double currentFrame = SDL_GetTicks() / 1000.0;
		deltaTime = currentFrame - lastFrame;
		lastFrame = currentFrame;

		frameCount++;
		if (currentFrame - lastTime >= 1.0)
		{
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
						  chunkManager ? chunkManager->chunkCount() : 0,
						  drawList.size(), seed,
						  paused ? " [PAUSED]" : "", benchTag);
			SDL_SetWindowTitle(window, title);
		}

		{
			PROFILE_SCOPE("Events");
			handleEvents();
		}
		{
			PROFILE_SCOPE("Benchmark");
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
		tickStreaming(deltaTime);
		{
			PROFILE_SCOPE("Highlight");
			updateHighlight();
		}

		int pixelW = 0, pixelH = 0;
		SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);
		if (pixelW == 0 || pixelH == 0)
		{
			GetProfiler().endFrame();
			continue;
		}

		if (framebufferResized ||
			static_cast<uint32_t>(pixelW) != swapchain->getExtent().width ||
			static_cast<uint32_t>(pixelH) != swapchain->getExtent().height)
		{
			PROFILE_SCOPE("Resize");
			swapchain->recreate(static_cast<uint32_t>(pixelW), static_cast<uint32_t>(pixelH));
			worldRenderer->onSwapchainRecreate(*swapchain);
			windowWidth = pixelW;
			windowHeight = pixelH;
			framebufferResized = false;
		}

		uint32_t imageIndex = 0;
		{
			PROFILE_SCOPE("Acquire");
			if (!frameCtx->beginFrame(*swapchain, imageIndex))
			{
				framebufferResized = true;
				GetProfiler().endFrame();
				continue;
			}
		}

		const uint32_t frameIndex = frameCtx->frameIndex();
		// Frame slot is free (fence waited): recycle retired GPU buffers + reset staging slice.
		resourceRetire.beginFrame(frameNumber);
		stagingRing.beginFrame(frameIndex);

		if (imgui)
		{
			PROFILE_SCOPE("ImGui");
			imgui->beginFrame();
			drawUi();
			imgui->endFrame();
		}

		{
			PROFILE_SCOPE("UpdateUBO");
			const float farPlane = static_cast<float>(renderSettings.maxRenderDistance) * 1.25f;
			// Underwater: camera inside a water voxel (coarse sample via chunk manager)
			bool underwater = false;
			if (chunkManager)
			{
				const glm::vec3 eye = camera.getPosition();
				if (Chunk *ch = chunkManager->getChunkAtWorldPos(eye))
				{
					const int chunkX = static_cast<int>(std::floor(eye.x / static_cast<float>(CHUNK_SIZE)));
					const int chunkZ = static_cast<int>(std::floor(eye.z / static_cast<float>(CHUNK_SIZE)));
					const int lx = static_cast<int>(std::floor(eye.x)) - chunkX * CHUNK_SIZE;
					const int ly = static_cast<int>(std::floor(eye.y));
					const int lz = static_cast<int>(std::floor(eye.z)) - chunkZ * CHUNK_SIZE;
					if (lx >= 0 && lx < CHUNK_SIZE && ly >= 0 && ly < CHUNK_HEIGHT && lz >= 0 && lz < CHUNK_SIZE)
						underwater = (static_cast<TextureType>(ch->getVoxel(lx, ly, lz).type) == WATER);
				}
			}
			worldRenderer->postSettings().underwater = underwater;
			worldRenderer->updateFrameUBO(frameIndex, camera,
									static_cast<float>(pixelW), static_cast<float>(pixelH), farPlane,
									static_cast<float>(currentFrame), shaderParams,
									renderSettings.shadowCascadeFar, underwater);
		}

		const int uploadBudget = uploadBudgetThisFrame;
		{
			PROFILE_SCOPE("Record");
			worldRenderer->recordFrame(
				frameCtx->commandBuffer(), frameIndex, imageIndex, *swapchain, drawList, shadowList, clearColor,
				[&](VkCommandBuffer cmd) {
					PROFILE_SCOPE("MeshUpload");
					if (!chunkManager || uploadBudget <= 0 || paused)
						return;
					const int n = chunkManager->uploadPendingMeshes(
						vkContext->getAllocator(), stagingRing, cmd, resourceRetire, camera, uploadBudget);
					if (n <= 0)
						return;
					// Make staged mesh data visible to vertex/index fetch in later passes.
					VkMemoryBarrier barrier{};
					barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
					barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
					barrier.dstAccessMask =
						VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
					vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
										 VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier, 0, nullptr,
										 0, nullptr);
				},
				[&](VkCommandBuffer cmd) {
					if (imgui)
						imgui->recordDraw(cmd);
				});
		}

		{
			PROFILE_SCOPE("Present");
			if (!frameCtx->submitAndPresent(*swapchain, imageIndex))
				framebufferResized = true;
		}

		++frameNumber;

		GetProfiler().endFrame();

		// Sync legacy RenderTiming from the frame we just closed (Streaming panel).
		renderTiming.frustumCulling = GetProfiler().lastScopeMs("Visibility");
		renderTiming.chunkGeneration = GetProfiler().lastScopeMs("GenDispatch");
		renderTiming.meshGeneration = GetProfiler().lastScopeMs("MeshDispatch");
		renderTiming.chunkRendering = GetProfiler().lastScopeMs("MeshUpload");
		renderTiming.uiRendering = GetProfiler().lastScopeMs("ImGui");
		renderTiming.totalFrame = GetProfiler().lastFrameMs();

		sampleBenchmarkFrame();
	}
}
