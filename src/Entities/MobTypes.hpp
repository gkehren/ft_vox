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

using EntityId = uint64_t;
using SpawnGroupId = uint64_t;

struct MobRenderState
{
    MobSpecies species{};
    glm::vec3 position{};
    float yaw{}, gait{}, look{}, flap{}, stride{};
};
} // namespace entities
