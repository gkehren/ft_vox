#pragma once
#include "MobSystem.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <vector>

namespace entities
{
struct MobVertex
{
    glm::vec3 position, normal;
    glm::vec2 uv;
};
enum class MobBone
{
    Body,
    Head,
    Leg0,
    Leg1,
    Leg2,
    Leg3,
    WingLeft,
    WingRight
};
struct MobPart
{
    uint32_t firstVertex{}, vertexCount{}, texture{};
    glm::vec3 pivot{};
    float pitch{};
    MobBone bone{};
};
struct MobModel
{
    std::vector<MobPart> parts;
};
struct MobModels
{
    std::vector<MobVertex> vertices;
    std::array<MobModel, kMobSpeciesCount> models;
    MobModels();
};
glm::mat4 mobPartTransform(const MobRenderState &state, const MobPart &part);
} // namespace entities
