#pragma once
#include <Entities/MobTypes.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <cstdint>
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
/// One used UV region of a mob skin image, in texel coordinates (half-open
/// [x, x + w) x [y, y + h)). Rects of the same texture never overlap.
struct MobFaceRect
{
    uint32_t x, y, w, h;
};
/// UV face rectangles of every part bound to `texture`, in texel units of the
/// image that texture is sampled with. Face UV corners are integer model
/// pixels, and all supported skin shapes map model pixels to texels with the
/// uniform scale imgW / 64 (the same scale the sampler's uvScale folding
/// implies on both axes). Consumed by the UV-rect-aware mip generator and by
/// the synthetic-skin GPU tests.
std::vector<MobFaceRect> mobTextureFaceRects(const MobModels &models, uint32_t texture, uint32_t imgW,
                                             uint32_t imgH);
glm::mat4 mobPartTransform(const MobRenderState &state, const MobPart &part);
} // namespace entities
