#pragma once

#include <glm/glm.hpp>
#include <utils.hpp>

struct ShaderParameters
{
	// Fog
	bool automaticAtmosphere = true;
	float fogStart = 220.0f;
	float fogEnd = 680.0f;
	float fogDensity = 0.18f;
	glm::vec3 fogColor = {0.42f, 0.57f, 0.70f};

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
	float ambientStrength = 0.24f;
	float diffuseIntensity = 0.78f;
	float lightLevels = 5.0f;

	// Day/Night cycle
	bool dayCycleEnabled = true;
	float dayTime = 0.25f; // 0.0 sunrise, 0.25 noon, 0.5 sunset, 0.75 midnight
	float dayCycleSpeed = 0.002f;

	// visual
	float saturationLevel = 1.08f;
	float colorBoost = 1.0f;
};

struct RenderSettings
{
	bool wireframeMode{false};
	bool chunkBorders{false};
	bool paused{false};
	int visibleChunksCount{0};	// Output, updated by rendering logic
	int visibleVoxelsCount{0};	// Output, updated by rendering logic
	int minRenderDistance{320}; // Min view distance, potentially in blocks or units
	int maxRenderDistance{480}; // Max view distance
	int raycastDistance{8};
	bool vsyncEnabled{true};

	// Per-second chunk pipeline throughput — frame-rate-independent budgets.
	// These control how many operations are dispatched per second regardless
	// of VSync or uncapped FPS. Increase to load faster; decrease to reduce
	// per-frame CPU spikes on slower machines.
	int loadPerSec{5000};	// chunk allocations from queue / sec
	int genPerSec{5000};	// terrain-gen job dispatches / sec
	int meshPerSec{5000};	// mesh job dispatches / sec
	int uploadPerSec{5000}; // GPU glBufferData uploads / sec (main-thread stall)
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
	float bloomThreshold{1.15f};
	float bloomIntensity{0.16f};
	bool fxaaEnabled{true};
	bool autoExposureEnabled{true};
	float exposure{0.9f};
	float exposureCompensation{1.0f};
	int toneMapper{0}; // 0 = ACES, 1 = Reinhard
	float gamma{2.2f};

	// God rays (volumetric light scattering)
	bool godRaysEnabled{true};
	float godRaysDensity{0.85f};
	float godRaysWeight{0.022f};
	float godRaysDecay{0.965f};
	float godRaysExposure{0.55f};
	bool godRaysDynamicBoostEnabled{true};
	bool godRaysBoostPreview{false};
	float godRaysDramaticBoost{2.4f};
};

struct VoxelHighlight
{
	bool active{false};
	glm::vec3 position{0.0f};
	glm::vec3 color{0.8f, 0.2f, 0.2f}; // Default to red (e.g., for destruction)
};
