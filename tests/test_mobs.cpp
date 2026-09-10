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
    // Valid boundary settings stay bounded and NaN-free (solo groups, every
    // chunk-column eligible, single attempt per scan).
    MobSystem tight;
    tight.reset(42);
    tight.settings.minGroupSize = 1;
    tight.settings.maxGroupSize = 1;
    tight.settings.spawnChance = 1.0;
    tight.settings.maxGroupAttemptsPerScan = 1;
    for (int i = 0; i < 600; ++i)
    {
        tight.update(1.0 / 60, w, {0, 0, 0}, 112);
        CHECK(tight.mobs().size() <= MobSettings::capacity);
        for (auto &mob : tight.mobs())
            CHECK(std::isfinite(mob.body.position.x) && std::isfinite(mob.body.position.z));
    }
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
static void animationTimeline()
{
    // Cosmetic animation time follows the same previous/current snapshot
    // contract as pose channels (issue #129).
    World w;
    auto m = walker();
    physics::QueryStats q;
    tickMob(m, w, 1.0 / 60, {}, q);
    CHECK(m.previousAge == 0);
    CHECK(std::abs(m.age - 1.0 / 60) < 1e-12);
    tickMob(m, w, 1.0 / 60, {}, q);
    CHECK(std::abs(m.previousAge - 1.0 / 60) < 1e-12);
    CHECK(std::abs(m.age - 2.0 / 60) < 1e-12);
}
static void subTickCosmetics()
{
    // At a 240 Hz render cadence the idle look phase must advance on
    // sub-tick frames instead of holding one value per 60 Hz tick.
    World w;
    w.spawn = false;
    MobSystem s;
    s.reset(7);
    CHECK(s.add(MobSpecies::Cow, {0.5, 0.001, 0.5}, 1, 1, w));
    std::vector<MobRenderState> states;
    for (int i = 0; i < 8; ++i) // two completed fixed ticks
        s.update(1.0 / 240, w, {0, 0, 0}, 112);
    int changes = 0;
    float last = 0;
    for (int i = 0; i < 16; ++i) // four fixed ticks of sub-tick samples
    {
        s.update(1.0 / 240, w, {0, 0, 0}, 112);
        s.renderStates(states);
        CHECK(states.size() == 1);
        CHECK(std::isfinite(states[0].look));
        CHECK(std::abs(states[0].look) <= 0.25f + 1e-6f);
        if (i && states[0].look != last)
            ++changes;
        last = states[0].look;
    }
    // Quantized sampling would repeat each tick's phase four times.
    CHECK(changes >= 12);
}
static void chickenFlap()
{
    World w;
    w.spawn = false;
    MobSystem s;
    s.reset(7);
    CHECK(s.add(MobSpecies::Chicken, {0.5, 0.001, 0.5}, 1, 1, w));
    std::vector<MobRenderState> states;
    // Grounded chickens never flap, whatever the render phase.
    for (int i = 0; i < 12; ++i)
    {
        s.update(1.0 / 240, w, {0, 0, 0}, 112);
        s.renderStates(states);
        CHECK(s.mobs()[0].body.grounded);
        CHECK(states[0].flap == 0.0f);
    }
    // Remove every solid cell: the chicken falls and stays airborne, so the
    // wing-flap phase is exposed at sub-tick cadence.
    w.ledge = -10000;
    w.hole = true;
    for (int i = 0; i < 4; ++i) // one fixed tick to leave the ground
        s.update(1.0 / 240, w, {0, 0, 0}, 112);
    int changes = 0;
    float last = 0;
    for (int i = 0; i < 16; ++i)
    {
        s.update(1.0 / 240, w, {0, 0, 0}, 112);
        s.renderStates(states);
        CHECK(!s.mobs()[0].body.grounded);
        CHECK(std::isfinite(states[0].flap));
        CHECK(states[0].flap > 0.099f && states[0].flap < 1.501f);
        if (i && states[0].flap != last)
            ++changes;
        last = states[0].flap;
    }
    CHECK(changes >= 12);
}
static void poseTimeAlignment()
{
    // The cosmetic phase must sample lerp(previousAge, age, alpha), not the
    // current tick's age and not age + alpha * fixedStep.
    World w;
    w.spawn = false;
    std::vector<MobRenderState> states;
    for (double alpha : {0.0, 0.25, 0.5, 0.75, 0.999})
    {
        MobSystem s;
        s.reset(7);
        CHECK(s.add(MobSpecies::Cow, {0.5, 0.001, 0.5}, 1, 1, w));
        for (int i = 0; i < 5; ++i) // complete fixed ticks leave alpha at 0
            s.update(s.settings.fixedStep, w, {0, 0, 0}, 112);
        s.update(alpha * s.settings.fixedStep, w, {0, 0, 0}, 112);
        s.renderStates(states);
        const auto &m = s.mobs()[0];
        const double renderAge = m.previousAge + (m.age - m.previousAge) * alpha;
        const float expected = float(std::sin(renderAge * 0.7 + double(m.id % 100)) * 0.25);
        CHECK(std::abs(states[0].look - expected) < 1e-6);
    }
}
static void renderCadenceInvariance()
{
    // The same simulated interval must land on the same cosmetic phase for
    // any render cadence: frame rate changes sampling density, not the
    // animation timeline.
    World w;
    w.spawn = false;
    std::vector<MobRenderState> states;
    std::vector<float> looks;
    for (int fps : {30, 60, 120, 144, 240})
    {
        MobSystem s;
        s.reset(123);
        CHECK(s.add(MobSpecies::Cow, {0.5, 0.001, 0.5}, 1, 1, w));
        for (int i = 0; i < fps * 12; ++i) // 12 s = 720 exact fixed ticks
            s.update(1.0 / fps, w, {0, 0, 0}, 112);
        CHECK(s.droppedSteps() == 0);
        s.renderStates(states);
        CHECK(std::abs(s.mobs()[0].age - 12.0) < 1e-9);
        looks.push_back(states[0].look);
    }
    for (auto f : looks)
        CHECK(std::abs(f - looks[0]) < 1e-6);
}
static void suspensionResume()
{
    World w;
    w.spawn = false;
    MobSystem s;
    s.reset(7);
    CHECK(s.add(MobSpecies::Cow, {0.5, 0.001, 0.5}, 1, 1, w));
    for (int i = 0; i < 30; ++i)
        s.update(s.settings.fixedStep, w, {0, 0, 0}, 112);
    s.update(1.0 / 240, w, {0, 0, 0}, 112, true); // collapse interpolation
    std::vector<MobRenderState> states;
    s.renderStates(states);
    const float frozenLook = states[0].look;
    const double frozenAge = s.mobs()[0].age;
    for (int i = 0; i < 240; ++i) // one suspended second at 240 Hz
    {
        s.update(1.0 / 240, w, {0, 0, 0}, 112, true);
        s.renderStates(states);
        CHECK(states[0].look == frozenLook);
        CHECK(states[0].flap == 0.0f);
        CHECK(s.mobs()[0].age == frozenAge);
        CHECK(s.mobs()[0].previousAge == frozenAge);
    }
    // Resume: the phase continues from the frozen timeline with no
    // wall-clock-sized jump.
    s.update(1.0 / 240, w, {0, 0, 0}, 112);
    s.renderStates(states);
    CHECK(std::abs(states[0].look - frozenLook) < 1e-6);
    float advanced = states[0].look;
    for (int i = 0; i < 8; ++i)
    {
        s.update(1.0 / 240, w, {0, 0, 0}, 112);
        s.renderStates(states);
        advanced = states[0].look;
    }
    CHECK(advanced != frozenLook);
}
static void droppedStepPhase()
{
    // A stall longer than maxSteps drops whole ticks; cosmetic phase must
    // follow the retained simulation timeline, not the dropped wall clock.
    World w;
    w.spawn = false;
    MobSystem s;
    s.reset(7);
    CHECK(s.add(MobSpecies::Cow, {0.5, 0.001, 0.5}, 1, 1, w));
    s.update(0.2, w, {0, 0, 0}, 112); // requests 12 ticks, maxSteps=8
    CHECK(s.droppedSteps() == 4);
    std::vector<MobRenderState> states;
    s.renderStates(states);
    const auto &m = s.mobs()[0];
    CHECK(std::isfinite(states[0].look) && std::isfinite(states[0].flap));
    // Only the eight retained ticks advanced; the dropped four never reach
    // the animation clock.
    CHECK(std::abs(m.age - 8.0 / 60) < 1e-9);
    // The drained accumulator renders at alpha ~ 0, i.e. at previousAge.
    const float expected = float(std::sin(m.previousAge * 0.7 + double(m.id % 100)) * 0.25);
    CHECK(std::abs(states[0].look - expected) < 1e-6);
    for (int i = 0; i < 8; ++i) // normal frames resume smooth interpolation
    {
        s.update(1.0 / 240, w, {0, 0, 0}, 112);
        s.renderStates(states);
        CHECK(std::isfinite(states[0].look));
    }
}
static void renderHandoffReadOnly()
{
    World w;
    w.spawn = false;
    MobSystem s;
    s.reset(7);
    for (size_t i = 0; i < 4; ++i)
        CHECK(s.add(MobSpecies(i), {0.5 + i * 2.0, 0.001, 0.5}, i + 1, i + 1, w));
    for (int i = 0; i < 30; ++i)
        s.update(1.0 / 60, w, {2, 0, 0}, 112);
    std::vector<MobRenderState> a, b;
    s.renderStates(a);
    s.renderStates(b);
    CHECK(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i)
    {
        CHECK(a[i].look == b[i].look);
        CHECK(a[i].flap == b[i].flap);
        CHECK(a[i].position == b[i].position);
    }
    // Rendering consumes simulation state without mutating it.
    std::vector<double> before;
    for (auto &m : s.mobs())
        before.insert(before.end(),
                      {m.age, m.previousAge, m.randomState * 1.0, m.body.position.x, m.body.position.y,
                       m.body.position.z, m.yaw, m.gait, m.stride});
    s.renderStates(a);
    size_t k = 0;
    for (auto &m : s.mobs())
    {
        CHECK(m.age == before[k++]);
        CHECK(m.previousAge == before[k++]);
        CHECK(double(m.randomState) == before[k++]);
        CHECK(m.body.position.x == before[k++]);
        CHECK(m.body.position.y == before[k++]);
        CHECK(m.body.position.z == before[k++]);
        CHECK(m.yaw == before[k++]);
        CHECK(m.gait == before[k++]);
        CHECK(m.stride == before[k++]);
    }
    // Steady-state rendering does not allocate.
    counting = true;
    for (int i = 0; i < 100; ++i)
        s.renderStates(a);
    counting = false;
    CHECK(allocations == 0);
}
static void longRunPrecision()
{
    // One simulated hour: animation channels stay finite and bounded.
    World w;
    w.spawn = false;
    MobSystem s;
    s.reset(7);
    s.settings.maxSteps = 1000000;
    CHECK(s.add(MobSpecies::Chicken, {0.5, 0.001, 0.5}, 1, 1, w));
    w.ledge = -10000; // fall forever: flap phase active the whole run
    w.hole = true;
    s.update(3600, w, {0, 0, 0}, 112);
    std::vector<MobRenderState> states;
    s.renderStates(states);
    CHECK(std::isfinite(s.mobs()[0].age));
    CHECK(std::isfinite(states[0].look));
    CHECK(std::isfinite(states[0].flap));
    CHECK(std::abs(states[0].look) <= 0.25f + 1e-6f);
    CHECK(states[0].flap > 0.099f && states[0].flap < 1.501f);
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
    animationTimeline();
    subTickCosmetics();
    chickenFlap();
    poseTimeAlignment();
    renderCadenceInvariance();
    suspensionResume();
    droppedStepPhase();
    renderHandoffReadOnly();
    longRunPrecision();
    finiteSteering();
    population();
    timing();
    models();
    if (argc > 1 && std::string_view(argv[1]) == "--profile")
        profile();
    std::cout << "Passive mobs: " << failures << " failures\n";
    return failures ? 1 : 0;
}
