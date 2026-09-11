#include "Engine/GameUI.hpp"

#include <Engine/DebugUI/DebugUiEngine.hpp>
#include <Engine/DebugUI/DebugPanels.hpp>
#include <Engine/Profiler.hpp>
#include <Vulkan/StagingRing.hpp>
#include <ImGuiFileDialog.h>
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
const char *textureName(TextureType type)
{
	const auto index = static_cast<std::size_t>(type);

	if (index >= textureTypeString.size())
		return "unknown";

	const std::string_view name = textureTypeString[index];
	return name.empty() ? "unknown" : name.data();
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
	if (m_mapJob.cancel)
		m_mapJob.cancel->store(true, std::memory_order_relaxed);

	if (m_mapJob.future.valid())
		m_mapJob.future.wait();

	m_mapJob.reset();
	m_pendingUpload = {};

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
	m_mapImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	m_vk = nullptr;
	m_imm = nullptr;
}

void GameUI::invalidateBiomeMap()
{
	supersedeBiomeMapRequest();

	m_mapLastPublishedAt = 0.0;
	// The backing Vulkan texture remains allocated on the GPU for reuse,
	// but is marked inactive so the UI will not display stale world terrain.
	// It will be updated in-place when the next valid map completes.
	m_mapHasTexture = false;
	m_pendingUpload = {};
}

void GameUI::onImGuiVulkanBackendRecreate()
{
	// The old descriptor belonged to the backend pool destroyed during reinit.
	// The sampled image and sampler are application-owned and remain valid.
	m_mapDesc = VK_NULL_HANDLE;
	if (m_mapSampler != VK_NULL_HANDLE && m_mapImage.image != VK_NULL_HANDLE)
		m_mapDesc = ImGui_ImplVulkan_AddTexture(
			m_mapSampler, m_mapImage.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	if (m_mapDesc == VK_NULL_HANDLE)
		m_mapHasTexture = false;
}

bool GameUI::handleGlobalShortcut(int sdlKeycode, GameUIFrame &frame)
{
	// Intentionally global (issue #76): function keys no text field can
	// produce, so they work even while ImGui captures the keyboard.
	switch (sdlKeycode)
	{
	case SDLK_F1:
		m_debug.panels.hud = !m_debug.panels.hud;
		return true;
	case SDLK_F2:
		m_debug.panels.rendering = !m_debug.panels.rendering;
		return true;
	case SDLK_F3:
		m_debug.panels.streaming = !m_debug.panels.streaming;
		return true;
	case SDLK_F4:
		m_debug.panels.world = !m_debug.panels.world;
		return true;
	case SDLK_F5:
		m_debug.panels.help = !m_debug.panels.help;
		return true;
	case SDLK_F6:
		m_debug.panels.overlayHints = !m_debug.panels.overlayHints;
		return true;
	case SDLK_F7:
		m_debug.panels.performance = !m_debug.panels.performance;
		return true;
	case SDLK_F8:
		m_debug.panels.overview = !m_debug.panels.overview;
		return true;
	case SDLK_F9:
		m_debug.panels.chunkInspector = !m_debug.panels.chunkInspector;
		return true;
	case SDLK_F10:
		if (frame.render && frame.setVSync)
		{
			frame.render->vsyncEnabled = !frame.render->vsyncEnabled;
			frame.setVSync(frame.render->vsyncEnabled);
		}
		return true;
	case SDLK_F11:
		m_debug.panels.memory = !m_debug.panels.memory;
		return true;
	case SDLK_F12:
		m_debug.panels.renderDebug = !m_debug.panels.renderDebug;
		return true;
	default:
		break;
	}
	return false;
}

bool GameUI::handleGameplayShortcut(int sdlKeycode, GameUIFrame &frame)
{
	// Gameplay state changes (issue #76): the caller gates these behind
	// !wantCaptureKeyboard() so typing in an ImGui text field is inert.
	switch (sdlKeycode)
	{
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

bool GameUI::isFileDialogOpen() const
{
	return ImGuiFileDialog::Instance()->IsOpened();
}

void GameUI::draw(GameUIFrame &frame)
{
	// Refresh debug snapshots/histories/health first (issue #179). Throttled
	// internally; heavy sampling only runs while a consumer panel is open.
	debugui::updateDebugUiState(m_debug, frame, ImGui::GetTime());

	drawMenuBar(frame);

	if (m_debug.panels.hud)
		drawHud(frame);
	debugui::drawOverview(m_debug, frame);
	debugui::drawRendering(m_debug, frame);
	debugui::drawRenderDebug(m_debug, frame);
	debugui::drawStreaming(m_debug, frame);
	debugui::drawPerformance(m_debug, frame);
	debugui::drawChunkInspector(m_debug, frame);
	debugui::drawMemory(m_debug, frame);
	debugui::drawBenchmarkPanel(m_debug, frame);
	if (m_debug.panels.world)
		drawWorld(frame);
	if (m_debug.panels.help)
		drawHelp();

	// Report can stay open even if the panels that opened it are closed.
	if (frame.benchmark && frame.benchmark->showReport() && frame.benchmark->report().valid)
		debugui::drawBenchmarkReport(m_debug, frame);

	if (m_debug.panels.overlayHints && frame.mouseCaptured && *frame.mouseCaptured)
		drawOverlayHints(frame);

	// File dialog can outlive the Graphics panel; always process while open.
	displayResourcePackFileDialog(m_debug.resourcePackUi, m_debug.panels.rendering);
}

void GameUI::drawMenuBar(GameUIFrame &frame)
{
	if (ImGui::BeginMainMenuBar())
	{
		if (ImGui::BeginMenu("View"))
		{
			ImGui::MenuItem("HUD (F1)", "F1", &m_debug.panels.hud);
			ImGui::MenuItem("World / Biome (F4)", "F4", &m_debug.panels.world);
			ImGui::MenuItem("Help / Keys (F5)", "F5", &m_debug.panels.help);
			ImGui::MenuItem("On-screen hints (F6)", "F6", &m_debug.panels.overlayHints);
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
		if (ImGui::BeginMenu("Developer"))
		{
			ImGui::MenuItem("Overview (F8)", "F8", &m_debug.panels.overview);
			ImGui::Separator();
			ImGui::MenuItem("Performance (F7)", "F7", &m_debug.panels.performance);
			ImGui::MenuItem("Streaming (F3)", "F3", &m_debug.panels.streaming);
			ImGui::MenuItem("Memory (F11)", "F11", &m_debug.panels.memory);
			ImGui::MenuItem("Chunk inspector (F9)", "F9", &m_debug.panels.chunkInspector);
			ImGui::MenuItem("Render debug (F12)", "F12", &m_debug.panels.renderDebug);
			ImGui::MenuItem("Benchmark", nullptr, &m_debug.panels.benchmark);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Graphics"))
		{
			ImGui::MenuItem("Open panel", "F2", &m_debug.panels.rendering);
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
			ImGui::MenuItem("Keyboard reference", "F5", &m_debug.panels.help);
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
	if (!ImGui::Begin("HUD", &m_debug.panels.hud, ImGuiWindowFlags_AlwaysAutoResize))
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
			// Single canonical world -> voxel-column convention (floor).
			const glm::ivec2 column = worldToVoxelColumn(glm::vec2(p.x, p.z));
			const BiomeType biome = frame.generator->getBiomeAt(column.x, column.y);
			if (biome >= 0 && biome < BIOME_COUNT)
				ImGui::Text("Biome: %s", biomeTypeString[biome]);
		}

		if (frame.player)
		{
			bool flight = frame.playerFlight;
			ImGui::BeginDisabled(frame.benchmark && frame.benchmark->isActive());
			if (ImGui::Checkbox("Debug flight [V]", &flight) && frame.setPlayerFlight)
				frame.setPlayerFlight(flight);
			ImGui::EndDisabled();
			const auto &p = *frame.player;
			ImGui::Text("%s | %.2f blocks/s", frame.playerFlight ? "Flying" :
				p.body.waitingForTerrain ? "Waiting for terrain" :
				p.submerged.water + p.submerged.lava > 0 ? "Swimming" :
				p.body.grounded ? "Grounded" : "Airborne", glm::length(p.body.velocity));
			ImGui::TextDisabled("Physics: %u steps, %llu cells, %llu dropped steps",
				p.metrics.steps, static_cast<unsigned long long>(p.metrics.queries.cells),
				static_cast<unsigned long long>(p.metrics.droppedSteps));
			if (frame.playerStatus && *frame.playerStatus) ImGui::TextWrapped("%s", frame.playerStatus);
		}
		float speed = frame.camera->getMovementSpeed();
		ImGui::BeginDisabled(!frame.playerFlight);
		if (ImGui::SliderFloat("Fly speed", &speed, 1.f, 200.f, "%.1f"))
			frame.camera->setMovementSpeed(speed);
		ImGui::EndDisabled();
		float sens = frame.camera->getMouseSensitivity();
		if (ImGui::SliderFloat("Mouse sens", &sens, 0.02f, 0.5f, "%.3f"))
			frame.camera->setMouseSensitivity(sens);

		const char *modes[] = {"Perspective", "Isometric"};
		int mode = frame.camera->getMode() == CameraMode::ISOMETRIC ? 1 : 0;
		if (ImGui::Combo("Camera", &mode, modes, 2) && frame.setCameraMode)
			frame.setCameraMode(mode == 1 ? CameraMode::ISOMETRIC : CameraMode::PERSPECTIVE);
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
			for (std::size_t i = 0; i < textureTypeString.size(); ++i)
			{
				names.emplace_back(static_cast<int>(i), std::string{textureTypeString[i]});
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
    if (frame.mobsEnabled) {
        ImGui::Checkbox("Passive mobs", frame.mobsEnabled);
        ImGui::Text("Mobs: %zu active / %zu visible", frame.mobCount, frame.mobVisible);
    }
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
		if (frame.presentModeName)
			ImGui::TextDisabled("Vulkan present mode: %s%s",
								frame.presentModeName,
								frame.render->vsyncEnabled
									? ""
									: " (no refresh pacing / no FPS cap)");
	}

	ImGui::End();
}

void GameUI::drawWorld(GameUIFrame &frame)
{
	ImGui::SetNextWindowSize(ImVec2(320, 520), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("World", &m_debug.panels.world))
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
			supersedeBiomeMapRequest();
	}
	bool prevFollow = m_mapFollow;
	ImGui::Checkbox("Follow player", &m_mapFollow);
	if (m_mapFollow && !prevFollow)
		supersedeBiomeMapRequest();

	if (frame.camera && ImGui::Button("Center on player"))
	{
		const auto p = frame.camera->getPosition();
		m_mapCenter = {p.x, p.z};
		supersedeBiomeMapRequest();
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
	if (ImGui::BeginChild("BiomeLegend", ImVec2(0.f, 170.f),
						  ImGuiChildFlags_Borders))
	{
		const int cols = 2;
		if (ImGui::BeginTable("legend", cols,
							  ImGuiTableFlags_SizingStretchSame))
		{
			for (int i = 0; i < BIOME_COUNT; ++i)
			{
				ImGui::TableNextColumn();
				const ImVec4 col(kBiomeColors[i][0] / 255.f,
								kBiomeColors[i][1] / 255.f,
								kBiomeColors[i][2] / 255.f, 1.f);
				ImGui::ColorButton(biomeTypeString[i], col,
								   ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
				ImGui::SameLine();
				ImGui::TextUnformatted(biomeTypeString[i]);
			}
			ImGui::EndTable();
		}
	}
	ImGui::EndChild();

	ImGui::SeparatorText("Diagnostics");
	ImGui::TextWrapped(
		"CLI: test_terrain --world-stats / --profile. "
		"Runtime: ft_vox --seed N --benchmark seconds.");

	ImGui::End();
}

void GameUI::drawHelp()
{
	ImGui::SetNextWindowSize(ImVec2(420, 480), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Help / Shortcuts", &m_debug.panels.help))
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

		helpRow("W A S D", "Move");
		helpRow("Space", "Jump / swim up / fly up");
		helpRow("Shift", "Sprint / swim down / fly down");
		helpRow("V", "Toggle walk / debug flight");
		helpRow("Mouse", "Look");
		helpRow("LMB / RMB", "Break / place block");
		helpRow("T", "Cycle selected block");
		helpRow("B", "Toggle chunk borders");
		helpRow("C", "Capture / free mouse");
		helpRow("P", "Pause world tick");
		helpRow("Esc", "Quit");
		helpRow("F1", "Toggle HUD");
		helpRow("F2", "Graphics panel (settings)");
		helpRow("F3", "Streaming panel");
		helpRow("F4", "World / biome map");
		helpRow("F5", "This help");
		helpRow("F6", "On-screen hints");
		helpRow("F7", "Performance (CPU/GPU profiler)");
		helpRow("F8", "Overview dashboard");
		helpRow("F9", "Chunk inspector");
		helpRow("F10", "Toggle VSync");
		helpRow("F11", "Memory / workload");
		helpRow("F12", "Render debug views");
		helpRow("X (flight)", "Toggle flight speed boost");

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
		ImGui::Text("C free mouse · F1–F9/F11/F12 panels · T block (%s) · LMB/RMB edit · B borders", block);
	}
	ImGui::End();
}

void GameUI::ensureBiomeTexture(int size)
{
	if (!m_vk)
		return;
	if (canReuseBiomeTexture(m_mapImage.image != VK_NULL_HANDLE,
							 m_mapDesc != VK_NULL_HANDLE,
							 m_mapImageSize,
							 size))
	{
		return;
	}

	if (m_mapImage.image != VK_NULL_HANDLE)
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
	m_mapImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void GameUI::recordPendingBiomeMapUpload(VkCommandBuffer cmd, StagingRing &stagingRing)
{
	if (m_pendingUpload.rgba.empty() || m_mapImage.image == VK_NULL_HANDLE)
		return;

	// Drop deferred uploads that have been superseded
	if (m_pendingUpload.requestId != m_mapRequestId)
	{
		m_pendingUpload = {};
		return;
	}

	const VkDeviceSize dataSize = m_pendingUpload.rgba.size() * sizeof(uint8_t);
	VkDeviceSize stagingOffset = 0;
	void *stagingPtr = nullptr;
	if (!stagingRing.alloc(dataSize, stagingOffset, stagingPtr))
	{
		// Slice was full this frame; defer upload to next frame.
		return;
	}

	std::memcpy(stagingPtr, m_pendingUpload.rgba.data(), dataSize);

	cmdTransitionImageLayout(cmd, m_mapImage.image, m_mapImageLayout,
							 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
							 m_mapImage.mipLevels, m_mapImage.arrayLayers);

	VkBufferImageCopy region{};
	region.bufferOffset = stagingOffset;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;
	region.imageOffset = {0, 0, 0};
	region.imageExtent = {m_pendingUpload.width, m_pendingUpload.height, 1};

	vkCmdCopyBufferToImage(cmd, stagingRing.buffer(), m_mapImage.image,
						   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	cmdTransitionImageLayout(cmd, m_mapImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
							 m_mapImage.mipLevels, m_mapImage.arrayLayers);

	m_mapImageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	m_mapHasTexture = true;
	m_mapLastPublishedAt = SDL_GetTicks() / 1000.0;
	m_pendingUpload.rgba.clear();
	m_pendingUpload.width = 0;
	m_pendingUpload.height = 0;
	m_pendingUpload.requestId = 0;
}

void GameUI::tickBiomeMap(GameUIFrame &frame)
{
	if (!frame.camera)
		return;

	// Invalidate if active world generation or seed changed
	if (frame.worldGenerationId != m_currentWorldGenId || frame.seed != m_currentSeed)
	{
		m_currentWorldGenId = frame.worldGenerationId;
		m_currentSeed = frame.seed;
		invalidateBiomeMap();
	}

	const double now = SDL_GetTicks() / 1000.0;
	const glm::vec3 cam = frame.camera->getPosition();
	const glm::vec2 playerXZ(cam.x, cam.z);

	// Detect player movement BEFORE consuming a finished result
	const bool playerMoved = shouldSupersedeBiomeMap(playerXZ, m_mapLastPlayer, m_mapFollow);
	if (playerMoved)
	{
		supersedeBiomeMapRequest();
	}

	// Check and consume completed async map BEFORE checking periodic refresh
	if (m_mapJob.isReady())
	{
		BiomeMapResult res = m_mapJob.future.get();
		m_mapJob.future = {};
		m_mapJob.cancel.reset();

		if (isBiomeMapResultAcceptable(res, frame.worldGenerationId, frame.seed, m_mapRequestId))
		{
			GetProfiler().addWorkerSample("BiomeMap", static_cast<float>(res.elapsedMs), m_mapCaptureEpoch);
			paintBiomeMapPlayerDot(res.rgba, res.grid, playerXZ);
			m_mapCenter = res.center;
			ensureBiomeTexture(res.size);
			m_pendingUpload = BiomeMapUpload{
				.rgba = std::move(res.rgba),
				.width = static_cast<uint32_t>(res.size),
				.height = static_cast<uint32_t>(res.size),
				.requestId = res.requestId
			};
		}
		else
		{
			// Late completion from older generation, seed, or superseded request:
			// drop cleanly and request fresh update for the active view
			m_mapNeedsUpdate = true;
		}
	}

	const bool running = m_mapJob.isRunning() || hasPendingBiomeMapUpload();

	// Periodic refresh triggered ONLY when no job is currently running
	const bool timeElapsed = (now - m_mapLastPublishedAt) >= 1.0;
	if (!running && timeElapsed)
	{
		requestBiomeMapRefresh();
	}

	if (!running && m_mapNeedsUpdate)
	{
		if (m_mapFollow)
			m_mapCenter = playerXZ;

		m_mapLastPlayer = playerXZ;
		m_mapNeedsUpdate = false;

		if (m_mapRequestId == 0)
			m_mapRequestId = 1;

		const uint64_t requestId = m_mapRequestId;
		auto cancelToken = std::make_shared<std::atomic<bool>>(false);
		m_mapCaptureEpoch = GetProfiler().captureEpoch();
		m_mapJob.requestId = requestId;
		m_mapJob.cancel = cancelToken;

		// Sequential/small-map scratch retains dense capacity across refreshes,
		// bounded by kMaxDenseDomainPoints and owned by GameUI. Parallel tiles
		// use separate thread-local scratch with only tiled-sized retention.
		if (!m_mapScratch)
		{
			m_mapScratch = std::make_shared<TerrainGenerator::BiomeRegionScratch>();
			m_mapScratch->retainedPointsCap = TerrainGenerator::kMaxDenseDomainPoints;
		}

		BiomeMapRequest req{
			.requestId = requestId,
			.worldGenerationId = frame.worldGenerationId,
			.seed = frame.seed,
			.center = m_mapCenter,
			.size = m_mapSize,
			.zoom = m_mapZoom,
			.cancelToken = cancelToken,
			.scratch = m_mapScratch
		};

		if (frame.submitBiomeMap)
		{
			m_mapJob.future = frame.submitBiomeMap(req);
		}
		else
		{
			m_mapJob.future = std::async(std::launch::async, [req]() {
				return generateBiomeMap(req);
			});
		}
	}
}
