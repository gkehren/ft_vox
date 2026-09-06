#pragma once
#include <Chunk/ChunkCollisionView.hpp>
#include <Entities/MobSystem.hpp>

// Scope must end before streaming/edit publication. No entity owns this adapter.
class ChunkMobWorld final : public entities::MobWorld
{
  public:
    ChunkMobWorld(const ChunkManager &chunks, const TerrainGenerator &generator)
        : view(chunks), generator(generator)
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
    ChunkCollisionView view;
    const TerrainGenerator &generator;
};
