#pragma once
#include <Entities/MobTypes.hpp>
#include <Physics/VoxelCollision.hpp>
#include <array>
#include <optional>
#include <vector>
#include <span>

namespace entities
{
struct SpeciesSettings
{
    glm::dvec3 size;
    double speed;
};
inline const std::array<SpeciesSettings, kMobSpeciesCount> speciesSettings{
    {{{0.9, 1.4, 0.9}, 1.1}, {{0.9, 0.9, 0.9}, 1.0}, {{0.9, 1.3, 0.9}, 1.0}, {{0.4, 0.7, 0.4}, 0.8}}};
static_assert(speciesSettings.size() == kMobSpeciesCount);
struct MobSettings
{
    static constexpr size_t capacity = kMaxMobCount;

    double spawnMin = 24.0;
    double spawnMax = 80.0;
    double retireDistance = 112.0;

    double spawnInterval = 0.5;  // seconds between population scans
    double spawnChance = 0.22;   // per chunk-column group candidacy
    uint32_t minGroupSize = 2;
    uint32_t maxGroupSize = 4;
    uint32_t maxGroupAttemptsPerScan = 4;

    double fixedStep = 1.0 / 60.0;
    uint32_t maxSteps = 8;
};
// Surface returns feet on grass in an eligible biome, or no candidate. Unknown
// terrain is never inferred from procedural height: actual voxels are required.
class MobWorld : public physics::VoxelCollisionWorld
{
  public:
    virtual std::optional<glm::dvec3> surface(int x, int z) const = 0;
};
struct Mob
{
    EntityId id{};
    SpawnGroupId origin{};
    uint64_t randomState{};
    MobSpecies species{};
    physics::Body body;
    glm::dvec3 previous{};
    double yaw{}, previousYaw{}, targetYaw{}, timer{}, gait{}, previousGait{}, age{};
    double stride{}, previousStride{}, shoreTimer{};
    bool walking{};
};
// One fixed-step controller, reusable with synthetic voxel worlds and no engine.
void tickMob(Mob &, const physics::VoxelCollisionWorld &, double dt, std::span<const glm::dvec3> neighbors,
             physics::QueryStats &);
class MobSystem
{
  public:
    MobSystem();
    void reset(uint64_t seed);
    void update(double dt, const MobWorld &world, glm::dvec3 observer, double availableRadius,
                bool suspended = false);
    void renderStates(std::vector<MobRenderState> &out) const;
    const std::vector<Mob> &mobs() const { return m_mobs; }
    uint64_t droppedSteps() const { return m_dropped; }
    const physics::QueryStats &queryStats() const { return m_queries; }
    MobSettings settings;
    // Also used by deterministic fixtures; rejects unknown, fluid or occupied bodies.
    bool add(MobSpecies species, glm::dvec3 feet, uint64_t id, uint64_t origin,
             const physics::VoxelCollisionWorld &world);

  private:
    struct Group
    {
        uint64_t key;
        glm::dvec2 center;
    };
    void populate(const MobWorld &, glm::dvec3 observer, double radius);
    void step(const MobWorld &);
    std::vector<Mob> m_mobs;
    std::vector<Group> m_groups;
    uint64_t m_seed{}, m_dropped{};
    double m_accumulator{}, m_spawnTimer{};
    physics::QueryStats m_queries{};
};
} // namespace entities
