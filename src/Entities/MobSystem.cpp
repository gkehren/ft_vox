#include "MobSystem.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>

namespace entities
{
namespace
{
uint64_t mix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
double random(uint64_t &state)
{
    state = mix(state);
    return double(state >> 11) * 0x1.0p-53;
}
double angle(double v)
{
    return std::remainder(v, 2 * std::numbers::pi);
}
double distance2(glm::dvec3 a, glm::dvec3 b)
{
    auto d = glm::dvec2(a.x - b.x, a.z - b.z);
    return glm::dot(d, d);
}
bool dry(const physics::VoxelCollisionWorld &w, const physics::Body &b, physics::QueryStats &q)
{
    if (!physics::clear(w, b.bounds(), q))
        return false;
    auto i = physics::immersion(w, b.bounds(), q);
    return i.water + i.lava < 0.001;
}
// Only invariants the fixed-step loop and populate() arithmetic actually rely
// on (uint32 group-size subtraction, division by fixedStep, percentage draw).
// Deliberately lenient about, e.g., retireDistance < spawnMax.
void validateSettings(const MobSettings &s)
{
    assert(std::isfinite(s.spawnMin) && std::isfinite(s.spawnMax) && std::isfinite(s.retireDistance));
    assert(std::isfinite(s.spawnInterval) && std::isfinite(s.spawnChance) && std::isfinite(s.fixedStep));
    assert(s.spawnMin >= 0.0 && s.spawnMax >= s.spawnMin);
    assert(s.spawnInterval > 0.0);
    assert(s.spawnChance >= 0.0 && s.spawnChance <= 1.0);
    assert(s.minGroupSize > 0 && s.maxGroupSize >= s.minGroupSize);
    assert(s.maxGroupAttemptsPerScan > 0);
    assert(s.fixedStep > 0.0);
    assert(s.maxSteps > 0);
}
} // namespace
MobSystem::MobSystem()
{
    m_mobs.reserve(MobSettings::capacity);
    m_groups.reserve(256);
}
void MobSystem::reset(uint64_t seed)
{
    m_seed = seed;
    m_mobs.clear();
    m_groups.clear();
    m_accumulator = m_spawnTimer = 0;
    m_dropped = 0;
}
bool MobSystem::add(MobSpecies species, glm::dvec3 feet, uint64_t id, uint64_t origin,
                    const physics::VoxelCollisionWorld &world)
{
    if (m_mobs.size() >= MobSettings::capacity || size_t(species) >= kMobSpeciesCount)
        return false;
    for (auto &m : m_mobs)
        if (m.id == id)
            return false;
    Mob m{};
    m.id = id;
    m.origin = origin;
    m.randomState = mix(id ^ m_seed);
    m.species = species;
    m.body.position = feet;
    m.body.size = speciesSettings[size_t(species)].size;
    if (!dry(world, m.body, m_queries))
        return false;
    auto support = physics::sweep(world, m.body.bounds(), {0, -0.05, 0}, m_queries);
    if (!support.solidGround)
        return false;
    for (auto &other : m_mobs)
        if (physics::overlaps(m.body.bounds(), other.body.bounds()))
            return false;
    m.body.grounded = true;
    m.previous = feet;
    m.yaw = m.previousYaw = m.targetYaw = random(m.randomState) * 6.283185307;
    m.timer = 2 + 4 * random(m.randomState);
    assert(size_t(m.species) < kMobSpeciesCount);
    m_mobs.push_back(m);
    return true;
}
void MobSystem::populate(const MobWorld &world, glm::dvec3 observer, double radius)
{
    struct Candidate
    {
        int x, z;
        double distance;
    };
    std::array<Candidate, 256> candidates{};
    size_t count = 0;
    const int r = int(std::ceil(radius / 16)), cx = int(std::floor(observer.x / 16)),
              cz = int(std::floor(observer.z / 16));
    for (int z = cz - r; z <= cz + r; ++z)
        for (int x = cx - r; x <= cx + r; ++x)
        {
            double d = distance2(observer, {x * 16.0 + 8, 0, z * 16.0 + 8});
            if (d > radius * radius || d < settings.spawnMin * settings.spawnMin ||
                count == candidates.size())
                continue;
            candidates[count++] = {x, z, d};
        }
    std::sort(candidates.begin(), candidates.begin() + count, [](const auto &a, const auto &b) {
        if (a.distance != b.distance)
            return a.distance < b.distance;
        return a.z != b.z ? a.z < b.z : a.x < b.x;
    });
    int attempts = 0;
    for (size_t i = 0; i < count && m_mobs.size() < MobSettings::capacity; ++i)
    {
        auto c = candidates[i];
        const uint64_t key = (uint64_t(uint32_t(c.x)) << 32) | uint32_t(c.z);
        uint64_t rng = mix(key ^ mix(m_seed));
        if (random(rng) > settings.spawnChance)
            continue;
        if (std::any_of(m_groups.begin(), m_groups.end(), [&](auto &g) { return g.key == key; }))
            continue;
        // Retry unpublished chunks on a later scan, without monopolizing the budget.
        if (!world.sample({c.x * 16 + 8, 128, c.z * 16 + 8}).available)
            continue;
        if (++attempts > int(settings.maxGroupAttemptsPerScan))
            break;
        const auto speciesIndex = size_t(random(rng) * double(kMobSpeciesCount));
        MobSpecies species = MobSpecies(std::min(speciesIndex, kMobSpeciesCount - 1));
        const int members =
            int(settings.minGroupSize) + int(random(rng) * double(settings.maxGroupSize - settings.minGroupSize + 1));
        bool spawned = false;
        for (int j = 0; j < members; ++j)
        {
            int x = c.x * 16 + 3 + int(random(rng) * 10), z = c.z * 16 + 3 + int(random(rng) * 10);
            auto feet = world.surface(x, z);
            if (!feet || distance2(*feet, observer) < settings.spawnMin * settings.spawnMin ||
                distance2(*feet, observer) > radius * radius)
                continue;
            spawned |= add(species, *feet, mix(key ^ mix(m_seed) ^ uint64_t(j + 1)), key, world);
        }
        // A processed group is not topped up after its members wander away.
        // Failed grass/clearance candidates are also stable until the area retires.
        (void)spawned;
        m_groups.push_back({key, {c.x * 16.0 + 8, c.z * 16.0 + 8}});
    }
}
void MobSystem::step(const MobWorld &world)
{
    const double dt = settings.fixedStep;
    // Snapshot neighbors so separation does not depend on integration order.
    std::array<glm::dvec3, MobSettings::capacity> positions{};
    for (size_t i = 0; i < m_mobs.size(); ++i)
        positions[i] = m_mobs[i].body.position;
    for (auto &m : m_mobs)
        tickMob(m, world, dt, std::span(positions.data(), m_mobs.size()), m_queries);
}
void tickMob(Mob &m, const physics::VoxelCollisionWorld &world, double dt,
             std::span<const glm::dvec3> neighbors, physics::QueryStats &queries)
{
    m.previous = m.body.position;
    m.previousYaw = m.yaw;
    m.previousGait = m.gait;
    m.previousStride = m.stride;
    m.age += dt;
    m.timer -= dt;
    if (m.timer <= 0)
    {
        m.walking = !m.walking;
        m.timer = m.walking ? 3 + 5 * random(m.randomState) : 2 + 4 * random(m.randomState);
        m.targetYaw = m.yaw + (random(m.randomState) - 0.5) * 4.5;
    }
    m.yaw += std::clamp(angle(m.targetYaw - m.yaw), -2.0 * dt, 2.0 * dt);
    glm::dvec3 direction(std::sin(m.yaw), 0, -std::cos(m.yaw));
    for (size_t i = 0; i < neighbors.size(); ++i)
    {
        auto away = m.body.position - neighbors[i];
        away.y = 0;
        double d = glm::length(away);
        if (d > 0.001 && d < 1.5)
            direction += away / d * ((1.5 - d) * 0.4);
    }
    // Neighbor separation can cancel the walk direction exactly; normalizing a
    // zero vector would poison position/velocity/voxel queries with NaN.
    const double directionSq = glm::dot(direction, direction);
    if (directionSq > 1e-12)
        direction /= std::sqrt(directionSq);
    else
        direction = glm::dvec3{std::sin(m.yaw), 0.0, -std::cos(m.yaw)};
    auto immersion = physics::immersion(world, m.body.bounds(), queries);
    bool swimming = immersion.water + immersion.lava > 0.1;
    m.shoreTimer -= dt;
    if (swimming && m.shoreTimer <= 0)
    {
        m.shoreTimer = 0.5;
        bool found = false;
        for (double range : {1.5, 3.0, 5.0})
        {
            for (int i = 0; i < 8 && !found; ++i)
            {
                double a = m.yaw + i * 0.785398163;
                auto probe = m.body;
                probe.position += glm::dvec3(std::sin(a) * range, 1.1, -std::cos(a) * range);
                if (!dry(world, probe, queries))
                    continue;
                auto hit = physics::sweep(world, probe.bounds(), {0, -2.0, 0}, queries);
                if (hit.solidGround)
                {
                    probe.position.y -= hit.time * 2;
                    if (dry(world, probe, queries))
                    {
                        m.targetYaw = a;
                        found = true;
                    }
                }
            }
            if (found)
                break;
        }
    }
    double speed = m.walking ? speciesSettings[size_t(m.species)].speed : 0;
    if (swimming)
        speed = 0.7;
    glm::dvec3 horizontal = direction * speed * dt;
    if (!swimming && m.body.grounded && speed > 0)
    {
        physics::Body probe = m.body;
        // Probe a body-width ahead for cliffs/water, not just the next tiny tick.
        probe.position += direction * (0.35 + m.body.size.x * 0.5);
        auto supportProbe = probe;
        supportProbe.size.x = supportProbe.size.z = 0.05;
        auto ground = physics::sweep(world, supportProbe.bounds(), {0, -1.05, 0}, queries);
        if (!ground.solidGround || !dry(world, probe, queries))
        {
            // Try a full-block step, checking the entire vertical and horizontal sweep.
            auto up = physics::sweep(world, m.body.bounds(), {0, 1.01, 0}, queries);
            physics::Body raised = m.body;
            raised.position.y += 1.01;
            auto forward =
                physics::sweep(world, raised.bounds(), direction * (0.35 + m.body.size.x * 0.5), queries);
            auto ahead = raised;
            ahead.position += direction * (0.35 + m.body.size.x * 0.5);
            auto down = physics::sweep(world, ahead.bounds(), {0, -1.06, 0}, queries);
            if (!up.hit && !forward.hit && down.solidGround && dry(world, ahead, queries) && down.time < 0.5)
            {
                // Approach the riser before jumping: an early hop lands before
                // reaching the block, especially for slow chickens.
                auto riser =
                    physics::sweep(world, m.body.bounds(), direction * std::max(0.08, speed * dt), queries);
                if (riser.hit && !riser.unknown)
                    m.body.velocity.y = 8.5;
            }
            else
            {
                horizontal = {0, 0, 0};
                m.targetYaw = m.yaw + 1.2 + random(m.randomState) * 1.5;
                m.timer = std::min(m.timer, 0.6);
            }
        }
    }
    m.body.velocity.x = horizontal.x / dt;
    m.body.velocity.z = horizontal.z / dt;
    if (swimming)
    {
        auto obstacle = physics::sweep(world, m.body.bounds(), direction * 0.35, queries);
        auto raised = m.body;
        raised.position.y += 1.05;
        if (obstacle.hit && !obstacle.unknown &&
            !physics::sweep(world, m.body.bounds(), {0, 1.05, 0}, queries).hit &&
            !physics::sweep(world, raised.bounds(), direction * 0.7, queries).hit)
            m.body.velocity.y = 5.0;
    }
    if (swimming)
        m.body.velocity.y += (12.0 * (immersion.water + immersion.lava) - 3.0 - m.body.velocity.y * 4) * dt;
    else
        m.body.velocity.y =
            std::max(m.body.velocity.y - 24 * dt, m.species == MobSpecies::Chicken ? -3.0 : -30.0);
    physics::move(world, m.body, m.body.velocity * dt, queries);
    double travelled = std::sqrt(distance2(m.body.position, m.previous));
    m.gait += travelled * 7;
    m.stride += (std::clamp(travelled / dt, 0.0, 1.0) - m.stride) * std::min(1.0, 10 * dt);
    if (speed > 0 && travelled < 0.0001 && m.body.grounded && !m.body.waitingForTerrain)
        m.timer = std::min(m.timer, 0.25);
    // Any future AI producing NaN must trip here in Debug, not leak into AABB
    // voxel queries (where floor(NaN) casts are undefined behavior).
    assert(std::isfinite(m.body.position.x) && std::isfinite(m.body.position.y) &&
           std::isfinite(m.body.position.z));
    assert(std::isfinite(m.yaw) && std::isfinite(m.targetYaw));
}

void MobSystem::update(double dt, const MobWorld &world, glm::dvec3 observer, double availableRadius,
                       bool suspended)
{
    validateSettings(settings);
    m_queries = {};
    if (suspended)
    {
        m_accumulator = 0;
        for (auto &m : m_mobs)
        {
            m.previous = m.body.position;
            m.previousYaw = m.yaw;
            m.previousGait = m.gait;
            m.previousStride = m.stride;
        }
        return;
    }
    const double retire = std::min(settings.retireDistance, std::max(0.0, availableRadius));
    std::erase_if(m_mobs, [&](auto &m) {
        return distance2(m.body.position, observer) > retire * retire ||
               !world.sample(glm::ivec3(glm::floor(m.body.position))).available;
    });
    std::erase_if(m_groups, [&](auto &g) {
        auto d = g.center - glm::dvec2(observer.x, observer.z);
        return glm::dot(d, d) > retire * retire;
    });
    dt = std::isfinite(dt) ? std::max(dt, 0.0) : 0.0;
    m_spawnTimer -= dt;
    if (m_spawnTimer <= 0)
    {
        populate(world, observer, std::min(settings.spawnMax, retire));
        m_spawnTimer = settings.spawnInterval;
    }
    m_accumulator += dt;
    uint32_t steps = 0;
    while (m_accumulator + 1e-12 >= settings.fixedStep && steps < settings.maxSteps)
    {
        step(world);
        m_accumulator -= settings.fixedStep;
        ++steps;
    }
    if (m_accumulator >= settings.fixedStep)
    {
        auto dropped = uint64_t(m_accumulator / settings.fixedStep);
        m_dropped += dropped;
        m_accumulator = std::fmod(m_accumulator, settings.fixedStep);
    }
}
void MobSystem::renderStates(std::vector<MobRenderState> &out) const
{
    out.clear();
    const double a = std::clamp(m_accumulator / settings.fixedStep, 0.0, 1.0);
    for (auto &m : m_mobs)
        out.push_back({m.species, glm::vec3(glm::mix(m.previous, m.body.position, a)),
                       float(m.previousYaw + angle(m.yaw - m.previousYaw) * a),
                       float(m.previousGait + (m.gait - m.previousGait) * a),
                       float(std::sin(m.age * 0.7 + double(m.id % 100)) * 0.25),
                       m.body.grounded ? 0.f : float(std::sin(m.age * 25) * 0.7 + 0.8),
                       float(m.previousStride + (m.stride - m.previousStride) * a)});
}
} // namespace entities
