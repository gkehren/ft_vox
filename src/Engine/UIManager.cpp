#include "UIManager.hpp"
#include "Engine.hpp"
#include <SDL3/SDL.h>
#include <cmath>

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
	ImGui::Text("Render distance: %d | %d", renderSettings.minRenderDistance, renderSettings.maxRenderDistance);
	if (engine)
	{
		const auto &camPos = engine->getCamera().getPosition();
		ImGui::Text("X/Y/Z: (%.1f, %.1f, %.1f)", camPos.x, camPos.y, camPos.z);
		const auto &playerChunkPos = engine->getPlayerChunkPos();
		ImGui::Text("Player chunk: (%d, %d)", playerChunkPos.x, playerChunkPos.y);
		if (auto *gen = engine->getTerrainGenerator()) {
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
	ImGui::Checkbox("Chunk borders", &renderSettings.chunkBorders);
	ImGui::InputInt("Chunk loaded max", &renderSettings.chunkLoadedMax);
	ImGui::Checkbox("Pause", &renderSettings.paused);

	ImGui::Text("Screen size: %d x %d", winWidth, winHeight);

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

void UIManager::updateBiomeMap()
{
	// TODO: Implement biome map update logic
}

void UIManager::renderBiomeMap()
{
	updateBiomeMap(); // Ensure map is up-to-date

	ImGui::Begin("Biome Map");
	if (biomeMap.textureID != 0)
	{
		ImGui::Image((ImTextureID)(intptr_t)biomeMap.textureID, ImVec2(biomeMap.mapSize, biomeMap.mapSize));
	}
	ImGui::SliderFloat("Zoom", &biomeMap.zoom, 0.1f, 10.0f);
	ImGui::Checkbox("Auto-follow Player", &biomeMap.autoFollowPlayer);
	if (ImGui::Button("Recenter Map"))
	{
		biomeMap.needsUpdate = true; // Force update on recenter
	}
	ImGui::Text("Center: (%.1f, %.1f)", biomeMap.center.x, biomeMap.center.y);
	ImGui::End();
}
