#pragma once

#include <glm/glm.hpp>
#include <cstdint>

/// Single std140 FrameUBO contract shared by C++ and GLSL (terrain/water/sky/overlay).
/// GLSL layout is AUTO-GENERATED from this struct by cmake/GenerateFrameUboGlsl.cmake
/// → frame_ubo.inc.glsl (included by shaders). Do not hand-edit the generated file.
struct FrameUBO
{
	glm::mat4 view;
	glm::mat4 projection;
	glm::mat4 cascadeMatrix0;
	glm::mat4 cascadeMatrix1;
	glm::mat4 cascadeMatrix2;
	glm::vec4 viewPos;
	glm::vec4 lightDirection;
	glm::vec4 fogColor;
	glm::vec4 fogParams;	 // start, end, density, heightFalloff
	glm::vec4 lightParams;	 // ambient, diffuse, lightLevels, colorBoost
	glm::vec4 visualParams;	 // saturation, contrast, colorBoost, unused
	glm::vec4 sunDir;
	glm::vec4 moonDir;
	glm::vec4 skyParams;	 // time, day, sunset, night
	glm::vec4 cascadeSplits; // xyz ends, w = count
	glm::vec4 moonAmbient;	 // rgb + strength
	glm::vec4 lightingParams;	 // blockLightScale, emissiveScale, fogBaseY, underwater
	glm::vec4 waterParams;	 // wave, refraction, specular, foam
	glm::vec4 cascadeBiasScales; // xyz = normalized-depth units per world texel (worldUnitsPerTexel/depthSpan), w = map resolution
	glm::vec4 cascadeTexelWorldSizes; // xyz = world units per texel XY per cascade (debug density), w unused
	glm::vec4 cascadeGridOffsets01;   // xy = cascade0 absolute grid offset, zw = cascade1 (ints as floats)
	glm::vec4 cascadeGridOffsets2;	  // xy = cascade2 absolute grid offset, zw unused
};

// 17 mat4/vec4 slots after the 5 matrices: 5*64 + 17*16 = 320+272 = 592
static_assert(sizeof(FrameUBO) == 592, "FrameUBO std140 size must match GLSL (no dead postParams)");
