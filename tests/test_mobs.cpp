#include <Entities/MobSystem.hpp>
#include <Entities/MobModel.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>
#include <string_view>

static bool counting = false;
static size_t allocations = 0;
void *operator new(size_t n)
{
    if (counting)
        ++allocations;
    if (auto p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](size_t n)
{
    return ::operator new(n);
}
void operator delete(void *p) noexcept
{
    std::free(p);
}
void operator delete[](void *p) noexcept
{
    std::free(p);
}
void operator delete(void *p, size_t) noexcept
{
    std::free(p);
}
void operator delete[](void *p, size_t) noexcept
{
    std::free(p);
}
using namespace entities;
static int failures = 0;
#define CHECK(x)                                                                                             \
    do                                                                                                       \
    {                                                                                                        \
        if (!(x))                                                                                            \
        {                                                                                                    \
            std::cerr << "FAIL " << __LINE__ << ": " #x "\n";                                                \
            ++failures;                                                                                      \
        }                                                                                                    \
    } while (0)
class World : public MobWorld
{
  public:
    int floor = 0, wall = 10000, ceiling = 10000, unknown = 10000, ledge = 10000, step = 0, water = -10000;
    bool spawn = true, hole = false;
    physics::Cell sample(glm::ivec3 p) const override
    {
        if (p.x >= unknown)
            return {};
        if (p.x >= wall || p.y >= ceiling)
            return {true, true};
        int ground = floor + (p.x >= ledge ? step : 0);
        if (p.y < ground && !(hole && p.x >= ledge))
            return {true, true};
        if (p.y < water)
            return {true, false, physics::Medium::Water};
        return {true, false};
    }
    std::optional<glm::dvec3> surface(int x, int z) const override
    {
        if (!spawn || x >= unknown)
            return {};
        return glm::dvec3(x + 0.5, floor + 0.001, z + 0.5);
    }
};
static Mob walker(MobSpecies species = MobSpecies::Cow)
{
    Mob m{};
    m.id = 1;
    m.randomState = 123;
    m.species = species;
    m.body.size = speciesSettings[size_t(species)].size;
    m.body.position = {0, 0.001, 0};
    m.body.grounded = true;
    m.yaw = m.targetYaw = 1.5707963267948966;
    m.walking = true;
    m.timer = 1000;
    return m;
}
static void tick(Mob &m, const World &w, int n)
{
    physics::QueryStats q;
    for (int i = 0; i < n; ++i)
        tickMob(m, w, 1.0 / 60, {}, q);
}
static void controller()
{
    for (size_t species = 0; species < kMobSpeciesCount; ++species)
    {
        World w;
        auto m = walker(MobSpecies(species));
        tick(m, w, 120);
        CHECK(m.body.position.x > 1);
        CHECK(std::abs(m.body.position.y) < 0.01);
        CHECK(m.gait > 0);
        CHECK(m.body.grounded);
        w.wall = 3;
        tick(m, w, 600);
        CHECK(m.body.bounds().max.x <= 3.001);
        w = World{};
        w.ledge = 2;
        w.step = 1;
        m = walker(MobSpecies(species));
        double maxY = 0;
        for (int i = 0; i < 600; ++i)
        {
            tick(m, w, 1);
            maxY = std::max(maxY, m.body.position.y);
        }
        CHECK(maxY > 0.9);
        CHECK(m.body.position.x > 2.0);
        w = World{};
        w.ledge = 2;
        w.hole = true;
        m = walker(MobSpecies(species));
        tick(m, w, 600);
        CHECK(m.body.position.y > -0.01);
        CHECK(m.body.position.x < 2);
        w = World{};
        w.unknown = 2;
        m = walker(MobSpecies(species));
        tick(m, w, 300);
        CHECK(m.body.bounds().max.x <= 2.001);
        w = World{};
        m = walker(MobSpecies(species));
        w.floor = -3;
        tick(m, w, 180);
        CHECK(m.body.position.y < -2.9 && m.body.position.y > -3.01);
        w = World{};
        m = walker(MobSpecies(species));
        w.floor = -4;
        w.water = 0;
        tick(m, w, 480);
        CHECK(m.body.position.y > -2.0);
        CHECK(m.body.position.y < 0.1);
    }
    World w;
    auto m = walker();
    w.unknown = -1;
    const auto original = m.body.position;
    tick(m, w, 120);
    CHECK(m.body.position == original);
    CHECK(m.body.waitingForTerrain);
    // Low ceiling forbids stepping; it must not teleport through the obstruction.
    w = World{};
    w.ledge = 2;
    w.step = 1;
    w.ceiling = 2;
    m = walker();
    tick(m, w, 360);
    CHECK(m.body.bounds().max.x < 2.001);
}
static void population()
{
    World w;
    MobSystem a, b;
    a.reset(42);
    b.reset(42);
    for (int i = 0; i < 600; ++i)
    {
        a.update(1.0 / 60, w, {0, 0, 0}, 112);
        b.update(1.0 / 60, w, {0, 0, 0}, 112);
    }
    CHECK(!a.mobs().empty());
    CHECK(a.mobs().size() <= 48);
    CHECK(a.mobs().size() == b.mobs().size());
    std::set<uint64_t> ids;
    for (size_t i = 0; i < a.mobs().size(); ++i)
    {
        CHECK(ids.insert(a.mobs()[i].id).second);
        CHECK(a.mobs()[i].body.position == b.mobs()[i].body.position);
    }
    auto count = a.mobs().size();
    a.update(20, w, {0, 0, 0}, 112, true);
    CHECK(a.mobs().size() == count);
    CHECK(a.droppedSteps() == 0);
    a.update(1.0, w, {0, 0, 0}, 112);
    CHECK(a.droppedSteps() > 0);
    a.update(0, w, {1000, 0, 1000}, 0);
    CHECK(a.mobs().empty());
    a.reset(42);
    CHECK(a.mobs().empty());
    w.unknown = -10000;
    for (int i = 0; i < 60; ++i)
        a.update(1.0 / 60, w, {0, 0, 0}, 112);
    CHECK(a.mobs().empty());
    w.unknown = 10000;
    for (int i = 0; i < 120; ++i)
        a.update(1.0 / 60, w, {-80, 0, -80}, 112);
    CHECK(!a.mobs().empty());
    // No surface candidates means no forced spawn even in an empty loaded world.
    w.spawn = false;
    b.reset(42);
    for (int i = 0; i < 600; ++i)
        b.update(1.0 / 60, w, {0, 0, 0}, 112);
    CHECK(b.mobs().empty());
    // Long traversal keeps ownership bounded, including group retirement.
    w.spawn = true;
    for (int i = 0; i < 10000; ++i)
    {
        a.update(1.0 / 30, w, {i * 0.3, 0, i * 0.13}, 112);
        CHECK(a.mobs().size() <= 48);
    }
}
static void timing()
{
    World w;
    w.spawn = false;
    std::vector<glm::dvec3> final;
    for (int fps : {30, 60, 144, 240})
    {
        MobSystem s;
        s.reset(123);
        CHECK(s.add(MobSpecies::Pig, {0, 0.001, 0}, 1, 1, w));
        for (int i = 0; i < fps * 20; ++i)
            s.update(1.0 / fps, w, {0, 0, 0}, 112);
        final.push_back(s.mobs()[0].body.position);
        CHECK(s.droppedSteps() == 0);
    }
    for (auto p : final)
        CHECK(glm::length(p - final[0]) < 1e-7);
}
static void contracts()
{
    World w;
    MobSystem s;
    s.reset(42);
    // Out-of-range species is rejected before any state mutation.
    CHECK(!s.add(static_cast<MobSpecies>(255), {0.5, 0.001, 0.5}, 1, 1, w));
    CHECK(s.mobs().empty());
    // Duplicate ids are rejected and leave the population unchanged.
    CHECK(s.add(MobSpecies::Cow, {0.5, 0.001, 0.5}, 42, 42, w));
    CHECK(!s.add(MobSpecies::Pig, {8.5, 0.001, 0.5}, 42, 42, w));
    CHECK(s.mobs().size() == 1);
    // Capacity is a hard ceiling.
    for (size_t i = s.mobs().size(); i < MobSettings::capacity; ++i)
        CHECK(s.add(MobSpecies(i % kMobSpeciesCount), {8.0 + (i % 8) * 4.0, 0.001, 8.0 + (i / 8) * 4.0},
                    100 + i, 100 + i, w));
    CHECK(s.mobs().size() == MobSettings::capacity);
    CHECK(!s.add(MobSpecies::Chicken, {-4.5, 0.001, 0.5}, 999, 999, w));
    CHECK(s.mobs().size() == MobSettings::capacity);
    // Reset clears population and simulation bookkeeping; ids become reusable.
    s.update(1.0 / 60, w, {16, 0, 12}, 112);
    s.reset(7);
    CHECK(s.mobs().empty());
    CHECK(s.droppedSteps() == 0);
    CHECK(s.add(MobSpecies::Cow, {0.5, 0.001, 0.5}, 42, 42, w));
    CHECK(s.mobs().size() == 1);
}
static void finiteSteering()
{
    // Two neighbors dead ahead at 0.25 blocks contribute separation vectors
    // that cancel the walk direction exactly (yaw 0 = exact -Z). While
    // airborne the obstacle probes are skipped, so a normalized zero would
    // flow straight into velocity/position; steering must fall back to yaw.
    World w;
    auto m = walker();
    m.yaw = m.targetYaw = 0;
    m.body.grounded = false;
    m.body.position = {0, 2.0, 0};
    const std::array<glm::dvec3, 2> neighbors{glm::dvec3{0, 2.0, -0.25}, glm::dvec3{0, 2.0, -0.25}};
    physics::QueryStats q;
    for (int i = 0; i < 120; ++i)
        tickMob(m, w, 1.0 / 60, neighbors, q);
    CHECK(std::isfinite(m.body.position.x));
    CHECK(std::isfinite(m.body.position.y));
    CHECK(std::isfinite(m.body.position.z));
    CHECK(std::isfinite(m.body.velocity.x));
    CHECK(std::isfinite(m.body.velocity.y));
    CHECK(std::isfinite(m.body.velocity.z));
    CHECK(std::isfinite(m.yaw));
    CHECK(std::isfinite(m.targetYaw));
    // 48 densely packed mobs stay finite over a long run.
    MobSystem s;
    s.reset(7);
    for (size_t i = 0; i < MobSettings::capacity; ++i)
        CHECK(s.add(MobSpecies(i % kMobSpeciesCount), {(i % 8) * 1.5, 0.001, (i / 8) * 1.5}, i + 1, i + 1, w));
    for (int i = 0; i < 1000; ++i)
    {
        s.update(1.0 / 60, w, {5.0, 0, 4.0}, 112);
        for (auto &mob : s.mobs())
        {
            CHECK(std::isfinite(mob.body.position.x));
            CHECK(std::isfinite(mob.body.position.y));
            CHECK(std::isfinite(mob.body.position.z));
            CHECK(std::isfinite(mob.yaw));
            CHECK(std::isfinite(mob.targetYaw));
        }
    }
}
static void models()
{
    MobModels models;
    for (auto &model : models.models)
        CHECK(model.parts.size() >= 6);
    for (auto &v : models.vertices)
    {
        CHECK(std::isfinite(v.position.x));
        CHECK(std::abs(glm::length(v.normal) - 1) < 0.001);
        CHECK(v.uv.x >= 0 && v.uv.x <= 1 && v.uv.y >= 0 && v.uv.y <= 1);
    }
    MobRenderState state{};
    state.position = {10, 20, 30};
    for (auto &model : models.models)
        for (auto &part : model.parts)
        {
            auto matrix = mobPartTransform(state, part);
            CHECK(std::isfinite(matrix[3].y));
            CHECK(part.firstVertex + part.vertexCount <= models.vertices.size());
        }
}
static void profile()
{
    World w;
    w.spawn = false;
    MobSystem s;
    s.reset(42);
    for (int i = 0; i < 48; ++i)
        CHECK(s.add(MobSpecies(i % kMobSpeciesCount), {(i % 8) * 4.0, 0.001, (i / 8) * 4.0}, i + 1, i + 1, w));
    for (int i = 0; i < 600; ++i)
        s.update(1.0 / 60, w, {16, 0, 12}, 112);
    std::vector<MobRenderState> states;
    states.reserve(48);
    std::vector<double> times;
    times.reserve(6000);
    allocations = 0;
    uint64_t cells = 0;
    for (int i = 0; i < 6000; ++i)
    {
        auto start = std::chrono::steady_clock::now();
        counting = true;
        s.update(1.0 / 60, w, {16, 0, 12}, 112);
        s.renderStates(states);
        counting = false;
        cells += s.queryStats().cells;
        times.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    CHECK(allocations == 0);
    CHECK(s.mobs().size() == 48);
    double sum = 0;
    for (auto t : times)
        sum += t;
    std::sort(times.begin(), times.end());
    std::cout << "48 mobs CPU mean_ms=" << sum / times.size() << " p95_ms=" << times[times.size() * 95 / 100]
              << " cells/tick=" << cells / times.size() << " allocations=" << allocations
              << " dropped=" << s.droppedSteps() << "\n";
}
int main(int argc, char **argv)
{
    if (argc > 1 && std::string_view(argv[1]) == "--trace")
    {
        World w;
        w.ledge = 2;
        w.step = 1;
        auto m = walker();
        for (int i = 0; i < 300; ++i)
        {
            tick(m, w, 1);
            if (i % 10 == 0)
                std::cout << i << " pos " << m.body.position.x << "," << m.body.position.y << ","
                          << m.body.position.z << " yaw " << m.yaw << " grounded " << m.body.grounded
                          << " vy " << m.body.velocity.y << "\n";
        }
        return 0;
    }
    controller();
    contracts();
    finiteSteering();
    population();
    timing();
    models();
    if (argc > 1 && std::string_view(argv[1]) == "--profile")
        profile();
    std::cout << "Passive mobs: " << failures << " failures\n";
    return failures ? 1 : 0;
}
