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
	if (sp.ambientStrength < 0.18f || sp.ambientStrength > 0.28f)
		ok = fail("ambient out of outdoor-balanced range [0.18, 0.28]");
	if (sp.diffuseIntensity < 0.75f || sp.diffuseIntensity > 0.98f)
		ok = fail("diffuse out of outdoor-balanced range [0.75, 0.98]");

	// Mutate and re-pack to prove knobs are not hard-coded in packer
	sp.colorBoost = 1.55f;
	sp.saturationLevel = 1.4f;
	sp.contrastLevel = 1.2f;
	packFrameLightVisual(sp, lightParams, visualParams);
	if (std::abs(lightParams.w - 1.55f) > 1e-5f || std::abs(visualParams.x - 1.4f) > 1e-5f)
		ok = fail("packFrameLightVisual does not pass through mutated knobs");

	PostProcessSettings pp{};
	if (pp.exposure < 0.88f || pp.exposure > 1.05f)
		ok = fail("default post exposure out of balanced range [0.88, 1.05]");
	if (pp.postSaturation < 0.98f || pp.postSaturation > 1.08f)
		ok = fail("default postSaturation out of balanced range [0.98, 1.08]");
	if (pp.postContrast < 1.0f || pp.postContrast > 1.08f)
		ok = fail("default postContrast out of balanced range [1.0, 1.08]");

	// --- Quality presets (shipped applyPreset; Low lighter than High/Cinematic) ---
	{
		PostProcessSettings low{}, med{}, high{}, cine{};
		// Dirty knobs then re-apply to prove applicator overwrites
		low.ssaoIntensity = 9.f;
		low.bloomBlurIterations = 99;
		low.applyPreset(GraphicsQualityPreset::Low);
		med.applyPreset(GraphicsQualityPreset::Medium);
		high.applyPreset(GraphicsQualityPreset::High);
		cine.applyPreset(GraphicsQualityPreset::Cinematic);

		if (low.qualityPreset != GraphicsQualityPreset::Low)
			ok = fail("applyPreset(Low) must set qualityPreset");
		if (med.qualityPreset != GraphicsQualityPreset::Medium)
			ok = fail("applyPreset(Medium) must set qualityPreset");
		if (low.ssaoEnabled)
			ok = fail("Low preset should disable SSAO");
		if (!med.ssaoEnabled || !high.ssaoEnabled || !cine.ssaoEnabled)
			ok = fail("Medium/High/Cinematic should enable SSAO");
		if (!(low.ssaoIntensity < med.ssaoIntensity && med.ssaoIntensity < high.ssaoIntensity &&
			  high.ssaoIntensity < cine.ssaoIntensity))
			ok = fail("SSAO intensity must increase Low < Medium < High < Cinematic");
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
		if (!(cine.filmGrain > med.filmGrain && cine.vignette > med.vignette))
			ok = fail("Cinematic should push film grain and vignette above Medium");
		// Underwater runtime state preserved
		PostProcessSettings uw{};
		uw.underwater = true;
		uw.underwaterStrength = 1.25f;
		uw.applyPreset(GraphicsQualityPreset::Low);
		if (!uw.underwater || std::abs(uw.underwaterStrength - 1.25f) > 1e-5f)
			ok = fail("applyPreset must preserve underwater state");
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
