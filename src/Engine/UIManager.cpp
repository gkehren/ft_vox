#include "UIManager.hpp"
#include "Engine.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <Chunk/ChunkPool.hpp>

UIManager::UIManager(Engine *engineInstance, SDL_Window *window, int &windowWidth, int &windowHeight)
	: engine(engineInstance), sdlWindow(window), winWidth(windowWidth), winHeight(windowHeight)
{
	if (engine)
	{
		selectedTexture = engine->getSelectedTexture();
	}
}

UIManager::~UIManager()
{
	if (biomeMap.textureID)
	{
		glDeleteTextures(1, &biomeMap.textureID);
		biomeMap.textureID = 0;
	}
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();
}

void UIManager::init(SDL_GLContext glContext)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	(void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	ImGui::StyleColorsDark();

	ImGui_ImplSDL3_InitForOpenGL(sdlWindow, glContext);
	ImGui_ImplOpenGL3_Init("#version 460");
}

void UIManager::processEvent(SDL_Event *event)
{
	ImGui_ImplSDL3_ProcessEvent(event);
}

void UIManager::update()
{
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();

	ImGui::Begin("ft_vox");

	ImGui::Text("FPS: %.1f (%.1f ms)", ImGui::GetIO().Framerate, engine->getDeltaTime() * 1000.0f);
	ImGui::Text("Visible chunks: %d", renderSettings.visibleChunksCount);
	ImGui::Text("Voxel count: %d", renderSettings.visibleVoxelsCount);
	ImGui::SetNextItemWidth(180);
	if (ImGui::SliderInt("Min render dist", &renderSettings.minRenderDistance, 32, renderSettings.maxRenderDistance))
	{
		renderSettings.minRenderDistance = std::min(renderSettings.minRenderDistance, renderSettings.maxRenderDistance);
	}
	ImGui::SetNextItemWidth(180);
	if (ImGui::SliderInt("Max render dist", &renderSettings.maxRenderDistance, renderSettings.minRenderDistance, 4096))
	{
		renderSettings.maxRenderDistance = std::max(renderSettings.maxRenderDistance, renderSettings.minRenderDistance);
		if (engine && engine->getRenderer())
			engine->getRenderer()->setRenderDistance(static_cast<float>(renderSettings.maxRenderDistance));
	}
	if (engine)
	{
		const auto &camPos = engine->getCamera().getPosition();
		ImGui::Text("X/Y/Z: (%.1f, %.1f, %.1f)", camPos.x, camPos.y, camPos.z);
		const auto &playerChunkPos = engine->getPlayerChunkPos();
		ImGui::Text("Player chunk: (%d, %d)", playerChunkPos.x, playerChunkPos.y);
		if (auto *gen = engine->getTerrainGenerator())
		{
			BiomeType biome = gen->getBiomeAt(
				static_cast<int>(std::floor(camPos.x)),
				static_cast<int>(std::floor(camPos.z)));
			ImGui::Text("Biome: %s", biomeTypeString[biome]);
		}
		ImGui::Text("Speed: %.1f", engine->getCamera().getMovementSpeed());
	}
	ImGui::Text("Selected texture: %s (%d)", textureTypeString.at(selectedTexture).c_str(), static_cast<int>(selectedTexture));
	ImGui::InputInt("Raycast distance", &renderSettings.raycastDistance);

	if (ImGui::Checkbox("Wireframe", &renderSettings.wireframeMode))
	{
		engine->setWireframeMode(renderSettings.wireframeMode); // Notify engine
	}
	if (ImGui::Checkbox("VSync", &renderSettings.vsyncEnabled))
	{
		engine->setVSync(renderSettings.vsyncEnabled); // Update SDL interval on toggle
	}
	ImGui::Checkbox("Chunk borders", &renderSettings.chunkBorders);
	ImGui::Checkbox("Pause", &renderSettings.paused);

	ImGui::Separator();
	ImGui::Text("Chunk pipeline (per second)");
	ImGui::SetNextItemWidth(120);
	ImGui::InputInt("Load/s", &renderSettings.loadPerSec);
	ImGui::SetNextItemWidth(120);
	ImGui::InputInt("Gen/s", &renderSettings.genPerSec);
	ImGui::SetNextItemWidth(120);
	ImGui::InputInt("Mesh/s", &renderSettings.meshPerSec);
	ImGui::SetNextItemWidth(120);
	ImGui::InputInt("Upload/s", &renderSettings.uploadPerSec);
	renderSettings.loadPerSec = std::max(1, renderSettings.loadPerSec);
	renderSettings.genPerSec = std::max(1, renderSettings.genPerSec);
	renderSettings.meshPerSec = std::max(1, renderSettings.meshPerSec);
	renderSettings.uploadPerSec = std::max(1, renderSettings.uploadPerSec);

	ImGui::Text("Screen size: %d x %d", winWidth, winHeight);

	ImGui::Separator();
	if (engine && engine->getChunkManager() && engine->getChunkManager()->getChunkPool())
	{
		ChunkPool *pool = engine->getChunkManager()->getChunkPool();
		ImGui::Text("Chunk Pool Stats:");
		ImGui::Text("  Capacity: %zu", pool->capacity());
		ImGui::Text("  Acquired: %zu", pool->acquiredCount());
		ImGui::Text("  Free: %zu", pool->freeCount());
		if (pool->overflowCount() > 0)
			ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "  Overflow: %zu", pool->overflowCount());
		else
			ImGui::Text("  Overflow: %zu", pool->overflowCount());
	}

	ImGui::Separator();

	if (ImGui::CollapsingHeader("Performance"))
	{
		if (ImGui::BeginTable("Performance", 2, ImGuiTableFlags_Borders))
		{
			ImGui::TableSetupColumn("Operation");
			ImGui::TableSetupColumn("Time (ms)");
			ImGui::TableHeadersRow();

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("Frustum culling");
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", renderTiming.frustumCulling);

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("Chunk generation");
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", renderTiming.chunkGeneration);

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("Mesh generation");
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", renderTiming.meshGeneration);

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("Chunk rendering");
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", renderTiming.chunkRendering);

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("Total frame");
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", renderTiming.totalFrame);

			ImGui::EndTable();
		}
	}

	ImGui::End(); // End "ft_vox" window

	handleServerControls();
	handleShaderParametersWindow(); // Added call
	renderBiomeMap();				// This also calls updateBiomeMap if needed
}

void UIManager::render()
{
	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void UIManager::handleServerControls()
{
	ImGui::Begin("Network Controls"); // Always begin the window

	if (!engine)
	{
		ImGui::Text("Engine instance not available.");
		ImGui::End();
		return;
	}

	if (engine->getServer())
	{
		ImGui::Text("Running as SERVER");
		ImGui::Text("Connected clients: %zu", engine->getServer()->getClientCount());
		if (ImGui::Button("Stop Server"))
		{
			engine->stopServer();
		}
	}
	else if (engine->getClient())
	{
		if (engine->getClient()->isConnected())
		{
			ImGui::Text("Connected to SERVER at %s", ipInputBuffer);
			ImGui::Text("Player ID: %d", engine->getClient()->getPlayerId());
			if (ImGui::Button("Disconnect"))
			{
				engine->disconnectClient();
			}
		}
		else
		{
			ImGui::Text("Attempting to connect to %s...", ipInputBuffer);
			if (ImGui::Button("Cancel Connection"))
			{
				engine->disconnectClient(); // Or a more specific cancel
			}
		}
	}
	else // Neither server nor client is active, but engine is valid
	{
		ImGui::InputText("Server IP", ipInputBuffer, sizeof(ipInputBuffer));
		if (ImGui::Button("Connect to Server"))
		{
			engine->connectToServer(ipInputBuffer);
		}
		ImGui::SameLine();
		if (ImGui::Button("Start Server"))
		{
			engine->startServer();
		}
	}
	ImGui::End();
}

void UIManager::handleShaderParametersWindow()
{
	ImGui::Begin("Shader Parameters");

	if (ImGui::CollapsingHeader("Fog"))
	{
		ImGui::SliderFloat("Fog Start", &shaderParams.fogStart, 0.0f, 1000.0f);
		ImGui::SliderFloat("Fog End", &shaderParams.fogEnd, 0.0f, 1000.0f);
		ImGui::SliderFloat("Fog Density", &shaderParams.fogDensity, 0.0f, 1.0f);
		ImGui::ColorEdit3("Fog Color", (float *)&shaderParams.fogColor);
	}

	if (ImGui::CollapsingHeader("Lighting"))
	{
		ImGui::Checkbox("Day/Night Cycle", &shaderParams.dayCycleEnabled);
		ImGui::SliderFloat("Day Time", &shaderParams.dayTime, 0.0f, 1.0f, "%.3f");
		ImGui::SliderFloat("Cycle Speed", &shaderParams.dayCycleSpeed, 0.0f, 0.05f, "%.5f");

		if (!shaderParams.dayCycleEnabled)
		{
			ImGui::SliderFloat3("Sun Direction", (float *)&shaderParams.sunDirection, -1.0f, 1.0f);
			shaderParams.sunDirection = glm::normalize(shaderParams.sunDirection); // Normalize after edit
		}
		ImGui::SliderFloat("Ambient Strength", &shaderParams.ambientStrength, 0.0f, 1.0f);
		ImGui::SliderFloat("Diffuse Intensity", &shaderParams.diffuseIntensity, 0.0f, 1.0f);
		ImGui::SliderFloat("Light Levels", &shaderParams.lightLevels, 1.0f, 16.0f);
	}

	if (ImGui::CollapsingHeader("Visual"))
	{
		ImGui::SliderFloat("Saturation Level", &shaderParams.saturationLevel, 0.0f, 3.0f);
		ImGui::SliderFloat("Color Boost", &shaderParams.colorBoost, 0.0f, 3.0f);
		ImGui::SliderFloat("Gamma", &shaderParams.gamma, 0.1f, 3.0f);
	}

	if (ImGui::CollapsingHeader("Post Processing"))
	{
		ImGui::Checkbox("Bloom", &postProcessSettings.bloomEnabled);
		if (postProcessSettings.bloomEnabled)
		{
			ImGui::SliderFloat("Bloom Threshold", &postProcessSettings.bloomThreshold, 0.0f, 5.0f);
			ImGui::SliderFloat("Bloom Intensity", &postProcessSettings.bloomIntensity, 0.0f, 2.0f);
		}
		ImGui::Checkbox("FXAA", &postProcessSettings.fxaaEnabled);
		ImGui::SliderFloat("Exposure", &postProcessSettings.exposure, 0.1f, 5.0f);
		ImGui::SliderFloat("PP Gamma", &postProcessSettings.gamma, 0.5f, 4.0f);
		const char *toneMappers[] = {"ACES Filmic", "Reinhard"};
		ImGui::Combo("Tone Mapper", &postProcessSettings.toneMapper, toneMappers, IM_ARRAYSIZE(toneMappers));

		ImGui::Separator();
		ImGui::Checkbox("God Rays", &postProcessSettings.godRaysEnabled);
		if (postProcessSettings.godRaysEnabled)
		{
			ImGui::SliderFloat("GR Density", &postProcessSettings.godRaysDensity, 0.1f, 3.0f);
			ImGui::SliderFloat("GR Weight", &postProcessSettings.godRaysWeight, 0.001f, 0.05f, "%.4f");
			ImGui::SliderFloat("GR Decay", &postProcessSettings.godRaysDecay, 0.9f, 1.0f, "%.3f");
			ImGui::SliderFloat("GR Exposure", &postProcessSettings.godRaysExposure, 0.0f, 1.0f);
		}
	}

	ImGui::End();
}

// Biome map colors indexed by BiomeType
static constexpr unsigned char biomeColors[BIOME_COUNT][3] = {
	{100, 150, 200}, // BIOME_FROZEN_OCEAN   - icy blue
	{220, 230, 240}, // BIOME_SNOWY_TUNDRA   - near white
	{130, 160, 180}, // BIOME_SNOWY_TAIGA    - blue-grey
	{160, 210, 230}, // BIOME_ICE_SPIKES     - cyan-white
	{30, 100, 180},	 // BIOME_OCEAN          - deep blue
	{210, 200, 150}, // BIOME_BEACH          - sandy
	{140, 200, 90},	 // BIOME_PLAINS         - light green
	{50, 130, 50},	 // BIOME_FOREST         - green
	{100, 170, 80},	 // BIOME_BIRCH_FOREST   - pale green
	{30, 80, 30},	 // BIOME_DARK_FOREST    - dark green
	{80, 100, 60},	 // BIOME_SWAMP          - dark olive
	{60, 130, 200},	 // BIOME_RIVER          - river blue
	{220, 200, 100}, // BIOME_DESERT         - sand yellow
	{190, 170, 80},	 // BIOME_SAVANNA        - golden
	{40, 160, 40},	 // BIOME_JUNGLE         - bright green
	{200, 100, 30},	 // BIOME_BADLANDS       - orange-red
	{150, 150, 150}, // BIOME_MOUNTAINS      - grey
	{220, 220, 230}, // BIOME_SNOWY_MOUNTAINS - white-grey
};

void UIManager::updateBiomeMap()
{
	if (!engine)
		return;
	auto *gen = engine->getTerrainGenerator();
	if (!gen)
		return;

	const auto &camPos = engine->getCamera().getPosition();
	const glm::vec2 playerXZ(camPos.x, camPos.z);

	// --- Upload completed background generation ---
	if (m_biomeMapReady.load(std::memory_order_acquire))
	{
		m_biomeMapReady.store(false, std::memory_order_relaxed);

		// Draw player dot on the freshly-generated buffer (main thread has latest position).
		// Pixel (xi, yi) in the map corresponds to world:
		//   worldX = (m_pendingGridStartX + xi) * step - NOISE_OFFSET
		// Solving for xi given the player's world position:
		//   xi = (playerX + NOISE_OFFSET) * zoom - m_pendingGridStartX
		const int size = biomeMap.mapSize;
		const float noiseOffset = TerrainGenerator::NOISE_OFFSET;
		const int dotX = static_cast<int>(std::round((playerXZ.x + noiseOffset) * m_pendingZoom)) - m_pendingGridStartX;
		const int dotY = static_cast<int>(std::round((playerXZ.y + noiseOffset) * m_pendingZoom)) - m_pendingGridStartZ;
		auto paintPixel = [&](int px, int py, unsigned char r, unsigned char g, unsigned char b)
		{
			if (px >= 0 && px < size && py >= 0 && py < size)
			{
				const int idx = (py * size + px) * 3;
				m_biomeMapPending[idx + 0] = r;
				m_biomeMapPending[idx + 1] = g;
				m_biomeMapPending[idx + 2] = b;
			}
		};
		for (int dy = -4; dy <= 4; dy++)
			for (int dx = -4; dx <= 4; dx++)
				if (dx * dx + dy * dy <= 16)
					paintPixel(dotX + dx, dotY + dy, 0, 0, 0);
		for (int dy = -2; dy <= 2; dy++)
			for (int dx = -2; dx <= 2; dx++)
				if (dx * dx + dy * dy <= 4)
					paintPixel(dotX + dx, dotY + dy, 255, 255, 255);

		if (biomeMap.textureID == 0)
		{
			glGenTextures(1, &biomeMap.textureID);
			glBindTexture(GL_TEXTURE_2D, biomeMap.textureID);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, size, size, 0, GL_RGB, GL_UNSIGNED_BYTE, m_biomeMapPending.data());
		}
		else
		{
			glBindTexture(GL_TEXTURE_2D, biomeMap.textureID);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RGB, GL_UNSIGNED_BYTE, m_biomeMapPending.data());
		}
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	// --- Decide whether to launch a new background generation ---
	const double currentTime = SDL_GetTicks() / 1000.0;
	const bool timeElapsed = (currentTime - biomeMap.lastUpdateTime) >= 1.0;
	const bool playerMoved = (glm::length(playerXZ - biomeMap.lastPlayerPos) > 1.0f);

	// Don't enqueue if a generation is still in flight
	const bool isRunning = m_biomeMapFuture.valid() &&
						   m_biomeMapFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready;

	if (!isRunning && (biomeMap.needsUpdate || timeElapsed || playerMoved))
	{
		if (biomeMap.autoFollowPlayer)
			biomeMap.center = playerXZ;

		biomeMap.lastUpdateTime = currentTime;
		biomeMap.lastPlayerPos = playerXZ;
		biomeMap.needsUpdate = false;

		// Capture by value — the lambda runs on a separate thread
		const glm::vec2 center = biomeMap.center;
		const int size = biomeMap.mapSize;
		const float step = 1.0f / biomeMap.zoom;

		// Store the snapshot used by this texture so the dot is drawn consistently.
		// Compute the same integer grid-start that getBiomeRegion will use, so the
		// dot formula is pixel-perfect rather than relying on an approximate float.
		const float noiseOffset = TerrainGenerator::NOISE_OFFSET;
		m_pendingCenter = center;
		m_pendingZoom = biomeMap.zoom;
		const float invStep = biomeMap.zoom; // zoom == 1/step
		m_pendingGridStartX = static_cast<int>(std::round((center.x + noiseOffset) * invStep - size * 0.5f));
		m_pendingGridStartZ = static_cast<int>(std::round((center.y + noiseOffset) * invStep - size * 0.5f));

		m_biomeMapPending.resize(size * size * 3);

		m_biomeMapFuture = std::async(std::launch::async, [this, gen, center, size, step]()
									  {
			std::vector<BiomeType> biomes;
			gen->getBiomeRegion(center.x, center.y, step, size, size, biomes);

			for (int i = 0; i < size * size; i++)
			{
				m_biomeMapPending[i * 3 + 0] = biomeColors[biomes[i]][0];
				m_biomeMapPending[i * 3 + 1] = biomeColors[biomes[i]][1];
				m_biomeMapPending[i * 3 + 2] = biomeColors[biomes[i]][2];
			}

			m_biomeMapReady.store(true, std::memory_order_release); });
	}
}

void UIManager::renderBiomeMap()
{
	ImGui::Begin("Biome Map");

	// Controls that may trigger a refresh
	float prevZoom = biomeMap.zoom;
	ImGui::SliderFloat("Zoom", &biomeMap.zoom, 0.1f, 10.0f);
	if (biomeMap.zoom != prevZoom)
		biomeMap.needsUpdate = true;

	bool prevFollow = biomeMap.autoFollowPlayer;
	ImGui::Checkbox("Auto-follow Player", &biomeMap.autoFollowPlayer);
	if (biomeMap.autoFollowPlayer && !prevFollow)
		biomeMap.needsUpdate = true;

	ImGui::SameLine();
	if (ImGui::Button("Recenter"))
	{
		if (engine)
		{
			const auto &p = engine->getCamera().getPosition();
			biomeMap.center = {p.x, p.z};
		}
		biomeMap.needsUpdate = true;
	}

	updateBiomeMap();

	if (biomeMap.textureID != 0)
	{
		const float displaySize = static_cast<float>(biomeMap.mapSize);
		ImGui::Image((ImTextureID)(intptr_t)biomeMap.textureID, ImVec2(displaySize, displaySize));
	}
	else
	{
		ImGui::Text("Generating biome map...");
	}

	ImGui::Text("Center: (%.0f, %.0f)", biomeMap.center.x, biomeMap.center.y);

	// Biome legend
	if (ImGui::CollapsingHeader("Legend"))
	{
		const ImVec2 swatchSize(14.0f, 14.0f);
		for (int i = 0; i < BIOME_COUNT; i++)
		{
			const ImVec4 col(
				biomeColors[i][0] / 255.0f,
				biomeColors[i][1] / 255.0f,
				biomeColors[i][2] / 255.0f,
				1.0f);
			ImGui::ColorButton(biomeTypeString[i], col,
							   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, swatchSize);
			ImGui::SameLine();
			ImGui::TextUnformatted(biomeTypeString[i]);
		}
	}

	ImGui::End();
}
