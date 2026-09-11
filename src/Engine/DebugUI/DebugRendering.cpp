// Rendering panels (issue #179, category navigation issue #185):
//  - drawRendering ("Graphics", F2): settings shell — left category rail,
//    right content pane filled by graphics::drawCategory
//    (GraphicsCategories.cpp). Settings only.
//  - drawRenderDebug ("Render Debug", F12): the single discoverable surface
//    for diagnostic views + environment readouts. All debug selectors write
//    the SAME state the old Graphics combos did
//    (ShaderParameters::shadowDebug / waterDebugView,
//    PostProcessSettings::ssaoDebugView) — no second state path. A setting
//    changes behavior; a debug view explains it.

#include <Engine/DebugUI/DebugPanels.hpp>
#include <Engine/DebugUI/GraphicsUi.hpp>
#include <Engine/UiScale.hpp>
#include <Engine/DebugUI/DebugPanelUtil.hpp>
#include <Engine/GameUI.hpp>
#include <Engine/GpuProfile.hpp>
#include <Vulkan/VkGpuProfiler.hpp>

#include <imgui/imgui.h>
#include <imgui/imgui_impl_vulkan.h>

#include <cfloat>

namespace debugui
{

void drawRendering(UiState &s, GameUIFrame &frame)
{
	const float scale = ui::effectiveScale(frame.uiScale);
	ImGui::SetNextWindowSize(ImVec2(ui::scaled(430.f, scale), ui::scaled(540.f, scale)), ImGuiCond_FirstUseEver);
	// Only the minimum is scaled (usability floor at any UI scale); no
	// arbitrary maximum — a device-pixel cap broke resizing at 175/200%.
	ImGui::SetNextWindowSizeConstraints(ImVec2(ui::scaled(360.f, scale), ui::scaled(320.f, scale)),
										ImVec2(FLT_MAX, FLT_MAX));
	if (!ImGui::Begin(ui::windows::kGraphics, &s.panels.rendering))
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

	// Category navigation (issue #185): short names so the rail never scrolls
	// at the default height; the selection persists for the session in UiState.
	if (s.graphicsCategory < 0 || s.graphicsCategory >= graphics::kCategoryCount)
		s.graphicsCategory = 0;
	ImGui::BeginChild("##nav", ImVec2(ui::scaled(108.f, scale), 0.f), ImGuiChildFlags_Borders);
	for (int i = 0; i < graphics::kCategoryCount; ++i)
		if (ImGui::Selectable(graphics::categoryName(static_cast<graphics::Category>(i)),
							  s.graphicsCategory == i))
			s.graphicsCategory = i;
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild("##content", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders);
	graphics::drawCategory(static_cast<graphics::Category>(s.graphicsCategory), s, frame);
	ImGui::EndChild();

	ImGui::End();
}

void drawRenderDebug(UiState &s, GameUIFrame &frame)
{
	const float scale = ui::effectiveScale(frame.uiScale);
	ImGui::SetNextWindowSize(ImVec2(ui::scaled(380.f, scale), ui::scaled(520.f, scale)), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(ui::windows::kRenderDebug, &s.panels.renderDebug))
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

	ImGui::SeparatorText("Debug views");
	ImGui::TextDisabled("Single state path — same fields the old Graphics\ncombos wrote. A debug view explains behavior; it does\nnot change it.");
	{
		const char *shadowDebugNames[] = {"Off", "Cascade index", "Blend bands", "Texel density", "Receiver depth"};
		int shadowDebugIdx = int(sp.shadowDebug);
		if (ImGui::Combo("Shadow view", &shadowDebugIdx, shadowDebugNames, IM_ARRAYSIZE(shadowDebugNames)))
			sp.shadowDebug = float(shadowDebugIdx);

		const char *waterDebugNames[] = {"Off", "Wave normal", "Optical distance", "Fresnel", "SSR confidence"};
		int waterDebugIdx = int(sp.waterDebugView);
		if (ImGui::Combo("Water view", &waterDebugIdx, waterDebugNames, IM_ARRAYSIZE(waterDebugNames)))
			sp.waterDebugView = float(waterDebugIdx);

		ImGui::BeginDisabled(!pp.ssaoEnabled);
		const char *ssaoDebugViews[] = {"Off", "AO (final)", "AO (raw)", "Normals (view)"};
		int ssaoIdx = pp.ssaoDebugView;
		if (ImGui::Combo("SSAO view", &ssaoIdx, ssaoDebugViews, IM_ARRAYSIZE(ssaoDebugViews)))
			pp.ssaoDebugView = ssaoIdx;
		ImGui::EndDisabled();
		if (!pp.ssaoEnabled && ImGui::IsItemHovered())
			ImGui::SetTooltip("Enable SSAO in Graphics (F2) first.");
	}

	// Environment readouts (issue #185): moved out of the Graphics categories
	// — diagnostics live here, settings live in the categories.
	ImGui::SeparatorText("Environment");
	ImGui::Text("Day / sunset / night: %.2f / %.2f / %.2f",
				sp.dayFactor, sp.sunsetFactor, sp.nightFactor);
	ImGui::Text("Underwater: %s", pp.underwater ? "yes" : "no");

	// Exposure state: the ONLY GPU->CPU traffic here, pulled on demand at
	// ~10 Hz while this panel is visible on the fence-waited frame slot.
	ImGui::SeparatorText("Exposure");
	if (frame.worldRenderer->autoExposureSupported())
	{
		static float s_lastRefresh = -1.0f;
		const float now = static_cast<float>(ImGui::GetTime());
		if (now - s_lastRefresh >= 0.1f)
		{
			frame.worldRenderer->refreshExposureReadout(frame.frameIndex);
			s_lastRefresh = now;
		}
		const auto &exp = frame.worldRenderer->exposureReadout();
		ImGui::Text("Metered: %.2f EV", exp.meteredLogLum);
		ImGui::Text("Exposure: %.3f (target %.3f)", exp.adaptedExposure, exp.targetExposure);
		const char *clampTxt = exp.clampState == 1 ? "min clamp" : exp.clampState == 2 ? "max clamp" : "in range";
		ImGui::TextDisabled("Target %s", clampTxt);
	}
	else
	{
		ImGui::TextDisabled("Auto-exposure readout unsupported on this GPU");
		ImGui::Text("Manual exposure: %.3f (EV comp %+.1f)", pp.exposure, pp.exposureCompensation);
	}

	// Pipeline summary: what is active and what it costs.
	ImGui::SeparatorText("Pipeline");
	const char *presetNames[] = {"Low", "Medium", "High", "Cinematic"};
	ImGui::Text("Quality: %s   |   shadow map %u (active %u)",
				presetNames[int(pp.qualityPreset)], uint32_t(pp.shadowMapSize),
				frame.worldRenderer->activeShadowMapSize());
	ImGui::Text("Bloom %s  |  FXAA %s  |  SSAO %s  |  God rays %s",
				pp.bloomEnabled ? "on" : "off", pp.fxaaEnabled ? "on" : "off",
				pp.ssaoEnabled ? "on" : "off", pp.godRaysEnabled ? "on" : "off");
	ImGui::Text("Tone mapper: %s  |  auto exposure %s",
				pp.toneMapper == 0 ? "ACES" : "Reinhard",
				pp.autoExposureEnabled && frame.worldRenderer->autoExposureSupported() ? "on" : "off");
	ImGui::Text("Draw list: %zu   |   indirect commands: %u",
				frame.drawCount, frame.worldRenderer->lastIndirectCommandCount());

	if (frame.gpu && frame.gpu->latest().serial)
	{
		const GpuFrameSample &latest = frame.gpu->latest();
		ImGui::TextDisabled("Per-pass GPU cost (intervals overlap):");
		for (size_t i = 1; i < kGpuPassCount; ++i)
			if (latest.present[i])
				ImGui::Text("  %-12s %8.3f ms", kGpuPassNames[i], latest.ms[i]);
	}

	ImGui::End();
}

} // namespace debugui
