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
    const BlockMedium contained = blockContainedMedium(type);
    Medium medium = Medium::Air;
    if (contained == BlockMedium::Water)
        medium = Medium::Water;
    else if (contained == BlockMedium::Lava)
        medium = Medium::Lava;

    switch (type)
    {
    case AIR:
    case SHORT_GRASS:
    case FERN:
    case WILDFLOWER:
    case DRY_SHRUB:
    case LILY_PAD:
    case WATER:
    case KELP:
    case KELP_TOP:
    case SEAGRASS:
    case LAVA:
        return {true, false, medium};

    default:
        return {true, true, medium};
    }
}
} // namespace physics
