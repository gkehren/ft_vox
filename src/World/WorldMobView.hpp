#pragma once

// =============================================================================
// WorldMobView — CPU-only scoped mob query view over VoxelWorld (issue #208,
// Epic #204 Phase D). Replaces the temporary ChunkMobWorld adapter, which was
// REMOVED when the mob simulation moved behind sim::GameSimulation:
//
//   * collision sampling delegates to WorldCollisionView (the exact historical
//     semantics: unknown cells, pending-edit overlay, boundary envelope);
//   * surface() keeps the established biome + grass eligibility scan
//     byte-for-byte (only actual voxels, never procedural height inference).
//
// The DERIVED entity light sampling that ChunkMobWorld also wrapped is NOT
// part of this view: light caches are render-owned (RenderWorldBridge). The
// graphical host samples them AFTER taking the mob snapshots
// (sampleSmoothedLight); a headless simulation never needs them
// (MobRenderState.localSkylight < 0 means "unassigned"). Removing the nested
// world -> render lock acquisition also resolves the Phase C lock-order note:
// no path holds both map locks at once anymore (all main-thread regardless).
//
// Scoped-view contract (same as WorldCollisionView): holds the world map lock
// for its lifetime; no edits/unloads may run while it exists. Main thread only.
// =============================================================================

#include <Entities/MobSystem.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <World/WorldCollisionView.hpp>

class WorldMobView final : public entities::MobWorld
{
  public:
    // Parameters are named *Ref so no initializer can silently shadow a member.
    WorldMobView(const VoxelWorld &worldRef, const TerrainGenerator &generatorRef)
        : view(worldRef), generator(generatorRef)
    {
    }

    physics::Cell sample(glm::ivec3 p) const override { return view.sample(p); }

    std::optional<glm::dvec3> surface(int x, int z) const override
    {
        const auto biome = generator.getBiomeAt(x, z);
        if (biome != BIOME_PLAINS && biome != BIOME_FLOWER_MEADOW && biome != BIOME_FOREST &&
            biome != BIOME_BIRCH_FOREST && biome != BIOME_AUTUMN_FOREST && biome != BIOME_CHERRY_GROVE)
            return {};
        for (int y = WORLD_HEIGHT - 1; y >= 0; --y)
        {
            TextureType type = AIR;
            auto c = view.sampleWithType({x, y, z}, &type);
            if (!c.available || c.medium != physics::Medium::Air)
                return {};
            if (c.solid)
            {
                if (type == GRASS_TOP || type == GRASS_SIDE)
                    return glm::dvec3(x + 0.5, y + 1.001, z + 0.5);
                return {};
            }
        }
        return {};
    }

  private:
    WorldCollisionView view;
    const TerrainGenerator &generator;
};
