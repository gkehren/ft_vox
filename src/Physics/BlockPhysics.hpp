#pragma once
#include "VoxelCollision.hpp"
#include <utils.hpp>

namespace physics
{
// Physical behavior is an explicit gameplay policy, deliberately independent
// of render shape and transparency: moving a block from Cube to Cross must
// never silently change its collision medium.
inline Cell blockCell(TextureType type)
{
    switch (type)
    {
    case AIR:
    case SHORT_GRASS:
    case FERN:
    case WILDFLOWER:
    case DRY_SHRUB:
    case LILY_PAD:
        return {true, false, Medium::Air};

    case WATER:
    case KELP:
    case KELP_TOP:
    case SEAGRASS:
        return {true, false, Medium::Water};

    case LAVA:
        return {true, false, Medium::Lava};

    default:
        return {true, true, Medium::Air};
    }
}
} // namespace physics
