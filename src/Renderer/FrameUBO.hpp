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
};

// 16 mat4/vec4 slots after the 5 matrices: 5*64 + 13*16 = 320+208 = 528
// matrices: view,proj,c0,c1,c2 = 5 * 64 = 320
// remaining: 13 * 16 = 208 → total 528
static_assert(sizeof(FrameUBO) == 528, "FrameUBO std140 size must match GLSL (no dead postParams)");
