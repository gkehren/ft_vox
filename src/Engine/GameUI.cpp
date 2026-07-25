#include "Engine/GameUI.hpp"

#include <Engine/Profiler.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_impl_vulkan.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <array>
#include <vector>

namespace
{
// Biome map colors indexed by BiomeType (from legacy UIManager).
constexpr unsigned char kBiomeColors[BIOME_COUNT][3] = {
	{100, 150, 200}, // FROZEN_OCEAN
	{220, 230, 240}, // SNOWY_TUNDRA
	{130, 160, 180}, // SNOWY_TAIGA
	{160, 210, 230}, // ICE_SPIKES
	{30, 100, 180},	 // OCEAN
	{210, 200, 150}, // BEACH
	{140, 200, 90},	 // PLAINS
	{50, 130, 50},	 // FOREST
	{100, 170, 80},	 // BIRCH_FOREST
	{30, 80, 30},	 // DARK_FOREST
	{80, 100, 60},	 // SWAMP
	{60, 130, 200},	 // RIVER
	{220, 200, 100}, // DESERT
	{190, 170, 80},	 // SAVANNA
	{40, 160, 40},	 // JUNGLE
	{200, 100, 30},	 // BADLANDS
	{150, 150, 150}, // MOUNTAINS
	{220, 220, 230}, // SNOWY_MOUNTAINS
};

const char *textureName(TextureType t)
{
	auto it = textureTypeString.find(t);
	return it != textureTypeString.end() ? it->second.c_str() : "unknown";
}

void helpRow(const char *keys, const char *action)
{
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::TextUnformatted(keys);
	ImGui::TableNextColumn();
	ImGui::TextUnformatted(action);
}
} // namespace

GameUI::~GameUI()
{
	shutdown();
}

void GameUI::init(VkContext &context, ImmediateCommands &imm)
{
	m_vk = &context;
	m_imm = &imm;
}

void GameUI::shutdown()
{
	if (m_mapFuture.valid())
		m_mapFuture.wait();

	if (m_vk && m_vk->getDevice() != VK_NULL_HANDLE)
	{
		m_vk->waitIdle();
		if (m_mapDesc != VK_NULL_HANDLE)
		{
			ImGui_ImplVulkan_RemoveTexture(m_mapDesc);
			m_mapDesc = VK_NULL_HANDLE;
		}
		if (m_mapSampler != VK_NULL_HANDLE)
		{
			vkDestroySampler(m_vk->getDevice(), m_mapSampler, nullptr);
			m_mapSampler = VK_NULL_HANDLE;
		}
		if (m_mapImage.image)
			destroyImage(m_vk->getAllocator(), m_vk->getDevice(), m_mapImage);
	}
	m_mapHasTexture = false;
	m_mapImageSize = 0;
	m_vk = nullptr;
	m_imm = nullptr;
}

bool GameUI::handleShortcut(int sdlKeycode, GameUIFrame &frame)
{
	switch (sdlKeycode)
	{
	case SDLK_F1:
		m_showHud = !m_showHud;
		return true;
	case SDLK_F2:
		m_showGraphics = !m_showGraphics;
		return true;
	case SDLK_F3:
		m_showStreaming = !m_showStreaming;
		return true;
	case SDLK_F4:
		m_showWorld = !m_showWorld;
		return true;
	case SDLK_F5:
		m_showHelp = !m_showHelp;
		return true;
	case SDLK_F6:
		m_showOverlayHints = !m_showOverlayHints;
		return true;
	case SDLK_F7:
		m_showProfiler = !m_showProfiler;
		return true;
	case SDLK_F10:
		if (frame.render && frame.setVSync)
		{
			frame.render->vsyncEnabled = !frame.render->vsyncEnabled;
			frame.setVSync(frame.render->vsyncEnabled);
		}
		return true;
	case SDLK_P:
		if (frame.paused)
		{
			*frame.paused = !*frame.paused;
			return true;
		}
		break;
	default:
		break;
	}
	return false;
}

void GameUI::draw(GameUIFrame &frame)
{
	drawMenuBar(frame);

	if (m_showHud)
		drawHud(frame);
	if (m_showGraphics)
		drawGraphics(frame);
	if (m_showStreaming)
		drawStreaming(frame);
	if (m_showProfiler)
		drawProfiler(frame);
	if (m_showWorld)
		drawWorld(frame);
	if (m_showHelp)
		drawHelp();

	// Report can stay open even if Profiler panel is closed.
	if (frame.benchmark && frame.benchmark->showReport() && frame.benchmark->report().valid)
		drawBenchmarkReport(frame);

	if (m_showOverlayHints && frame.mouseCaptured && *frame.mouseCaptured)
		drawOverlayHints(frame);

	// File dialog can outlive the Graphics panel; always process while open.
	displayResourcePackFileDialog(m_resourcePackUi, m_showGraphics);
}

void GameUI::drawMenuBar(GameUIFrame &frame)
{
	if (ImGui::BeginMainMenuBar())
	{
		if (ImGui::BeginMenu("View"))
		{
			ImGui::MenuItem("HUD (F1)", "F1", &m_showHud);
			ImGui::MenuItem("Graphics (F2)", "F2", &m_showGraphics);
			ImGui::MenuItem("Streaming (F3)", "F3", &m_showStreaming);
			ImGui::MenuItem("Profiler (F7)", "F7", &m_showProfiler);
			ImGui::MenuItem("World / Biome (F4)", "F4", &m_showWorld);
			ImGui::MenuItem("Help / Keys (F5)", "F5", &m_showHelp);
			ImGui::MenuItem("On-screen hints (F6)", "F6", &m_showOverlayHints);
			ImGui::Separator();
			if (frame.mouseCaptured)
			{
				if (ImGui::MenuItem(*frame.mouseCaptured ? "Release mouse" : "Capture mouse", "C"))
					*frame.mouseCaptured = !*frame.mouseCaptured;
			}
			if (frame.paused)
				ImGui::MenuItem("Pause world tick", "P", frame.paused);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Graphics"))
		{
			ImGui::MenuItem("Open panel", "F2", &m_showGraphics);
			if (frame.render && frame.setVSync)
			{
				if (ImGui::MenuItem("VSync", "F10", &frame.render->vsyncEnabled))
					frame.setVSync(frame.render->vsyncEnabled);
			}
			if (frame.showChunkBorders)
				ImGui::MenuItem("Chunk borders", "B", frame.showChunkBorders);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Help"))
		{
			ImGui::MenuItem("Keyboard reference", "F5", &m_showHelp);
			ImGui::EndMenu();
		}

		// Status strip on the right
		const float w = ImGui::GetWindowWidth();
		char status[160];
		std::snprintf(status, sizeof(status), "%.0f FPS  |  seed %d  |  %s",
					  frame.fps, frame.seed,
					  (frame.paused && *frame.paused) ? "PAUSED" : "live");
		const float tw = ImGui::CalcTextSize(status).x;
		ImGui::SetCursorPosX(w - tw - 16.f);
		ImGui::TextUnformatted(status);

		ImGui::EndMainMenuBar();
	}
}

void GameUI::drawHud(GameUIFrame &frame)
{
	ImGui::SetNextWindowPos(ImVec2(12, 28), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("HUD", &m_showHud, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::End();
		return;
	}

	ImGui::Text("%.1f FPS  (%.2f ms)", frame.fps, frame.frameMs);
	ImGui::Text("Seed: %d", frame.seed);
	ImGui::Text("Viewport: %d × %d", frame.windowW, frame.windowH);

	if (frame.camera)
	{
		const glm::vec3 p = frame.camera->getPosition();
		ImGui::SeparatorText("Player");
		ImGui::Text("Pos  %.1f  %.1f  %.1f", p.x, p.y, p.z);
		ImGui::Text("Look yaw %.0f°  pitch %.0f°", frame.camera->getYaw(), frame.camera->getPitch());
		const int cx = static_cast<int>(std::floor(p.x / CHUNK_SIZE));
		const int cz = static_cast<int>(std::floor(p.z / CHUNK_SIZE));
		ImGui::Text("Chunk (%d, %d)", cx, cz);

		if (frame.generator)
		{
			const BiomeType biome = frame.generator->getBiomeAt(
				static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.z)));
			if (biome >= 0 && biome < BIOME_COUNT)
				ImGui::Text("Biome: %s", biomeTypeString[biome]);
		}

		float speed = frame.camera->getMovementSpeed();
		if (ImGui::SliderFloat("Fly speed", &speed, 1.f, 200.f, "%.1f"))
			frame.camera->setMovementSpeed(speed);
		float sens = frame.camera->getMouseSensitivity();
		if (ImGui::SliderFloat("Mouse sens", &sens, 0.02f, 0.5f, "%.3f"))
			frame.camera->setMouseSensitivity(sens);

		const char *modes[] = {"Perspective", "Isometric"};
		int mode = frame.camera->getMode() == CameraMode::ISOMETRIC ? 1 : 0;
		if (ImGui::Combo("Camera", &mode, modes, 2))
			frame.camera->setMode(mode == 1 ? CameraMode::ISOMETRIC : CameraMode::PERSPECTIVE);
		if (frame.camera->getMode() == CameraMode::ISOMETRIC)
		{
			float z = frame.camera->getIsometricZoom();
			if (ImGui::SliderFloat("Iso zoom", &z, 16.f, 256.f))
				frame.camera->setIsometricZoom(z);
		}
	}

	ImGui::SeparatorText("Interaction");
	if (frame.selectedTexture)
	{
		// Build sorted name list once per frame (cheap — COUNT is small).
		static std::vector<std::pair<int, std::string>> names;
		if (names.empty())
		{
			for (const auto &kv : textureTypeString)
			{
				if (kv.first == AIR || kv.first == COUNT)
					continue;
				names.emplace_back(static_cast<int>(kv.first), kv.second);
			}
			std::sort(names.begin(), names.end(),
					  [](const auto &a, const auto &b) { return a.second < b.second; });
		}
		int cur = static_cast<int>(*frame.selectedTexture);
		std::string preview = textureName(*frame.selectedTexture);
		if (ImGui::BeginCombo("Block [T]", preview.c_str()))
		{
			for (const auto &n : names)
			{
				const bool sel = (n.first == cur);
				if (ImGui::Selectable(n.second.c_str(), sel))
					*frame.selectedTexture = static_cast<TextureType>(n.first);
				if (sel)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
	}
	if (frame.render)
		ImGui::SliderInt("Raycast dist", &frame.render->raycastDistance, 2, 32);

	if (frame.highlight)
	{
		if (frame.highlight->active)
			ImGui::Text("Target: %.0f %.0f %.0f",
						frame.highlight->position.x, frame.highlight->position.y,
						frame.highlight->position.z);
		else
			ImGui::TextDisabled("Target: —");
	}

	ImGui::SeparatorText("Toggles");
	if (frame.showChunkBorders)
		ImGui::Checkbox("Chunk borders [B]", frame.showChunkBorders);
	if (frame.showDemoPlayers)
		ImGui::Checkbox("Demo players", frame.showDemoPlayers);
	if (frame.mouseCaptured)
	{
		if (ImGui::Checkbox("Capture mouse [C]", frame.mouseCaptured))
		{ /* Engine applies relative mode */
		}
	}
	if (frame.paused)
		ImGui::Checkbox("Pause [P]", frame.paused);
	if (frame.render && frame.setVSync)
	{
		if (ImGui::Checkbox("VSync [F10]", &frame.render->vsyncEnabled))
			frame.setVSync(frame.render->vsyncEnabled);
	}

	ImGui::End();
}

void GameUI::drawGraphics(GameUIFrame &frame)
{
	ImGui::SetNextWindowSize(ImVec2(400, 520), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Graphics", &m_showGraphics))
	{
		ImGui::End();
		return;
	}

	if (!frame.shader || !frame.worldRenderer)
	{
		ImGui::TextDisabled("Renderer not ready.");
		ImGui::End();
		return;
	}

	auto &sp = *frame.shader;
	auto &pp = frame.worldRenderer->postSettings();

	if (ImGui::CollapsingHeader("Quality preset", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const char *presetNames[] = {"Low", "Medium", "High", "Cinematic"};
		int presetIdx = static_cast<int>(pp.qualityPreset);
		if (ImGui::Combo("Graphics quality", &presetIdx, presetNames, IM_ARRAYSIZE(presetNames)))
		{
			pp.applyPreset(static_cast<GraphicsQualityPreset>(presetIdx));
		}
		ImGui::TextDisabled("Packs SSAO / bloom / god rays / grain. Manual sliders below still work.");
	}

	drawResourcePackSection(frame, m_resourcePackUi, m_showGraphics);

	if (ImGui::CollapsingHeader("Atmosphere / Fog", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Checkbox("Automatic atmosphere", &sp.automaticAtmosphere);
		if (sp.automaticAtmosphere)
		{
			ImGui::Text("Fog start/end: %.0f / %.0f", sp.fogStart, sp.fogEnd);
			ImGui::Text("Density: %.2f", sp.fogDensity);
			ImGui::ColorEdit3("Fog color", &sp.fogColor.x, ImGuiColorEditFlags_NoInputs);
		}
		else
		{
			ImGui::SliderFloat("Fog start", &sp.fogStart, 0.f, 1000.f);
			ImGui::SliderFloat("Fog end", &sp.fogEnd, sp.fogStart + 1.f, 1400.f);
			ImGui::SliderFloat("Fog density", &sp.fogDensity, 0.f, 1.f);
			ImGui::ColorEdit3("Fog color", &sp.fogColor.x);
		}
		ImGui::SliderFloat("Height falloff", &sp.fogHeightFalloff, 0.f, 0.05f, "%.4f");
		ImGui::SliderFloat("Fog base Y", &sp.fogBaseY, 0.f, 200.f);
	}

	if (ImGui::CollapsingHeader("Lighting / Day cycle", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Checkbox("Day/night cycle", &sp.dayCycleEnabled);
		ImGui::SliderFloat("Day time", &sp.dayTime, 0.f, 1.f, "%.3f");
		ImGui::SliderFloat("Cycle speed", &sp.dayCycleSpeed, 0.f, 0.05f, "%.5f");

		auto preset = [&](float t) {
			sp.dayCycleEnabled = false;
			sp.dayTime = t;
		};
		if (ImGui::Button("Sunrise"))
			preset(0.0f);
		ImGui::SameLine();
		if (ImGui::Button("Noon"))
			preset(0.25f);
		ImGui::SameLine();
		if (ImGui::Button("Sunset"))
			preset(0.5f);
		ImGui::SameLine();
		if (ImGui::Button("Midnight"))
			preset(0.75f);

		ImGui::Text("Day / sunset / night: %.2f / %.2f / %.2f",
					sp.dayFactor, sp.sunsetFactor, sp.nightFactor);
		ImGui::SliderFloat("Ambient", &sp.ambientStrength, 0.f, 1.f);
		ImGui::SliderFloat("Diffuse", &sp.diffuseIntensity, 0.f, 1.5f);
		ImGui::SliderFloat("Light levels", &sp.lightLevels, 1.f, 16.f);
		ImGui::SliderFloat("Moon ambient", &sp.moonAmbientStrength, 0.f, 1.5f);
		ImGui::SliderFloat("Block light scale", &sp.blockLightScale, 0.f, 2.f);
		ImGui::SliderFloat("Emissive scale", &sp.emissiveScale, 0.f, 3.f);
	}

	if (ImGui::CollapsingHeader("Water (Tier 1)", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::SliderFloat("Wave strength", &sp.waterWaveStrength, 0.f, 0.5f);
		ImGui::SliderFloat("Refraction", &sp.waterRefraction, 0.f, 0.12f);
		ImGui::SliderFloat("Specular", &sp.waterSpecular, 0.f, 3.f);
		ImGui::SliderFloat("Foam", &sp.waterFoamStrength, 0.f, 2.f);
		ImGui::Text("Underwater: %s", pp.underwater ? "yes" : "no");
		ImGui::SliderFloat("Underwater strength", &pp.underwaterStrength, 0.f, 1.5f);
	}

	if (ImGui::CollapsingHeader("Shadows (CSM)"))
	{
		if (frame.render)
			ImGui::SliderFloat("Cascade far", &frame.render->shadowCascadeFar, 64.f, 512.f);
	}

	if (ImGui::CollapsingHeader("Visual"))
	{
		ImGui::SliderFloat("Saturation", &sp.saturationLevel, 0.f, 3.f);
		ImGui::SliderFloat("Color boost", &sp.colorBoost, 0.5f, 2.5f);
		ImGui::SliderFloat("Contrast", &sp.contrastLevel, 0.5f, 1.8f);
	}

	if (ImGui::CollapsingHeader("Post-processing", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Checkbox("Bloom", &pp.bloomEnabled);
		if (pp.bloomEnabled)
		{
			ImGui::SliderFloat("Bloom threshold", &pp.bloomThreshold, 0.f, 5.f);
			ImGui::SliderFloat("Bloom intensity", &pp.bloomIntensity, 0.f, 2.f);
			ImGui::SliderInt("Bloom blur iters", &pp.bloomBlurIterations, 1, 5);
		}
		ImGui::Checkbox("FXAA", &pp.fxaaEnabled);
		ImGui::SliderFloat("Exposure", &pp.exposure, 0.1f, 5.f);
		ImGui::SliderFloat("Gamma", &pp.gamma, 0.5f, 4.f);
		ImGui::SliderFloat("Post saturation", &pp.postSaturation, 0.5f, 2.f);
		ImGui::SliderFloat("Post contrast", &pp.postContrast, 0.5f, 1.8f);
		ImGui::SliderFloat("Film grain", &pp.filmGrain, 0.f, 0.12f, "%.3f");
		ImGui::SliderFloat("Vignette", &pp.vignette, 0.f, 1.f);
		const char *toneMappers[] = {"ACES Filmic", "Reinhard"};
		ImGui::Combo("Tone mapper", &pp.toneMapper, toneMappers, IM_ARRAYSIZE(toneMappers));

		ImGui::Separator();
		ImGui::Checkbox("SSAO", &pp.ssaoEnabled);
		if (pp.ssaoEnabled)
		{
			ImGui::SliderFloat("SSAO radius", &pp.ssaoRadius, 0.1f, 2.f);
			ImGui::SliderFloat("SSAO bias", &pp.ssaoBias, 0.001f, 0.1f, "%.4f");
			ImGui::SliderFloat("SSAO intensity", &pp.ssaoIntensity, 0.f, 2.f);
		}

		ImGui::Separator();
		ImGui::Checkbox("God rays", &pp.godRaysEnabled);
		if (pp.godRaysEnabled)
		{
			ImGui::SliderFloat("Density", &pp.godRaysDensity, 0.1f, 3.f);
			ImGui::SliderFloat("Weight", &pp.godRaysWeight, 0.001f, 0.05f, "%.4f");
			ImGui::SliderFloat("Decay", &pp.godRaysDecay, 0.9f, 1.f, "%.3f");
			ImGui::SliderFloat("GR exposure", &pp.godRaysExposure, 0.f, 1.f);
			ImGui::Checkbox("Depth occlusion", &pp.godRaysDepthOcclusion);
			ImGui::Checkbox("Dynamic boost", &pp.godRaysDynamicBoostEnabled);
			if (pp.godRaysDynamicBoostEnabled)
			{
				ImGui::SliderFloat("Dramatic boost", &pp.godRaysDramaticBoost, 1.f, 4.f, "%.2fx");
				ImGui::Checkbox("Boost preview", &pp.godRaysBoostPreview);
			}
		}
	}

	ImGui::End();
}

void GameUI::drawStreaming(GameUIFrame &frame)
{
	ImGui::SetNextWindowSize(ImVec2(380, 420), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Streaming", &m_showStreaming))
	{
		ImGui::End();
		return;
	}

	if (!frame.render || !frame.chunks)
	{
		ImGui::TextDisabled("Chunk manager not ready.");
		ImGui::End();
		return;
	}

	auto &rs = *frame.render;
	ImGui::SeparatorText("Distance");
	const int prevMaxRd = rs.maxRenderDistance;
	ImGui::SliderInt("View distance (blocks)", &rs.maxRenderDistance, 64, 512);
	if (rs.minRenderDistance > rs.maxRenderDistance)
		rs.minRenderDistance = rs.maxRenderDistance;
	ImGui::SliderInt("Full-mesh near range", &rs.minRenderDistance, 32, rs.maxRenderDistance);
	ImGui::SliderFloat("Front load bias", &rs.streamFrontBias, 0.f, 0.6f, "%.2f");
	ImGui::TextDisabled("Ahead reach ~ ×%.2f, behind ~ ×%.2f",
						1.0f / std::sqrt(1.0f - glm::clamp(rs.streamFrontBias, 0.f, 0.9f)),
						1.0f / std::sqrt(1.0f + glm::clamp(rs.streamFrontBias, 0.f, 0.9f)));
	ImGui::TextDisabled("Unload at ~1.5× view distance");

	const size_t poolNeed = estimateChunkPoolCapacity(rs.maxRenderDistance);
	if (frame.pool && rs.maxRenderDistance != prevMaxRd)
		frame.pool->ensureCapacity(poolNeed);
	ImGui::TextDisabled("Pool need for this view: ~%zu chunks", poolNeed);

	ImGui::SeparatorText("Pipeline budgets (ops / sec)");
	ImGui::SliderInt("Load/s", &rs.loadPerSec, 10, 600);
	ImGui::SliderInt("Gen/s", &rs.genPerSec, 5, 400);
	ImGui::SliderInt("Mesh/s", &rs.meshPerSec, 5, 300);
	ImGui::SliderInt("Upload/s", &rs.uploadPerSec, 5, 400);
	ImGui::SliderFloat("Stream ms/frame", &rs.maxStreamMs, 0.f, 16.f, "%.1f");
	ImGui::SliderFloat("Shadow dist", &rs.shadowDistance, 64.f, 320.f, "%.0f");

	ImGui::SeparatorText("Live stats");
	ImGui::Text("Loaded chunks: %zu", frame.chunks->chunkCount());
	ImGui::Text("Draw list:     %zu", frame.drawCount);
	ImGui::Text("Queue load/gen/mesh: %zu / %zu / %zu",
				frame.chunks->pendingLoadCount(),
				frame.chunks->pendingGenJobs(),
				frame.chunks->pendingMeshJobs());

	if (frame.pool)
	{
		ImGui::SeparatorText("Chunk pool");
		ImGui::Text("Capacity %zu  |  free %zu  |  acquired %zu",
					frame.pool->capacity(), frame.pool->freeCount(), frame.pool->acquiredCount());
		ImGui::Text("Need ~%zu for view %d  |  grows: %zu",
					poolNeed, rs.maxRenderDistance, frame.pool->growEvents());
		if (frame.pool->freeCount() == 0)
			ImGui::TextColored(ImVec4(1.f, 0.55f, 0.2f, 1.f),
							   "Pool full — load back-pressure active");
		if (frame.pool->rejectCount() > 0)
			ImGui::TextColored(ImVec4(1.f, 0.55f, 0.2f, 1.f), "Acquire rejects: %zu",
							   frame.pool->rejectCount());
	}

	{
		ImGui::SeparatorText("CPU timings (ms)");
		Profiler &prof = GetProfiler();
		if (ImGui::BeginTable("perf", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("Stage");
			ImGui::TableSetupColumn("ms");
			ImGui::TableHeadersRow();
			auto row = [](const char *name, float v) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(name);
				ImGui::TableNextColumn();
				ImGui::Text("%.2f", v);
			};
			row("Visibility", prof.lastScopeMs("Visibility"));
			row("Gen dispatch", prof.lastScopeMs("GenDispatch"));
			row("Mesh dispatch", prof.lastScopeMs("MeshDispatch"));
			row("Mesh upload", prof.lastScopeMs("MeshUpload"));
			row("Streaming", prof.lastScopeMs("Streaming"));
			row("Acquire (fence)", prof.lastScopeMs("Acquire"));
			row("Record", prof.lastScopeMs("Record"));
			row("Frame total", prof.lastFrameMs() > 0.f ? prof.lastFrameMs() : frame.frameMs);
			ImGui::EndTable();
		}
		ImGui::TextDisabled("Full hierarchy + graph: Profiler (F7)");
	}

	if (frame.deviceName)
	{
		ImGui::SeparatorText("Device");
		ImGui::TextWrapped("%s", frame.deviceName);
		ImGui::Text("Vulkan %u.%u  |  validation %s",
					VK_VERSION_MAJOR(frame.vkApiVersion),
					VK_VERSION_MINOR(frame.vkApiVersion),
					frame.validation ? "on" : "off");
	}

	ImGui::End();
}

void GameUI::drawProfiler(GameUIFrame &frame)
{
	ImGui::SetNextWindowSize(ImVec2(480, 560), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Profiler", &m_showProfiler))
	{
		ImGui::End();
		return;
	}

	Profiler &prof = GetProfiler();

	bool capturing = prof.enabled();
	if (ImGui::Checkbox("Capture", &capturing))
		prof.setEnabled(capturing);
	ImGui::SameLine();
	if (ImGui::Button("Clear history"))
		prof.clearHistory();
	ImGui::SameLine();
	ImGui::TextDisabled("CPU scopes · previous frame");

	const float frameMs = prof.lastFrameMs() > 0.f ? prof.lastFrameMs() : frame.frameMs;
	const float avgMs = prof.avgFrameMs() > 0.f ? prof.avgFrameMs() : frameMs;
	const float fpsEst = prof.fpsEstimate() > 0.f ? prof.fpsEstimate() : frame.fps;
	const float p1 = prof.onePercentLowMs();

	ImGui::SeparatorText("Frame");
	ImGui::Text("%.1f FPS  |  %.2f ms  |  avg %.2f ms", fpsEst, frameMs, avgMs);
	if (p1 > 0.f)
		ImGui::Text("1%% low (slow frames): %.2f ms  (~%.0f FPS)", p1, p1 > 1e-4f ? 1000.f / p1 : 0.f);

	// Frame-time history graph (chronological order)
	{
		const int n = prof.historyCount();
		std::vector<float> ordered;
		ordered.reserve(static_cast<size_t>(n > 0 ? n : 1));
		if (n > 0)
		{
			const float *hist = prof.frameHistory();
			const int write = prof.historyWriteIndex();
			const int start = (n < Profiler::kHistorySize) ? 0 : write;
			for (int i = 0; i < n; ++i)
				ordered.push_back(hist[(start + i) % Profiler::kHistorySize]);
		}
		else
		{
			ordered.push_back(frameMs);
		}

		float maxY = 16.7f;
		for (float v : ordered)
			maxY = std::max(maxY, v * 1.1f);
		maxY = std::max(maxY, 33.3f);

		ImGui::PlotLines("##ft", ordered.data(), static_cast<int>(ordered.size()), 0,
						 nullptr, 0.f, maxY, ImVec2(-1.f, 80.f));
		// Budget guides
		ImGui::TextDisabled("Graph scale 0–%.0f ms  (16.7 = 60 FPS, 33.3 = 30 FPS)", maxY);
	}

	// Hierarchy
	ImGui::SeparatorText("Hierarchy (last frame)");
	const float denom = frameMs > 1e-4f ? frameMs : 1.f;
	if (ImGui::BeginTable("scopes", 4,
						  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
						  ImVec2(0.f, 220.f)))
	{
		ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 56.f);
		ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 48.f);
		ImGui::TableSetupColumn("bar", ImGuiTableColumnFlags_WidthFixed, 100.f);
		ImGui::TableHeadersRow();

		const int count = prof.lastEntryCount();
		const ProfileEntry *entries = prof.lastEntries();
		for (int i = 0; i < count; ++i)
		{
			const ProfileEntry &e = entries[i];
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			// Indent by depth
			if (e.depth > 0)
			{
				ImGui::Dummy(ImVec2(static_cast<float>(e.depth) * 12.f, 0.f));
				ImGui::SameLine(0.f, 0.f);
			}
			ImGui::TextUnformatted(e.name ? e.name : "?");

			ImGui::TableNextColumn();
			const float ms = e.durationMs;
			ImVec4 col(0.55f, 0.9f, 0.55f, 1.f);
			if (ms >= 8.f)
				col = ImVec4(1.f, 0.4f, 0.35f, 1.f);
			else if (ms >= 2.f)
				col = ImVec4(1.f, 0.85f, 0.35f, 1.f);
			ImGui::TextColored(col, "%.2f", ms);

			ImGui::TableNextColumn();
			const float pct = 100.f * ms / denom;
			ImGui::Text("%.0f", pct);

			ImGui::TableNextColumn();
			const float frac = std::clamp(ms / denom, 0.f, 1.f);
			ImGui::ProgressBar(frac, ImVec2(-1.f, 0.f), "");
		}
		if (count == 0)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("No samples yet — wait a frame or enable Capture");
		}
		ImGui::EndTable();
	}

	// Worker jobs
	ImGui::SeparatorText("Worker CPU (thread pool)");
	ImGui::TextDisabled("Totals can exceed frame time (parallel workers).");
	const int wc = prof.workerSnapshotCount();
	if (wc == 0)
	{
		ImGui::TextDisabled("No worker samples this frame");
	}
	else if (ImGui::BeginTable("workers", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("Job");
		ImGui::TableSetupColumn("count");
		ImGui::TableSetupColumn("avg ms");
		ImGui::TableSetupColumn("total ms");
		ImGui::TableHeadersRow();
		const WorkerSnapshot *ws = prof.workerSnapshots();
		for (int i = 0; i < wc; ++i)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(ws[i].name ? ws[i].name : "?");
			ImGui::TableNextColumn();
			ImGui::Text("%llu", static_cast<unsigned long long>(ws[i].count));
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", ws[i].avgMs);
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", ws[i].totalMs);
		}
		ImGui::EndTable();
	}

	// Pipeline snapshot
	ImGui::SeparatorText("Pipeline snapshot");
	if (frame.chunks)
	{
		ImGui::Text("Chunks %zu  |  draw %zu  |  q load/gen/mesh %zu / %zu / %zu",
					frame.chunks->chunkCount(), frame.drawCount,
					frame.chunks->pendingLoadCount(), frame.chunks->pendingGenJobs(),
					frame.chunks->pendingMeshJobs());
	}
	if (frame.pool)
	{
		ImGui::Text("Pool free %zu / %zu  |  acquired %zu",
					frame.pool->freeCount(), frame.pool->capacity(), frame.pool->acquiredCount());
	}
	ImGui::Text("Shadow list: use Streaming panel for budgets");

	// Spikes
	const int sc = prof.spikeCount();
	if (sc > 0)
	{
		ImGui::SeparatorText("Spikes (>20 ms)");
		const SpikeRecord *sp = prof.spikes();
		for (int i = sc - 1; i >= 0; --i)
		{
			ImGui::Text("%.1f ms  top: %s (%.1f ms)", sp[i].frameMs,
						sp[i].topScope ? sp[i].topScope : "?", sp[i].topMs);
		}
	}

	// -------- Benchmark --------
	if (frame.benchmark)
	{
		Benchmark &bench = *frame.benchmark;
		BenchmarkConfig &cfg = bench.config();
		ImGui::SeparatorText("Benchmark");
		ImGui::TextDisabled("Reload seed, orbit path, scored report.");

		const bool active = bench.isActive();
		ImGui::BeginDisabled(active);
		ImGui::InputInt("Seed", &cfg.seed);
		if (cfg.seed <= 0)
			cfg.seed = 42;
		ImGui::SliderFloat("Duration (s)", &cfg.durationSec, 10.f, 180.f, "%.0f");
		ImGui::SliderFloat("Warmup (s)", &cfg.warmupSec, 0.f, 15.f, "%.1f");
		ImGui::SliderFloat("Orbit radius", &cfg.pathRadius, 64.f, 320.f, "%.0f");
		ImGui::SliderInt("Orbits", &cfg.pathOrbits, 1, 6);
		ImGui::Checkbox("Force VSync off", &cfg.forceVsyncOff);
		ImGui::TextUnformatted("Path: Orbit (look at spawn)");
		ImGui::EndDisabled();

		if (!active)
		{
			if (ImGui::Button("Start benchmark", ImVec2(-1.f, 0.f)))
				bench.requestStart();
		}
		else
		{
			const char *phaseStr = "…";
			switch (bench.phase())
			{
			case BenchmarkPhase::Reloading:
				phaseStr = "Reloading world…";
				break;
			case BenchmarkPhase::Warmup:
				phaseStr = "Warmup (streaming fill)";
				break;
			case BenchmarkPhase::Running:
				phaseStr = "Measuring";
				break;
			default:
				break;
			}
			ImGui::Text("%s", phaseStr);
			ImGui::ProgressBar(bench.totalProgress(), ImVec2(-1.f, 0.f));
			ImGui::Text("Elapsed %.1fs  |  remain measure %.1fs  |  FPS ~%.0f",
						bench.elapsedSec(), bench.remainingMeasureSec(), frame.fps);
			if (ImGui::Button("Cancel", ImVec2(-1.f, 0.f)))
				bench.cancel();
		}

		if (bench.report().valid && !active)
		{
			if (ImGui::Button("Show last report"))
				bench.setShowReport(true);
			ImGui::SameLine();
			const BenchmarkReport &r = bench.report();
			ImGui::Text("Last score: %d (%c)", r.score, r.grade);
		}
	}

	ImGui::End();
}

void GameUI::drawBenchmarkReport(GameUIFrame &frame)
{
	if (!frame.benchmark)
		return;
	Benchmark &bench = *frame.benchmark;
	const BenchmarkReport &r = bench.report();
	bool open = true;
	ImGui::SetNextWindowSize(ImVec2(440, 520), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Benchmark Report", &open))
	{
		ImGui::End();
		if (!open)
			bench.setShowReport(false);
		return;
	}
	if (!open)
	{
		bench.setShowReport(false);
		ImGui::End();
		return;
	}

	ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.3f, 1.f), "SCORE  %d  /  10000", r.score);
	ImGui::SameLine();
	ImGui::Text("  Grade %c", r.grade);
	ImGui::Text("Seed %d  |  %.0fs measure (+%.0fs warmup)  |  %d frames", r.seed, r.durationSec,
				r.warmupSec, r.frames);

	ImGui::SeparatorText("Frame times");
	if (ImGui::BeginTable("bm_ft", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
	{
		auto row = [](const char *k, const char *fmt, auto... args) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(k);
			ImGui::TableNextColumn();
			ImGui::Text(fmt, args...);
		};
		row("Avg FPS", "%.1f", r.avgFps);
		row("1%% low FPS", "%.1f", r.onePercentLowFps);
		row("Avg / min / max ms", "%.2f / %.2f / %.2f", r.avgMs, r.minMs, r.maxMs);
		row("p50 / p95 / p99 ms", "%.2f / %.2f / %.2f", r.p50Ms, r.p95Ms, r.p99Ms);
		row("Frames >16.7 ms", "%d", r.framesOver16ms);
		row("Frames >33.3 ms", "%d", r.framesOver33ms);
		ImGui::EndTable();
	}

	ImGui::SeparatorText("CPU scopes (avg ms)");
	if (ImGui::BeginTable("bm_sc", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
	{
		auto row = [](const char *k, float v) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(k);
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", v);
		};
		row("Streaming", r.avgStreaming);
		row("Visibility", r.avgVisibility);
		row("Acquire", r.avgAcquire);
		row("Record", r.avgRecord);
		row("MeshUpload", r.avgMeshUpload);
		row("ImGui", r.avgImGui);
		row("Present", r.avgPresent);
		ImGui::EndTable();
	}

	ImGui::SeparatorText("Worker jobs");
	if (ImGui::BeginTable("bm_wk", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("Job");
		ImGui::TableSetupColumn("count");
		ImGui::TableSetupColumn("avg ms");
		ImGui::TableSetupColumn("total ms");
		ImGui::TableHeadersRow();
		auto wrow = [](const char *n, uint64_t c, float a, float t) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(n);
			ImGui::TableNextColumn();
			ImGui::Text("%llu", static_cast<unsigned long long>(c));
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", a);
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", t);
		};
		wrow("TerrainGen", r.terrainGenJobs, r.terrainGenAvgMs, r.terrainGenTotalMs);
		wrow("MeshBuild", r.meshBuildJobs, r.meshBuildAvgMs, r.meshBuildTotalMs);
		wrow("MeshLOD", r.meshLodJobs, r.meshLodAvgMs, r.meshLodTotalMs);
		ImGui::EndTable();
	}

	ImGui::SeparatorText("Build / revision");
	ImGui::Text("Revision: %s%s", r.revisionLabel.c_str(), r.gitDirty ? "  (dirty)" : "");
	ImGui::Text("Branch: %s", r.gitBranch.c_str());
	ImGui::TextWrapped("Describe: %s", r.gitDescribe.c_str());
	ImGui::Text("Built (UTC): %s", r.buildUtc.c_str());

	ImGui::SeparatorText("Peaks & settings");
	ImGui::Text("Chunks %zu  |  draw %zu  |  queues %zu / %zu / %zu", r.peakChunks, r.peakDraw,
				r.peakPendingLoad, r.peakPendingGen, r.peakPendingMesh);
	ImGui::Text("View %d  |  %dx%d  |  VSync %s", r.viewDistance, r.windowW, r.windowH,
				r.vsync ? "on" : "off");
	if (!r.deviceName.empty())
		ImGui::TextWrapped("%s", r.deviceName.c_str());

	ImGui::Separator();
	ImGui::TextWrapped(
		"Score: 45%% avgFPS@60 + 15%% headroom + 30%% 1%%low@60 + 10%% stability; "
		"up to -15%% for frames >33ms. Higher is better.");

	if (ImGui::Button("Copy summary"))
	{
		const std::string text = bench.formatReportText();
		ImGui::SetClipboardText(text.c_str());
	}
	ImGui::SameLine();
	if (ImGui::Button("Save summary"))
	{
		const std::string path = bench.saveReportToFile("benchmarks");
		if (path.empty())
			ImGui::OpenPopup("bench_save_fail");
		else
			ImGui::OpenPopup("bench_save_ok");
	}
	ImGui::SameLine();
	if (ImGui::Button("Close"))
		bench.setShowReport(false);

	if (ImGui::BeginPopup("bench_save_ok"))
	{
		ImGui::Text("Saved:");
		ImGui::TextWrapped("%s", bench.lastSavedPath().c_str());
		if (ImGui::Button("OK"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (ImGui::BeginPopup("bench_save_fail"))
	{
		ImGui::TextWrapped("Failed to write benchmarks/… (check cwd permissions).");
		if (ImGui::Button("OK"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if (!bench.lastSavedPath().empty())
		ImGui::TextDisabled("Last save: %s", bench.lastSavedPath().c_str());

	ImGui::End();
}

void GameUI::drawWorld(GameUIFrame &frame)
{
	ImGui::SetNextWindowSize(ImVec2(320, 520), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("World", &m_showWorld))
	{
		ImGui::End();
		return;
	}

	ImGui::Text("Seed: %d", frame.seed);
	ImGui::TextWrapped("Procedural infinite terrain (FastNoise2). "
					   "Chunks stream around the player; no world save yet.");

	ImGui::SeparatorText("Biome map");
	tickBiomeMap(frame);

	float prevZoom = m_mapZoom;
	if (ImGui::SliderFloat("Zoom", &m_mapZoom, 0.1f, 8.f, "%.2f"))
	{
		if (m_mapZoom != prevZoom)
			m_mapNeedsUpdate = true;
	}
	bool prevFollow = m_mapFollow;
	ImGui::Checkbox("Follow player", &m_mapFollow);
	if (m_mapFollow && !prevFollow)
		m_mapNeedsUpdate = true;

	if (frame.camera && ImGui::Button("Center on player"))
	{
		const auto p = frame.camera->getPosition();
		m_mapCenter = {p.x, p.z};
		m_mapNeedsUpdate = true;
	}

	if (m_mapHasTexture && m_mapDesc != VK_NULL_HANDLE)
	{
		const float display = 256.f;
		ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(m_mapDesc)),
					 ImVec2(display, display));
	}
	else
	{
		ImGui::TextDisabled("Generating biome map…");
	}
	ImGui::Text("Center: (%.0f, %.0f)", m_mapCenter.x, m_mapCenter.y);

	ImGui::SeparatorText("Legend");
	const int cols = 2;
	if (ImGui::BeginTable("legend", cols))
	{
		for (int i = 0; i < BIOME_COUNT; ++i)
		{
			ImGui::TableNextColumn();
			const ImVec4 col(kBiomeColors[i][0] / 255.f, kBiomeColors[i][1] / 255.f,
							kBiomeColors[i][2] / 255.f, 1.f);
			ImGui::ColorButton(biomeTypeString[i], col, ImGuiColorEditFlags_NoTooltip,
							   ImVec2(12, 12));
			ImGui::SameLine();
			ImGui::TextUnformatted(biomeTypeString[i]);
		}
		ImGui::EndTable();
	}

	ImGui::End();
}

void GameUI::drawHelp()
{
	ImGui::SetNextWindowSize(ImVec2(420, 460), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Help / Shortcuts", &m_showHelp))
	{
		ImGui::End();
		return;
	}

	ImGui::TextWrapped("ft_vox — Vulkan voxel engine. Release the mouse with C to use the UI freely.");

	if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, 140.f);
		ImGui::TableSetupColumn("Action");
		ImGui::TableHeadersRow();

		helpRow("W A S D", "Move (fly)");
		helpRow("Space / Shift", "Up / down");
		helpRow("Mouse", "Look");
		helpRow("LMB / RMB", "Break / place block");
		helpRow("T", "Cycle selected block");
		helpRow("B", "Toggle chunk borders");
		helpRow("C", "Capture / free mouse");
		helpRow("P", "Pause world tick");
		helpRow("Esc", "Quit");
		helpRow("F1", "Toggle HUD");
		helpRow("F2", "Graphics panel");
		helpRow("F3", "Streaming panel");
		helpRow("F4", "World / biome map");
		helpRow("F5", "This help");
		helpRow("F6", "On-screen hints");
		helpRow("F7", "CPU profiler + benchmark");
		helpRow("F10", "Toggle VSync");
		helpRow("X (iso)", "Speed boost (isometric)");

		ImGui::EndTable();
	}

	ImGui::Separator();
	ImGui::TextDisabled("Removed / unavailable");
	ImGui::BulletText("Wireframe mode (no Vulkan pipeline yet)");
	ImGui::BulletText("Multiplayer network panel (not re-wired)");
	ImGui::BulletText("OpenGL shader hot-reload / FreeType HUD text");

	ImGui::End();
}

void GameUI::drawOverlayHints(GameUIFrame &frame)
{
	const ImGuiIO &io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 36.f),
							ImGuiCond_Always, ImVec2(0.5f, 1.f));
	ImGui::SetNextWindowBgAlpha(0.35f);
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
							 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
							 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
	if (ImGui::Begin("##hints", nullptr, flags))
	{
		const char *block = frame.selectedTexture ? textureName(*frame.selectedTexture) : "?";
		ImGui::Text("C free mouse · F1–F7 panels · T block (%s) · LMB/RMB edit · B borders", block);
	}
	ImGui::End();
}

void GameUI::ensureBiomeTexture(int size)
{
	if (!m_vk || !m_imm)
		return;
	if (m_mapHasTexture && m_mapImageSize == size)
		return;

	m_vk->waitIdle();
	if (m_mapDesc != VK_NULL_HANDLE)
	{
		ImGui_ImplVulkan_RemoveTexture(m_mapDesc);
		m_mapDesc = VK_NULL_HANDLE;
	}
	if (m_mapImage.image)
		destroyImage(m_vk->getAllocator(), m_vk->getDevice(), m_mapImage);
	if (m_mapSampler == VK_NULL_HANDLE)
	{
		VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		si.magFilter = VK_FILTER_NEAREST;
		si.minFilter = VK_FILTER_NEAREST;
		si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		if (vkCreateSampler(m_vk->getDevice(), &si, nullptr, &m_mapSampler) != VK_SUCCESS)
			return;
	}

	m_mapImage = createImage2D(m_vk->getAllocator(), m_vk->getDevice(),
							   static_cast<uint32_t>(size), static_cast<uint32_t>(size),
							   VK_FORMAT_R8G8B8A8_UNORM,
							   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	m_mapDesc = ImGui_ImplVulkan_AddTexture(m_mapSampler, m_mapImage.view,
											VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	m_mapImageSize = size;
	m_mapHasTexture = (m_mapDesc != VK_NULL_HANDLE);
}

void GameUI::uploadBiomeTexture(const std::vector<unsigned char> &rgb, int size)
{
	if (!m_vk || !m_imm || rgb.size() < static_cast<size_t>(size * size * 3))
		return;

	ensureBiomeTexture(size);
	if (!m_mapHasTexture)
		return;

	// Expand RGB → RGBA for R8G8B8A8.
	std::vector<unsigned char> rgba(static_cast<size_t>(size * size * 4));
	for (int i = 0; i < size * size; ++i)
	{
		rgba[i * 4 + 0] = rgb[i * 3 + 0];
		rgba[i * 4 + 1] = rgb[i * 3 + 1];
		rgba[i * 4 + 2] = rgb[i * 3 + 2];
		rgba[i * 4 + 3] = 255;
	}

	uploadImage2D(m_vk->getAllocator(), *m_imm, m_mapImage, rgba.data(),
				  rgba.size() * sizeof(unsigned char));
}

void GameUI::tickBiomeMap(GameUIFrame &frame)
{
	if (!frame.generator || !frame.camera)
		return;

	const glm::vec3 cam = frame.camera->getPosition();
	const glm::vec2 playerXZ(cam.x, cam.z);

	// Upload completed async map
	if (m_mapReady.load(std::memory_order_acquire))
	{
		m_mapReady.store(false, std::memory_order_relaxed);

		// Paint player dot
		const int size = m_mapSize;
		const float noiseOffset = TerrainGenerator::NOISE_OFFSET;
		const int dotX = static_cast<int>(std::round((playerXZ.x + noiseOffset) * m_mapPendingZoom)) -
						 m_mapPendingGridX;
		const int dotY = static_cast<int>(std::round((playerXZ.y + noiseOffset) * m_mapPendingZoom)) -
						 m_mapPendingGridZ;
		auto paint = [&](int px, int py, unsigned char r, unsigned char g, unsigned char b) {
			if (px < 0 || py < 0 || px >= size || py >= size)
				return;
			const int idx = (py * size + px) * 3;
			m_mapPending[idx + 0] = r;
			m_mapPending[idx + 1] = g;
			m_mapPending[idx + 2] = b;
		};
		for (int dy = -4; dy <= 4; ++dy)
			for (int dx = -4; dx <= 4; ++dx)
				if (dx * dx + dy * dy <= 16)
					paint(dotX + dx, dotY + dy, 0, 0, 0);
		for (int dy = -2; dy <= 2; ++dy)
			for (int dx = -2; dx <= 2; ++dx)
				if (dx * dx + dy * dy <= 4)
					paint(dotX + dx, dotY + dy, 255, 255, 255);

		m_mapCenter = m_mapPendingCenter;
		uploadBiomeTexture(m_mapPending, size);
	}

	const double now = SDL_GetTicks() / 1000.0;
	const bool timeElapsed = (now - m_mapLastUpdate) >= 1.0;
	const bool playerMoved = glm::length(playerXZ - m_mapLastPlayer) > 8.f;
	const bool running = m_mapFuture.valid() &&
						 m_mapFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready;

	if (!running && (m_mapNeedsUpdate || timeElapsed || playerMoved))
	{
		if (m_mapFollow)
			m_mapCenter = playerXZ;

		m_mapLastUpdate = now;
		m_mapLastPlayer = playerXZ;
		m_mapNeedsUpdate = false;

		const glm::vec2 center = m_mapCenter;
		const int size = m_mapSize;
		const float step = 1.f / m_mapZoom;
		const float noiseOffset = TerrainGenerator::NOISE_OFFSET;
		m_mapPendingCenter = center;
		m_mapPendingZoom = m_mapZoom;
		m_mapPendingGridX = static_cast<int>(std::round((center.x + noiseOffset) * m_mapZoom - size * 0.5f));
		m_mapPendingGridZ = static_cast<int>(std::round((center.y + noiseOffset) * m_mapZoom - size * 0.5f));
		m_mapPending.resize(static_cast<size_t>(size * size * 3));

		TerrainGenerator *gen = frame.generator;
		m_mapFuture = std::async(std::launch::async, [this, gen, center, size, step]() {
			std::vector<BiomeType> biomes;
			gen->getBiomeRegion(center.x, center.y, step, size, size, biomes);
			for (int i = 0; i < size * size; ++i)
			{
				const int b = static_cast<int>(biomes[i]);
				const int bi = (b >= 0 && b < BIOME_COUNT) ? b : 0;
				m_mapPending[i * 3 + 0] = kBiomeColors[bi][0];
				m_mapPending[i * 3 + 1] = kBiomeColors[bi][1];
				m_mapPending[i * 3 + 2] = kBiomeColors[bi][2];
			}
			m_mapReady.store(true, std::memory_order_release);
		});
	}
}
