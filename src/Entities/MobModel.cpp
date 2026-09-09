#include "MobModel.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace entities
{
MobModels::MobModels()
{
    // Minecraft box UV unfolding. Coordinates and pivots use model pixels,
    // +Y down, feet at Y=24. Convert once to world units (+Y up).
    auto box = [&](int species, int texture, MobBone bone, glm::vec3 pivot, glm::vec3 lo, glm::vec3 size,
                   glm::vec2 uv, float pitch = 0.f, float inflate = 0.f) {
        MobPart p{uint32_t(vertices.size()),
                  36,
                  uint32_t(texture),
                  {pivot.x / 16, (24 - pivot.y) / 16, pivot.z / 16},
                  pitch,
                  bone};
        const float x = lo.x - inflate, y = lo.y - inflate, z = lo.z - inflate;
        const float X = lo.x + size.x + inflate, Y = lo.y + size.y + inflate, Z = lo.z + size.z + inflate;
        const float u = uv.x, v = uv.y, w = size.x, h = size.y, d = size.z;
        auto face = [&](std::array<glm::vec3, 4> points, glm::vec3 normal, glm::vec4 rect) {
            const std::array<glm::vec2, 4> tex{
                {{rect.z, rect.y}, {rect.x, rect.y}, {rect.x, rect.w}, {rect.z, rect.w}}};
            for (int i : {0, 1, 2, 0, 2, 3})
            {
                auto a = points[i];
                a.y = -a.y;
                normal = glm::normalize(normal);
                vertices.push_back({a / 16.f, {normal.x, -normal.y, normal.z}, tex[i] / glm::vec2(64, 32)});
            }
        };
        face({glm::vec3{X, y, z}, {x, y, z}, {x, y, Z}, {X, y, Z}}, {0, -1, 0}, {u + d, v, u + d + w, v + d});
        face({glm::vec3{X, Y, Z}, {x, Y, Z}, {x, Y, z}, {X, Y, z}}, {0, 1, 0},
             {u + d + w, v, u + d + w + w, v + d});
        face({glm::vec3{x, y, z}, {x, y, Z}, {x, Y, Z}, {x, Y, z}}, {-1, 0, 0}, {u, v + d, u + d, v + d + h});
        face({glm::vec3{X, y, Z}, {X, y, z}, {X, Y, z}, {X, Y, Z}}, {1, 0, 0},
             {u + d + w, v + d, u + d + w + d, v + d + h});
        face({glm::vec3{X, y, z}, {x, y, z}, {x, Y, z}, {X, Y, z}}, {0, 0, -1},
             {u + d, v + d, u + d + w, v + d + h});
        face({glm::vec3{x, y, Z}, {X, y, Z}, {X, Y, Z}, {x, Y, Z}}, {0, 0, 1},
             {u + d + w + d, v + d, u + d + w + d + w, v + d + h});
        models[species].parts.push_back(p);
    };
    using B = MobBone;
    // Cow (12-pixel legs, horizontal rotated torso, horns follow head).
    box(0, 0, B::Head, {0, 4, -8}, {-4, -4, -6}, {8, 8, 6}, {0, 0});
    box(0, 0, B::Head, {0, 4, -8}, {-5, -5, -4}, {1, 3, 1}, {22, 0});
    box(0, 0, B::Head, {0, 4, -8}, {4, -5, -4}, {1, 3, 1}, {22, 0});
    box(0, 0, B::Body, {0, 5, 2}, {-6, -10, -7}, {12, 18, 10}, {18, 4}, -1.570796327f);
    box(0, 0, B::Body, {0, 5, 2}, {-2, 2, -8}, {4, 6, 1}, {52, 0}, -1.570796327f);
    for (int i = 0; i < 4; ++i)
        box(0, 0, B(int(B::Leg0) + i), {i % 2 ? 4.f : -4.f, 12, i < 2 ? 7.f : -6.f}, {-2, 0, -2}, {4, 12, 4},
            {0, 16});
    // Pig.
    box(1, 1, B::Head, {0, 12, -6}, {-4, -4, -8}, {8, 8, 8}, {0, 0});
    box(1, 1, B::Head, {0, 12, -6}, {-2, 0, -9}, {4, 3, 1}, {16, 16});
    box(1, 1, B::Body, {0, 11, 2}, {-5, -10, -7}, {10, 16, 8}, {28, 8}, -1.570796327f);
    for (int i = 0; i < 4; ++i)
        box(1, 1, B(int(B::Leg0) + i), {i % 2 ? 3.f : -3.f, 18, i < 2 ? 7.f : -5.f}, {-2, 0, -2}, {4, 6, 4},
            {0, 16});
    // Sheep base and two fleece layers; no shearing or color variants.
    box(2, 2, B::Head, {0, 6, -8}, {-3, -4, -6}, {6, 6, 8}, {0, 0});
    box(2, 2, B::Body, {0, 5, 2}, {-4, -10, -7}, {8, 16, 6}, {28, 8}, -1.570796327f);
    for (int i = 0; i < 4; ++i)
        box(2, 2, B(int(B::Leg0) + i), {i % 2 ? 3.f : -3.f, 12, i < 2 ? 7.f : -5.f}, {-2, 0, -2}, {4, 12, 4},
            {0, 16});
    for (int t : {5, 4})
    {
        float f = t == 5 ? 0.95f : 1.f;
        box(2, t, B::Head, {0, 6, -8}, {-3, -4, -4}, {6, 6, 6}, {0, 0}, 0, 0.6f * f);
        box(2, t, B::Body, {0, 5, 2}, {-4, -10, -7}, {8, 16, 6}, {28, 8}, -1.570796327f, 1.75f * f);
        for (int i = 0; i < 4; ++i)
            box(2, t, B(int(B::Leg0) + i), {i % 2 ? 3.f : -3.f, 12, i < 2 ? 7.f : -5.f}, {-2, 0, -2},
                {4, 6, 4}, {0, 16}, 0, 0.5f * f);
    }
    // Chicken head, beak/wattle, torso, legs and wings.
    box(3, 3, B::Head, {0, 15, -4}, {-2, -6, -2}, {4, 6, 3}, {0, 0});
    box(3, 3, B::Head, {0, 15, -4}, {-2, -4, -4}, {4, 2, 2}, {14, 0});
    box(3, 3, B::Head, {0, 15, -4}, {-1, -2, -3}, {2, 2, 2}, {14, 4});
    box(3, 3, B::Body, {0, 16, 0}, {-3, -4, -3}, {6, 8, 6}, {0, 9}, -1.570796327f);
    for (int i = 0; i < 2; ++i)
        box(3, 3, B(int(B::Leg0) + i), {i ? 1.f : -1.f, 19, 1}, {-1, 0, -3}, {3, 5, 3}, {26, 0});
    box(3, 3, B::WingLeft, {-4, 13, 0}, {0, 0, -3}, {1, 4, 6}, {24, 13});
    box(3, 3, B::WingRight, {4, 13, 0}, {-1, 0, -3}, {1, 4, 6}, {24, 13});
}
glm::mat4 mobPartTransform(const MobRenderState &s, const MobPart &p)
{
    auto m = glm::translate(glm::mat4(1), s.position);
    m = glm::rotate(m, -s.yaw, glm::vec3(0, 1, 0));
    m = glm::translate(m, p.pivot);
    float pitch = p.pitch;
    if (p.bone == MobBone::Head)
        m = glm::rotate(m, s.look, glm::vec3(0, 1, 0));
    if (p.bone >= MobBone::Leg0 && p.bone <= MobBone::Leg3)
    {
        int leg = int(p.bone) - int(MobBone::Leg0);
        pitch += std::sin(s.gait) * s.stride * 0.55f * ((leg == 0 || leg == 3) ? 1.f : -1.f);
    }
    if (p.bone == MobBone::WingLeft || p.bone == MobBone::WingRight)
        m = glm::rotate(m, s.flap * (p.bone == MobBone::WingLeft ? 1.f : -1.f), glm::vec3(0, 0, 1));
    return glm::rotate(m, pitch, glm::vec3(1, 0, 0));
}
std::vector<MobFaceRect> mobTextureFaceRects(const MobModels &models, uint32_t texture, uint32_t imgW,
                                             uint32_t imgH)
{
    std::vector<MobFaceRect> rects;
    const uint32_t scale = imgW / 64u;
    for (const auto &species : models.models)
    {
        for (const auto &part : species.parts)
        {
            if (part.texture != texture)
                continue;
            // 6 faces x 6 vertices, baked contiguously per part; each face's
            // 4 UV corners are exactly its rect corners in model pixels.
            for (uint32_t face = 0; face < 6; ++face)
            {
                const size_t base = part.firstVertex + face * 6;
                float u0 = 1.0f, v0 = 1.0f, u1 = 0.0f, v1 = 0.0f;
                for (size_t v = base; v < base + 6 && v < models.vertices.size(); ++v)
                {
                    const glm::vec2 &uv = models.vertices[v].uv;
                    u0 = std::min(u0, uv.x);
                    v0 = std::min(v0, uv.y);
                    u1 = std::max(u1, uv.x);
                    v1 = std::max(v1, uv.y);
                }
                // uv corners are exact multiples of 1/64, 1/32 (powers of two).
                const uint32_t rx = uint32_t(std::lround(u0 * 64.0f)) * scale;
                const uint32_t ry = uint32_t(std::lround(v0 * 32.0f)) * scale;
                const uint32_t rw = (uint32_t(std::lround(u1 * 64.0f)) - uint32_t(std::lround(u0 * 64.0f))) * scale;
                const uint32_t rh = (uint32_t(std::lround(v1 * 32.0f)) - uint32_t(std::lround(v0 * 32.0f))) * scale;
                if (rw == 0 || rh == 0 || rx + rw > imgW || ry + rh > imgH)
                    continue; // defensive: never emit a rect outside the image
                rects.push_back({rx, ry, rw, rh});
            }
        }
    }
    // Distinct faces never overlap in these layouts, but different parts can
    // share identical rects (e.g. the two cow horns) — deduplicate.
    std::sort(rects.begin(), rects.end(), [](const MobFaceRect &a, const MobFaceRect &b) {
        return a.y != b.y ? a.y < b.y : a.x != b.x ? a.x < b.x : a.h != b.h ? a.h < b.h : a.w < b.w;
    });
    rects.erase(std::unique(rects.begin(), rects.end(),
                            [](const MobFaceRect &a, const MobFaceRect &b) {
                                return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
                            }),
                rects.end());
    return rects;
}
} // namespace entities
