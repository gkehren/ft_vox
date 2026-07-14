#pragma once

#include <glm/glm.hpp>
#include <utils.hpp>

struct ShaderParameters
{
	// Fog — softer / farther so terrain chroma is not washed out
	bool automaticAtmosphere = true;
	float fogStart = 260.0f;
	float fogEnd = 780.0f;
	float fogDensity = 0.12f;
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
	float ambientStrength = 0.26f;
	float diffuseIntensity = 0.85f;
	float lightLevels = 5.0f;

	// Day/Night cycle
	bool dayCycleEnabled = true;
	float dayTime = 0.25f; // 0.0 sunrise, 0.25 noon, 0.5 sunset, 0.75 midnight
	float dayCycleSpeed = 0.002f;

	// visual — saturation & boost are packed into FrameUBO (see packFrameLightVisual)
	// Kept moderate: boost was previously dead (always 1.0); slight lift only.
	float saturationLevel = 1.06f;
	float colorBoost = 1.04f;
	float contrastLevel = 1.02f;
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
};

struct RenderTiming
{
	float frustumCulling{0.0f};
	float chunkGeneration{0.0f}; // Voxel data generation
	float meshGeneration{0.0f};
	float chunkRendering{0.0f};
	float uiRendering{0.0f}; // For ImGui rendering pass
	float totalFrame{0.0f};
};

struct PostProcessSettings
{
	bool bloomEnabled{true};
	float bloomThreshold{1.10f};
	float bloomIntensity{0.16f};
	/// Horizontal+vertical pairs (3 ≈ former 5 quality, ~40% fewer fullscreen blurs).
	int bloomBlurIterations{3};
	bool fxaaEnabled{true};
	bool autoExposureEnabled{true};
	float exposure{0.98f};
	float exposureCompensation{1.0f};
	int toneMapper{0}; // 0 = ACES, 1 = Reinhard
	float gamma{2.2f};
	// Mild post punch only (1.0 = neutral)
	float postSaturation{1.03f};
	float postContrast{1.02f};

	// God rays (volumetric light scattering)
	bool godRaysEnabled{true};
	float godRaysDensity{0.85f};
	float godRaysWeight{0.022f};
	float godRaysDecay{0.965f};
	float godRaysExposure{0.55f};
	bool godRaysDynamicBoostEnabled{true};
	bool godRaysBoostPreview{false};
	float godRaysDramaticBoost{2.2f};
};

struct VoxelHighlight
{
	bool active{false};
	glm::vec3 position{0.0f};
	glm::vec3 color{0.8f, 0.2f, 0.2f}; // Default to red (e.g., for destruction)
};
