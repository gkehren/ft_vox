#include "Engine/Engine.hpp"

#include <SDL3/SDL_vulkan.h>
#include <utils.hpp>
#include <imgui/imgui.h>

#include <cstdio>
#include <cmath>
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
		// Vivid day fog (sky blue) → warm sunset → deep night (not grey).
		const glm::vec3 dayFog(0.52f, 0.74f, 0.98f);
		const glm::vec3 sunsetFog(0.95f, 0.48f, 0.28f);
		const glm::vec3 nightFog(0.04f, 0.06f, 0.14f);
		sp.fogColor = dayFog * sp.dayFactor + sunsetFog * sp.sunsetFactor + nightFog * sp.nightFactor;
		// Keep ambient high enough that grass stays green in shade
		sp.ambientStrength = 0.22f + 0.14f * sp.dayFactor + 0.06f * sp.sunsetFactor;
		sp.diffuseIntensity = 0.55f + 0.45f * sp.dayFactor + 0.12f * sp.sunsetFactor;
		sp.fogDensity = 0.08f + 0.12f * sp.nightFactor;
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

	// macOS: Homebrew's libvulkan is not on the default dyld search path, so
	// SDL_Vulkan_LoadLibrary(nullptr) often fails with "Failed to load Vulkan
	// Portability library" even when vulkaninfo works. Try well-known paths first.
	{
		const char *explicitPath = std::getenv("FT_VOX_VULKAN_LIB");
		bool loaded = false;
		if (explicitPath && explicitPath[0] != '\0')
		{
			loaded = SDL_Vulkan_LoadLibrary(explicitPath);
			if (!loaded)
				std::cerr << "FT_VOX_VULKAN_LIB failed (" << explicitPath << "): " << SDL_GetError() << "\n";
		}
#if defined(__APPLE__)
		static const char *kMacCandidates[] = {
			"/opt/homebrew/lib/libvulkan.1.dylib", // Apple Silicon Homebrew
			"/opt/homebrew/lib/libvulkan.dylib",
			"/usr/local/lib/libvulkan.1.dylib", // Intel Homebrew
			"/usr/local/lib/libvulkan.dylib",
			// Prefer the loader (not bare MoltenVK) so VK_ICD_FILENAMES works.
		};
		if (!loaded)
		{
			for (const char *path : kMacCandidates)
			{
				if (SDL_Vulkan_LoadLibrary(path))
				{
					std::cout << "Vulkan library: " << path << "\n";
					loaded = true;
					break;
				}
			}
		}
#endif
		if (!loaded)
			loaded = SDL_Vulkan_LoadLibrary(nullptr);
		if (!loaded)
		{
			throw std::runtime_error(
				std::string("Failed to load Vulkan library: ") + SDL_GetError() +
				"\n  On macOS install MoltenVK + loader and set:\n"
				"    export VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json\n"
				"  Optional: export FT_VOX_VULKAN_LIB=/opt/homebrew/lib/libvulkan.1.dylib");
		}
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

	stagingRing.init(vkContext->getAllocator(), TerrainRenderer::kMaxFramesInFlight);
	resourceRetire.init(vkContext->getAllocator(), TerrainRenderer::kMaxFramesInFlight);

	terrain = std::make_unique<TerrainRenderer>();
	terrain->init(*vkContext, *swapchain, *immediate);

	imgui = std::make_unique<ImGuiLayer>();
	imgui->init(window, *vkContext, *swapchain, *immediate);

	gameUi = std::make_unique<GameUI>();
	gameUi->init(*vkContext, *immediate);

	const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
	threadPool = std::make_unique<ThreadPool>(hw);
	chunkPool = std::make_unique<ChunkPool>(1600);

	renderSettings.minRenderDistance = 128;
	renderSettings.maxRenderDistance = 256;
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
	std::cout << "  C free mouse · F1–F5 panels · F10 VSync · Esc quit\n";
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
	terrain.reset();
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
												  chunkPool.get(), renderTiming);

	chunkManager->generateInitialArea(camera.getPosition(), kBootstrapRadius,
									  vkContext->getAllocator(), *immediate);

	{
		const glm::vec3 pos = camera.getPosition();
		for (int y = CHUNK_HEIGHT - 1; y > 0; --y)
		{
			if (chunkManager->isVoxelActive(glm::vec3(pos.x, static_cast<float>(y), pos.z)))
			{
				camera.setPosition(glm::vec3(pos.x, static_cast<float>(y + 3), pos.z));
				break;
			}
		}
	}

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
	if (showDemoPlayers && terrain)
		terrain->overlays().setPlayers(demoPlayers);

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
	if (!chunkManager)
		return;
	if (paused)
	{
		chunkManager->updateVisibility(camera, windowWidth, windowHeight, renderSettings);
		chunkManager->collectDrawList(drawList);
		chunkManager->collectShadowList(shadowList, camera, renderSettings.shadowDistance);
		uploadBudgetThisFrame = 0;
		return;
	}

	const double frameDt = std::min(dt, 0.05);

	chunkManager->processFinishedJobs();
	chunkManager->processDeferredReleases(resourceRetire);
	chunkManager->updateStreaming(camera, renderSettings);

	static double genAccum = 0.0, meshAccum = 0.0, uploadAccum = 0.0;
	const int loadBudget = budgetFromRate(renderSettings.loadPerSec, frameDt, streamAccum);
	const int genBudget = budgetFromRate(renderSettings.genPerSec, frameDt, genAccum);
	const int meshBudget = budgetFromRate(renderSettings.meshPerSec, frameDt, meshAccum);
	uploadBudgetThisFrame = std::max(budgetFromRate(renderSettings.uploadPerSec, frameDt, uploadAccum), 1);

	chunkManager->processChunkLoading(std::max(loadBudget, 1));
	chunkManager->generatePendingVoxels(camera, renderSettings, std::max(genBudget, 1));
	chunkManager->meshPendingChunks(camera, renderSettings, std::max(meshBudget, 1));
	// GPU uploads are recorded inside recordFrame (after acquire) — no waitIdle.
	chunkManager->updateVisibility(camera, windowWidth, windowHeight, renderSettings);
	chunkManager->collectDrawList(drawList);
	chunkManager->collectShadowList(shadowList, camera, renderSettings.shadowDistance);

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
	if (terrain)
		terrain->overlays().setHighlight(highlight);
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
				if (terrain)
					terrain->overlays().setShowChunkBorders(showChunkBorders);
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
			if (mouseCaptured && !(imgui && imgui->wantCaptureMouse()))
			{
				camera.processMouseMovement(static_cast<float>(event.motion.xrel),
											static_cast<float>(event.motion.yrel));
			}
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
			if (imgui && imgui->wantCaptureMouse())
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
	if (paused)
		return;
	if (imgui && imgui->wantCaptureKeyboard())
		return;
	const bool *keys = SDL_GetKeyboardState(nullptr);
	camera.processKeyboard(dt, keys);
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
	f.terrain = terrain.get();
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
	if (terrain)
		terrain->overlays().setShowChunkBorders(showChunkBorders);

	if (terrain)
		terrain->overlays().setPlayers(showDemoPlayers ? demoPlayers : std::vector<OverlayPlayer>{});
}

void Engine::run()
{
	running = true;
	lastFrame = SDL_GetTicks() / 1000.0;
	lastTime = lastFrame;

	const VkClearColorValue clearColor = {{0.38f, 0.58f, 0.92f, 1.0f}};

	while (running)
	{
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
			std::snprintf(title, sizeof(title),
						  "ft_vox — %.0f FPS | chunks %zu | draw %zu | seed %d%s",
						  fps,
						  chunkManager ? chunkManager->chunkCount() : 0,
						  drawList.size(), seed,
						  paused ? " [PAUSED]" : "");
			SDL_SetWindowTitle(window, title);
		}

		handleEvents();
		processInput(deltaTime);
		tickDayCycle(deltaTime);
		tickStreaming(deltaTime);
		updateHighlight();

		int pixelW = 0, pixelH = 0;
		SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);
		if (pixelW == 0 || pixelH == 0)
			continue;

		if (framebufferResized ||
			static_cast<uint32_t>(pixelW) != swapchain->getExtent().width ||
			static_cast<uint32_t>(pixelH) != swapchain->getExtent().height)
		{
			swapchain->recreate(static_cast<uint32_t>(pixelW), static_cast<uint32_t>(pixelH));
			terrain->onSwapchainRecreate(*swapchain);
			windowWidth = pixelW;
			windowHeight = pixelH;
			framebufferResized = false;
		}

		uint32_t imageIndex = 0;
		if (!terrain->beginAcquire(*swapchain, imageIndex, frameIndex))
		{
			framebufferResized = true;
			continue;
		}

		// Frame slot is free (fence waited): recycle retired GPU buffers + reset staging slice.
		resourceRetire.beginFrame(frameNumber);
		stagingRing.beginFrame(frameIndex);

		if (imgui)
		{
			imgui->beginFrame();
			drawUi();
			imgui->endFrame();
		}

		const float farPlane = static_cast<float>(renderSettings.maxRenderDistance) * 1.25f;
		terrain->updateFrameUBO(frameIndex, camera,
								static_cast<float>(pixelW), static_cast<float>(pixelH), farPlane,
								static_cast<float>(currentFrame), shaderParams);

		const int uploadBudget = uploadBudgetThisFrame;
		terrain->recordFrame(
			frameIndex, imageIndex, *swapchain, drawList, shadowList, clearColor,
			[&](VkCommandBuffer cmd) {
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

		if (!terrain->submitPresent(*swapchain, imageIndex, frameIndex))
			framebufferResized = true;

		frameIndex = (frameIndex + 1) % TerrainRenderer::kMaxFramesInFlight;
		++frameNumber;
	}
}
