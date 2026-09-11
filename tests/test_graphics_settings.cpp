// Graphics settings core (issue #185): canonical presetValues, truthful
// Custom detection via matchesPreset, applyPreset underwater preservation and
// the graphics-panel scoped resets. Pure logic — no ImGui, no window, no GPU.

#include <Engine/EngineDefs.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
bool fail(const std::string &msg)
{
	std::cerr << "FAIL: " << msg << "\n";
	return false;
}

// Full field comparison of two packs except qualityPreset: applyPreset stamps
// the tag but presetValues deliberately leaves it at the constructor default.
bool sameSettingsExceptPresetTag(const PostProcessSettings &a, const PostProcessSettings &b)
{
	return a.shadowMapSize == b.shadowMapSize && a.fxaaEnabled == b.fxaaEnabled &&
		   a.bloomEnabled == b.bloomEnabled && a.bloomThreshold == b.bloomThreshold &&
		   a.bloomIntensity == b.bloomIntensity && a.bloomBlurIterations == b.bloomBlurIterations &&
		   a.ssaoEnabled == b.ssaoEnabled && a.ssaoRadius == b.ssaoRadius &&
		   a.ssaoIntensity == b.ssaoIntensity && a.ssaoDirections == b.ssaoDirections &&
		   a.ssaoSteps == b.ssaoSteps && a.ssaoDebugView == b.ssaoDebugView &&
		   a.godRaysEnabled == b.godRaysEnabled && a.godRaysDensity == b.godRaysDensity &&
		   a.godRaysWeight == b.godRaysWeight && a.godRaysDecay == b.godRaysDecay &&
		   a.godRaysExposure == b.godRaysExposure &&
		   a.godRaysDynamicBoostEnabled == b.godRaysDynamicBoostEnabled &&
		   a.godRaysBoostPreview == b.godRaysBoostPreview &&
		   a.godRaysDramaticBoost == b.godRaysDramaticBoost &&
		   a.godRaysDepthOcclusion == b.godRaysDepthOcclusion && a.filmGrain == b.filmGrain &&
		   a.vignette == b.vignette && a.postSaturation == b.postSaturation &&
		   a.postContrast == b.postContrast && a.exposure == b.exposure &&
		   a.exposureCompensation == b.exposureCompensation && a.toneMapper == b.toneMapper &&
		   a.gamma == b.gamma && a.autoExposureEnabled == b.autoExposureEnabled &&
		   a.autoExposureMiddleGrey == b.autoExposureMiddleGrey &&
		   a.autoExposureMinEv == b.autoExposureMinEv && a.autoExposureMaxEv == b.autoExposureMaxEv &&
		   a.autoExposureSpeedUp == b.autoExposureSpeedUp &&
		   a.autoExposureSpeedDown == b.autoExposureSpeedDown && a.underwater == b.underwater &&
		   a.underwaterStrength == b.underwaterStrength &&
		   a.underwaterSurfaceY == b.underwaterSurfaceY;
}

void dirtyPresetFields(PostProcessSettings &pp)
{
	pp.shadowMapSize = 512;
	pp.bloomThreshold = 9.0f;
	pp.ssaoEnabled = !pp.ssaoEnabled;
	pp.ssaoIntensity = 9.0f;
	pp.godRaysDecay = 0.1f;
	pp.gamma = 2.2f;
	pp.exposure = 3.0f;
	pp.exposureCompensation = 1.5f;
	pp.filmGrain = 0.0f;
	pp.vignette = 0.9f;
	pp.toneMapper = 1;
	pp.autoExposureEnabled = false;
	pp.autoExposureSpeedDown = 4.0f;
}
} // namespace

int main()
{
	bool ok = true;
	const GraphicsQualityPreset kAllPresets[] = {
		GraphicsQualityPreset::Low,
		GraphicsQualityPreset::Medium,
		GraphicsQualityPreset::High,
		GraphicsQualityPreset::Cinematic,
	};

	// --- presetValues must equal the applyPreset output for every preset ---
	{
		for (const GraphicsQualityPreset preset : kAllPresets)
		{
			PostProcessSettings junk{};
			dirtyPresetFields(junk);
			PostProcessSettings applied = junk;
			applied.applyPreset(preset);
			const PostProcessSettings canonical = PostProcessSettings::presetValues(preset);
			if (!sameSettingsExceptPresetTag(applied, canonical))
				ok = fail("applyPreset output must equal presetValues for every field (issue #185)");
			if (applied.qualityPreset != preset)
				ok = fail("applyPreset must stamp qualityPreset");
		}
		// Fixture sanity: the junk writes must actually perturb fields
		// matchesPreset compares, or the equality above proves nothing.
		PostProcessSettings dirtied{};
		dirtyPresetFields(dirtied);
		if (sameSettingsExceptPresetTag(dirtied, PostProcessSettings{}))
			ok = fail("dirtyPresetFields fixture must perturb at least one preset-controlled field");
	}

	// --- applyPreset preserves runtime submersion + Render Debug state ---
	{
		PostProcessSettings pp{};
		pp.underwater = true;
		pp.underwaterStrength = 0.7f;
		pp.underwaterSurfaceY = 42.0f;
		pp.ssaoDebugView = 2;
		pp.applyPreset(GraphicsQualityPreset::High);
		if (!pp.underwater || pp.underwaterStrength != 0.7f || pp.underwaterSurfaceY != 42.0f)
			ok = fail("applyPreset must preserve underwater / underwaterStrength / underwaterSurfaceY");
		if (pp.ssaoDebugView != 2)
			ok = fail("applyPreset must never touch ssaoDebugView (Render Debug owns it, issue #185)");
		if (PostProcessSettings::presetValues(GraphicsQualityPreset::High).underwater)
			ok = fail("presetValues starts from fresh defaults, so underwater must be false");
	}

	// --- effectiveSsaoDebugView: a stale view can never outlive SSAO ---
	{
		PostProcessSettings live{};
		live.ssaoEnabled = true;
		live.ssaoDebugView = 3;
		if (effectiveSsaoDebugView(live) != 3)
			ok = fail("effectiveSsaoDebugView must pass the view through while SSAO is on");
		PostProcessSettings dead{};
		dead.ssaoEnabled = false;
		dead.ssaoDebugView = 3;
		if (effectiveSsaoDebugView(dead) != 0)
			ok = fail("effectiveSsaoDebugView must resolve to Off when SSAO is disabled");
	}

	// --- matchesPreset: truthful Custom detection (issue #185) ---
	{
		const PostProcessSettings def{};
		if (!PostProcessSettings::matchesPreset(def, GraphicsQualityPreset::Medium))
			ok = fail("constructor defaults must match the Medium preset (house rule)");
		const PostProcessSettings high = PostProcessSettings::presetValues(GraphicsQualityPreset::High);
		if (!PostProcessSettings::matchesPreset(high, GraphicsQualityPreset::High))
			ok = fail("presetValues(High) must match High");
		if (PostProcessSettings::matchesPreset(high, GraphicsQualityPreset::Medium))
			ok = fail("presetValues(High) must not match Medium");

		PostProcessSettings bloomOnly = high;
		bloomOnly.bloomIntensity = 0.55f;
		if (PostProcessSettings::matchesPreset(bloomOnly, GraphicsQualityPreset::High))
			ok = fail("a single preset-controlled tweak must trip Custom detection");

		PostProcessSettings debugOnly = high;
		debugOnly.ssaoDebugView = 3;
		if (!PostProcessSettings::matchesPreset(debugOnly, GraphicsQualityPreset::High))
			ok = fail("ssaoDebugView is a diagnostic and must not affect Custom detection");

		PostProcessSettings speedOnly = high;
		speedOnly.autoExposureSpeedDown = 0.5f;
		if (PostProcessSettings::matchesPreset(speedOnly, GraphicsQualityPreset::High))
			ok = fail("autoExposureSpeedDown is preset-controlled and must trip Custom detection");
	}

	// --- Reset-to-preset flow (the UI's "Reset to High") ---
	{
		PostProcessSettings pp = PostProcessSettings::presetValues(GraphicsQualityPreset::High);
		pp.vignette = 0.9f;
		pp.toneMapper = 1;
		if (PostProcessSettings::matchesPreset(pp, GraphicsQualityPreset::High))
			ok = fail("perturbed settings must not report matching High");
		pp.applyPreset(GraphicsQualityPreset::High);
		if (!PostProcessSettings::matchesPreset(pp, GraphicsQualityPreset::High))
			ok = fail("re-applying High must restore the exact canonical pack (issue #185)");
	}

	// --- All four presets are mutually distinguishable on the compared set ---
	{
		for (const GraphicsQualityPreset p : kAllPresets)
			for (const GraphicsQualityPreset q : kAllPresets)
				if (p != q && PostProcessSettings::matchesPreset(PostProcessSettings::presetValues(p), q))
					ok = fail("two different presets must never collide on the compared field set");
	}

	// --- resetGraphicsLighting: Lighting category only ---
	{
		ShaderParameters sp{};
		sp.dayCycleEnabled = false;
		sp.dayTime = 0.99f;
		sp.dayCycleSpeed = 5.0f;
		sp.ambientStrength = 9.0f;
		sp.diffuseIntensity = 9.0f;
		sp.moonAmbientStrength = 9.0f;
		sp.blockLightScale = 9.0f;
		sp.emissiveScale = 9.0f;
		sp.materialSaturation = 9.0f;
		sp.materialColorBoost = 9.0f;
		sp.materialContrast = 9.0f;
		// Out-of-category fields that must survive the scoped reset
		sp.fogStart = -123.0f;
		sp.fogColor = glm::vec3(-1.0f, -2.0f, -3.0f);
		sp.dayFactor = -4.0f;
		sp.waterDebugView = 2.0f;
		sp.shadowDebug = 4.0f;

		resetGraphicsLighting(sp);
		const ShaderParameters def{};
		if (sp.dayCycleEnabled != def.dayCycleEnabled || sp.dayTime != def.dayTime ||
			sp.dayCycleSpeed != def.dayCycleSpeed || sp.ambientStrength != def.ambientStrength ||
			sp.diffuseIntensity != def.diffuseIntensity ||
			sp.moonAmbientStrength != def.moonAmbientStrength ||
			sp.blockLightScale != def.blockLightScale || sp.emissiveScale != def.emissiveScale ||
			sp.materialSaturation != def.materialSaturation ||
			sp.materialColorBoost != def.materialColorBoost ||
			sp.materialContrast != def.materialContrast)
			ok = fail("resetGraphicsLighting must restore its 11 fields to constructor defaults (issue #185)");
		if (sp.fogStart != -123.0f || sp.fogColor != glm::vec3(-1.0f, -2.0f, -3.0f) || sp.dayFactor != -4.0f)
			ok = fail("resetGraphicsLighting must not touch fog/atmosphere or derived day factors");
		if (sp.waterDebugView != 2.0f || sp.shadowDebug != 4.0f)
			ok = fail("resetGraphicsLighting must not touch debug views");
	}

	// --- resetGraphicsWater: water sliders + underwater strength only ---
	{
		ShaderParameters sp{};
		PostProcessSettings pp{};
		sp.waterWaveStrength = 9.0f;
		sp.waterRefraction = 9.0f;
		sp.waterSpecular = 9.0f;
		sp.waterFoamStrength = 9.0f;
		sp.waterRoughness = 9.0f;
		pp.underwaterStrength = 0.7f;
		sp.waterDebugView = 3.0f;

		resetGraphicsWater(sp, pp);
		const ShaderParameters defSp{};
		const PostProcessSettings defPp{};
		if (sp.waterWaveStrength != defSp.waterWaveStrength || sp.waterRefraction != defSp.waterRefraction ||
			sp.waterSpecular != defSp.waterSpecular || sp.waterFoamStrength != defSp.waterFoamStrength ||
			sp.waterRoughness != defSp.waterRoughness)
			ok = fail("resetGraphicsWater must restore the five water sliders (issue #185)");
		if (pp.underwaterStrength != defPp.underwaterStrength)
			ok = fail("resetGraphicsWater must restore the underwater post strength");
		if (sp.waterDebugView != 3.0f)
			ok = fail("waterDebugView is a diagnostic and must survive resetGraphicsWater");
	}

	// --- resetGraphicsPost: Post category only, everything else preserved ---
	{
		// Base pack High; junk out-of-category state, dirty every post group.
		// The tag models real UI state, where the last applyPreset set it
		// (presetValues leaves the constructor default until it stamps tags).
		PostProcessSettings pp = PostProcessSettings::presetValues(GraphicsQualityPreset::High);
		pp.qualityPreset = GraphicsQualityPreset::High;
		pp.shadowMapSize = 4096;
		pp.ssaoDebugView = 3;
		pp.underwater = true;
		pp.underwaterStrength = 0.7f;
		pp.underwaterSurfaceY = 42.0f;
		pp.fxaaEnabled = true;
		pp.bloomEnabled = false;
		pp.bloomThreshold = 9.0f;
		pp.bloomIntensity = 9.0f;
		pp.bloomBlurIterations = 1;
		pp.ssaoEnabled = false;
		pp.ssaoRadius = 9.0f;
		pp.ssaoIntensity = 9.0f;
		pp.ssaoDirections = 4;
		pp.ssaoSteps = 1;
		pp.godRaysEnabled = false;
		pp.godRaysDensity = 9.0f;
		pp.godRaysWeight = 9.0f;
		pp.godRaysDecay = 0.1f;
		pp.godRaysExposure = 9.0f;
		pp.godRaysDynamicBoostEnabled = false;
		pp.godRaysBoostPreview = true;
		pp.godRaysDramaticBoost = 1.0f;
		pp.godRaysDepthOcclusion = false;
		pp.filmGrain = 0.0f;
		pp.vignette = 0.0f;
		pp.postSaturation = 0.5f;
		pp.postContrast = 0.5f;
		pp.exposure = 5.0f;
		pp.exposureCompensation = 3.0f;
		pp.toneMapper = 1;
		pp.gamma = 2.5f;
		pp.autoExposureEnabled = false;
		pp.autoExposureMiddleGrey = 2.0f;
		pp.autoExposureMinEv = -6.0f;
		pp.autoExposureMaxEv = 6.0f;
		pp.autoExposureSpeedUp = 10.0f;
		pp.autoExposureSpeedDown = 10.0f;

		resetGraphicsPost(pp);

		const PostProcessSettings canon = PostProcessSettings::presetValues(GraphicsQualityPreset::High);
		// Out-of-category / runtime state must survive untouched (issue #185):
		// shadow resolution is the Shadows category, the SSAO debug view is a
		// Render Debug diagnostic, submersion is engine runtime state.
		if (pp.shadowMapSize != 4096)
			ok = fail("resetGraphicsPost must not touch shadowMapSize (Shadows category)");
		if (pp.qualityPreset != GraphicsQualityPreset::High)
			ok = fail("resetGraphicsPost must not change the qualityPreset tag");
		if (pp.ssaoDebugView != 3)
			ok = fail("resetGraphicsPost must not touch ssaoDebugView (Render Debug owns it)");
		if (!pp.underwater || pp.underwaterStrength != 0.7f || pp.underwaterSurfaceY != 42.0f)
			ok = fail("resetGraphicsPost must preserve underwater runtime state");
		// Every post-owned field must be back at the pack's canonical value:
		if (pp.fxaaEnabled != canon.fxaaEnabled || pp.bloomEnabled != canon.bloomEnabled ||
			pp.bloomThreshold != canon.bloomThreshold || pp.bloomIntensity != canon.bloomIntensity ||
			pp.bloomBlurIterations != canon.bloomBlurIterations || pp.ssaoEnabled != canon.ssaoEnabled ||
			pp.ssaoRadius != canon.ssaoRadius || pp.ssaoIntensity != canon.ssaoIntensity ||
			pp.ssaoDirections != canon.ssaoDirections || pp.ssaoSteps != canon.ssaoSteps ||
			pp.godRaysEnabled != canon.godRaysEnabled || pp.godRaysDensity != canon.godRaysDensity ||
			pp.godRaysWeight != canon.godRaysWeight || pp.godRaysDecay != canon.godRaysDecay ||
			pp.godRaysExposure != canon.godRaysExposure ||
			pp.godRaysDynamicBoostEnabled != canon.godRaysDynamicBoostEnabled ||
			pp.godRaysBoostPreview != canon.godRaysBoostPreview ||
			pp.godRaysDramaticBoost != canon.godRaysDramaticBoost ||
			pp.godRaysDepthOcclusion != canon.godRaysDepthOcclusion ||
			pp.filmGrain != canon.filmGrain || pp.vignette != canon.vignette ||
			pp.postSaturation != canon.postSaturation || pp.postContrast != canon.postContrast ||
			pp.exposure != canon.exposure || pp.exposureCompensation != canon.exposureCompensation ||
			pp.toneMapper != canon.toneMapper || pp.gamma != canon.gamma ||
			pp.autoExposureEnabled != canon.autoExposureEnabled ||
			pp.autoExposureMiddleGrey != canon.autoExposureMiddleGrey ||
			pp.autoExposureMinEv != canon.autoExposureMinEv || pp.autoExposureMaxEv != canon.autoExposureMaxEv ||
			pp.autoExposureSpeedUp != canon.autoExposureSpeedUp ||
			pp.autoExposureSpeedDown != canon.autoExposureSpeedDown)
			ok = fail("resetGraphicsPost must restore every post field to the current pack (issue #185)");
	}

	// --- Atmosphere semantics pin the disabled-control rationale (#185) ---
	{
		// automaticAtmosphere=true: the six derived atmosphere fields are
		// engine-owned, which is why the UI disables their sliders.
		ShaderParameters auto_{};
		auto_.automaticAtmosphere = true;
		auto_.dayTime = 0.5f; // noon
		auto_.fogColor = glm::vec3(-1.0f, -1.0f, -1.0f);
		auto_.fogStart = -123.0f;
		auto_.fogEnd = -456.0f;
		auto_.fogDensity = -0.5f;
		auto_.ambientStrength = -1.0f;
		auto_.diffuseIntensity = -1.0f;
		updateAtmosphereFromDayTime(auto_);
		if (auto_.fogColor == glm::vec3(-1.0f, -1.0f, -1.0f) || auto_.fogStart == -123.0f ||
			auto_.fogEnd == -456.0f || auto_.fogDensity == -0.5f ||
			auto_.ambientStrength == -1.0f || auto_.diffuseIntensity == -1.0f)
			ok = fail("automaticAtmosphere must overwrite fog/color/ambient/diffuse from dayTime");

		// automaticAtmosphere=false: manual values survive while the derived
		// factors and sun direction still update every frame.
		ShaderParameters manual{};
		manual.automaticAtmosphere = false;
		manual.dayTime = 0.5f; // noon
		const glm::vec3 manualFog(0.11f, 0.22f, 0.33f);
		manual.fogColor = manualFog;
		manual.fogStart = 111.0f;
		manual.fogEnd = 222.0f;
		manual.fogDensity = 0.321f;
		manual.ambientStrength = 0.123f;
		manual.diffuseIntensity = 0.456f;
		manual.sunDirection = glm::vec3(9.0f, 9.0f, 9.0f);
		updateAtmosphereFromDayTime(manual);
		if (manual.fogColor != manualFog || manual.fogStart != 111.0f || manual.fogEnd != 222.0f ||
			manual.fogDensity != 0.321f || manual.ambientStrength != 0.123f ||
			manual.diffuseIntensity != 0.456f)
			ok = fail("manual atmosphere values must survive updateAtmosphereFromDayTime");
		if (!(manual.dayFactor > 0.9f) || !(manual.nightFactor < 0.1f))
			ok = fail("dayTime 0.5 (noon) must yield full day (dayFactor > 0.9, nightFactor < 0.1)");
		if (manual.sunDirection == glm::vec3(9.0f, 9.0f, 9.0f))
			ok = fail("sunDirection must be re-derived from dayTime even in manual mode");
	}

	if (!ok)
	{
		std::cerr << "test_graphics_settings: FAILED\n";
		return EXIT_FAILURE;
	}
	std::cout << "test_graphics_settings: OK (presetValues + matchesPreset Custom detection + scoped resets + atmosphere semantics)\n";
	return EXIT_SUCCESS;
}
