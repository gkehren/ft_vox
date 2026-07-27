#pragma once

/// Lighting, fog, and post helper math shared by the renderer and unit tests.
/// Block light packing, cave fill, moon ambient, SSAO/god-ray clamps.

#include "utils.hpp"

#include <glm/glm.hpp>

#include <algorithm>
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

inline uint8_t blockLightEmission(uint8_t blockType)
{
	const float e = emissiveIntensityForBlock(blockType);
	if (e <= 0.0f)
		return 0;
	return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(e * 15.0f)), 1, 15));
}

inline uint32_t packLightBits(uint8_t skyLight, uint8_t blockLight)
{
	const uint32_t s = static_cast<uint32_t>(skyLight & 0xFu);
	const uint32_t b = static_cast<uint32_t>(blockLight & 0xFu);
	return (s << 14) | (b << 18);
}

inline void unpackLightBits(uint32_t packedData, uint8_t &skyLight, uint8_t &blockLight)
{
	skyLight = static_cast<uint8_t>((packedData >> 14) & 0xFu);
	blockLight = static_cast<uint8_t>((packedData >> 18) & 0xFu);
}

inline float combinedLightFactor(uint8_t skyLight, uint8_t blockLight, float dayFactor)
{
	const float sky = (static_cast<float>(skyLight) / 15.0f) * std::clamp(dayFactor, 0.0f, 1.0f);
	const float blk = static_cast<float>(blockLight) / 15.0f;
	return std::max(sky, blk);
}

/// Cave ambient floor when sky+block light are 0 (must match terrain.frag kCaveFloor).
inline constexpr float kCaveLightFloor = 0.22f;
/// Soft cave fill RGB scale (unlit rock stays readable). Must match terrain.frag.
inline constexpr float kCaveFillScale = 0.065f;
/// SSAO composite floor — must match composite.frag.
inline constexpr float kSsaoAoFloor = 0.62f;

/// Final local-light scale in terrain.frag — raised cave floor + smoothstep light curve.
/// No zero→full-light fallback (that washed outdoor canyons).
inline float localLightScale(float skyLight01, float blockLight01, float blockLightScale = 1.0f)
{
	const float skyL = std::clamp(skyLight01, 0.0f, 1.0f);
	const float blkL = std::clamp(blockLight01, 0.0f, 1.0f) * std::max(blockLightScale, 0.0f);
	const float local = std::clamp(std::max(skyL, blkL), 0.0f, 1.0f);
	// Hermite smoothstep so mid light levels pop; floor stays dark-but-readable
	const float curve = local * local * (3.0f - 2.0f * local);
	return kCaveLightFloor + (1.0f - kCaveLightFloor) * curve;
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
