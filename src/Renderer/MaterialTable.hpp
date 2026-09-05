#pragma once

#include "utils.hpp"
#include "Renderer/Lighting.hpp"
#include "Renderer/MinecraftTextures.hpp"

#include <glm/glm.hpp>
#include <array>
#include <cstdint>

/// CPU material policy (wind / emissive / ice). Shaders use matching helpers via tex index.
namespace materials
{

enum Flags : uint32_t
{
	None = 0,
	FoliageWind = 1u << 0,
	Emissive = 1u << 1,
	IceSpec = 1u << 2,
    RootedWind = 1u << 3,
};

struct MaterialInfo
{
	uint32_t flags{None};
	float windStrength{0.f};
	float emissive{0.f};
	float iceSpec{0.f};
};

inline MaterialInfo infoFor(TextureType t)
{
	MaterialInfo m{};
	if (blockIsFoliage(t))
	{
		m.flags |= FoliageWind;
		m.windStrength = 0.14f;
	}
	else if (blockShape(t) == BlockShape::Cross && t != SEAGRASS)
    {
        m.flags |= FoliageWind | RootedWind;
        m.windStrength = 0.08f;
    }
	else if (t == MOSS_BLOCK)
	{
		m.flags |= FoliageWind;
		m.windStrength = 0.06f;
	}
	if (t == SNOW || blockIsIce(t))
	{
		m.flags |= IceSpec;
		m.iceSpec = (t == SNOW) ? 0.62f : (t == PACKED_ICE ? 0.72f : 0.85f);
	}
	const float em = lighting::emissiveIntensityForBlock(static_cast<uint8_t>(t));
	if (em > 0.f)
	{
		m.flags |= Emissive;
		m.emissive = em;
	}
	return m;
}

inline MaterialInfo infoForIndex(uint8_t texIdx)
{
	if (texIdx >= static_cast<uint8_t>(TextureType::COUNT))
		return {};
	return infoFor(static_cast<TextureType>(texIdx));
}

inline bool hasFoliageWind(uint8_t texIdx)
{
	return (infoForIndex(texIdx).flags & FoliageWind) != 0;
}

inline float windStrength(uint8_t texIdx)
{
	return infoForIndex(texIdx).windStrength;
}

inline float emissiveStrength(uint8_t texIdx)
{
	return infoForIndex(texIdx).emissive;
}

inline float iceSpecStrength(uint8_t texIdx)
{
	return infoForIndex(texIdx).iceSpec;
}

/// GPU-facing table: one vec4 per TextureType (x=wind, y=emissive, z=ice, w=flags as float).
struct MaterialTableUBO
{
	std::array<glm::vec4, 256> entries{};
};

inline MaterialTableUBO buildGpuTable()
{
	MaterialTableUBO t{};
	for (int i = 0; i < static_cast<int>(TextureType::COUNT); ++i)
	{
		const MaterialInfo m = infoFor(static_cast<TextureType>(i));
		t.entries[static_cast<size_t>(i)] =
			glm::vec4(m.windStrength, m.emissive, m.iceSpec, static_cast<float>(m.flags));
	}
	return t;
}

} // namespace materials
