// Graphics settings categories (issue #185): one function per category of
// the restructured "Graphics" panel. DebugRendering.cpp owns the window and
// the navigation rail; everything here renders inside the content pane.
// Settings only — diagnostic views and environment readouts stay in
// drawRenderDebug (F12).

#include <Engine/DebugUI/GraphicsUi.hpp>

#include <Engine/GameUI.hpp>
#include <Engine/UiScale.hpp>
#include <Engine/UiTheme.hpp>

#include <imgui/imgui.h>

#include <cfloat>
#include <cstdio>

namespace graphics
{
namespace
{
const char *presetName(GraphicsQualityPreset preset)
{
	switch (preset)
	{
	case GraphicsQualityPreset::Low: return "Low";
	case GraphicsQualityPreset::Medium: return "Medium";
	case GraphicsQualityPreset::High: return "High";
	case GraphicsQualityPreset::Cinematic: return "Cinematic";
	}
	return "?";
}
} // namespace

const char *categoryName(Category c)
{
	switch (c)
	{
	case Category::General: return "General";
	case Category::Display: return "Display";
	case Category::Lighting: return "Lighting";
	case Category::Atmosphere: return "Atmosphere";
	case Category::Shadows: return "Shadows";
	case Category::Water: return "Water";
	case Category::Post: return "Post";
	case Category::Resources: return "Resources";
	}
	return "?";
}

void drawCategory(Category c, debugui::UiState &s, GameUIFrame &frame)
{
	switch (c)
	{
	case Category::General: drawGeneral(s, frame); break;
	case Category::Display: drawDisplay(s, frame); break;
	case Category::Lighting: drawLighting(s, frame); break;
	case Category::Atmosphere: drawAtmosphere(s, frame); break;
	case Category::Shadows: drawShadows(s, frame); break;
	case Category::Water: drawWater(s, frame); break;
	case Category::Post: drawPost(s, frame); break;
	case Category::Resources: drawResources(s, frame); break;
	}
}

void drawGeneral(debugui::UiState &s, GameUIFrame &frame)
{
	(void)s;
	auto &pp = frame.worldRenderer->postSettings();

	ImGui::SeparatorText("Quality");
	const char *presetNames[] = {"Low", "Medium", "High", "Cinematic"};
	int presetIdx = static_cast<int>(pp.qualityPreset);
	if (ImGui::Combo("Quality", &presetIdx, presetNames, IM_ARRAYSIZE(presetNames)))
		pp.applyPreset(static_cast<GraphicsQualityPreset>(presetIdx));

	// Issue #185: surface drift from the selected pack instead of hiding it —
	// matchesPreset ignores the runtime underwater state + debug views.
	// Stacked vertically (no SameLine chain) so the reset button stays
	// visible at 360 px docked width.
	const bool custom = !PostProcessSettings::matchesPreset(pp, pp.qualityPreset);
	if (custom)
	{
		ImGui::Text("Preset: Custom (based on %s)", presetName(pp.qualityPreset));
		char resetLabel[48];
		std::snprintf(resetLabel, sizeof(resetLabel), "Reset to %s",
					  presetName(pp.qualityPreset));
		if (ImGui::Button(resetLabel))
			pp.applyPreset(pp.qualityPreset);
	}
	else
	{
		ImGui::TextUnformatted("Preset:");
		ImGui::SameLine();
		ui::statusBadge(presetName(pp.qualityPreset), ui::StatusKind::Ok);
	}
	ui::helpMarker("Presets control shadow resolution, FXAA, bloom, SSAO, god rays, film "
				   "grain, vignette, post grade and the exposure/tonemap baseline. The pack "
				   "also drives runtime water tiers in the renderer (SSR march budget, "
				   "water shadows / caustics). Lighting, atmosphere, water appearance, "
				   "display, resource packs, UI scale and debug views are never touched.");
	ImGui::TextDisabled("Packs shadow resolution / SSAO / bloom / god rays / grain / spatial AA "
						"(Low: off) plus the runtime water quality tier.");
}

void drawDisplay(debugui::UiState &s, GameUIFrame &frame)
{
	(void)s;
	if (!frame.render)
	{
		ImGui::TextDisabled("Swapchain not ready.");
		return;
	}
	auto &rs = *frame.render;

	ImGui::SeparatorText("Presentation");
	// Single settings home for VSync (issue #185).
	// F10 remains the global quick action.
	bool vsync = rs.vsyncEnabled;
	// "(applying)" must also cover the frame where the toggle just happened:
	// vsyncPending is filled by drawUi before the panel draws, so a fresh
	// toggle would otherwise show stale feedback for one frame.
	bool applying = frame.vsyncPending;
	if (ImGui::Checkbox("VSync", &vsync) && frame.setVSync)
	{
		frame.setVSync(vsync);
		applying = true;
	}
	if (!rs.vsyncEnabled)
	{
		ImGui::SameLine();
		ui::helpMarker("No pacing or FPS cap — tearing is possible.");
	}
	ImGui::SameLine();
	ui::helpMarker("F10 toggles VSync from anywhere.");

	char present[96];
	std::snprintf(present, sizeof(present), "%s%s",
				  frame.presentModeName ? frame.presentModeName
										: (rs.vsyncEnabled ? "FIFO" : "Immediate"),
				  applying ? " (applying)" : "");
	ui::metric("Present mode", "%s", present);
}

void drawLighting(debugui::UiState &s, GameUIFrame &frame)
{
	(void)s;
	auto &sp = *frame.shader;

	ImGui::SeparatorText("Cycle");
	ImGui::Checkbox("Enabled", &sp.dayCycleEnabled);
	ImGui::SliderFloat("Time", &sp.dayTime, 0.f, 1.f, "%.3f");
	ImGui::SliderFloat("Speed", &sp.dayCycleSpeed, 0.f, 0.05f, "%.5f");

	auto preset = [&](float t) {
		sp.dayCycleEnabled = false;
		sp.dayTime = t;
	};
	// Preset values match the actual sun curve in updateAtmosphereFromDayTime
	// (sunAngle = dayTime*2π − π/2): noon peaks at 0.5, midnight is 0.0.
	// 2×2 grid instead of one SameLine row: full column width keeps the
	// buttons readable at 360 px docked width (issue #185 review).
	if (ImGui::BeginTable("##day_presets", 2))
	{
		ImGui::TableNextColumn();
		if (ImGui::Button("Sunrise", ImVec2(-FLT_MIN, 0)))
			preset(0.25f);
		ImGui::TableNextColumn();
		if (ImGui::Button("Noon", ImVec2(-FLT_MIN, 0)))
			preset(0.5f);
		ImGui::TableNextColumn();
		if (ImGui::Button("Sunset", ImVec2(-FLT_MIN, 0)))
			preset(0.75f);
		ImGui::TableNextColumn();
		if (ImGui::Button("Midnight", ImVec2(-FLT_MIN, 0)))
			preset(0.0f);
		ImGui::EndTable();
	}

	ImGui::SeparatorText("Lighting");
	// Automatic atmosphere overwrites ambient/diffuse every frame — the
	// sliders stay visible but disabled so the state is truthful (issue #185).
	const bool autoAtmosphere = sp.automaticAtmosphere;
	ImGui::BeginDisabled(autoAtmosphere);
	ImGui::SliderFloat("Ambient", &sp.ambientStrength, 0.f, 1.f);
	ImGui::SliderFloat("Diffuse", &sp.diffuseIntensity, 0.f, 1.5f);
	ImGui::EndDisabled();
	if (autoAtmosphere)
		ui::helpMarker("Driven by Automatic atmosphere every frame — disable it in the Atmosphere category to edit.");
	ImGui::SliderFloat("Moon ambient", &sp.moonAmbientStrength, 0.f, 1.5f);
	ImGui::SliderFloat("Block light", &sp.blockLightScale, 0.f, 2.f);
	ImGui::SliderFloat("Emissive", &sp.emissiveScale, 0.f, 3.f);

	ImGui::SeparatorText("Materials");
	// Issue #161: these knobs grade the terrain and mob lit-material shaders
	// only — not water, and not the composited frame (that is Post ▸ Tone /
	// color). Keep saying so; generic labels proved misleading.
	ImGui::TextDisabled("Terrain and mob materials, before full-frame post.");
	ImGui::SliderFloat("Material saturation", &sp.materialSaturation, 0.f, 3.f);
	ImGui::SliderFloat("Material color boost", &sp.materialColorBoost, 0.5f, 2.5f);
	ImGui::SliderFloat("Material contrast", &sp.materialContrast, 0.5f, 1.8f);

	ImGui::Separator();
	if (ImGui::Button("Reset lighting"))
		resetGraphicsLighting(sp);
}

void drawAtmosphere(debugui::UiState &s, GameUIFrame &frame)
{
	(void)s;
	auto &sp = *frame.shader;

	ImGui::Checkbox("Automatic atmosphere", &sp.automaticAtmosphere);
	ui::helpMarker("Derives fog color/density and ambient/diffuse from the day cycle every frame.");

	ImGui::SeparatorText("Fog");
	// While automatic the engine overwrites fog each frame — show the live
	// engine-computed values but disabled so the state is truthful (#185).
	ImGui::BeginDisabled(sp.automaticAtmosphere);
	ImGui::SliderFloat("Start", &sp.fogStart, 0.f, 1000.f);
	ImGui::SliderFloat("End", &sp.fogEnd, sp.automaticAtmosphere ? 0.f : sp.fogStart + 1.f, 1400.f);
	ImGui::SliderFloat("Density", &sp.fogDensity, 0.f, 1.f);
	if (sp.automaticAtmosphere)
		ImGui::ColorEdit3("Color", &sp.fogColor.x, ImGuiColorEditFlags_NoInputs);
	else
		ImGui::ColorEdit3("Color", &sp.fogColor.x);
	ImGui::EndDisabled();

	ImGui::SeparatorText("Height falloff");
	ImGui::SliderFloat("Height falloff", &sp.fogHeightFalloff, 0.f, 0.05f, "%.4f");
	ImGui::SliderFloat("Base Y", &sp.fogBaseY, 0.f, 200.f);
}

void drawShadows(debugui::UiState &s, GameUIFrame &frame)
{
	(void)s;
	auto &pp = frame.worldRenderer->postSettings();

	ImGui::SeparatorText("Quality");
	// Shadow quality tier (issue #137): the engine recreates the shadow map
	// array deferred when the requested size differs. All four CLI sizes are
	// selectable so an override is not silently relabeled.
	const char *shadowSizeNames[] = {"512", "1024", "2048", "4096"};
	int shadowSizeIdx = pp.shadowMapSize <= 512 ? 0 : (pp.shadowMapSize <= 1024 ? 1 : (pp.shadowMapSize <= 2048 ? 2 : 3));
	if (ImGui::Combo("Resolution", &shadowSizeIdx, shadowSizeNames, IM_ARRAYSIZE(shadowSizeNames)))
		pp.shadowMapSize = shadowSizeIdx == 0 ? 512 : shadowSizeIdx == 1 ? 1024 : shadowSizeIdx == 2 ? 2048 : 4096;
	ui::helpMarker("Applied deferred — recreates the shadow map array before the next frame.");
	ImGui::TextDisabled("Active: %u", frame.worldRenderer->activeShadowMapSize());

	ImGui::SeparatorText("Range");
	if (frame.render)
	{
		auto &rs = *frame.render;
		// Moved from Streaming (issue #185): shadow reach is a rendering
		// quality knob; Streaming keeps the streaming budgets only.
		ImGui::SliderFloat("Shadow distance", &rs.shadowDistance, 64.f, 320.f, "%.0f");
		ui::helpMarker("Radius in blocks around the camera that casts shadows.");
		ImGui::SliderFloat("Cascade far", &rs.shadowCascadeFar, 64.f, 512.f);
	}
}

void drawWater(debugui::UiState &s, GameUIFrame &frame)
{
	(void)s;
	auto &sp = *frame.shader;
	auto &pp = frame.worldRenderer->postSettings();

	ImGui::SeparatorText("Surface");
	ImGui::SliderFloat("Wave strength", &sp.waterWaveStrength, 0.f, 0.5f);
	ImGui::SliderFloat("Refraction", &sp.waterRefraction, 0.f, 0.12f);
	ImGui::SliderFloat("Specular", &sp.waterSpecular, 0.f, 3.f);
	ImGui::SliderFloat("Foam", &sp.waterFoamStrength, 0.f, 2.f);
	ImGui::SliderFloat("Roughness", &sp.waterRoughness, 0.04f, 0.35f, "%.2f");

	ImGui::SeparatorText("Underwater");
	ImGui::SliderFloat("Strength", &pp.underwaterStrength, 0.f, 1.5f);
	ImGui::TextDisabled("State and debug views: Render Debug (F12).");

	ImGui::Separator();
	if (ImGui::Button("Reset water"))
		resetGraphicsWater(sp, pp);
}

void drawPost(debugui::UiState &s, GameUIFrame &frame)
{
	(void)s;
	auto &pp = frame.worldRenderer->postSettings();

	ImGui::SeparatorText("Exposure");
	// True capability, not just the setting: without fragment SSBO stores
	// the renderer runs the manual path regardless of the checkbox, so it
	// must stay togglable and the manual slider must stay editable.
	const bool autoSupported = frame.worldRenderer->autoExposureSupported();
	const bool autoActive = pp.autoExposureEnabled && autoSupported;
	ImGui::BeginDisabled(!autoSupported);
	ImGui::Checkbox("Auto exposure", &pp.autoExposureEnabled);
	// Disabled widgets eat hover by default — AllowWhenDisabled keeps the
	// capability explanation reachable on unsupported GPUs (issue #185 review).
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip(autoSupported
							  ? "Meter HDR scene luminance and adapt exposure over time (eye adaptation)."
							  : "Fragment SSBO stores unavailable on this GPU — the manual path is used.");
	ImGui::EndDisabled();
	if (!autoSupported)
		ImGui::TextDisabled("Auto exposure unavailable on this GPU.");
	// Greyed out while auto exposure actually drives the frame, but shows
	// (and stays editable for) the value used as soon as auto is off.
	ImGui::BeginDisabled(autoActive);
	ImGui::SliderFloat("Manual exposure", &pp.exposure, 0.1f, 5.f);
	ImGui::EndDisabled();
	ImGui::SliderFloat("Compensation (EV)", &pp.exposureCompensation, -3.0f, 3.0f, "%.1f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Auto-exposure bias in stops. 0 = neutral, +1 doubles the target exposure.");
	if (autoActive && ImGui::CollapsingHeader("Advanced auto-exposure"))
	{
		ImGui::Indent();
		ImGui::SliderFloat("Middle grey", &pp.autoExposureMiddleGrey, 0.1f, 2.0f, "%.2f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Target scene luminance (pre-tonemap). 0.18 = photographic middle grey.");
		ImGui::SliderFloat("Min EV", &pp.autoExposureMinEv, -6.0f, 0.0f, "%.1f");
		ImGui::SliderFloat("Max EV", &pp.autoExposureMaxEv, 0.0f, 6.0f, "%.1f");
		ImGui::SliderFloat("Adapt speed (brighten)", &pp.autoExposureSpeedUp, 0.25f, 10.0f, "%.2f /s");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How fast exposure drops when the scene brightens.");
		ImGui::SliderFloat("Adapt speed (darken)", &pp.autoExposureSpeedDown, 0.25f, 10.0f, "%.2f /s");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How fast exposure rises when the scene darkens (eye dilation).");
		ImGui::Unindent();
	}

	ImGui::SeparatorText("Tone / color");
	const char *toneMappers[] = {"ACES Filmic", "Reinhard"};
	ImGui::Combo("Tone mapper", &pp.toneMapper, toneMappers, IM_ARRAYSIZE(toneMappers));
	ImGui::SliderFloat("Gamma", &pp.gamma, 0.5f, 2.5f);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Artistic midtone grade (1.0 = neutral linear display)");
	// "Post" prefix keeps these distinct from the material grade (issue #161).
	ImGui::SliderFloat("Post saturation", &pp.postSaturation, 0.5f, 2.f);
	ImGui::SliderFloat("Post contrast", &pp.postContrast, 0.5f, 1.8f);

	ImGui::SeparatorText("Effects");
	ImGui::Checkbox("Bloom", &pp.bloomEnabled);
	if (pp.bloomEnabled)
	{
		ImGui::SliderFloat("Threshold", &pp.bloomThreshold, 0.f, 5.f);
		// "##bloom"/"##ssao" disambiguate the shared visible labels.
		ImGui::SliderFloat("Intensity##bloom", &pp.bloomIntensity, 0.f, 2.f);
		ImGui::SliderInt("Blur iterations", &pp.bloomBlurIterations, 1, 5);
	}
	ImGui::Checkbox("Spatial AA (FXAA 3.11)", &pp.fxaaEnabled);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Dedicated FXAA 3.11 pass on the tone-mapped image (issue #143):\nimproves voxel silhouettes and foliage edges; Off keeps the direct composite path.\nThe Low preset disables AA.");
	// Presets and post resets never touch ssaoDebugView (Render Debug owns
	// it, and the renderer gates stale views via effectiveSsaoDebugView).
	// Clearing it here is deliberate UX: an explicit SSAO disable also
	// dismisses the AO debug view the user was looking at.
	if (ImGui::Checkbox("SSAO", &pp.ssaoEnabled) && !pp.ssaoEnabled)
		pp.ssaoDebugView = 0;
	if (pp.ssaoEnabled)
	{
		ImGui::SliderFloat("Radius", &pp.ssaoRadius, 0.05f, 3.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Occluder search radius in view-space meters at the pixel's depth (isotropic)");
		ImGui::SliderFloat("Intensity##ssao", &pp.ssaoIntensity, 0.f, 2.f);
		ImGui::SliderInt("Directions", &pp.ssaoDirections, 4, 8);
		ImGui::SliderInt("Steps", &pp.ssaoSteps, 1, 4);
	}
	ImGui::SliderFloat("Film grain", &pp.filmGrain, 0.f, 0.12f, "%.3f");
	ImGui::SliderFloat("Vignette", &pp.vignette, 0.f, 1.f);

	ImGui::Checkbox("God rays", &pp.godRaysEnabled);
	if (pp.godRaysEnabled)
	{
		ImGui::SliderFloat("Density", &pp.godRaysDensity, 0.1f, 3.f);
		ImGui::SliderFloat("Weight", &pp.godRaysWeight, 0.001f, 0.05f, "%.4f");
		ImGui::SliderFloat("Decay", &pp.godRaysDecay, 0.9f, 1.f, "%.3f");
		ImGui::SliderFloat("Exposure", &pp.godRaysExposure, 0.f, 1.f);
		ImGui::Checkbox("Depth occlusion", &pp.godRaysDepthOcclusion);
		ImGui::Checkbox("Dynamic boost", &pp.godRaysDynamicBoostEnabled);
		if (pp.godRaysDynamicBoostEnabled && ImGui::CollapsingHeader("Advanced"))
		{
			// Boost preview renders the boosted energy directly — a tuning
			// aid, never a shipping look (unchanged since before #185).
			ImGui::Checkbox("Boost preview", &pp.godRaysBoostPreview);
			ImGui::SliderFloat("Dramatic boost", &pp.godRaysDramaticBoost, 1.f, 4.f, "%.2fx");
		}
	}

	ImGui::Separator();
	if (ImGui::Button("Reset post"))
		resetGraphicsPost(pp);
	ui::helpMarker("Restores the current quality pack's post-processing values. "
				   "Shadow resolution (Shadows category) and debug views are untouched.");
}

void drawResources(debugui::UiState &s, GameUIFrame &frame)
{
	drawResourcePackSection(frame, s.resourcePackUi, s.panels.rendering);
}

} // namespace graphics
