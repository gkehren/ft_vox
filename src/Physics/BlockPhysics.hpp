#pragma once
#include "VoxelCollision.hpp"
#include <utils.hpp>
#include <Renderer/MinecraftTextures.hpp>

namespace physics
{
// Physical behavior is independent of render transparency and append-only IDs.
inline Cell blockCell(TextureType type)
{
    if (type == AIR) return {true, false, Medium::Air};
    if (type == WATER || type == KELP || type == KELP_TOP || type == SEAGRASS) return {true, false, Medium::Water};
    if (blockIsSmallDetail(type)) return {true, false, Medium::Air};
    if (type == LAVA) return {true, false, Medium::Lava};
    return {true, true, Medium::Air};
}
} // namespace physics
