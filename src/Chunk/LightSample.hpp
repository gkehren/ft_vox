#pragma once

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lighting
{

// --- Local entity voxel lighting (issue #128) -------------------------------
struct LocalVoxelLight
{
	float skylight{1.0f};       // normalized 0..1
	glm::vec3 blockRgb{0.0f};   // normalized RGB4 -> linear 0..1
};

inline LocalVoxelLight lerpLocalVoxelLight(const LocalVoxelLight &a, const LocalVoxelLight &b, float t)
{
	return {
		std::lerp(a.skylight, b.skylight, t),
		glm::mix(a.blockRgb, b.blockRgb, t)
	};
}

inline uint16_t packVoxelLight(uint8_t skyLight, uint8_t blockR, uint8_t blockG, uint8_t blockB)
{
	return static_cast<uint16_t>(
		(blockR & 0xFu) |
		((blockG & 0xFu) << 4) |
		((blockB & 0xFu) << 8) |
		((static_cast<uint16_t>(skyLight) & 0xFu) << 12));
}

inline uint16_t packVoxelLightRGB4(uint8_t skyLight, uint16_t blockRGB4)
{
	return static_cast<uint16_t>((blockRGB4 & 0x0FFFu) | ((static_cast<uint16_t>(skyLight) & 0xFu) << 12));
}

inline void unpackVoxelLight(uint16_t packed, uint8_t &skyLight, uint8_t &blockR,
							 uint8_t &blockG, uint8_t &blockB)
{
	blockR = static_cast<uint8_t>(packed & 0xFu);
	blockG = static_cast<uint8_t>((packed >> 4) & 0xFu);
	blockB = static_cast<uint8_t>((packed >> 8) & 0xFu);
	skyLight = static_cast<uint8_t>((packed >> 12) & 0xFu);
}

inline LocalVoxelLight unpackLocalVoxelLight(uint16_t packed)
{
	uint8_t sky = 0, r = 0, g = 0, b = 0;
	unpackVoxelLight(packed, sky, r, g, b);
	return {
		static_cast<float>(sky) * (1.0f / 15.0f),
		glm::vec3(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)) * (1.0f / 15.0f)
	};
}

} // namespace lighting
