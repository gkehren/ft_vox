#pragma once

// =============================================================================
// ChunkMobWorld — temporary mob adapter (issue #206; Phase D #208 removes it).
// Since Phase C (#207) the authoritative collision/query semantics are
// consumed DIRECTLY from the CPU-only WorldCollisionView, and the derived
// entity light sampling comes from the RenderWorldBridge's light caches —
// the former ChunkCollisionView wrapper is gone.
//
// LOCK ORDER (issue #206/#207 review): this view acquires world -> render
// (the CPU view holds the world map lock; the render shared lock is held for
// the light-cache reads). The bridge's mesh dispatch acquires render ->
// world. The two orders are only safe because BOTH paths run exclusively on
// the main thread today — neither map lock is ever contended across threads
// (workers take only the completion-queue and pool mutexes). When Phase D/E
// separates these across threads, this inversion MUST be resolved; do not
// introduce a second thread that takes both locks without revisiting this.
// =============================================================================

#include <Renderer/RenderWorldBridge.hpp>
#include <World/WorldCollisionView.hpp>
#include <Entities/MobSystem.hpp>

// Scope must end before streaming/edit publication. No entity owns this adapter.
class ChunkMobWorld final : public entities::MobWorld
{
  public:
    ChunkMobWorld(const VoxelWorld &world, const RenderWorldBridge &bridge,
                  const TerrainGenerator &generator)
        : view(world), renderLock(bridge.m_mutex), bridge(bridge), generator(generator)
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
    // Derived render light caches: the render lock is held for the view's
    // lifetime, so the Unlocked sampling contract is satisfied.
    lighting::LocalVoxelLight sampleLight(const glm::vec3 &pos) const override
    {
        return bridge.sampleSmoothedLightUnlocked(pos);
    }

  private:
    WorldCollisionView view;
    std::shared_lock<std::shared_mutex> renderLock;
    const RenderWorldBridge &bridge;
    const TerrainGenerator &generator;
};
