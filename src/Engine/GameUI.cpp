// ImGuiFileDialog (reached through GameUI.hpp) defines
// IMGUI_DEFINE_MATH_OPERATORS after imgui.h has already been included,
// which makes a later imgui_internal.h include fail its guard. Defining it
// here first lets imgui.h implement the operators and satisfies both.
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include "Engine/GameUI.hpp"

#include <Engine/BuildInfo.hpp>
#include <Engine/DebugUI/DebugUiEngine.hpp>
#include <Engine/DebugUI/DebugPanels.hpp>
#include <Engine/Profiler.hpp>
#include <Engine/UiShortcuts.hpp>
#include <Engine/UiTheme.hpp>
#include <Vulkan/StagingRing.hpp>
#include <ImGuiFileDialog.h>
#include <imgui/imgui.h>
#include <imgui/imgui_impl_vulkan.h>
#include <imgui/imgui_internal.h>
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
	m_mapGrid = {};
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
		// Issue #184: F1 owns the Status Overlay density — cycling through
		// Off / Minimal / Detailed replaces the old catch-all HUD toggle.
		m_debug.panels.statusOverlay =
			playerui::nextStatusOverlayDensity(m_debug.panels.statusOverlay);
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

	const ui::ShellToggles toggles{
		&m_debug.panels.statusOverlay, &m_debug.panels.playerPanel,
		&m_debug.panels.rendering, &m_debug.panels.streaming,
		&m_debug.panels.world, &m_debug.panels.help, &m_debug.panels.overlayHints,
		&m_debug.panels.overview, &m_debug.panels.performance, &m_debug.panels.playerDiagnostics,
		&m_debug.panels.renderDebug,
		&m_debug.panels.chunkInspector, &m_debug.panels.memory, &m_debug.panels.benchmark,
		&m_helpTabRequest};

	m_shell.beginFrame();
	m_shell.drawMainMenuBar(frame, toggles);

	if (m_debug.panels.statusOverlay != playerui::StatusOverlayDensity::Off)
		drawStatusOverlay(frame);
	if (m_debug.panels.playerPanel)
		drawPlayerPanel(frame);
	debugui::drawOverview(m_debug, frame);
	debugui::drawRendering(m_debug, frame);
	debugui::drawRenderDebug(m_debug, frame);
	debugui::drawStreaming(m_debug, frame);
	debugui::drawPerformance(m_debug, frame);
	debugui::drawPlayerDiagnostics(m_debug, frame);
	debugui::drawChunkInspector(m_debug, frame);
	debugui::drawMemory(m_debug, frame);
	debugui::drawBenchmarkPanel(m_debug, frame);
	if (m_debug.panels.world)
		drawWorld(frame);
	if (m_debug.panels.help)
		drawHelp(frame);

	// Report can stay open even if the panels that opened it are closed.
	if (frame.benchmark && frame.benchmark->showReport() && frame.benchmark->report().valid)
		debugui::drawBenchmarkReport(m_debug, frame);

	if (m_debug.panels.overlayHints && frame.mouseCaptured && *frame.mouseCaptured)
		drawOverlayHints(frame);

	// File dialog can outlive the Graphics panel; always process while open.
	displayResourcePackFileDialog(m_debug.resourcePackUi, m_debug.panels.rendering);
}

void GameUI::drawStatusOverlay(GameUIFrame &frame)
{
	// True gameplay overlay (issue #184): compact, read-only, semi-
	// transparent and non-interactive — it never steals gameplay mouse or
	// keyboard input. Anchored inside the shell's central game-view rect
	// (the passthru dock node), so side/bottom docks never cover it; offsets
	// follow the UI scale (issue #183).
	const ui::UiRect gameRect = m_shell.gameViewRect();
	const float scale = ui::effectiveScale(frame.uiScale);
	ImGui::SetNextWindowPos(ImVec2(gameRect.pos.x + ui::scaled(12.f, scale),
								   gameRect.pos.y + ui::scaled(12.f, scale)),
							ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.55f);
	constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
									   ImGuiWindowFlags_AlwaysAutoResize |
									   ImGuiWindowFlags_NoSavedSettings |
									   ImGuiWindowFlags_NoFocusOnAppearing |
									   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
									   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking;
	if (!ImGui::Begin("##statusOverlay", nullptr, flags))
	{
		ImGui::End();
		return;
	}

	// Frame health line is the whole Minimal density; the ms value is the
	// hierarchical-profiler CPU frame time — the same quantity the shell
	// status strip and Performance panel show, not the paced sim delta.
	ImGui::Text("%.0f FPS · %.1f ms", frame.fps, frame.cpuFrameMs);

	if (playerui::statusOverlayShowsWorld(m_debug.panels.statusOverlay))
	{
		const playerui::PlayerSnapshot &p = frame.player;
		ImGui::Text("XYZ %.1f / %.1f / %.1f", p.position.x, p.position.y, p.position.z);
		ImGui::Text("Chunk %d / %d · %s", p.chunkX, p.chunkZ,
					playerui::biomeDisplayName(p.biome));
		if (frame.selectedTexture)
			ImGui::Text("Selected: %s", textureName(*frame.selectedTexture));
		if (frame.highlight && frame.highlight->active)
			ImGui::Text("Target: %d / %d / %d",
						static_cast<int>(std::floor(frame.highlight->position.x)),
						static_cast<int>(std::floor(frame.highlight->position.y)),
						static_cast<int>(std::floor(frame.highlight->position.z)));
	}

	// State badges only while meaningful (issue #184) — never a permanent
	// wall of flags.
	const playerui::PlayerSnapshot &p = frame.player;
	const struct
	{
		const char *label;
		ui::StatusKind kind;
		bool active;
	} badges[] = {
		{"PAUSED", ui::StatusKind::Warn, frame.paused && *frame.paused},
		{"FLIGHT", ui::StatusKind::Info, p.flight},
		{"WAITING FOR TERRAIN", ui::StatusKind::Warn, p.waitingForTerrain},
		{"SWIMMING", ui::StatusKind::Info, p.swimming},
	};
	bool first = true;
	for (const auto &badge : badges)
	{
		if (!badge.active)
			continue;
		if (!first)
			ImGui::SameLine(0.f, ui::scaled(6.f, scale));
		ui::statusBadge(badge.label, badge.kind);
		first = false;
	}

	ImGui::End();
}

void GameUI::drawPlayerPanel(GameUIFrame &frame)
{
	const float scale = ui::effectiveScale(frame.uiScale);
	ImGui::SetNextWindowSize(ImVec2(ui::scaled(360.f, scale), ui::scaled(440.f, scale)),
							 ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(ui::windows::kPlayer, &m_debug.panels.playerPanel))
	{
		ImGui::End();
		return;
	}

	const playerui::PlayerSnapshot &p = frame.player;
	const bool benchmarkActive = frame.benchmark && frame.benchmark->isActive();

	// Interactive locomotion controls (issue #184 §2). All display values
	// come from the snapshot and all mutations go through the frame commands
	// — the panel never touches the Camera or player controller directly.
	// Raw physics counters live in the developer console's Player
	// Diagnostics window now.
	ui::sectionHeader("Movement");
	ImGui::BeginDisabled(benchmarkActive);
	bool flight = p.flight;
	if (ImGui::Checkbox("Debug flight [V]", &flight) && frame.setPlayerFlight)
		frame.setPlayerFlight(flight);
	ImGui::EndDisabled();
	float speed = p.cameraMovementSpeed;
	ImGui::BeginDisabled(!p.flight);
	if (ImGui::SliderFloat("Fly speed", &speed, 1.f, 200.f, "%.1f") &&
		frame.setCameraMovementSpeed)
		frame.setCameraMovementSpeed(speed);
	ImGui::EndDisabled();
	// Label is a string literal -> data() is null-terminated (printf-safe,
	// no per-frame allocation; issue #184 §7).
	ImGui::Text("%s · %.2f blocks/s", playerui::playerMotionLabel(p).data(), p.speed);
	if (p.status && *p.status)
		ImGui::TextWrapped("%s", p.status);

	// Camera behavior controls; isometric zoom only while relevant.
	ui::sectionHeader("Camera");
	const char *modes[] = {"Perspective", "Isometric"};
	int mode = p.cameraViewMode == playerui::CameraViewMode::Isometric ? 1 : 0;
	if (ImGui::Combo("Camera mode", &mode, modes, 2) && frame.setCameraMode)
		frame.setCameraMode(mode == 1 ? CameraMode::ISOMETRIC : CameraMode::PERSPECTIVE);
	if (p.cameraViewMode == playerui::CameraViewMode::Isometric)
	{
		float zoom = p.isometricZoom;
		if (ImGui::SliderFloat("Zoom", &zoom, 16.f, 256.f) && frame.setIsometricZoom)
			frame.setIsometricZoom(zoom);
	}
	float sensitivity = p.mouseSensitivity;
	if (ImGui::SliderFloat("Sensitivity", &sensitivity, 0.02f, 0.5f, "%.3f") &&
		frame.setMouseSensitivity)
		frame.setMouseSensitivity(sensitivity);

	ui::sectionHeader("Interaction");
	if (frame.selectedTexture)
	{
		// Stable alphabetical palette built once per process (issue #184 §7:
		// no per-frame sorting or allocation for static names).
		static const std::vector<playerui::BlockPaletteEntry> palette =
			playerui::buildBlockPalette();
		const int current = static_cast<int>(*frame.selectedTexture);
		if (ImGui::BeginCombo("Block [T]", textureName(*frame.selectedTexture)))
		{
			for (const playerui::BlockPaletteEntry &entry : palette)
			{
				const bool selected = (entry.id == current);
				if (ImGui::Selectable(entry.name.c_str(), selected))
					*frame.selectedTexture = static_cast<TextureType>(entry.id);
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
	}
	if (frame.render)
		ImGui::SliderInt("Raycast distance", &frame.render->raycastDistance, 2, 32);
	if (frame.highlight)
	{
		if (frame.highlight->active)
			ImGui::Text("Target: %d / %d / %d",
						static_cast<int>(std::floor(frame.highlight->position.x)),
						static_cast<int>(std::floor(frame.highlight->position.y)),
						static_cast<int>(std::floor(frame.highlight->position.z)));
		else
			ImGui::TextDisabled("Target: —");
	}

	// Only toggles that are frequently useful mid-gameplay (issue #184 §2).
	// Pause / mouse capture stay in the World menu + shortcuts; VSync moved
	// to the Graphics panel; mob/physics counters moved to the console.
	ui::sectionHeader("World toggles");
	if (frame.showChunkBorders)
		ImGui::Checkbox("Chunk borders [B]", frame.showChunkBorders);
	if (frame.showDemoPlayers)
		ImGui::Checkbox("Demo players", frame.showDemoPlayers);
	if (frame.mobsEnabled)
		ImGui::Checkbox("Passive mobs", frame.mobsEnabled);

	ImGui::End();
}

void GameUI::drawWorld(GameUIFrame &frame)
{
	const float scale = ui::effectiveScale(frame.uiScale);
	ImGui::SetNextWindowSize(ImVec2(ui::scaled(380.f, scale), ui::scaled(560.f, scale)),
							 ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(ui::windows::kWorld, &m_debug.panels.world))
	{
		ImGui::End();
		return;
	}

	// Drive the async biome-map pipeline first so the pending indicator and
	// overlays reflect this frame's state. The tick only consumes accepted
	// results and (re)dispatches superseded requests — no synchronous
	// generation or GPU work here (issue #186 §12).
	tickBiomeMap(frame);

	const glm::vec2 playerXZ(frame.player.position.x, frame.player.position.z);

	// World identity header (issue #186 §10): high-level facts only. The
	// persistence/save-status slot is deliberately left to #180 — until then
	// the wording stays neutral instead of advertising a product limitation.
	ui::metric("Seed", "%d", frame.seed);
	ui::metric("Player", "%.0f, %.0f", playerXZ.x, playerXZ.y);
	ImGui::TextDisabled("Chunk %d, %d", frame.player.chunkX, frame.player.chunkZ);

	ImGui::Spacing();
	bool prevFollow = m_mapFollow;
	ImGui::Checkbox("Follow player", &m_mapFollow);
	if (m_mapFollow && !prevFollow)
		supersedeBiomeMapRequest();
	ImGui::SameLine();
	if (ImGui::Button("Center"))
	{
		m_mapCenter = playerXZ;
		supersedeBiomeMapRequest();
	}
	ImGui::SameLine();
	ImGui::Checkbox("View dist", &m_mapShowViewDistance);
	ImGui::SameLine();
	ImGui::Checkbox("Chunk", &m_mapShowChunkMarker);

	// The biome map is the dominant surface (issue #186 §7): fill the
	// available width, keep the square aspect, and reserve the same height
	// while the first map generates so the layout does not jump.
	const float mapSizePx = std::max(ImGui::GetContentRegionAvail().x, ui::scaled(160.f, scale));
	const bool hasTexture = m_mapHasTexture && m_mapDesc != VK_NULL_HANDLE;
	ImVec2 mapMin{0.f, 0.f}, mapMax{0.f, 0.f};
	bool mapDrawn = false;
	if (hasTexture)
	{
		ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(m_mapDesc)),
					 ImVec2(mapSizePx, mapSizePx));
		mapMin = ImGui::GetItemRectMin();
		mapMax = ImGui::GetItemRectMax();
		mapDrawn = true;
	}
	else
	{
		ImGui::Dummy(ImVec2(mapSizePx, mapSizePx));
		const ImVec2 rMin = ImGui::GetItemRectMin();
		const ImVec2 rMax = ImGui::GetItemRectMax();
		const char *label = "Generating biome map…";
		const ImVec2 ts = ImGui::CalcTextSize(label);
		ImGui::GetWindowDrawList()->AddText(
			ImVec2((rMin.x + rMax.x - ts.x) * 0.5f, (rMin.y + rMax.y - ts.y) * 0.5f),
			ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
	}

	// Mouse-wheel zoom while hovering the map (issue #186 §9): exponential
	// steps feel equal per notch; the wheel key is claimed so hovering the
	// map does not scroll the panel. Supersede semantics are identical to
	// the slider: every change bumps the request id and cancels the old job.
	if (mapDrawn && ImGui::IsItemHovered())
	{
		const float wheel = ImGui::GetIO().MouseWheel;
		if (wheel != 0.f)
		{
			const float prev = m_mapZoom;
			m_mapZoom = std::clamp(m_mapZoom * std::pow(1.15f, wheel), 0.1f, 8.f);
			if (m_mapZoom != prev)
				supersedeBiomeMapRequest();
			ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
		}
	}

	if (mapDrawn && m_mapGrid.valid())
	{
		// Cheap draw-list overlays (issue #186 §8), mapped through the grid
		// of the PUBLISHED texture so markers stay glued to the shown pixels
		// even while a newer request with different zoom/center is in flight.
		ImDrawList *draw = ImGui::GetWindowDrawList();
		draw->PushClipRect(mapMin, mapMax, true);
		const float pxPerMapPx = (mapMax.x - mapMin.x) / static_cast<float>(m_mapGrid.width);
		const auto toScreen = [&](glm::vec2 world) {
			const glm::vec2 pixel = biomeMapContinuousPixel(m_mapGrid, world);
			return ImVec2(mapMin.x + pixel.x * pxPerMapPx, mapMin.y + pixel.y * pxPerMapPx);
		};
		const ImVec2 playerPx = toScreen(playerXZ);

		// View-distance circle: reflects the configured radius at the shown
		// scale. Labelled as view distance — front bias/unload hysteresis
		// make residency asymmetric, so this is not "loaded chunks".
		if (m_mapShowViewDistance && frame.render)
		{
			const float radiusPx =
				static_cast<float>(frame.render->maxRenderDistance) / m_mapGrid.step * pxPerMapPx;
			draw->AddCircle(playerPx, radiusPx, IM_COL32(255, 255, 255, 90), 0, 1.4f);
		}

		// Current-chunk footprint marker (16 blocks per side).
		if (m_mapShowChunkMarker)
		{
			const glm::vec2 chunkWorld(static_cast<float>(frame.player.chunkX) * CHUNK_SIZE,
									   static_cast<float>(frame.player.chunkZ) * CHUNK_SIZE);
			const ImVec2 chunkMin = toScreen(chunkWorld);
			const float chunkPx =
				static_cast<float>(CHUNK_SIZE) / m_mapGrid.step * pxPerMapPx;
			draw->AddRect(chunkMin, ImVec2(chunkMin.x + chunkPx, chunkMin.y + chunkPx),
						  IM_COL32(255, 230, 120, 110), 0.f, 0.f, 1.2f);
		}

		// Live player marker (the baked dot from the last accepted result
		// can trail the player between refreshes).
		const float markerR = ui::scaled(4.f, scale);
		draw->AddCircleFilled(playerPx, markerR, IM_COL32(255, 255, 255, 230));
		draw->AddCircle(playerPx, markerR + 1.5f, IM_COL32(0, 0, 0, 200), 0, 2.0f);

		// Map-center marker while navigation is detached from the player.
		if (!m_mapFollow)
		{
			const ImVec2 centerPx = toScreen(m_mapCenter);
			const float arm = ui::scaled(5.f, scale);
			draw->AddLine(ImVec2(centerPx.x - arm, centerPx.y),
						  ImVec2(centerPx.x + arm, centerPx.y), IM_COL32(255, 255, 255, 140), 1.2f);
			draw->AddLine(ImVec2(centerPx.x, centerPx.y - arm),
						  ImVec2(centerPx.x, centerPx.y + arm), IM_COL32(255, 255, 255, 140), 1.2f);
		}
		draw->PopClipRect();

		if (m_mapShowViewDistance && frame.render && ImGui::IsItemHovered())
			ImGui::SetTooltip("View distance: %d blocks", frame.render->maxRenderDistance);
	}

	// Pending state (issue #186 §9): visible while a job or upload is in
	// flight; the previous map stays on screen meanwhile.
	if (m_mapJob.isRunning() || hasPendingBiomeMapUpload())
		ImGui::TextDisabled("Updating map…");

	float prevZoom = m_mapZoom;
	if (ImGui::SliderFloat("Zoom", &m_mapZoom, 0.1f, 8.f, "%.2f×"))
	{
		if (m_mapZoom != prevZoom)
			supersedeBiomeMapRequest();
	}

	if (ImGui::CollapsingHeader("Legend"))
	{
		if (ImGui::BeginChild("BiomeLegend", ImVec2(0.f, ui::scaled(170.f, scale)),
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
									   ImGuiColorEditFlags_NoTooltip,
									   ImVec2(ui::scaled(12.f, scale), ui::scaled(12.f, scale)));
					ImGui::SameLine();
					ImGui::TextUnformatted(biomeTypeString[i]);
				}
				ImGui::EndTable();
			}
		}
		ImGui::EndChild();
	}

	ImGui::End();
}

void GameUI::drawHelp(GameUIFrame &frame)
{
	const float scale = ui::effectiveScale(frame.uiScale);
	ImGui::SetNextWindowSize(ImVec2(ui::scaled(560.f, scale), ui::scaled(500.f, scale)), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(ui::windows::kHelp, &m_debug.panels.help))
	{
		ImGui::End();
		return;
	}

	// One-shot tab request (Help > Controls / Help > About / ft_vox > About):
	// opening the window selects the requested tab; consumed once.
	const ui::HelpTabRequest tabRequest = m_helpTabRequest;
	m_helpTabRequest = ui::HelpTabRequest::None;

	if (ImGui::BeginTabBar("HelpTabs"))
	{
		if (ImGui::BeginTabItem("Controls", nullptr,
								tabRequest == ui::HelpTabRequest::Controls
									? ImGuiTabItemFlags_SetSelected
									: ImGuiTabItemFlags_None))
		{
			ImGui::TextWrapped("ft_vox — Vulkan voxel sandbox engine. Press C to free the mouse and use the UI.");
			ImGui::Spacing();

			ui::sectionHeader("Movement");
			if (ImGui::BeginTable("keys_move", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
			{
				ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, ui::scaled(140.f, scale));
				ImGui::TableSetupColumn("Action");
				ImGui::TableHeadersRow();
				helpRow("W A S D", "Move");
				helpRow("Space", "Jump / swim up / fly up");
				helpRow("Shift", "Sprint / swim down / fly down");
				helpRow("Mouse", "Look");
				ImGui::EndTable();
			}

			ui::sectionHeader("Interaction");
			if (ImGui::BeginTable("keys_gameplay", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
			{
				ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, ui::scaled(140.f, scale));
				ImGui::TableSetupColumn("Action");
				ImGui::TableHeadersRow();
				helpRow("LMB / RMB", "Break / place block");
				for (const ui::ShortcutRef &ref : ui::kShortcuts)
					if (!ref.global)
						helpRow(ref.key, ref.action);
				helpRow("Esc", "Quit");
				ImGui::EndTable();
			}

			ui::sectionHeader("UI / debug shortcuts");
			if (ImGui::BeginTable("keys_ui", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
			{
				ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, ui::scaled(140.f, scale));
				ImGui::TableSetupColumn("Action");
				ImGui::TableHeadersRow();
				for (const ui::ShortcutRef &ref : ui::kShortcuts)
					if (ref.global)
						helpRow(ref.key, ref.action);
				ImGui::EndTable();
			}
			ImGui::TextDisabled("Gameplay keys are inert while typing in a text field.");
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("About", nullptr,
								tabRequest == ui::HelpTabRequest::About
									? ImGuiTabItemFlags_SetSelected
									: ImGuiTabItemFlags_None))
		{
			ImGui::TextUnformatted("ft_vox — Vulkan voxel sandbox engine");
			ImGui::TextWrapped("Procedural infinite terrain, greedy meshing, cascaded shadows, HDR post.");

			ui::sectionHeader("Build");
			static const std::string sRevision = BuildInfo::revisionLabel();
			static const std::string sBranch = BuildInfo::gitBranch();
			ui::metric("Revision", "%s", sRevision.c_str());
			ui::metric("Branch", "%s", sBranch.c_str());
			ui::metric("Built (UTC)", "%s", BuildInfo::buildUtc());
#ifdef NDEBUG
			ui::metric("Build type", "Release");
#else
			ui::metric("Build type", "Debug");
#endif

			ui::sectionHeader("Runtime");
			ui::metric("Renderer", "Vulkan %u.%u",
					   VK_VERSION_MAJOR(frame.vkApiVersion), VK_VERSION_MINOR(frame.vkApiVersion));
			if (frame.deviceName)
				ui::metric("Device", "%s", frame.deviceName);
			ui::metric("Validation", frame.validation ? "on" : "off");
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::End();
}

void GameUI::drawOverlayHints(GameUIFrame &frame)
{
	const ImGuiIO &io = ImGui::GetIO();
	const float scale = ui::effectiveScale(frame.uiScale);
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - ui::scaled(36.f, scale)),
							ImGuiCond_Always, ImVec2(0.5f, 1.f));
	ImGui::SetNextWindowBgAlpha(0.35f);
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
							 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
							 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
							 ImGuiWindowFlags_NoDocking;
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

	// Drop deferred uploads that have been superseded while they waited for
	// staging space. Only the pending upload (pixels + its grid) is dropped;
	// the published grid is untouched so the older texture keeps its own
	// mapping (issue #191 review).
	if (isBiomeMapUploadSuperseded(m_pendingUpload, m_mapRequestId))
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
	// The GPU recording just committed THESE pixels: publish their grid with
	// them, atomically (issue #191 review). At any instant m_mapGrid
	// describes exactly the texture currently displayed.
	m_mapGrid = m_pendingUpload.grid;
	m_mapHasTexture = true;
	m_mapLastPublishedAt = SDL_GetTicks() / 1000.0;
	m_pendingUpload.rgba.clear();
	m_pendingUpload.width = 0;
	m_pendingUpload.height = 0;
	m_pendingUpload.requestId = 0;
	m_pendingUpload.grid = {};
}

void GameUI::tickBiomeMap(GameUIFrame &frame)
{
	// Invalidate if active world generation or seed changed
	if (frame.worldGenerationId != m_currentWorldGenId || frame.seed != m_currentSeed)
	{
		m_currentWorldGenId = frame.worldGenerationId;
		m_currentSeed = frame.seed;
		invalidateBiomeMap();
	}

	const double now = SDL_GetTicks() / 1000.0;
	// Player position comes from the read-only frame snapshot (issue #186
	// architecture note): the map pipeline reads it like any other UI
	// consumer instead of reaching into the Camera.
	const glm::vec2 playerXZ(frame.player.position.x, frame.player.position.z);

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
			// Stage the pixels AND their grid together (issue #191 review):
			// the published grid only switches when the GPU recording of
			// THIS upload commits, never at CPU-accept time.
			m_pendingUpload = BiomeMapUpload{
				.rgba = std::move(res.rgba),
				.width = static_cast<uint32_t>(res.size),
				.height = static_cast<uint32_t>(res.size),
				.requestId = res.requestId,
				.grid = res.grid
			};
			ensureBiomeTexture(res.size);
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
