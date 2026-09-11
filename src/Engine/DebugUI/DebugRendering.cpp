// Rendering panels (issue #179):
//  - drawRendering ("Graphics", F2): user-facing tuning only — quality
//    preset, resource pack, atmosphere, lighting, water/shadow/post knobs.
//  - drawRenderDebug ("Render Debug", F12): the single discoverable surface
//    for diagnostic views. All debug selectors write the SAME state the old
//    Graphics combos did (ShaderParameters::shadowDebug / waterDebugView,
//    PostProcessSettings::ssaoDebugView) — no second state path. A setting
//    changes behavior; a debug view explains it.

#include <Engine/DebugUI/DebugPanels.hpp>
#include <Engine/DebugUI/DebugPanelUtil.hpp>
#include <Engine/GameUI.hpp>
#include <Engine/GpuProfile.hpp>
#include <Vulkan/VkGpuProfiler.hpp>

#include <imgui/imgui.h>
#include <imgui/imgui_impl_vulkan.h>

namespace debugui
{

void drawRendering(UiState &s, GameUIFrame &frame)
{
	ImGui::SetNextWindowSize(ImVec2(400, 520), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Graphics", &s.panels.rendering))
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
		ImGui::TextDisabled("Packs shadow resolution / SSAO / water SSR & shadows / bloom / god rays / grain / spatial AA (Low: off). Manual sliders below still work.");
	}

	drawResourcePackSection(frame, s.resourcePackUi, s.panels.rendering);
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
		// Preset values match the actual sun curve in updateAtmosphereFromDayTime
		// (sunAngle = dayTime*2π − π/2): noon peaks at 0.5, midnight is 0.0.
		if (ImGui::Button("Sunrise"))
			preset(0.25f);
		ImGui::SameLine();
		if (ImGui::Button("Noon"))
			preset(0.5f);
		ImGui::SameLine();
		if (ImGui::Button("Sunset"))
			preset(0.75f);
		ImGui::SameLine();
		if (ImGui::Button("Midnight"))
			preset(0.0f);

		ImGui::Text("Day / sunset / night: %.2f / %.2f / %.2f",
					sp.dayFactor, sp.sunsetFactor, sp.nightFactor);
		ImGui::SliderFloat("Ambient", &sp.ambientStrength, 0.f, 1.f);
		ImGui::SliderFloat("Diffuse", &sp.diffuseIntensity, 0.f, 1.5f);
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
		ImGui::SliderFloat("Water roughness", &sp.waterRoughness, 0.04f, 0.35f, "%.2f");
		ImGui::Text("Underwater: %s", pp.underwater ? "yes" : "no");
		ImGui::SliderFloat("Underwater strength", &pp.underwaterStrength, 0.f, 1.5f);
		ImGui::TextDisabled("Diagnostic water views: Render Debug (F12).");
	}

	if (ImGui::CollapsingHeader("Shadows (CSM)"))
	{
		if (frame.render)
			ImGui::SliderFloat("Cascade far", &frame.render->shadowCascadeFar, 64.f, 512.f);
		// Shadow quality tier (issue #137): the engine recreates the shadow
		// map array deferred when the requested size differs. All four CLI
		// sizes are selectable so an override is not silently relabeled.
		const char *shadowSizeNames[] = {"512", "1024 (Low/Medium)", "2048 (High/Cinematic)", "4096"};
		int shadowSizeIdx = pp.shadowMapSize <= 512 ? 0 : (pp.shadowMapSize <= 1024 ? 1 : (pp.shadowMapSize <= 2048 ? 2 : 3));
		if (ImGui::Combo("Shadow resolution", &shadowSizeIdx, shadowSizeNames, IM_ARRAYSIZE(shadowSizeNames)))
			pp.shadowMapSize = shadowSizeIdx == 0 ? 512 : shadowSizeIdx == 1 ? 1024 : shadowSizeIdx == 2 ? 2048 : 4096;
		ImGui::TextDisabled("Diagnostic shadow views: Render Debug (F12).");
	}

	// Issue #161: these knobs grade the terrain and mob lit-material shaders
	// only — not water, and not the composited frame (that is the post
	// stack's "Post saturation" / "Post contrast"). The header and tooltip
	// must keep saying so; generic "Visual" labels proved misleading.
	if (ImGui::CollapsingHeader("Material grading (terrain & mobs)"))
	{
		ImGui::TextDisabled("Terrain + mob materials only; water and full-frame\n"
							"grading live under Post-processing.");
		ImGui::SliderFloat("Material saturation", &sp.materialSaturation, 0.f, 3.f);
		ImGui::SliderFloat("Material color boost", &sp.materialColorBoost, 0.5f, 2.5f);
		ImGui::SliderFloat("Material contrast", &sp.materialContrast, 0.5f, 1.8f);
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
		ImGui::Checkbox("Spatial AA (FXAA 3.11)", &pp.fxaaEnabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Dedicated FXAA 3.11 pass on the tone-mapped image (issue #143):\nimproves voxel silhouettes and foliage edges; Off keeps the direct composite path.\nThe Low preset disables AA.");
		// True capability, not just the setting: without fragment SSBO stores
		// the renderer runs the manual path regardless of the checkbox, so it
		// must stay togglable and the manual slider must stay editable.
		const bool autoSupported = frame.worldRenderer->autoExposureSupported();
		const bool autoActive = pp.autoExposureEnabled && autoSupported;
		ImGui::BeginDisabled(!autoSupported);
		ImGui::Checkbox("Auto exposure", &pp.autoExposureEnabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(autoSupported
								  ? "Meter HDR scene luminance and adapt exposure over time (eye adaptation)."
								  : "Fragment SSBO stores unavailable on this GPU — the manual path is used.");
		ImGui::EndDisabled();
		// Greyed out while auto exposure actually drives the frame, but shows
		// (and stays editable for) the value used as soon as auto is off.
		ImGui::BeginDisabled(autoActive);
		ImGui::SliderFloat("Manual exposure", &pp.exposure, 0.1f, 5.f);
		ImGui::EndDisabled();
		ImGui::SliderFloat("Compensation (EV)", &pp.exposureCompensation, -3.0f, 3.0f, "%.1f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Auto-exposure bias in stops. 0 = neutral, +1 doubles the target exposure.");
		if (autoActive)
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
		ImGui::SliderFloat("Gamma", &pp.gamma, 0.5f, 2.5f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Artistic midtone grade (1.0 = neutral linear display)");
		ImGui::SliderFloat("Post saturation", &pp.postSaturation, 0.5f, 2.f);
		ImGui::SliderFloat("Post contrast", &pp.postContrast, 0.5f, 1.8f);
		ImGui::SliderFloat("Film grain", &pp.filmGrain, 0.f, 0.12f, "%.3f");
		ImGui::SliderFloat("Vignette", &pp.vignette, 0.f, 1.f);
		const char *toneMappers[] = {"ACES Filmic", "Reinhard"};
		ImGui::Combo("Tone mapper", &pp.toneMapper, toneMappers, IM_ARRAYSIZE(toneMappers));

		ImGui::Separator();
		// Leaving SSAO off while a debug view is selected would freeze the
		// frame on that debug output (composite checks the debug flag before
		// ssaoEnabled) with the selector hidden — reset it on disable.
		if (ImGui::Checkbox("SSAO", &pp.ssaoEnabled) && !pp.ssaoEnabled)
			pp.ssaoDebugView = 0;
		if (pp.ssaoEnabled)
		{
			ImGui::SliderFloat("SSAO radius (m)", &pp.ssaoRadius, 0.05f, 3.0f);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Occluder search radius in view-space meters at the pixel's depth (isotropic)");
			ImGui::SliderFloat("SSAO intensity", &pp.ssaoIntensity, 0.f, 2.f);
			ImGui::SliderInt("SSAO directions", &pp.ssaoDirections, 4, 8);
			ImGui::SliderInt("SSAO steps", &pp.ssaoSteps, 1, 4);
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
		ImGui::TextDisabled("Diagnostic views (shadow / water / SSAO / exposure):\nRender Debug (F12).");
	}

	ImGui::End();
}

void drawRenderDebug(UiState &s, GameUIFrame &frame)
{
	ImGui::SetNextWindowSize(ImVec2(380, 520), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Render Debug", &s.panels.renderDebug))
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
