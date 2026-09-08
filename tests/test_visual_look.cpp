// Proves shipped visual packing + default look knobs (no grey-wash defaults).
// Drives real packFrameLightVisual + TerrainGenerator biome palette.

#include <Engine/EngineDefs.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <utils.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
float channelChroma(const glm::vec3 &c)
{
	const float mx = std::max(c.x, std::max(c.y, c.z));
	const float mn = std::min(c.x, std::min(c.y, c.z));
	return mx - mn;
}

bool fail(const std::string &msg)
{
	std::cerr << "FAIL: " << msg << "\n";
	return false;
}
} // namespace

int main()
{
	bool ok = true;

	// --- Real packing path used by WorldRenderer::updateFrameUBO ---
	ShaderParameters sp{}; // shipped defaults
	glm::vec4 lightParams{};
	glm::vec4 visualParams{};
	packFrameLightVisual(sp, lightParams, visualParams);

	if (std::abs(lightParams.w - sp.colorBoost) > 1e-5f)
		ok = fail("colorBoost must pack into lightParams.w (terrain.frag samples it)");
	if (std::abs(visualParams.x - sp.saturationLevel) > 1e-5f)
		ok = fail("saturationLevel must pack into visualParams.x");
	if (std::abs(visualParams.y - sp.contrastLevel) > 1e-5f)
		ok = fail("contrastLevel must pack into visualParams.y");
	if (std::abs(visualParams.z - sp.colorBoost) > 1e-5f)
		ok = fail("colorBoost should also be mirrored in visualParams.z");

	// Mid-path defaults: readable chroma, not washed-out or neon
	if (sp.colorBoost < 1.0f || sp.colorBoost > 1.08f)
		ok = fail("default colorBoost out of balanced range [1.0, 1.08]");
	if (sp.saturationLevel < 1.0f || sp.saturationLevel > 1.10f)
		ok = fail("default saturationLevel out of balanced range [1.0, 1.10]");
	if (sp.contrastLevel < 1.0f || sp.contrastLevel > 1.08f)
		ok = fail("default contrastLevel out of balanced range [1.0, 1.08]");
	// Re-baselined for the linear-light pipeline (issue #135): sRGB-decoded
	// albedo is ~2.3x darker mid-tones than the old gamma-as-linear sampling.
	// Playability-first: bright, readable nights and sunny days.
	if (sp.ambientStrength < 0.30f || sp.ambientStrength > 0.46f)
		ok = fail("ambient out of re-baselined linear-light range [0.30, 0.46]");
	if (sp.diffuseIntensity < 0.80f || sp.diffuseIntensity > 1.00f)
		ok = fail("diffuse out of re-baselined linear-light range [0.80, 1.00]");

	// Mutate and re-pack to prove knobs are not hard-coded in packer
	sp.colorBoost = 1.55f;
	sp.saturationLevel = 1.4f;
	sp.contrastLevel = 1.2f;
	packFrameLightVisual(sp, lightParams, visualParams);
	if (std::abs(lightParams.w - 1.55f) > 1e-5f || std::abs(visualParams.x - 1.4f) > 1e-5f)
		ok = fail("packFrameLightVisual does not pass through mutated knobs");

	PostProcessSettings pp{};
	if (pp.exposure < 1.10f || pp.exposure > 1.40f)
		ok = fail("default post exposure out of re-baselined single-transfer range [1.10, 1.40]");
	if (pp.postSaturation < 0.98f || pp.postSaturation > 1.08f)
		ok = fail("default postSaturation out of balanced range [0.98, 1.08]");
	if (pp.postContrast < 1.0f || pp.postContrast > 1.08f)
		ok = fail("default postContrast out of balanced range [1.0, 1.08]");
	if (std::abs(pp.gamma - 1.0f) > 1e-4f)
		ok = fail("default gamma must be 1.0f (neutral display-linear midtone baseline)");
	// Auto exposure (issue #140): compensation is EV stops (0 = neutral, +1
	// doubles the exposure), auto mode defaults on, and the five tunables
	// ship with the same defaults as the CPU reference in AutoExposure.hpp.
	if (std::abs(pp.exposureCompensation) > 1e-6f)
		ok = fail("default exposureCompensation must be 0 EV (neutral)");
	if (!pp.autoExposureEnabled)
		ok = fail("auto exposure should default on");
	if (pp.autoExposureMiddleGrey != 1.0f || pp.autoExposureMinEv != -4.0f ||
		pp.autoExposureMaxEv != 4.0f || pp.autoExposureSpeedUp != 3.0f ||
		pp.autoExposureSpeedDown != 1.25f)
		ok = fail("auto-exposure defaults drifted (middleGrey 1, minEv -4, maxEv +4, speedUp 3, speedDown 1.25)");

	// --- Quality presets (shipped applyPreset; Low lighter than High/Cinematic) ---
	{
		PostProcessSettings low{}, med{}, high{}, cine{};
		// Dirty knobs then re-apply to prove applicator overwrites
		const auto dirtyAutoExposure = [](PostProcessSettings &p) {
			p.autoExposureEnabled = false;
			p.exposureCompensation = 1.5f;
			p.autoExposureMiddleGrey = 0.2f;
			p.autoExposureMinEv = -1.0f;
			p.autoExposureMaxEv = 2.0f;
			p.autoExposureSpeedUp = 9.0f;
			p.autoExposureSpeedDown = 4.0f;
		};
		dirtyAutoExposure(low);
		dirtyAutoExposure(med);
		dirtyAutoExposure(high);
		dirtyAutoExposure(cine);
		low.ssaoIntensity = 9.f;
		low.bloomBlurIterations = 99;
		low.ssaoDebugView = 3;
		low.applyPreset(GraphicsQualityPreset::Low);
		med.applyPreset(GraphicsQualityPreset::Medium);
		high.applyPreset(GraphicsQualityPreset::High);
		cine.applyPreset(GraphicsQualityPreset::Cinematic);

		if (low.qualityPreset != GraphicsQualityPreset::Low)
			ok = fail("applyPreset(Low) must set qualityPreset");
		if (med.qualityPreset != GraphicsQualityPreset::Medium)
			ok = fail("applyPreset(Medium) must set qualityPreset");
		if (std::abs(low.gamma - 1.0f) > 1e-4f || std::abs(med.gamma - 1.0f) > 1e-4f ||
			std::abs(high.gamma - 1.0f) > 1e-4f || std::abs(cine.gamma - 1.0f) > 1e-4f)
			ok = fail("quality presets must maintain neutral gamma = 1.0f");
		if (low.ssaoEnabled)
			ok = fail("Low preset should disable SSAO");
		if (!med.ssaoEnabled || !high.ssaoEnabled || !cine.ssaoEnabled)
			ok = fail("Medium/High/Cinematic should enable SSAO");
		if (!(low.ssaoIntensity < med.ssaoIntensity && med.ssaoIntensity < high.ssaoIntensity &&
			  high.ssaoIntensity < cine.ssaoIntensity))
			ok = fail("SSAO intensity must increase Low < Medium < High < Cinematic");
		// Horizon-AO cost knobs: quality scales the estimator budget (never down)
		if (!(low.ssaoDirections <= med.ssaoDirections && med.ssaoDirections <= high.ssaoDirections &&
			  high.ssaoDirections <= cine.ssaoDirections))
			ok = fail("SSAO directions must be non-decreasing Low < Medium < High < Cinematic");
		if (!(low.ssaoSteps <= med.ssaoSteps && med.ssaoSteps <= high.ssaoSteps &&
			  high.ssaoSteps <= cine.ssaoSteps))
			ok = fail("SSAO steps must be non-decreasing Low < Medium < High < Cinematic");
		if (low.ssaoDebugView != 0)
			ok = fail("applyPreset must reset the SSAO debug view to Off");
		// Spatial AA tier (issue #143): only Low drops the FXAA 3.11 pass;
		// Medium/High/Cinematic keep the production spatial path.
		if (low.fxaaEnabled)
			ok = fail("Low preset should disable spatial AA (issue #143)");
		if (!med.fxaaEnabled || !high.fxaaEnabled || !cine.fxaaEnabled)
			ok = fail("Medium/High/Cinematic should enable spatial AA (issue #143)");
		// House rule: constructor defaults == Medium preset values
		{
			PostProcessSettings def{}, medDef{};
			medDef.applyPreset(GraphicsQualityPreset::Medium);
			if (def.ssaoEnabled != medDef.ssaoEnabled ||
				std::abs(def.ssaoRadius - medDef.ssaoRadius) > 1e-5f ||
				std::abs(def.ssaoIntensity - medDef.ssaoIntensity) > 1e-5f ||
				def.ssaoDirections != medDef.ssaoDirections ||
				def.ssaoSteps != medDef.ssaoSteps ||
				def.ssaoDebugView != medDef.ssaoDebugView)
				ok = fail("SSAO defaults must equal the Medium preset (house rule: Medium = constructor defaults)");
		}
		if (!(low.bloomBlurIterations < med.bloomBlurIterations &&
			  med.bloomBlurIterations < high.bloomBlurIterations &&
			  high.bloomBlurIterations <= cine.bloomBlurIterations))
			ok = fail("bloom blur budget must not decrease with higher presets");
		if (!(low.bloomIntensity < med.bloomIntensity && med.bloomIntensity < high.bloomIntensity))
			ok = fail("bloom intensity should rise Low < Medium < High");
		if (low.godRaysEnabled)
			ok = fail("Low preset should disable god rays");
		if (!high.godRaysEnabled || !cine.godRaysEnabled)
			ok = fail("High/Cinematic should enable god rays");
		// Shadow quality tier (issue #137): Low/Medium target 1024, High/Cinematic 2048.
		if (low.shadowMapSize != 1024 || med.shadowMapSize != 1024)
			ok = fail("Low/Medium presets must target a 1024 shadow map");
		if (high.shadowMapSize != 2048 || cine.shadowMapSize != 2048)
			ok = fail("High/Cinematic presets must target a 2048 shadow map");
		if (!(cine.filmGrain > med.filmGrain && cine.vignette > med.vignette))
			ok = fail("Cinematic should push film grain and vignette above Medium");
		// Underwater runtime state preserved
		PostProcessSettings uw{};
		uw.underwater = true;
		uw.underwaterStrength = 1.25f;
		uw.applyPreset(GraphicsQualityPreset::Low);
		if (!uw.underwater || std::abs(uw.underwaterStrength - 1.25f) > 1e-5f)
			ok = fail("applyPreset must preserve underwater state");
		// Auto exposure resets (issue #140): every preset re-engages auto
		// mode and resets compensation + the five tunables to the defaults.
		const auto checkAutoExposureReset = [&](const PostProcessSettings &p, const char *name) {
			if (!p.autoExposureEnabled)
				ok = fail(std::string(name) + " preset must re-enable auto exposure");
			if (std::abs(p.exposureCompensation) > 1e-6f)
				ok = fail(std::string(name) + " preset must reset exposureCompensation to 0 EV");
			if (p.autoExposureMiddleGrey != 1.0f || p.autoExposureMinEv != -4.0f ||
				p.autoExposureMaxEv != 4.0f || p.autoExposureSpeedUp != 3.0f ||
				p.autoExposureSpeedDown != 1.25f)
				ok = fail(std::string(name) + " preset must reset the auto-exposure tunables to defaults");
		};
		checkAutoExposureReset(low, "Low");
		checkAutoExposureReset(med, "Medium");
		checkAutoExposureReset(high, "High");
		checkAutoExposureReset(cine, "Cinematic");
	}

	// --- Biome palette: readable chroma, not neon ---
	const BiomeType vividBiomes[] = {
		BIOME_PLAINS, BIOME_FOREST, BIOME_JUNGLE, BIOME_SAVANNA, BIOME_DESERT, BIOME_BADLANDS};
	for (BiomeType b : vividBiomes)
	{
		const BiomeConfig &cfg = TerrainGenerator::getBiomeConfig(b);
		const float ch = channelChroma(cfg.grassColor);
		if (ch < 0.18f)
		{
			ok = fail(std::string("biome grass too grey (chroma=") + std::to_string(ch) +
					  ") for " + biomeTypeString[b]);
		}
		if (ch > 0.65f)
		{
			ok = fail(std::string("biome grass oversaturated (chroma=") + std::to_string(ch) +
					  ") for " + biomeTypeString[b]);
		}
	}

	// Plains vs desert must be distinctly different hues
	const auto &plains = TerrainGenerator::getBiomeConfig(BIOME_PLAINS).grassColor;
	const auto &desert = TerrainGenerator::getBiomeConfig(BIOME_DESERT).grassColor;
	const float dist = std::abs(plains.x - desert.x) + std::abs(plains.y - desert.y) +
					   std::abs(plains.z - desert.z);
	if (dist < 0.30f)
		ok = fail("plains vs desert grass colors are not distinct enough");

	// Jungle should be greener than desert
	const auto &jungle = TerrainGenerator::getBiomeConfig(BIOME_JUNGLE).grassColor;
	if (!(jungle.y > desert.y && jungle.y > jungle.x))
		ok = fail("jungle grass should be green-dominant vs desert gold");

	if (!ok)
	{
		std::cerr << "test_visual_look: FAILED\n";
		return EXIT_FAILURE;
	}
	std::cout << "test_visual_look: OK (packing + defaults + quality presets + biome chroma)\n";
	return EXIT_SUCCESS;
}
