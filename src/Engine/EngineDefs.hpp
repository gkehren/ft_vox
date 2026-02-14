#pragma once

#include <glm/glm.hpp>
#include <utils.hpp>

struct ShaderParameters
{
	// Fog
	float fogStart = 160.0f;
	float fogEnd = 480.0f;
	float fogDensity = 0.8f;
	glm::vec3 fogColor = {0.75f, 0.85f, 1.0f};

	// Lighting
	glm::vec3 sunDirection = glm::normalize(glm::vec3(0.8, 1.0, 0.6));
	float ambientStrength = 0.2f;
	float diffuseIntensity = 0.7f;
	float lightLevels = 5.0f;

	// Day/Night cycle
	bool dayCycleEnabled = true;
	float dayTime = 0.25f; // 0.0 to 1.0 (0.25 is sunrise, 0.5 is noon, 0.75 is sunset, 0.0/1.0 is midnight)
	float dayCycleSpeed = 0.002f;

	// visual
	float saturationLevel = 1.2f;
	float colorBoost = 1.0f;
	float gamma = 1.8f;
};

struct RenderSettings
{
	bool wireframeMode{false};
	bool chunkBorders{false};
	bool paused{false};
	int visibleChunksCount{0};	// Output, updated by rendering logic
	int visibleVoxelsCount{0};	// Output, updated by rendering logic
	int chunkLoadedMax{5};		// Max load radius in chunks from player
	int minRenderDistance{320}; // Min view distance, potentially in blocks or units
	int maxRenderDistance{480}; // Max view distance
	int raycastDistance{8};
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
	float bloomThreshold{1.0f};
	float bloomIntensity{0.3f};
	bool fxaaEnabled{true};
	float exposure{1.0f};
	int toneMapper{0}; // 0 = ACES, 1 = Reinhard
	float gamma{2.2f};

	// God rays (volumetric light scattering)
	bool godRaysEnabled{true};
	float godRaysDensity{1.0f};
	float godRaysWeight{0.01f};
	float godRaysDecay{0.97f};
	float godRaysExposure{0.3f};

};

struct VoxelHighlight
{
	bool active{false};
	glm::vec3 position{0.0f};
	glm::vec3 color{0.8f, 0.2f, 0.2f}; // Default to red (e.g., for destruction)
};
