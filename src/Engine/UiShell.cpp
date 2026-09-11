#include "Engine/UiShell.hpp"

#include <Engine/GameUI.hpp>
#include <Engine/UiShortcuts.hpp>
#include <Engine/UiStatus.hpp>
#include <Engine/UiTheme.hpp>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace ui
{
void UiShell::beginFrame()
{
	// Persistent root dockspace. The passthru central node keeps the game
	// view visible and clickable; it survives resize / swapchain recreation
	// because it is recreated from the main viewport every frame. The user's
	// docking state persists through the normal imgui.ini settings.
	const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(
		0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

	// Dock-builder operations must run while the dockspace node exists, so
	// menu actions apply next frame; startup queuing applies on the first.
	if (m_resetLayoutQueued)
	{
		m_resetLayoutQueued = false;
		resetLayout(dockspaceId);
	}
	if (m_defaultLayoutQueued)
	{
		m_defaultLayoutQueued = false;
		applyDefaultDeveloperLayout(dockspaceId);
	}
}

void UiShell::resetLayout(ImGuiID dockspaceId)
{
	// Undock every window and recreate an empty dockspace. Predictable: no
	// imgui.ini surgery required, open windows simply float again.
	ImGui::DockBuilderRemoveNode(dockspaceId);
	ImGui::DockBuilderAddNode(dockspaceId,
							  ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
	ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);
	ImGui::DockBuilderFinish(dockspaceId);
	ImGui::MarkIniSettingsDirty();
}

void UiShell::applyDefaultDeveloperLayout(ImGuiID dockspaceId)
{
	ImGui::DockBuilderRemoveNode(dockspaceId);
	ImGui::DockBuilderAddNode(dockspaceId,
							  ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
	ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

	// left: primary panels (tabbed) | right: developer tools + help |
	// bottom: performance & benchmark | center: game view (empty passthru).
	ImGuiID dockLeft = 0, dockRest = 0;
	ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.24f, &dockLeft, &dockRest);
	ImGuiID dockRight = 0, dockCenter = 0;
	ImGui::DockBuilderSplitNode(dockRest, ImGuiDir_Right, 0.26f, &dockRight, &dockCenter);
	ImGuiID dockBottom = 0, dockCenterTop = 0;
	ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.28f, &dockBottom, &dockCenterTop);

	ImGui::DockBuilderDockWindow(windows::kGraphics, dockLeft);
	ImGui::DockBuilderDockWindow(windows::kStreaming, dockLeft);
	ImGui::DockBuilderDockWindow(windows::kWorld, dockLeft);

	ImGui::DockBuilderDockWindow(windows::kHelp, dockRight);
	ImGui::DockBuilderDockWindow(windows::kOverview, dockRight);
	ImGui::DockBuilderDockWindow(windows::kRenderDebug, dockRight);
	ImGui::DockBuilderDockWindow(windows::kChunkInspector, dockRight);
	ImGui::DockBuilderDockWindow(windows::kMemory, dockRight);

	ImGui::DockBuilderDockWindow(windows::kPerformance, dockBottom);
	ImGui::DockBuilderDockWindow(windows::kBenchmark, dockBottom);

	ImGui::DockBuilderFinish(dockspaceId);
	ImGui::MarkIniSettingsDirty();
}

void UiShell::drawMainMenuBar(GameUIFrame &frame, const ShellToggles &toggles)
{
	if (!ImGui::BeginMainMenuBar())
		return;

	const bool paused = frame.paused && *frame.paused;

	if (ImGui::BeginMenu("ft_vox"))
	{
		if (ImGui::MenuItem("About ft_vox"))
		{
			*toggles.help = true;
			*toggles.helpTabRequest = HelpTabRequest::About;
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Exit", "Esc"))
		{
			if (frame.requestExit)
				frame.requestExit();
		}
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("World"))
	{
		if (frame.paused)
			ImGui::MenuItem("Pause world tick", shortcutKeyName(SDLK_P), frame.paused);
		if (frame.mouseCaptured)
		{
			if (ImGui::MenuItem(*frame.mouseCaptured ? "Release mouse" : "Capture mouse",
								shortcutKeyName(SDLK_C)))
				*frame.mouseCaptured = !*frame.mouseCaptured;
		}
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("View"))
	{
		ImGui::MenuItem("Status overlay (HUD)", shortcutKeyName(SDLK_F1), toggles.hud);
		ImGui::MenuItem("Graphics", shortcutKeyName(SDLK_F2), toggles.graphics);
		ImGui::MenuItem("Streaming", shortcutKeyName(SDLK_F3), toggles.streaming);
		ImGui::MenuItem("World / biome map", shortcutKeyName(SDLK_F4), toggles.world);
		ImGui::MenuItem("On-screen hints", shortcutKeyName(SDLK_F6), toggles.overlayHints);
		ImGui::Separator();
		if (ImGui::BeginMenu("UI scale"))
		{
			static constexpr const char *kScaleLabels[] = {"100%", "125%", "150%", "175%", "200%"};
			static_assert(IM_ARRAYSIZE(kScaleLabels) == IM_ARRAYSIZE(kSupportedScales));
			for (std::size_t i = 0; i < std::size(kSupportedScales); ++i)
			{
				const bool selected = frame.uiScale > 0.f && isSupportedScale(frame.uiScale) &&
									  std::fabs(frame.uiScale - kSupportedScales[i]) < 0.001f;
				if (ImGui::MenuItem(kScaleLabels[i], nullptr, selected) && frame.setUiScale)
					frame.setUiScale(kSupportedScales[i]);
			}
			ImGui::EndMenu();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Reset layout"))
			queueResetLayout();
		if (ImGui::MenuItem("Apply default developer layout"))
		{
			queueDefaultLayout();
			*toggles.graphics = true;
			*toggles.streaming = true;
			*toggles.world = true;
			*toggles.performance = true;
			*toggles.help = true;
		}
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Developer"))
	{
		ImGui::MenuItem("Overview", shortcutKeyName(SDLK_F8), toggles.overview);
		ImGui::Separator();
		ImGui::MenuItem("Performance", shortcutKeyName(SDLK_F7), toggles.performance);
		ImGui::MenuItem("Memory", shortcutKeyName(SDLK_F11), toggles.memory);
		ImGui::MenuItem("Chunk inspector", shortcutKeyName(SDLK_F9), toggles.chunkInspector);
		ImGui::MenuItem("Render debug", shortcutKeyName(SDLK_F12), toggles.renderDebug);
		ImGui::MenuItem("Benchmark", nullptr, toggles.benchmark);
		ImGui::Separator();
		// Runtime/debug toggles previously reachable from the pre-shell menu
		// bar; Developer is their coherent home now that Graphics lives in
		// View as settings.
		if (frame.showChunkBorders)
			ImGui::MenuItem("Chunk borders", shortcutKeyName(SDLK_B), frame.showChunkBorders);
		if (frame.render && frame.setVSync)
		{
			if (ImGui::MenuItem("VSync", shortcutKeyName(SDLK_F10), &frame.render->vsyncEnabled))
				frame.setVSync(frame.render->vsyncEnabled);
		}
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Help"))
	{
		if (ImGui::MenuItem("Controls", shortcutKeyName(SDLK_F5)))
		{
			*toggles.help = true;
			*toggles.helpTabRequest = HelpTabRequest::Controls;
		}
		if (ImGui::MenuItem("About ft_vox"))
		{
			*toggles.help = true;
			*toggles.helpTabRequest = HelpTabRequest::About;
		}
		ImGui::EndMenu();
	}

	// Right-side status strip: right-aligned within the window-local region
	// after the menus (plus a breathing gap), degrading FPS then CPU frame
	// time when the bar is crowded. Frame time uses the hierarchical-profiler
	// value (issue #179), not the paced simulation delta. The paused state
	// stays visible longest, presented as a badge.
	char fps[24];
	std::snprintf(fps, sizeof(fps), "%.0f FPS", frame.fps);
	char ms[24];
	std::snprintf(ms, sizeof(ms), "%.1f ms", frame.cpuFrameMs);
	const char *badgeLabel = paused ? "PAUSED" : "LIVE";

	const ImGuiStyle &style = ImGui::GetStyle();
	const StatusRegion region = statusRegion(ImGui::GetCursorPosX(), ImGui::GetWindowWidth(),
											 style.WindowPadding.x, style.ItemSpacing.x);

	const float fpsWidth = ImGui::CalcTextSize(fps).x;
	const float msWidth = ImGui::CalcTextSize(ms).x;
	const float pipeWidth = ImGui::CalcTextSize("|").x;
	const float badgeWidth = ImGui::CalcTextSize(badgeLabel).x + style.FramePadding.x * 1.5f + 2.f;
	const float separatorBlock = style.ItemSpacing.x * 2.f + pipeWidth;
	const StatusStripPlan plan = planStatusStrip(
		region.width, 0.f, fpsWidth, msWidth, badgeWidth, separatorBlock);

	float fieldEnd = 0.f; // region-relative right edge of the last drawn field
	if (plan.showFps)
	{
		ImGui::SameLine(region.start + plan.fpsX);
		ImGui::TextUnformatted(fps);
		fieldEnd = plan.fpsX + fpsWidth;
	}
	if (plan.showMs)
	{
		if (plan.showFps)
		{
			ImGui::SameLine(region.start + (fieldEnd + plan.msX) * 0.5f - pipeWidth * 0.5f);
			ImGui::TextDisabled("|");
		}
		ImGui::SameLine(region.start + plan.msX);
		ImGui::TextDisabled("%s", ms);
		fieldEnd = plan.msX + msWidth;
	}
	if (plan.showBadge)
	{
		if (plan.showFps || plan.showMs)
		{
			ImGui::SameLine(region.start + (fieldEnd + plan.badgeX) * 0.5f - pipeWidth * 0.5f);
			ImGui::TextDisabled("|");
		}
		ImGui::SameLine(region.start + plan.badgeX);
		statusBadge(badgeLabel, paused ? StatusKind::Warn : StatusKind::Ok);
	}

	ImGui::EndMainMenuBar();
}
} // namespace ui
