#pragma once

#include <glm/glm.hpp>
#include <utils.hpp>

#include <cmath>
#include <optional>

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
	// (lightLevels removed, issue #161: it packed into FrameUBO::lightParams.z
	// but no production shader consumed it — the slider was a live no-op. The
	// UBO lane stays reserved until a real lighting feature needs it.)
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

	// Material grading (issue #161) — applied by the terrain AND mob
	// lit-material shaders before fog (terrain.frag/mob.frag share the same
	// lighting + grade stage). Water and the composited frame are graded by
	// the separate full-frame post controls (PostProcessSettings::postSaturation
	// / postContrast) — never present these as world-wide equivalents.
	float materialSaturation = 1.08f;
	float materialColorBoost = 1.03f;
	float materialContrast = 1.06f;

	// Water (Tier 1) — mild defaults (strong refraction caused mirrored/grid artifacts)
	float waterWaveStrength = 0.08f;
	float waterRefraction = 0.012f;
	float waterSpecular = 1.15f;
	float waterFoamStrength = 0.55f;
	/// Wave-normal spread for the specular lobe / SSR blur (0.04 mirror … 0.35 choppy).
	float waterRoughness = 0.12f;
	/// Water surface diagnostics: 0 off, 1 wave normal, 2 optical distance,
	/// 3 Fresnel, 4 SSR confidence (renders raw values through the water pass).
	float waterDebugView = 0.0f;

	/// Shadow debug visualization (issue #137): 0 off, 1 cascade index color,
	/// 2 cascade blend bands, 3 world-units-per-texel density, 4 raw depth.
	float shadowDebug = 0.0f;
};

/// Packs light + material-grade knobs for FrameUBO std140 (matches the
/// terrain/mob lit-material shaders).
/// lightParams  = (ambient, diffuse, reserved, colorBoost) — .z is unused
///   since #161 removed the dead "Light levels" lane; keep 0 until a real
///   lighting feature defines it.
/// visualParams = (saturation, contrast, reserved, shadowDebug) — .z held a
///   duplicate colorBoost copy no shader read (removed, issue #161); .w is
///   overwritten by WorldRenderer with ShaderParameters::shadowDebug.
inline void packFrameLightVisual(const ShaderParameters &p, glm::vec4 &lightParams, glm::vec4 &visualParams)
{
	lightParams = glm::vec4(p.ambientStrength, p.diffuseIntensity, 0.0f, p.materialColorBoost);
	visualParams = glm::vec4(p.materialSaturation, p.materialContrast, 0.0f, 0.0f);
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
	// Entity light-cache-only worker dispatches / sec (issue #172): fills
	// ChunkLightStorage for meshed chunks entering the ~128m mob-lighting
	// radius without invalidating their render meshes. 0 disables the
	// dedicated path (caches then only populate during real mesh builds).
	int lightCachePerSec{96};
	// Shadow casters within this XZ radius (blocks). Caps shadow pass cost.
	float shadowDistance{160.f};
	/// Cascade far plane used for CSM split distances (view-space).
	float shadowCascadeFar{280.f};
	/// Max CPU ms per frame for load + gen-dispatch + mesh-dispatch (0 = unlimited).
	float maxStreamMs{6.0f};
};

/// Streaming tuning presets (issue #186). Values are documented and
/// reproducible: Balanced is exactly the engine-default streaming
/// configuration (RenderSettings member initializers); Conservative halves
/// the view radius/area pressure and tightens the CPU budget; Aggressive
/// pushes the view to the UI slider ceiling (640) and relaxes it.
enum class StreamingQualityPreset
{
	Conservative,
	Balanced,
	Aggressive
};

/// Canonical streaming settings for one preset. Exactly the fields a preset
/// owns; everything else in RenderSettings is untouched.
struct StreamingPresetValues
{
	int maxRenderDistance;   // blocks
	int minRenderDistance;   // blocks (full-quality near range)
	float streamFrontBias;   // 0..kSafeMaxStreamFrontBias
	int loadPerSec;
	int genPerSec;
	int meshPerSec;
	int lightCachePerSec;
	int uploadPerSec;
	float maxStreamMs;       // CPU streaming budget per frame
};

/// Canonical values for a preset. Balanced must stay identical to the
/// RenderSettings default member initializers; update both together.
inline StreamingPresetValues streamingPresetValues(StreamingQualityPreset preset)
{
	switch (preset)
	{
	case StreamingQualityPreset::Conservative:
		return {256, 128, 0.15f, 320, 240, 240, 64, 320, 4.0f};
	case StreamingQualityPreset::Aggressive:
		return {640, 320, 0.45f, 960, 720, 480, 128, 720, 10.0f};
	case StreamingQualityPreset::Balanced:
	default:
		return {512, 192, 0.30f, 640, 480, 360, 96, 520, 6.0f};
	}
}

/// Stamp a preset onto the streaming settings. Writes exactly the nine
/// preset-owned fields; no other RenderSettings field is touched.
inline void applyStreamingPreset(RenderSettings &rs, StreamingQualityPreset preset)
{
	const StreamingPresetValues v = streamingPresetValues(preset);
	rs.maxRenderDistance = v.maxRenderDistance;
	rs.minRenderDistance = v.minRenderDistance;
	rs.streamFrontBias = v.streamFrontBias;
	rs.loadPerSec = v.loadPerSec;
	rs.genPerSec = v.genPerSec;
	rs.meshPerSec = v.meshPerSec;
	rs.lightCachePerSec = v.lightCachePerSec;
	rs.uploadPerSec = v.uploadPerSec;
	rs.maxStreamMs = v.maxStreamMs;
}

/// True when the streaming settings equal the canonical preset values.
/// Exact == on ints/floats is correct for the same reason as the graphics
/// preset Custom detection: canonical values come from the very literals
/// above, so any hand-edited field must diverge.
inline bool matchesStreamingPreset(const RenderSettings &rs, StreamingQualityPreset preset)
{
	const StreamingPresetValues v = streamingPresetValues(preset);
	return rs.maxRenderDistance == v.maxRenderDistance && rs.minRenderDistance == v.minRenderDistance &&
		   rs.streamFrontBias == v.streamFrontBias && rs.loadPerSec == v.loadPerSec &&
		   rs.genPerSec == v.genPerSec && rs.meshPerSec == v.meshPerSec &&
		   rs.lightCachePerSec == v.lightCachePerSec && rs.uploadPerSec == v.uploadPerSec &&
		   rs.maxStreamMs == v.maxStreamMs;
}

/// The preset currently matched exactly, or nullopt for hand-edited
/// ("Custom") settings. UI surfaces must call this AFTER applying any click
/// so badges reflect the same frame's state (issue #191 review).
inline std::optional<StreamingQualityPreset> matchingStreamingPreset(const RenderSettings &rs)
{
	if (matchesStreamingPreset(rs, StreamingQualityPreset::Conservative))
		return StreamingQualityPreset::Conservative;
	if (matchesStreamingPreset(rs, StreamingQualityPreset::Balanced))
		return StreamingQualityPreset::Balanced;
	if (matchesStreamingPreset(rs, StreamingQualityPreset::Aggressive))
		return StreamingQualityPreset::Aggressive;
	return std::nullopt;
}

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

/// Named graphics quality packs. Beyond post knobs, the preset also scales
/// the water path (SSR march budget from High up, water shadows from Medium
/// up) — see WorldRenderer::updateFrameUBO.
enum class GraphicsQualityPreset
{
	Low = 0,
	Medium = 1,
	High = 2,
	Cinematic = 3,
};

/// Sentinel for "no local water surface known" — renders fully submerged
/// (issue #144). The engine replaces it with a per-frame voxel scan.
inline constexpr float kUnknownUnderwaterSurfaceY = 1e9f;

struct PostProcessSettings
{
	bool bloomEnabled{true};
	/// Higher default: bloomExtract soft-knee keeps sun/emissive peaks, not soft midtones.
	float bloomThreshold{1.45f};
	float bloomIntensity{0.12f};
	/// Horizontal+vertical pairs (3 ≈ former 5 quality, ~40% fewer fullscreen blurs).
	int bloomBlurIterations{3};
	/// Spatial anti-aliasing (issue #143): full FXAA 3.11 (quality preset 12)
	/// in a dedicated post pass, running on the tone-mapped sRGB-encoded LDR
	/// image instead of the former in-composite linear-HDR approximation.
	bool fxaaEnabled{true};
	bool autoExposureEnabled{true};
	/// Re-baselined for the single sRGB output transfer (issue #135): the old
	/// double output gamma no longer brightens mid-tones, so exposure lifts.
	/// Exact exposure of the MANUAL path; auto mode derives it from the HDR
	/// scene luminance with the settings below (issue #140).
	float exposure{1.25f};
	/// Exposure compensation in EV stops for the AUTO path: 0 is neutral,
	/// +1 doubles the target exposure (image gets brighter), -1 halves it.
	/// The manual path uses `exposure` exactly as set.
	float exposureCompensation{0.0f};
	/// Auto-exposure tuning (issue #140). middleGrey is the metered scene
	/// target luminance after exposure, before tone mapping (0.18 = middle
	/// gray). A 2x gain ceiling preserves the darkness of nights and caves.
	/// minEv/maxEv clamp the TARGET exposure in EV
	/// relative to 1.0 (exposure = 2^ev). speedUp/speedDown are inverse
	/// seconds, frame-rate independent: speedUp applies while the scene
	/// brightens (exposure drops), speedDown while it darkens (exposure
	/// rises, like eye dilation). Math: Renderer/AutoExposure.hpp.
	float autoExposureMiddleGrey{0.18f};
	float autoExposureMinEv{-4.0f};
	float autoExposureMaxEv{1.0f};
	float autoExposureSpeedUp{3.0f};
	float autoExposureSpeedDown{1.25f};
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
	/// SSAO debug view: 0=Off 1=FinalAO 2=RawAO 3=Normals. Owned by Render
	/// Debug (F12): presets and post resets never write it (issue #185); the
	/// renderer consumes effectiveSsaoDebugView() so a stale view can never
	/// outlive the SSAO pass it visualizes.
	int ssaoDebugView{0};

	// Underwater look (set by engine when camera is submerged)
	bool underwater{false};
	float underwaterStrength{1.0f};
	/// Local water-surface world Y for the submersion blend (issue #144):
	/// the engine scans up from the camera voxel each frame; the sentinel
	/// means "unknown surface" and renders fully submerged (debug toggle,
	/// visual tests before the scan landed).
	float underwaterSurfaceY{kUnknownUnderwaterSurfaceY};

	/// Shadow map resolution tier (issue #137): 1024 (Low/Medium) or 2048
	/// (High/Cinematic). Changing it recreates the shadow map array — the
	/// engine applies the change deferred, before the next frame.
	int shadowMapSize{1024};

	/// Last preset applied via applyPreset (UI combo). Manual tweaks do not
	/// clear it: UI Custom detection is `!matchesPreset(*this, qualityPreset)`
	/// (issue #185) so the label never claims a pack the values no longer are.
	GraphicsQualityPreset qualityPreset{GraphicsQualityPreset::Medium};

	/// Apply a named quality pack. Does not change underwater (runtime state).
	void applyPreset(GraphicsQualityPreset preset);
	/// Canonical settings produced by `preset` applied to fresh defaults
	/// (issue #185): source of truth for Custom-state detection and resets.
	/// A canonical object is complete — values AND the qualityPreset tag.
	[[nodiscard]] static PostProcessSettings presetValues(GraphicsQualityPreset preset);
	/// True when every preset-controlled field of `current` equals the canonical
	/// pack values (issue #185 Custom detection). Deliberately ignores
	/// ssaoDebugView and underwater runtime state: debug views and submersion are
	/// not part of quality-pack semantics.
	[[nodiscard]] static bool matchesPreset(const PostProcessSettings &current, GraphicsQualityPreset preset);
};

/// Effective SSAO debug view for the renderer: with SSAO off there is no AO
/// buffer to visualize, and the composite checks the debug flag before
/// ssaoEnabled — so a stale non-zero view must resolve to Off here rather
/// than by having presets or resets mutate Render Debug state (issue #185).
inline int effectiveSsaoDebugView(const PostProcessSettings &pp)
{
	return pp.ssaoEnabled ? pp.ssaoDebugView : 0;
}

/// Shared implementation: writes every preset-controlled field to the pack's
/// canonical value. Does not touch qualityPreset or underwater runtime state.
inline void applyPresetBody(PostProcessSettings &pp, GraphicsQualityPreset preset)
{
	// Shared grade defaults (Medium baseline)
	pp.exposure = 1.25f;
	pp.exposureCompensation = 0.0f; // EV stops: 0 is neutral
	pp.toneMapper = 0;
	pp.gamma = 1.0f;
	pp.postSaturation = 1.02f;
	pp.postContrast = 1.03f;
	pp.fxaaEnabled = true;
	pp.autoExposureEnabled = true;
	pp.autoExposureMiddleGrey = 0.18f;
	pp.autoExposureMinEv = -4.0f;
	pp.autoExposureMaxEv = 1.0f;
	pp.autoExposureSpeedUp = 3.0f;
	pp.autoExposureSpeedDown = 1.25f;
	pp.godRaysBoostPreview = false;
	pp.godRaysDepthOcclusion = true;
	// ssaoDebugView deliberately NOT written: debug views belong to Render
	// Debug; the renderer gates them via effectiveSsaoDebugView (issue #185).

	switch (preset)
	{
	case GraphicsQualityPreset::Low:
		pp.shadowMapSize = 1024;
		pp.fxaaEnabled = false; // issue #143 policy: Low drops post AA entirely
		pp.bloomEnabled = true;
		pp.bloomThreshold = 1.65f;
		pp.bloomIntensity = 0.06f;
		pp.bloomBlurIterations = 1;
		pp.ssaoEnabled = false;
		pp.ssaoRadius = 0.4f;
		pp.ssaoIntensity = 0.25f;
		pp.ssaoDirections = 4;
		pp.ssaoSteps = 2;
		pp.godRaysEnabled = false;
		pp.godRaysDensity = 0.70f;
		pp.godRaysWeight = 0.015f;
		pp.godRaysDecay = 0.97f;
		pp.godRaysExposure = 0.40f;
		pp.godRaysDynamicBoostEnabled = false;
		pp.godRaysDramaticBoost = 1.5f;
		pp.filmGrain = 0.012f;
		pp.vignette = 0.12f;
		break;
	case GraphicsQualityPreset::Medium:
		// Match constructor defaults (current balanced path)
		pp.shadowMapSize = 1024;
		pp.bloomEnabled = true;
		pp.bloomThreshold = 1.45f;
		pp.bloomIntensity = 0.12f;
		pp.bloomBlurIterations = 3;
		pp.ssaoEnabled = true;
		pp.ssaoRadius = 0.6f;
		pp.ssaoIntensity = 0.40f;
		pp.ssaoDirections = 4;
		pp.ssaoSteps = 3;
		pp.godRaysEnabled = true;
		pp.godRaysDensity = 0.85f;
		pp.godRaysWeight = 0.022f;
		pp.godRaysDecay = 0.965f;
		pp.godRaysExposure = 0.55f;
		pp.godRaysDynamicBoostEnabled = true;
		pp.godRaysDramaticBoost = 2.2f;
		pp.filmGrain = 0.028f;
		pp.vignette = 0.22f;
		break;
	case GraphicsQualityPreset::High:
		pp.shadowMapSize = 2048; // higher near-cascade resolution tier
		pp.bloomEnabled = true;
		pp.bloomThreshold = 1.30f;
		pp.bloomIntensity = 0.16f;
		pp.bloomBlurIterations = 4;
		pp.ssaoEnabled = true;
		pp.ssaoRadius = 0.8f;
		pp.ssaoIntensity = 0.55f;
		pp.ssaoDirections = 6;
		pp.ssaoSteps = 4;
		pp.godRaysEnabled = true;
		pp.godRaysDensity = 0.95f;
		pp.godRaysWeight = 0.028f;
		pp.godRaysDecay = 0.960f;
		pp.godRaysExposure = 0.62f;
		pp.godRaysDynamicBoostEnabled = true;
		pp.godRaysDramaticBoost = 2.6f;
		pp.filmGrain = 0.032f;
		pp.vignette = 0.28f;
		pp.postSaturation = 1.04f;
		pp.postContrast = 1.04f;
		break;
	case GraphicsQualityPreset::Cinematic:
		pp.shadowMapSize = 2048;
		pp.bloomEnabled = true;
		pp.bloomThreshold = 1.35f; // sun/emissive peaks only — no midtone wash
		pp.bloomIntensity = 0.20f;
		pp.bloomBlurIterations = 5;
		pp.ssaoEnabled = true;
		pp.ssaoRadius = 1.0f;
		pp.ssaoIntensity = 0.62f;
		pp.ssaoDirections = 8;
		pp.ssaoSteps = 4;
		pp.godRaysEnabled = true;
		pp.godRaysDensity = 1.05f;
		pp.godRaysWeight = 0.032f;
		pp.godRaysDecay = 0.955f;
		pp.godRaysExposure = 0.70f;
		pp.godRaysDynamicBoostEnabled = true;
		pp.godRaysDramaticBoost = 3.0f;
		pp.filmGrain = 0.036f;
		pp.vignette = 0.38f;
		pp.postSaturation = 1.10f; // counter ACES highlight desaturation
		pp.postContrast = 1.08f;
		pp.exposure = 1.20f; // keeps the cinematic slightly-dimmer offset vs Medium
		break;
	}
}

inline PostProcessSettings PostProcessSettings::presetValues(GraphicsQualityPreset preset)
{
	PostProcessSettings pp{};
	pp.applyPreset(preset);
	return pp;
}

inline bool PostProcessSettings::matchesPreset(const PostProcessSettings &current, GraphicsQualityPreset preset)
{
	// Exact == on floats is correct: canonical values come from the very
	// literals applyPresetBody writes, so any hand-edited field diverges
	// (issue #185 Custom detection must not hide a stale pack behind epsilon).
	const PostProcessSettings canon = presetValues(preset);
	return current.shadowMapSize == canon.shadowMapSize && current.fxaaEnabled == canon.fxaaEnabled &&
		   current.bloomEnabled == canon.bloomEnabled && current.bloomThreshold == canon.bloomThreshold &&
		   current.bloomIntensity == canon.bloomIntensity &&
		   current.bloomBlurIterations == canon.bloomBlurIterations && current.ssaoEnabled == canon.ssaoEnabled &&
		   current.ssaoRadius == canon.ssaoRadius && current.ssaoIntensity == canon.ssaoIntensity &&
		   current.ssaoDirections == canon.ssaoDirections && current.ssaoSteps == canon.ssaoSteps &&
		   current.godRaysEnabled == canon.godRaysEnabled && current.godRaysDensity == canon.godRaysDensity &&
		   current.godRaysWeight == canon.godRaysWeight && current.godRaysDecay == canon.godRaysDecay &&
		   current.godRaysExposure == canon.godRaysExposure &&
		   current.godRaysDynamicBoostEnabled == canon.godRaysDynamicBoostEnabled &&
		   current.godRaysBoostPreview == canon.godRaysBoostPreview &&
		   current.godRaysDramaticBoost == canon.godRaysDramaticBoost &&
		   current.godRaysDepthOcclusion == canon.godRaysDepthOcclusion && current.filmGrain == canon.filmGrain &&
		   current.vignette == canon.vignette && current.postSaturation == canon.postSaturation &&
		   current.postContrast == canon.postContrast && current.exposure == canon.exposure &&
		   current.exposureCompensation == canon.exposureCompensation &&
		   current.toneMapper == canon.toneMapper && current.gamma == canon.gamma &&
		   current.autoExposureEnabled == canon.autoExposureEnabled &&
		   current.autoExposureMiddleGrey == canon.autoExposureMiddleGrey &&
		   current.autoExposureMinEv == canon.autoExposureMinEv &&
		   current.autoExposureMaxEv == canon.autoExposureMaxEv &&
		   current.autoExposureSpeedUp == canon.autoExposureSpeedUp &&
		   current.autoExposureSpeedDown == canon.autoExposureSpeedDown;
}

/// Apply Low / Medium / High / Cinematic packs (post knobs plus the water
/// SSR / shadow scaling described above).
inline void PostProcessSettings::applyPreset(GraphicsQualityPreset preset)
{
	qualityPreset = preset;
	// Preserve runtime submersion state
	const bool wasUnderwater = underwater;
	const float underStr = underwaterStrength;
	const float underSurfaceY = underwaterSurfaceY;
	applyPresetBody(*this, preset);
	underwater = wasUnderwater;
	underwaterStrength = underStr;
	underwaterSurfaceY = underSurfaceY;
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

/// Graphics-panel scoped resets (issue #185): restore the Lighting category
/// (day cycle + lighting scales + material grading) to constructor defaults.
/// Fog/atmosphere fields are untouched (separate category), as are derived
/// day/sunset/night factors and all debug views. Derived from a fresh
/// default-constructed struct so the panel never duplicates default literals.
inline void resetGraphicsLighting(ShaderParameters &sp)
{
	const ShaderParameters def{};
	sp.dayCycleEnabled = def.dayCycleEnabled;
	sp.dayTime = def.dayTime;
	sp.dayCycleSpeed = def.dayCycleSpeed;
	sp.ambientStrength = def.ambientStrength;
	sp.diffuseIntensity = def.diffuseIntensity;
	sp.moonAmbientStrength = def.moonAmbientStrength;
	sp.blockLightScale = def.blockLightScale;
	sp.emissiveScale = def.emissiveScale;
	sp.materialSaturation = def.materialSaturation;
	sp.materialColorBoost = def.materialColorBoost;
	sp.materialContrast = def.materialContrast;
}

/// Restores the Water category: the five water appearance sliders plus the
/// underwater post strength. waterDebugView (diagnostic) is untouched.
/// Defaults come from fresh default-constructed structs (issue #185).
inline void resetGraphicsWater(ShaderParameters &sp, PostProcessSettings &pp)
{
	const ShaderParameters def{};
	sp.waterWaveStrength = def.waterWaveStrength;
	sp.waterRefraction = def.waterRefraction;
	sp.waterSpecular = def.waterSpecular;
	sp.waterFoamStrength = def.waterFoamStrength;
	sp.waterRoughness = def.waterRoughness;
	const PostProcessSettings defPp{};
	pp.underwaterStrength = defPp.underwaterStrength;
}

/// Restores the Post category to the CURRENT pack's canonical values without
/// re-applying the whole preset (issue #185): shadowMapSize belongs to the
/// Shadows category, ssaoDebugView belongs to Render Debug, and the
/// underwater runtime state plus the qualityPreset tag are not post settings.
inline void resetGraphicsPost(PostProcessSettings &pp)
{
	const PostProcessSettings preset = PostProcessSettings::presetValues(pp.qualityPreset);
	pp.fxaaEnabled = preset.fxaaEnabled;
	pp.bloomEnabled = preset.bloomEnabled;
	pp.bloomThreshold = preset.bloomThreshold;
	pp.bloomIntensity = preset.bloomIntensity;
	pp.bloomBlurIterations = preset.bloomBlurIterations;
	pp.ssaoEnabled = preset.ssaoEnabled;
	pp.ssaoRadius = preset.ssaoRadius;
	pp.ssaoIntensity = preset.ssaoIntensity;
	pp.ssaoDirections = preset.ssaoDirections;
	pp.ssaoSteps = preset.ssaoSteps;
	pp.godRaysEnabled = preset.godRaysEnabled;
	pp.godRaysDensity = preset.godRaysDensity;
	pp.godRaysWeight = preset.godRaysWeight;
	pp.godRaysDecay = preset.godRaysDecay;
	pp.godRaysExposure = preset.godRaysExposure;
	pp.godRaysDynamicBoostEnabled = preset.godRaysDynamicBoostEnabled;
	pp.godRaysBoostPreview = preset.godRaysBoostPreview;
	pp.godRaysDramaticBoost = preset.godRaysDramaticBoost;
	pp.godRaysDepthOcclusion = preset.godRaysDepthOcclusion;
	pp.filmGrain = preset.filmGrain;
	pp.vignette = preset.vignette;
	pp.postSaturation = preset.postSaturation;
	pp.postContrast = preset.postContrast;
	pp.exposure = preset.exposure;
	pp.exposureCompensation = preset.exposureCompensation;
	pp.toneMapper = preset.toneMapper;
	pp.gamma = preset.gamma;
	pp.autoExposureEnabled = preset.autoExposureEnabled;
	pp.autoExposureMiddleGrey = preset.autoExposureMiddleGrey;
	pp.autoExposureMinEv = preset.autoExposureMinEv;
	pp.autoExposureMaxEv = preset.autoExposureMaxEv;
	pp.autoExposureSpeedUp = preset.autoExposureSpeedUp;
	pp.autoExposureSpeedDown = preset.autoExposureSpeedDown;
}
