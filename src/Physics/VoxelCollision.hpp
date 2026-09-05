#pragma once

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// This module deliberately knows neither Chunk nor a rendering/material API.
namespace physics
{
constexpr double skin = 0.001;
enum class Medium : uint8_t { Air, Water, Lava };
struct Cell
{
    bool available{false};
    bool solid{false};
    Medium medium{Medium::Air};
};
struct Aabb
{
    glm::dvec3 min{}, max{};
};
struct Body
{
    glm::dvec3 position{}; // feet, horizontally centered
    glm::dvec3 velocity{};
    glm::dvec3 size{0.6, 1.8, 0.6};
    bool grounded{false};
    bool waitingForTerrain{false};
    Aabb bounds() const
    {
        return {position - glm::dvec3(size.x * 0.5, 0, size.z * 0.5),
                position + glm::dvec3(size.x * 0.5, size.y, size.z * 0.5)};
    }
};
struct QueryStats
{
    uint64_t cells{0};
    uint64_t iterations{0};
};
class VoxelCollisionWorld
{
public:
    virtual ~VoxelCollisionWorld() = default;
    virtual Cell sample(glm::ivec3 position) const = 0;
};
inline bool overlaps(const Aabb &a, const Aabb &b)
{
    return a.min.x < b.max.x && a.max.x > b.min.x &&
           a.min.y < b.max.y && a.max.y > b.min.y &&
           a.min.z < b.max.z && a.max.z > b.min.z;
}
inline Aabb voxelBounds(glm::ivec3 p)
{
    return {glm::dvec3(p), glm::dvec3(p) + 1.0};
}
template<class Fn> void visitCells(const Aabb &box, Fn &&fn)
{
    const glm::ivec3 lo(glm::floor(box.min));
    // Half-open bounds: touching an adjacent block is not penetration.
    const glm::ivec3 hi(glm::ceil(box.max) - 1.0);
    for (int x = lo.x; x <= hi.x; ++x)
        for (int z = lo.z; z <= hi.z; ++z)
            for (int y = lo.y; y <= hi.y; ++y)
                fn(glm::ivec3(x, y, z));
}
inline Cell query(const VoxelCollisionWorld &world, glm::ivec3 p, QueryStats &stats)
{
    ++stats.cells;
    return world.sample(p);
}
inline bool clear(const VoxelCollisionWorld &world, const Aabb &box, QueryStats &stats,
                  bool *unknown = nullptr)
{
    bool valid = true;
    visitCells(box, [&](glm::ivec3 p) {
        const Cell c = query(world, p, stats);
        valid &= c.available && !c.solid;
        if (unknown && !c.available) *unknown = true;
    });
    return valid;
}
struct Immersion { double water{0}, lava{0}; };
inline Immersion immersion(const VoxelCollisionWorld &world, const Aabb &box, QueryStats &stats)
{
    Immersion result;
    const glm::dvec3 size = box.max - box.min;
    const double volume = size.x * size.y * size.z;
    visitCells(box, [&](glm::ivec3 p) {
        const Cell c = query(world, p, stats);
        if (!c.available || c.medium == Medium::Air) return;
        const Aabb v = voxelBounds(p);
        const auto d = glm::max(glm::min(box.max, v.max) - glm::max(box.min, v.min), glm::dvec3(0));
        const double part = d.x * d.y * d.z / volume;
        if (c.medium == Medium::Water) result.water += part;
        else result.lava += part;
    });
    return result;
}
struct Hit
{
    double time{1.0};
    glm::ivec3 normal{0};
    bool hit{false};
    bool solidGround{false};
    bool unknown{false};
};
// Minkowski/slab sweep. A stationary axis must overlap strictly: this is
// essential for sliding across coplanar voxel seams without snagging.
inline Hit sweep(const VoxelCollisionWorld &world, const Aabb &box, glm::dvec3 delta,
                 QueryStats &stats)
{
    Hit nearest;
    const Aabb region{glm::min(box.min, box.min + delta) - skin,
                      glm::max(box.max, box.max + delta) + skin};
    visitCells(region, [&](glm::ivec3 p) {
        const Cell c = query(world, p, stats);
        if (c.available && !c.solid) return;
        const Aabb v = voxelBounds(p);
        glm::dvec3 entry(-std::numeric_limits<double>::infinity());
        double leave = std::numeric_limits<double>::infinity();
        for (int axis = 0; axis < 3; ++axis)
        {
            if (std::abs(delta[axis]) < 1e-12)
            {
                if (box.max[axis] <= v.min[axis] || box.min[axis] >= v.max[axis]) return;
            }
            else
            {
                const double a = (v.min[axis] - box.max[axis]) / delta[axis];
                const double b = (v.max[axis] - box.min[axis]) / delta[axis];
                entry[axis] = std::min(a, b);
                leave = std::min(leave, std::max(a, b));
            }
        }
        const double enter = std::max({entry.x, entry.y, entry.z});
        if (enter < -1e-10 || enter > 1.0 || enter > leave + 1e-10 || leave < 0.0) return;
        if (enter > nearest.time + 1e-10) return;
        if (!nearest.hit || enter < nearest.time - 1e-10) nearest = {};
        nearest.hit = true;
        nearest.time = std::max(0.0, enter);
        nearest.unknown |= !c.available;
        for (int axis = 0; axis < 3; ++axis)
            if (std::abs(entry[axis] - enter) < 1e-10)
            {
                nearest.normal[axis] = delta[axis] > 0 ? -1 : 1;
                if (axis == 1 && delta.y < 0 && c.available && c.solid)
                    nearest.solidGround = true;
            }
    });
    return nearest;
}
// Bounded recovery for edits / external repositioning. Unknown cells never
// cause a speculative teleport; unresolved overlaps leave the body stationary.
inline bool recover(const VoxelCollisionWorld &world, Body &body, QueryStats &stats)
{
    const auto original = body.position;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        bool unknown = false;
        if (clear(world, body.bounds(), stats, &unknown)) return true;
        if (unknown) { body.waitingForTerrain = true; break; }
        double best = std::numeric_limits<double>::infinity();
        glm::dvec3 correction(0);
        const Aabb box = body.bounds();
        visitCells(box, [&](glm::ivec3 p) {
            const Cell c = query(world, p, stats);
            if (!c.solid) return;
            const Aabb v = voxelBounds(p);
            for (int axis = 0; axis < 3; ++axis)
                for (double amount : {v.min[axis] - box.max[axis] - skin,
                                      v.max[axis] - box.min[axis] + skin})
                    if (std::abs(amount) < best)
                    {
                        best = std::abs(amount);
                        correction = glm::dvec3(0);
                        correction[axis] = amount;
                    }
        });
        if (best > 1.0) break;
        body.position += correction;
        if (glm::length(body.position - original) > 1.0)
        {
            body.position = original;
            body.velocity = glm::dvec3(0);
            return false;
        }
    }
    if (clear(world, body.bounds(), stats)) return true;
    body.position = original;
    body.velocity = glm::dvec3(0);
    return false;
}
inline void move(const VoxelCollisionWorld &world, Body &body, glm::dvec3 delta, QueryStats &stats)
{
    body.grounded = false;
    body.waitingForTerrain = false;
    if (!recover(world, body, stats)) return;
    for (int i = 0; i < 4 && glm::dot(delta, delta) > 1e-20; ++i)
    {
        ++stats.iterations;
        const Hit hit = sweep(world, body.bounds(), delta, stats);
        if (!hit.hit) { body.position += delta; break; }
        body.position += delta * hit.time;
        delta *= 1.0 - hit.time;
        body.grounded |= hit.solidGround;
        body.waitingForTerrain |= hit.unknown;
        for (int axis = 0; axis < 3; ++axis)
            if (hit.normal[axis])
            {
                body.position[axis] += hit.normal[axis] * skin;
                delta[axis] = 0;
                body.velocity[axis] = 0;
            }
    }
    if (body.velocity.y <= 0)
    {
        const Hit support = sweep(world, body.bounds(), {0, -skin * 2, 0}, stats);
        body.grounded |= support.hit && support.solidGround;
    }
}
} // namespace physics
