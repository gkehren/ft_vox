#pragma once

#include <glm/glm.hpp>
#include <utils.hpp>

#include <cmath>

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
	// Outdoor lighting — re-baselined for the linear-light pipeline (issue #135):
	// albedo now decodes sRGB->linear (mid-tones ~2.3x darker than the old
	// gamma-as-linear sampling), so ambient/diffuse lift proportionally.
	// Playability-first: bright enough that sunlit scenes feel sunny.
	float ambientStrength = 0.38f;
	float diffuseIntensity = 0.92f;
	float lightLevels = 5.0f;
	/// Cool moon fill at night (scales with nightFactor). Lifted for playability:
	/// nights must keep silhouettes and ground detail readable (issue #135).
	float moonAmbientStrength = 0.45f;
	/// Scales mesh block-light contribution in terrain shader.
	float blockLightScale = 1.0f;
	/// Scales emissive block HDR contribution.
	float emissiveScale = 1.0f;

	// Day/Night cycle — dayTime maps to sun angle 2πt − π/2:
	// 0.0 midnight, ~0.25 sunrise, 0.5 noon, ~0.75 sunset
	bool dayCycleEnabled = true;
	float dayTime = 0.5f;
	float dayCycleSpeed = 0.002f;

	// visual — gentle punch for readable sand/grass/water chroma
	float saturationLevel = 1.08f;
	float colorBoost = 1.03f;
	float contrastLevel = 1.06f;

	// Water (Tier 1) — mild defaults (strong refraction caused mirrored/grid artifacts)
	float waterWaveStrength = 0.08f;
	float waterRefraction = 0.012f;
	float waterSpecular = 1.15f;
	float waterFoamStrength = 0.55f;

	/// Shadow debug visualization (issue #137): 0 off, 1 cascade index color,
	/// 2 cascade blend bands, 3 world-units-per-texel density, 4 raw depth.
	float shadowDebug = 0.0f;
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
	int minRenderDistance{192}; // Within this range: full mesh (blocks)
	int maxRenderDistance{512}; // Streaming / unload radius (blocks)
	int raycastDistance{8};
	bool vsyncEnabled{true};

	// View-direction load bias: chunks ahead of the camera count as closer —
	// they load first and up to maxRenderDistance / sqrt(1-bias) farther out.
	// Capped at kSafeMaxStreamFrontBias so the furthest desired chunk center
	// remains inside the kChunkUnloadDistanceFactor unload hysteresis radius
	// (architectural invariant: desired footprint ⊆ unload region; see
	// StreamHelpers.hpp).
	float streamFrontBias{0.30f};

	// Per-second chunk pipeline throughput — frame-rate-independent budgets.
	// Increase to load faster; decrease to reduce per-frame CPU/GPU spikes.
	// Scaled for the larger default view distance (chunk count ~ r^2).
	// Gen/mesh run async on the ThreadPool, so high dispatch budgets mainly
	// cost main-thread dispatch time (capped by maxStreamMs).
	int loadPerSec{640};   // chunk allocations from queue / sec
	int genPerSec{480};	   // terrain-gen job dispatches / sec
	int meshPerSec{360};   // mesh job dispatches / sec
	int uploadPerSec{520}; // GPU mesh uploads / sec (async staging — no device idle)
	// Shadow casters within this XZ radius (blocks). Caps shadow pass cost.
	float shadowDistance{160.f};
	/// Cascade far plane used for CSM split distances (view-space).
	float shadowCascadeFar{280.f};
	/// Max CPU ms per frame for load + gen-dispatch + mesh-dispatch (0 = unlimited).
	float maxStreamMs{6.0f};
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
	/// Re-baselined for the single sRGB output transfer (issue #135): the old
	/// double output gamma no longer brightens mid-tones, so exposure lifts.
	float exposure{1.25f};
	float exposureCompensation{1.0f};
	int toneMapper{0}; // 0 = ACES, 1 = Reinhard
	/// Creative midtone gamma grade (1.0 = neutral display-linear; not framebuffer transfer).
	float gamma{1.0f};
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

	// SSAO — normal-aware horizon-based (GTAO-style) estimator at half res,
	// bilateral-upsampled to a full-res AO buffer. radius is expressed in
	// view-space meters (see ssao.frag.glsl); defaults below equal the
	// Medium preset.
	bool ssaoEnabled{true};
	float ssaoRadius{0.6f};  // view-space occluder search radius, meters (isotropic)
	float ssaoIntensity{0.40f};
	int ssaoDirections{4};   // horizon directions (4..8)
	int ssaoSteps{3};        // march steps per direction (1..4)
	int ssaoDebugView{0};    // 0=Off 1=FinalAO 2=RawAO 3=Normals

	// Underwater look (set by engine when camera is submerged)
	bool underwater{false};
	float underwaterStrength{1.0f};

	/// Shadow map resolution tier (issue #137): 1024 (Low/Medium) or 2048
	/// (High/Cinematic). Changing it recreates the shadow map array — the
	/// engine applies the change deferred, before the next frame.
	int shadowMapSize{1024};

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
	exposure = 1.25f;
	exposureCompensation = 1.0f;
	toneMapper = 0;
	gamma = 1.0f;
	postSaturation = 1.02f;
	postContrast = 1.03f;
	fxaaEnabled = true;
	autoExposureEnabled = true;
	godRaysBoostPreview = false;
	godRaysDepthOcclusion = true;
	ssaoDebugView = 0;  // diagnostics never persist across presets

	switch (preset)
	{
	case GraphicsQualityPreset::Low:
		shadowMapSize = 1024;
		bloomEnabled = true;
		bloomThreshold = 1.65f;
		bloomIntensity = 0.06f;
		bloomBlurIterations = 1;
		ssaoEnabled = false;
		ssaoRadius = 0.4f;
		ssaoIntensity = 0.25f;
		ssaoDirections = 4;
		ssaoSteps = 2;
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
		shadowMapSize = 1024;
		bloomEnabled = true;
		bloomThreshold = 1.45f;
		bloomIntensity = 0.12f;
		bloomBlurIterations = 3;
		ssaoEnabled = true;
		ssaoRadius = 0.6f;
		ssaoIntensity = 0.40f;
		ssaoDirections = 4;
		ssaoSteps = 3;
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
		shadowMapSize = 2048; // higher near-cascade resolution tier
		bloomEnabled = true;
		bloomThreshold = 1.30f;
		bloomIntensity = 0.16f;
		bloomBlurIterations = 4;
		ssaoEnabled = true;
		ssaoRadius = 0.8f;
		ssaoIntensity = 0.55f;
		ssaoDirections = 6;
		ssaoSteps = 4;
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
		shadowMapSize = 2048;
		bloomEnabled = true;
		bloomThreshold = 1.35f; // sun/emissive peaks only — no midtone wash
		bloomIntensity = 0.20f;
		bloomBlurIterations = 5;
		ssaoEnabled = true;
		ssaoRadius = 1.0f;
		ssaoIntensity = 0.62f;
		ssaoDirections = 8;
		ssaoSteps = 4;
		godRaysEnabled = true;
		godRaysDensity = 1.05f;
		godRaysWeight = 0.032f;
		godRaysDecay = 0.955f;
		godRaysExposure = 0.70f;
		godRaysDynamicBoostEnabled = true;
		godRaysDramaticBoost = 3.0f;
		filmGrain = 0.036f;
		vignette = 0.38f;
		postSaturation = 1.10f; // counter ACES highlight desaturation
		postContrast = 1.08f;
		exposure = 1.20f; // keeps the cinematic slightly-dimmer offset vs Medium
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

/// Derive sun/moon direction, day/sunset/night factors and — when
/// automaticAtmosphere is set — fog color/density and ambient/diffuse levels
/// from ShaderParameters::dayTime. Shared by the engine loop and the
/// deterministic visual-regression scenes so both pin identical atmosphere
/// for a given dayTime (same formula as the renderer's FrameUBO fill).
inline void updateAtmosphereFromDayTime(ShaderParameters &sp)
{
	const float dayTime = sp.dayTime;
	const float sunAngle = dayTime * 6.2831853f - 1.5707963f;
	const glm::vec3 sunDir =
		glm::normalize(glm::vec3(std::cos(sunAngle), std::sin(sunAngle) * 0.85f + 0.15f, 0.35f));
	sp.sunDirection = sunDir;
	sp.lightDirection = sunDir.y > 0.05f ? sunDir : -sunDir;

	sp.dayFactor = glm::smoothstep(-0.05f, 0.25f, sunDir.y);
	sp.nightFactor = glm::smoothstep(0.05f, -0.15f, sunDir.y);
	sp.sunsetFactor = glm::clamp(1.0f - std::abs(sunDir.y) * 3.0f, 0.0f, 1.0f) * (1.0f - sp.nightFactor);

	if (sp.automaticAtmosphere)
	{
		// Day fog: deep blue → warm sunset → near-black cinematic night
		const glm::vec3 dayFog(0.38f, 0.60f, 0.90f);
		const glm::vec3 sunsetFog(0.85f, 0.40f, 0.22f);
		const glm::vec3 nightFog(0.005f, 0.008f, 0.020f);
		sp.fogColor = dayFog * sp.dayFactor + sunsetFog * sp.sunsetFactor + nightFog * sp.nightFactor;
		// Linear-light baseline (#135): matches the manual noon defaults in
		// ShaderParameters (0.38 / 0.92 at full day). The terrain shader applies
		// the celestial phase (lightTint / dayLightFactor) itself.
		sp.ambientStrength = 0.13f + 0.25f * sp.dayFactor;
		sp.diffuseIntensity = 0.55f + 0.37f * sp.dayFactor;
		// Slightly denser, closer fog at night for mood (driven by dark nightFog)
		sp.fogDensity = 0.045f + 0.03f * sp.nightFactor;
		sp.fogStart = 300.0f - 80.0f * sp.nightFactor;
		sp.fogEnd = 880.0f - 180.0f * sp.nightFactor;
	}

	// Approximate sun world position for debug display.
	sp.sunPosition = sp.celestialOrbitCenter + sunDir * sp.celestialOrbitRadius;
	sp.moonPosition = sp.celestialOrbitCenter - sunDir * sp.celestialOrbitRadius;
}
