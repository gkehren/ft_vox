#include <Physics/PlayerController.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>
#include <new>
#include <random>

// Count heap traffic only inside simulation calls, excluding the test world's
// construction, output and collection of timing samples.
static bool countAllocations = false;
static size_t allocations = 0;
void *operator new(std::size_t n)
{
    if (countAllocations) ++allocations;
    if (void *p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

using namespace physics;
static int failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::cerr << "FAIL line " << __LINE__ << ": " #expr "\n"; ++failures; } } while (0)

class World final : public VoxelCollisionWorld
{
public:
    int floorY{0}, ceilingY{1000}, wallX{1000}, wallZ{1000}, unknownX{1000};
    int waterY{-1000};
    bool hole{false}, lava{false};
    std::array<glm::ivec3, 8> blocks{};
    unsigned blockCount{0};
    Cell sample(glm::ivec3 p) const override
    {
        if (p.x >= unknownX) return {};
        if ((p.y < floorY && !(hole && p.x >= 1)) || p.y >= ceilingY || p.x >= wallX || p.z >= wallZ)
            return {true, true};
        for (unsigned i = 0; i < blockCount; ++i)
            if (p == blocks[i]) return {true, true};
        if (p.y < waterY) return {true, false, lava ? Medium::Lava : Medium::Water};
        return {true, false};
    }
};
static void ticks(PlayerController &p, const World &w, int n, PlayerInput input = {})
{
    for (int i = 0; i < n; ++i)
    {
        p.advance(PlayerController::fixedStep, input, w);
        input.jumpPressed = false;
    }
}
static void collisions()
{
    World w;
    PlayerController p;
    p.reset({0.5, 60, 0.5});
    ticks(p, w, 500);
    CHECK(std::abs(p.body.position.y - skin) < 1e-8);
    CHECK(p.body.grounded);
    const auto rest = p.body.position;
    ticks(p, w, 500);
    CHECK(glm::length(p.body.position - rest) < 1e-8);

    // High-speed sweeps must hit a single voxel even when crossing many cells.
    Body body;
    QueryStats stats;
    w.blocks[0] = {2, 1, 0}; w.blockCount = 1;
    body.position = {-20, 1, 0.5};
    body.velocity = {500, 0, 0};
    move(w, body, {40, 0, 0}, stats);
    CHECK(body.bounds().max.x <= 2.0);
    CHECK(body.position.x > 1.69);
    CHECK(body.velocity.x == 0);

    // Slide against a wall across numerous coplanar seams in both directions.
    w.blockCount = 0; w.wallX = 2;
    p.reset({1.699, skin, -16.5});
    PlayerInput diagonal; diagonal.move = {1, 1};
    ticks(p, w, 600, diagonal);
    CHECK(p.body.position.x <= 1.7);
    CHECK(p.body.position.z > -2);
    CHECK(p.body.grounded);
    diagonal.move = {1, -1}; ticks(p, w, 600, diagonal);
    CHECK(p.body.position.z < -15);

    // Simultaneous corner normals stop both axes without order dependence.
    w.wallZ = 2;
    body.position = {0, 1, 0}; body.velocity = {10, 0, 10};
    move(w, body, {5, 0, 5}, stats);
    CHECK(body.bounds().max.x <= 2 && body.bounds().max.z <= 2);
    CHECK(body.velocity.x == 0 && body.velocity.z == 0);

    w = World{}; w.ceilingY = 3;
    p.reset({0.5, skin, 0.5}); ticks(p, w, 3);
    PlayerInput jump; jump.jumpPressed = true;
    double top = 0;
    for (int i = 0; i < 120; ++i)
    {
        ticks(p, w, 1, jump); jump.jumpPressed = false;
        top = std::max(top, p.body.bounds().max.y);
    }
    CHECK(top <= 3);
    CHECK(p.body.grounded);
    // Bodies of another size use the same solver (future mobs).
    body.size = {1.2, 2.5, 1.2}; body.position = {0, 10, 0};
    w.ceilingY = 1000;
    move(w, body, {0, -50, 0}, stats);
    CHECK(body.grounded && body.position.y >= 0);
}
static void movement()
{
    World w;
    PlayerController p;
    p.reset({0, skin, 0});
    PlayerInput input; input.move = {1, 1};
    ticks(p, w, 120, input);
    CHECK(std::abs(glm::length(p.body.velocity) - 4.5) < 1e-6);
    input.sprint = true; ticks(p, w, 120, input);
    CHECK(std::abs(glm::length(p.body.velocity) - 7) < 1e-6);
    ticks(p, w, 20);
    CHECK(glm::length(p.body.velocity) < 1e-9);

    p.reset({0, skin, 0}); ticks(p, w, 2);
    input = {}; input.jumpPressed = input.swimUp = true;
    double highest = 0;
    for (int i = 0; i < 200; ++i)
    {
        ticks(p, w, 1, input); input.jumpPressed = false;
        highest = std::max(highest, p.body.position.y);
    }
    CHECK(highest > 1.4 && highest < 1.6);
    CHECK(p.body.grounded); // holding Space did not repeat the jump

    // Coyote after deleting support, not after a genuine jump.
    w.floorY = 0; p.reset({0, skin, 0}); ticks(p, w, 2);
    w.floorY = -100; ticks(p, w, 6);
    input = {}; input.jumpPressed = true; ticks(p, w, 1, input);
    CHECK(p.body.velocity.y > 8);
    ticks(p, w, 2, input);
    CHECK(p.body.velocity.y < 8); // no second impulse

    // Buffered pre-landing press is consumed on the next supported tick.
    w.floorY = 0; p.reset({0, 0.12, 0}); p.body.velocity.y = -4;
    ticks(p, w, 10, input);
    CHECK(p.body.position.y > 0.2 && p.body.velocity.y > 0);

    // A full block is an obstacle: no automatic step-up.
    w.blocks[0] = {1, 0, 0}; w.blockCount = 1;
    p.reset({0, skin, 0.5}); input = {}; input.move = {1, 0};
    ticks(p, w, 120, input);
    CHECK(p.body.bounds().max.x <= 1);
    input.jumpPressed = true; ticks(p, w, 80, input);
    CHECK(p.body.position.x > 2);
}
static void streamingAndRecovery()
{
    World w; w.unknownX = 2;
    PlayerController p; p.reset({0, skin, 0});
    PlayerInput input; input.move = {1, 0};
    ticks(p, w, 120, input);
    CHECK(p.body.position.x <= 1.7 && p.body.waitingForTerrain);
    w.unknownX = 1000; ticks(p, w, 120, input);
    CHECK(p.body.position.x > 3);
    w.unknownX = -1000;
    const auto saved = p.body.position;
    ticks(p, w, 120, input);
    CHECK(p.body.position == saved);
    CHECK(!p.body.grounded && p.body.waitingForTerrain);
    CHECK(glm::length(p.body.velocity) == 0);

    // Unknown support is not a jumpable floor.
    class UnknownBelow final : public VoxelCollisionWorld
    { public: Cell sample(glm::ivec3 p) const override { return p.y < 0 ? Cell{} : Cell{true, false}; } } below;
    p.reset({0, skin, 0}); p.advance(0.05, {}, below);
    CHECK(!p.body.grounded);
    input = {}; input.jumpPressed = true; p.advance(0.05, input, below);
    CHECK(p.body.position.y < 0.01);

    // Small overlap recovery is bounded and never leaves a body embedded.
    w = World{}; p.reset({0, -0.1, 0}); ticks(p, w, 1);
    QueryStats stats;
    CHECK(clear(w, p.body.bounds(), stats));
    p.reset({0, -20, 0}); const auto deep = p.body.position; ticks(p, w, 1);
    CHECK(p.body.position == deep);

    // Recovery may not exceed its bound even when the final correction clears
    // a multi-block intersection. Test success/failure invariants on a corpus.
    std::mt19937 random(42);
    for (int i = 0; i < 1000; ++i)
    {
        w = World{}; w.floorY = -100; w.blockCount = 8;
        for (auto &block : w.blocks)
            block = {int(random() % 3) - 1, int(random() % 3), int(random() % 3) - 1};
        Body body; body.position = {0.35, 0.25, 0.65};
        const auto original = body.position;
        const bool resolved = recover(w, body, stats);
        CHECK(glm::length(body.position - original) <= 1.0 + 1e-10);
        CHECK(resolved ? clear(w, body.bounds(), stats) : body.position == original);
    }
}
static void water()
{
    World w; w.waterY = 5;
    PlayerController p; p.reset({0, 1, 0});
    PlayerInput input; input.move = {1, 0}; input.swimUp = true;
    ticks(p, w, 120, input);
    CHECK(p.body.position.y > 1.5);
    CHECK(p.body.velocity.x <= 2.5 + 1e-6);
    CHECK(p.submerged.water > 0);
    input.swimUp = false; input.swimDown = true; ticks(p, w, 120, input);
    CHECK(p.body.position.y < 1.5);
    w.lava = true; p.reset({0, 1, 0}); input = {}; input.move = {1, 0};
    ticks(p, w, 80, input);
    CHECK(p.submerged.lava > 0 && p.body.velocity.x <= 1.25 + 1e-6);

    // A distinct press near the surface allows a manual jump onto the bank.
    w = World{}; w.waterY = 3; w.blocks[0] = {1, 2, 0}; w.blockCount = 1;
    p.reset({0.5, 2, 0.5});
    input = {}; input.move = {1, 0}; input.jumpPressed = input.swimUp = true;
    double highest = 0;
    for (int i = 0; i < 120; ++i)
    {
        ticks(p, w, 1, input); input.jumpPressed = false;
        highest = std::max(highest, p.body.position.y);
    }
    CHECK(highest > 3 && p.body.position.x > 1.5);

    w = World{}; w.waterY = 5;
    p.reset({0, 1, 0}); input = {}; input.swimUp = true;
    ticks(p, w, 1200, input);
    CHECK(p.submerged.water > 0.65 && p.submerged.water < 0.9);
    input.jumpPressed = true; ticks(p, w, 1, input);
    CHECK(p.body.velocity.y > 8); // jump from naturally reached swim equilibrium
    input.jumpPressed = false;
    highest = p.body.position.y;
    for (int i = 0; i < 120; ++i)
    {
        ticks(p, w, 1, input);
        highest = std::max(highest, p.body.position.y);
    }
    CHECK(highest > 5);
}
static void cadence()
{
    World w;
    glm::dvec3 expected;
    for (int fps : {30, 60, 144, 240})
    {
        PlayerController p; p.reset({-16.5, skin, -0.5});
        PlayerInput input; input.move = {1, 0};
        for (int i = 0; i < fps * 2; ++i) p.advance(1.0 / fps, input, w);
        if (fps == 30) expected = p.body.position;
        CHECK(glm::length(p.body.position - expected) < 1e-8);
        CHECK(p.metrics.droppedSteps == 0);
        const auto eye = p.renderEye();
        CHECK(eye.x <= p.eye().x && p.eye().x - eye.x <= 4.5 / 120 + 1e-8);
    }
    PlayerController p; p.reset({0, skin, 0});
    p.advance(0.503, {}, w);
    CHECK(p.metrics.steps == 8 && p.metrics.droppedSteps == 52);
    p.suspend(); const auto pos = p.body.position;
    p.advance(0.001, {}, w);
    CHECK(p.metrics.steps == 0 && p.body.position == pos);
    PlayerInput jump; jump.jumpPressed = true;
    p.advance(0.001, jump, w); p.clearInput(); p.advance(0.02, {}, w);
    CHECK(p.body.velocity.y <= 0);
    p.reset({0, skin, 0}); p.advance(std::numeric_limits<double>::quiet_NaN(), {}, w);
    CHECK(p.metrics.steps == 0);

    // Identical timed jump commands under four render cadences.
    for (int fps : {30, 60, 144, 240})
    {
        p.reset({0, skin, 0});
        PlayerInput input; input.move = {0.6, 0.8};
        for (int frame = 0; frame < fps; ++frame)
        {
            input.jumpPressed = frame == fps / 2;
            p.advance(1.0 / fps, input, w);
        }
        if (fps == 30) expected = p.body.position;
        CHECK(glm::length(p.body.position - expected) < 1e-8);
    }

    p.reset({0, 8, 0});
    countAllocations = true;
    for (int i = 0; i < 2400; ++i)
    {
        PlayerInput input; input.move = {1, 0.3}; input.jumpPressed = i % 120 == 0;
        p.advance(PlayerController::fixedStep, input, w);
    }
    countAllocations = false;
    CHECK(allocations == 0);
}
static void profile()
{
    World w; w.wallX = 20; w.waterY = 2;
    PlayerController p;
    std::vector<double> timings; timings.reserve(12000);
    uint64_t cells = 0;
    for (int i = 0; i < 12000; ++i)
    {
        if (i % 600 == 0) p.reset({-10, 6, 0});
        PlayerInput input; input.move = {1, (i / 300) % 2 ? -0.5 : 0.5};
        input.jumpPressed = i % 80 == 0; input.swimUp = i % 120 < 80;
        const auto start = std::chrono::steady_clock::now();
        p.advance(PlayerController::fixedStep, input, w);
        timings.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        cells += p.metrics.queries.cells;
    }
    double sum = 0; for (double t : timings) sum += t;
    std::sort(timings.begin(), timings.end());
    std::cout << "Physics synthetic 12000 ticks: avg=" << sum / timings.size()
              << " ms p95=" << timings[timings.size() * 95 / 100]
              << " ms cells/tick=" << double(cells) / timings.size()
              << " dropped=" << p.metrics.droppedSteps << '\n';
}
int main(int argc, char **argv)
{
    collisions(); movement(); streamingAndRecovery(); water(); cadence();
    if (argc > 1 && std::string_view(argv[1]) == "--profile") profile();
    if (!failures) std::cout << "PASS: voxel physics and player controller\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
