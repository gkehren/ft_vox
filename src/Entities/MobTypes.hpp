#pragma once
#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>

namespace entities
{
// Shared, dependency-light entity data: safe for the renderer and the
// simulation to include without pulling physics, chunk or Vulkan headers.
enum class MobSpecies : uint8_t
{
    Cow,
    Pig,
    Sheep,
    Chicken,
    Count
};
inline constexpr size_t kMobSpeciesCount = size_t(MobSpecies::Count);

// Hard population ceiling shared by the simulation (capacity) and the
// renderer (instance/draw budget), without pulling simulation headers.
inline constexpr size_t kMaxMobCount = 48;

using EntityId = uint64_t;
using SpawnGroupId = uint64_t;

struct MobRenderState
{
    MobSpecies species{};
    glm::vec3 position{};
    float yaw{}, gait{}, look{}, flap{}, stride{};
    float localSkylight{-1.0f}; // < 0 means unassigned / sample from world
    glm::vec3 localBlockRgb{0.0f};
};

inline glm::vec3 mobLightSamplePosition(const MobRenderState &mob)
{
    // Sample at entity body center (0.5m above feet)
    return mob.position + glm::vec3(0.0f, 0.5f, 0.0f);
}
} // namespace entities
