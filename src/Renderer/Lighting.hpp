#pragma once

/// Lighting, fog, and post helper math shared by the renderer and unit tests.
/// Block light packing, cave fill, moon ambient, SSAO/god-ray clamps.

#include "utils.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace lighting
{
/// Exponential height fog factor in [0, 1] (optical-depth form).
inline float exponentialHeightFogFactor(float distance, float worldY, float cameraY,
										float density, float heightFalloff, float fogBaseY = 64.0f)
{
	const float d = std::max(distance, 0.0f);
	const float dens = std::max(density, 0.0f);
	const float hf = std::max(heightFalloff, 0.0f);
	const float avgY = 0.5f * (cameraY + worldY);
	const float heightTerm = std::exp(-hf * std::max(0.0f, avgY - fogBaseY));
	const float opticalDepth = dens * d * heightTerm;
	return 1.0f - std::exp(-opticalDepth);
}

// --- Outdoor look (fog / SSAO) — must match terrain.frag + composite defaults ---

/// Density-scale used in terrain.frag for the exp height fog term (was 0.0018 = too milky).
inline constexpr float kTerrainFogDensityScale = 0.0009f;
/// Max fog mix toward fog color (was 0.82 → 0.55; lowered again for midground chroma).
inline constexpr float kTerrainFogAmountCap = 0.45f;
/// Mild SSAO intensity ceiling used by composite clamp (game default is lower still).
inline constexpr float kSsaoIntensityMax = 0.85f;
inline constexpr float kSsaoIntensityDefault = 0.40f;

/// Combined fog amount matching terrain.frag (linear + density, capped).
/// Used by unit tests and documents the shipped outdoor fog curve.
inline float terrainFogAmount(float distance, float worldY, float cameraY,
							  float fogStart, float fogEnd, float fogDensity,
							  float heightFalloff, float fogBaseY,
							  float densityScale = kTerrainFogDensityScale,
							  float amountCap = kTerrainFogAmountCap)
{
	const float dist = std::max(distance, 0.0f);
	const float start = fogStart;
	const float end = std::max(fogStart + 1.0f, fogEnd);
	const float linearFog = std::clamp((dist - start) / (end - start), 0.0f, 1.0f);
	// smoothstep-ish via clamp of Hermite — approximate for tests; shader uses smoothstep
	const float t = linearFog;
	const float smoothLinear = t * t * (3.0f - 2.0f * t);
	const float avgY = 0.5f * (cameraY + worldY);
	const float heightTerm = std::exp(-std::max(heightFalloff, 0.0f) * std::max(0.0f, avgY - fogBaseY));
	const float densDist = std::max(0.0f, dist - fogStart * 0.25f);
	const float densityFog = 1.0f - std::exp(-densDist * std::max(fogDensity, 0.0f) * densityScale * heightTerm);
	return std::clamp(std::max(smoothLinear, densityFog), 0.0f, amountCap);
}

/// Clamp SSAO intensity for composite (prevents full-frame milky veil when UI maxed).
inline float clampSsaoIntensity(float intensity)
{
	return std::clamp(intensity, 0.0f, kSsaoIntensityMax);
}

inline glm::vec3 moonAmbientColor(float nightFactor, float moonAmbientStrength)
{
	const float n = std::clamp(nightFactor, 0.0f, 1.0f);
	const float s = std::max(moonAmbientStrength, 0.0f);
	const glm::vec3 cool(0.22f, 0.30f, 0.48f);
	return cool * (n * s);
}

inline float emissiveIntensityForBlock(uint8_t blockType)
{
	const auto t = static_cast<TextureType>(blockType);
	switch (t)
	{
	case REDSTONE_ORE:
		return 0.90f;
	case LAPIS_ORE:
		return 0.45f;
	case DIAMOND_ORE:
		return 0.25f;
	case EMERALD_ORE:
		return 0.20f;
	case GOLD_ORE:
		return 0.12f;
	case MAGMA:
		return 0.85f;
	case LAVA:
		return 1.0f;
	default:
		return 0.0f;
	}
}

// --- Colored block light (issue #141) ---------------------------------------
// A block light source is semantic data: a linear-space RGB color plus the
// 0..15 propagation intensity. Self-emission (emissiveIntensityForBlock — the
// HDR glow of the surface itself) and block-light propagation (light cast
// into neighboring voxels) stay conceptually separate: a material may have
// either or both. Colors are interpreted in linear light (issue #135).

struct BlockLightSource
{
	glm::vec3 colorLinear{0.0f};
	uint8_t intensity{0}; // 0..15, 0 = not a source
};

inline BlockLightSource blockLightSourceForBlock(uint8_t blockType)
{
	const auto t = static_cast<TextureType>(blockType);
	switch (t)
	{
	case LAVA: // warm orange/red
		return {glm::vec3(1.00f, 0.30f, 0.08f), 15};
	case MAGMA: // orange
		return {glm::vec3(1.00f, 0.45f, 0.12f), 13};
	case REDSTONE_ORE: // red
		return {glm::vec3(1.00f, 0.12f, 0.10f), 14};
	case LAPIS_ORE: // blue
		return {glm::vec3(0.20f, 0.40f, 1.00f), 7};
	case DIAMOND_ORE: // subtle cyan
		return {glm::vec3(0.40f, 0.85f, 1.00f), 4};
	case EMERALD_ORE: // subtle green
		return {glm::vec3(0.35f, 1.00f, 0.45f), 3};
	case GOLD_ORE: // warm yellow
		return {glm::vec3(1.00f, 0.70f, 0.25f), 2};
	default:
		return {};
	}
}

/// Propagation intensity (0..15) of the block-light source, or 0.
/// Drives the mesher's emissive-edit dirty heuristic — color-only changes do
/// not exist today (color is a function of type).
inline uint8_t blockLightEmission(uint8_t blockType)
{
	return blockLightSourceForBlock(blockType).intensity;
}

/// Fast source-membership test for bulk scans (halo fill, neighbor-arrival
/// band scans): true iff the block is a block-light source. Table-backed:
/// these scans run per voxel on the mesh-dispatch critical path.
inline const std::array<bool, 256> kIsBlockLightSourceTable = [] {
	std::array<bool, 256> table{};
	for (unsigned int i = 0; i < 256; ++i)
		table[i] = blockLightSourceForBlock(static_cast<uint8_t>(i)).intensity != 0;
	return table;
}();
inline bool isBlockLightSource(uint8_t blockType)
{
	return kIsBlockLightSourceTable[blockType];
}

// RGB4 voxel representation: three 4-bit channels packed R in bits 0-3,
// G in 4-7, B in 8-11 of a uint16_t (12 bits used). Matches the 16-level
// attenuation granularity of the historical scalar light field.
inline uint16_t packBlockLightRGB4(uint8_t r, uint8_t g, uint8_t b)
{
	return static_cast<uint16_t>((r & 0xFu) | ((g & 0xFu) << 4) | ((b & 0xFu) << 8));
}

inline void unpackBlockLightRGB4(uint16_t packed, uint8_t &r, uint8_t &g, uint8_t &b)
{
	r = static_cast<uint8_t>(packed & 0xFu);
	g = static_cast<uint8_t>((packed >> 4) & 0xFu);
	b = static_cast<uint8_t>((packed >> 8) & 0xFu);
}

/// Per-channel maximum — the overlap combination policy. Commutative and
/// associative, so the propagated field is independent of BFS traversal order.
inline uint16_t maxBlockLightRGB4(uint16_t a, uint16_t b)
{
	uint8_t ar, ag, ab, br, bg, bb;
	unpackBlockLightRGB4(a, ar, ag, ab);
	unpackBlockLightRGB4(b, br, bg, bb);
	return packBlockLightRGB4(std::max(ar, br), std::max(ag, bg), std::max(ab, bb));
}

/// One voxel step of attenuation: every channel decays by 1 toward 0.
inline uint16_t attenuateBlockLightRGB4(uint16_t v)
{
	uint8_t r, g, b;
	unpackBlockLightRGB4(v, r, g, b);
	r = static_cast<uint8_t>(r > 0 ? r - 1 : 0);
	g = static_cast<uint8_t>(g > 0 ? g - 1 : 0);
	b = static_cast<uint8_t>(b > 0 ? b - 1 : 0);
	return packBlockLightRGB4(r, g, b);
}

/// Scalar luminance proxy = max channel (drives emissive boost + CPU tests).
inline uint8_t blockLightLumaRGB4(uint16_t v)
{
	uint8_t r, g, b;
	unpackBlockLightRGB4(v, r, g, b);
	return std::max(r, std::max(g, b));
}

/// Packed RGB4 emission of a block type: each channel is the source color
/// scaled by the source intensity, quantized to 4 bits.
inline uint16_t blockLightEmissionRGB4(uint8_t blockType)
{
	const BlockLightSource src = blockLightSourceForBlock(blockType);
	if (src.intensity == 0)
		return 0;
	const auto chan = [&](float c) -> uint8_t {
		return static_cast<uint8_t>(
			std::clamp(static_cast<int>(std::lround(c * static_cast<float>(src.intensity))), 0, 15));
	};
	return packBlockLightRGB4(chan(src.colorLinear.r), chan(src.colorLinear.g), chan(src.colorLinear.b));
}

// Vertex packing — packedData bits (issue #110 layout, RGB extension #141):
//   0-2 normal, 3-10 textureIndex, 11 useBiomeColor, 12-13 AO, 14-17 skyLight,
//   18-21 block R, 22-25 block G, 26-29 block B, 30-31 spare.
inline uint32_t packLightBits(uint8_t skyLight, uint8_t blockR, uint8_t blockG, uint8_t blockB)
{
	return (static_cast<uint32_t>(skyLight & 0xFu) << 14) |
		   (static_cast<uint32_t>(blockR & 0xFu) << 18) |
		   (static_cast<uint32_t>(blockG & 0xFu) << 22) |
		   (static_cast<uint32_t>(blockB & 0xFu) << 26);
}

/// RGB4 nibbles map straight onto the vertex bit positions (R->18, G->22, B->26).
inline uint32_t packLightBitsRGB4(uint8_t skyLight, uint16_t blockRGB4)
{
	return packLightBits(skyLight, static_cast<uint8_t>(blockRGB4 & 0xFu),
						 static_cast<uint8_t>((blockRGB4 >> 4) & 0xFu),
						 static_cast<uint8_t>((blockRGB4 >> 8) & 0xFu));
}

inline void unpackLightBits(uint32_t packedData, uint8_t &skyLight, uint8_t &blockR,
							uint8_t &blockG, uint8_t &blockB)
{
	skyLight = static_cast<uint8_t>((packedData >> 14) & 0xFu);
	blockR = static_cast<uint8_t>((packedData >> 18) & 0xFu);
	blockG = static_cast<uint8_t>((packedData >> 22) & 0xFu);
	blockB = static_cast<uint8_t>((packedData >> 26) & 0xFu);
}

/// SSAO composite floor — safety-only clamp, the horizon-based estimator no
/// longer needs a high global floor. Must match composite.frag.
inline constexpr float kSsaoAoFloor = 0.10f;

/// Edge-aware neighbor selection for the SSAO normal reconstruction — the
/// selection policy of ssao.frag.glsl (keep the GLSL in sync; unit-tested in
/// test_render_helpers.cpp because the original bug was exactly a selection
/// defect: an invalid neighbor substituted with the center would win the
/// smaller-delta comparison with a fake zero delta). Both valid → the
/// smaller |delta| wins (ties → A); a single valid neighbor wins; none →
/// the axis contributes nothing.
enum class SsaoAxisPick
{
	A,
	B,
	None
};
inline SsaoAxisPick ssaoPickAxisDelta(bool validA, bool validB, float absDeltaA, float absDeltaB)
{
	if (validA && validB)
		return absDeltaA <= absDeltaB ? SsaoAxisPick::A : SsaoAxisPick::B;
	if (validA)
		return SsaoAxisPick::A;
	if (validB)
		return SsaoAxisPick::B;
	return SsaoAxisPick::None;
}

/// Weight for directional CSM: 0 deep caves, 1 open sky (matches terrain.frag sunReach).
/// Input is RAW sky light (not day-scaled) so moonlight shadows work at night.
inline float sunShadowWeight(float skyLight01)
{
	const float s = std::clamp(skyLight01, 0.0f, 1.0f);
	// smoothstep(0.05, 0.45, s)
	if (s <= 0.05f)
		return 0.f;
	if (s >= 0.45f)
		return 1.f;
	const float t = (s - 0.05f) / 0.40f;
	return t * t * (3.f - 2.f * t);
}

/// Cave fill remaining after combined light (1 = fully unlit).
inline float caveFillAmount(float combined01)
{
	return 1.f - std::clamp(combined01, 0.f, 1.f);
}

/// True when the god-rays pass writes a valid half-res target this frame.
/// Composite must use the same predicate — do not sample m_godRays when false
/// (target may be UNDEFINED or stale at night / below-horizon).
inline constexpr float kGodRaysSunVisibilityMin = 0.001f;
inline bool godRaysPassActive(bool enabled, float sunVisibility)
{
	return enabled && sunVisibility > kGodRaysSunVisibilityMin;
}


} // namespace lighting
