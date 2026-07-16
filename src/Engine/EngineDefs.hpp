#pragma once

#include <glm/glm.hpp>
#include <utils.hpp>

struct ShaderParameters
{
	// Fog — farther / thinner so midground keeps chroma (milky wash fix)
	bool automaticAtmosphere = true;
	float fogStart = 320.0f;
	float fogEnd = 900.0f;
	float fogDensity = 0.06f;
	/// Height fog falloff (higher = fog thins faster above fogBaseY).
	float fogHeightFalloff = 0.018f;
	float fogBaseY = 64.0f;
	glm::vec3 fogColor = {0.55f, 0.72f, 0.92f};

	// Lighting
	glm::vec3 celestialOrbitCenter = {0.0f, 96.0f, 0.0f};
	float celestialOrbitRadius = 4096.0f;
	glm::vec3 sunPosition = {0.0f, 4192.0f, 0.0f};
	glm::vec3 moonPosition = {0.0f, -4000.0f, 0.0f};
	glm::vec3 sunDirection = {0.0f, 1.0f, 0.0f};
	glm::vec3 lightDirection = sunDirection;
	float dayFactor = 1.0f;
	float sunsetFactor = 0.0f;
	float nightFactor = 0.0f;
	// Outdoor lighting — mid path (not milky, not neon, not washed-out beige)
	float ambientStrength = 0.22f;
	float diffuseIntensity = 0.88f;
	float lightLevels = 5.0f;
	/// Cool moon fill at night (scales with nightFactor).
	float moonAmbientStrength = 0.55f;
	/// Scales mesh block-light contribution in terrain shader.
	float blockLightScale = 1.0f;
	/// Scales emissive block HDR contribution.
	float emissiveScale = 1.0f;

	// Day/Night cycle
	bool dayCycleEnabled = true;
	float dayTime = 0.25f; // 0.0 sunrise, 0.25 noon, 0.5 sunset, 0.75 midnight
	float dayCycleSpeed = 0.002f;

	// visual — gentle punch for readable sand/grass/water chroma
	float saturationLevel = 1.04f;
	float colorBoost = 1.03f;
	float contrastLevel = 1.04f;

	// Water (Tier 1) — mild defaults (strong refraction caused mirrored/grid artifacts)
	float waterWaveStrength = 0.08f;
	float waterRefraction = 0.012f;
	float waterSpecular = 1.15f;
	float waterFoamStrength = 0.55f;
};

/// Packs light + visual knobs for FrameUBO std140 (matches terrain/sky shaders).
/// lightParams  = (ambient, diffuse, lightLevels, colorBoost)
/// visualParams = (saturation, contrast, colorBoost, unused)
inline void packFrameLightVisual(const ShaderParameters &p, glm::vec4 &lightParams, glm::vec4 &visualParams)
{
	lightParams = glm::vec4(p.ambientStrength, p.diffuseIntensity, p.lightLevels, p.colorBoost);
	visualParams = glm::vec4(p.saturationLevel, p.contrastLevel, p.colorBoost, 0.0f);
}

struct RenderSettings
{
	bool wireframeMode{false};
	bool chunkBorders{false};
	bool paused{false};
	int visibleChunksCount{0};	// Output, updated by rendering logic
	int visibleVoxelsCount{0};	// Output, updated by rendering logic
	int minRenderDistance{128}; // Within this range: full mesh (blocks)
	int maxRenderDistance{256}; // Streaming / unload radius (blocks)
	int raycastDistance{8};
	bool vsyncEnabled{true};

	// Per-second chunk pipeline throughput — frame-rate-independent budgets.
	// Increase to load faster; decrease to reduce per-frame CPU/GPU spikes.
	int loadPerSec{120};   // chunk allocations from queue / sec
	int genPerSec{80};	   // terrain-gen job dispatches / sec
	int meshPerSec{60};	   // mesh job dispatches / sec
	int uploadPerSec{100}; // GPU mesh uploads / sec (async staging — no device idle)
	// Shadow casters within this XZ radius (blocks). Caps shadow pass cost.
	float shadowDistance{160.f};
	/// Cascade far plane used for CSM split distances (view-space).
	float shadowCascadeFar{280.f};
	/// Max CPU ms per frame for load + gen-dispatch + mesh-dispatch (0 = unlimited).
	float maxStreamMs{4.0f};
};

/// Legacy flat timings filled from the hierarchical Profiler each frame.
/// Prefer GetProfiler() / F7 panel for new UI. Field meanings (main-thread ms):
/// frustumCulling = Visibility, chunkGeneration = GenDispatch,
/// meshGeneration = MeshDispatch, chunkRendering = MeshUpload,
/// uiRendering = ImGui, totalFrame = full frame.
struct RenderTiming
{
	float frustumCulling{0.0f};
	float chunkGeneration{0.0f};
	float meshGeneration{0.0f};
	float chunkRendering{0.0f};
	float uiRendering{0.0f};
	float totalFrame{0.0f};
};

/// Named graphics quality packs (existing post knobs only — no new effects).
enum class GraphicsQualityPreset
{
	Low = 0,
	Medium = 1,
	High = 2,
	Cinematic = 3,
};

struct PostProcessSettings
{
	bool bloomEnabled{true};
	/// Higher default: bloomExtract soft-knee keeps sun/emissive peaks, not soft midtones.
	float bloomThreshold{1.45f};
	float bloomIntensity{0.12f};
	/// Horizontal+vertical pairs (3 ≈ former 5 quality, ~40% fewer fullscreen blurs).
	int bloomBlurIterations{3};
	bool fxaaEnabled{true};
	bool autoExposureEnabled{true};
	float exposure{0.94f};
	float exposureCompensation{1.0f};
	int toneMapper{0}; // 0 = ACES, 1 = Reinhard
	float gamma{2.2f};
	// Gentle post grade — natural chroma without neon
	float postSaturation{1.02f};
	float postContrast{1.03f};
	// Quick-win style: film grain + vignette (composite.frag)
	float filmGrain{0.028f};
	float vignette{0.22f};

	// God rays (volumetric light scattering) — depth-aware when depthOcclusion enabled
	bool godRaysEnabled{true};
	float godRaysDensity{0.85f};
	float godRaysWeight{0.022f};
	float godRaysDecay{0.965f};
	float godRaysExposure{0.55f};
	bool godRaysDynamicBoostEnabled{true};
	bool godRaysBoostPreview{false};
	float godRaysDramaticBoost{2.2f};
	/// Occlude shafts by scene depth (geometry blocks light shafts).
	bool godRaysDepthOcclusion{true};

	// SSAO (half-res) — mild defaults; full intensity caused milky outdoor veil
	bool ssaoEnabled{true};
	float ssaoRadius{0.40f};
	float ssaoBias{0.035f};
	float ssaoIntensity{0.40f};

	// Underwater look (set by engine when camera is submerged)
	bool underwater{false};
	float underwaterStrength{1.0f};

	/// Last preset applied via applyPreset (UI combo). Manual tweaks do not clear this.
	GraphicsQualityPreset qualityPreset{GraphicsQualityPreset::Medium};

	/// Apply a named quality pack. Does not change underwater (runtime state).
	void applyPreset(GraphicsQualityPreset preset);
};

/// Apply Low / Medium / High / Cinematic packs onto existing post knobs only.
inline void PostProcessSettings::applyPreset(GraphicsQualityPreset preset)
{
	qualityPreset = preset;
	// Preserve runtime submersion state
	const bool wasUnderwater = underwater;
	const float underStr = underwaterStrength;

	// Shared grade defaults (Medium baseline)
	exposure = 0.94f;
	exposureCompensation = 1.0f;
	toneMapper = 0;
	gamma = 2.2f;
	postSaturation = 1.02f;
	postContrast = 1.03f;
	fxaaEnabled = true;
	autoExposureEnabled = true;
	godRaysBoostPreview = false;
	godRaysDepthOcclusion = true;
	ssaoBias = 0.035f;

	switch (preset)
	{
	case GraphicsQualityPreset::Low:
		bloomEnabled = true;
		bloomThreshold = 1.65f;
		bloomIntensity = 0.06f;
		bloomBlurIterations = 1;
		ssaoEnabled = false;
		ssaoRadius = 0.30f;
		ssaoIntensity = 0.25f;
		godRaysEnabled = false;
		godRaysDensity = 0.70f;
		godRaysWeight = 0.015f;
		godRaysDecay = 0.97f;
		godRaysExposure = 0.40f;
		godRaysDynamicBoostEnabled = false;
		godRaysDramaticBoost = 1.5f;
		filmGrain = 0.012f;
		vignette = 0.12f;
		break;
	case GraphicsQualityPreset::Medium:
		// Match constructor defaults (current balanced path)
		bloomEnabled = true;
		bloomThreshold = 1.45f;
		bloomIntensity = 0.12f;
		bloomBlurIterations = 3;
		ssaoEnabled = true;
		ssaoRadius = 0.40f;
		ssaoIntensity = 0.40f;
		godRaysEnabled = true;
		godRaysDensity = 0.85f;
		godRaysWeight = 0.022f;
		godRaysDecay = 0.965f;
		godRaysExposure = 0.55f;
		godRaysDynamicBoostEnabled = true;
		godRaysDramaticBoost = 2.2f;
		filmGrain = 0.028f;
		vignette = 0.22f;
		break;
	case GraphicsQualityPreset::High:
		bloomEnabled = true;
		bloomThreshold = 1.30f;
		bloomIntensity = 0.16f;
		bloomBlurIterations = 4;
		ssaoEnabled = true;
		ssaoRadius = 0.48f;
		ssaoIntensity = 0.55f;
		godRaysEnabled = true;
		godRaysDensity = 0.95f;
		godRaysWeight = 0.028f;
		godRaysDecay = 0.960f;
		godRaysExposure = 0.62f;
		godRaysDynamicBoostEnabled = true;
		godRaysDramaticBoost = 2.6f;
		filmGrain = 0.032f;
		vignette = 0.28f;
		postSaturation = 1.04f;
		postContrast = 1.04f;
		break;
	case GraphicsQualityPreset::Cinematic:
		bloomEnabled = true;
		bloomThreshold = 1.20f;
		bloomIntensity = 0.20f;
		bloomBlurIterations = 5;
		ssaoEnabled = true;
		ssaoRadius = 0.55f;
		ssaoIntensity = 0.70f;
		godRaysEnabled = true;
		godRaysDensity = 1.05f;
		godRaysWeight = 0.032f;
		godRaysDecay = 0.955f;
		godRaysExposure = 0.70f;
		godRaysDynamicBoostEnabled = true;
		godRaysDramaticBoost = 3.0f;
		filmGrain = 0.045f;
		vignette = 0.38f;
		postSaturation = 1.06f;
		postContrast = 1.06f;
		exposure = 0.92f;
		break;
	}

	underwater = wasUnderwater;
	underwaterStrength = underStr;
}

struct VoxelHighlight
{
	bool active{false};
	glm::vec3 position{0.0f};
	glm::vec3 color{0.8f, 0.2f, 0.2f}; // Default to red (e.g., for destruction)
};
