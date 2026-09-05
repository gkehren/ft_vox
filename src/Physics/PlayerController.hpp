#pragma once
#include "VoxelCollision.hpp"

namespace physics
{
struct PlayerSettings
{
    double walkSpeed{4.5}, sprintSpeed{7.0};
    double acceleration{45.0}, braking{60.0}, airAcceleration{12.0};
    double gravity{24.0}, jumpSpeed{8.5}, terminalSpeed{50.0};
    double swimSpeed{2.5}, waterGravity{4.0}, waterDrag{4.0};
    double coyoteTime{0.1}, jumpBuffer{0.12}, eyeHeight{1.62};
};
struct PlayerInput
{
    glm::dvec2 move{0}; // world X/Z, length <= 1
    bool sprint{false}, jumpPressed{false}, swimUp{false}, swimDown{false};
};
struct PhysicsMetrics
{
    QueryStats queries{};
    unsigned steps{0};
    uint64_t droppedSteps{0};
};
class PlayerController
{
public:
    static constexpr double fixedStep = 1.0 / 120.0;
    static constexpr unsigned maxSteps = 8;
    Body body{};
    PlayerSettings settings{};
    PhysicsMetrics metrics{};
    Immersion submerged{};

    void reset(glm::dvec3 feet)
    {
        body.position = previous = feet;
        body.velocity = glm::dvec3(0);
        body.grounded = body.waitingForTerrain = false;
        suspend();
        submerged = {};
    }
    // Pause/focus transitions must never replay accumulated time or input.
    void suspend()
    {
        accumulator = bufferedJump = coyote = swimJump = 0;
        previous = body.position;
        metrics.steps = 0;
        metrics.queries = {};
    }
    void clearInput() { bufferedJump = 0; }
    glm::dvec3 eye() const { return body.position + glm::dvec3(0, settings.eyeHeight, 0); }
    glm::dvec3 renderEye() const
    {
        return glm::mix(previous, body.position, accumulator / fixedStep) +
               glm::dvec3(0, settings.eyeHeight, 0);
    }
    void advance(double dt, const PlayerInput &input, const VoxelCollisionWorld &world)
    {
        metrics.steps = 0;
        metrics.queries = {};
        if (!std::isfinite(dt) || dt < 0) return;
        if (input.jumpPressed) bufferedJump = settings.jumpBuffer;
        // No unbounded integer conversion even after a suspended process.
        accumulator += std::min(dt, 60.0);
        while (accumulator + 1e-12 >= fixedStep && metrics.steps < maxSteps)
        {
            previous = body.position;
            tick(input, world);
            accumulator = std::max(0.0, accumulator - fixedStep);
            ++metrics.steps;
        }
        if (accumulator + 1e-12 >= fixedStep)
        {
            const auto dropped = static_cast<uint64_t>((accumulator + 1e-12) / fixedStep);
            metrics.droppedSteps += dropped;
            accumulator = std::max(0.0, accumulator - dropped * fixedStep);
        }
    }
private:
    glm::dvec3 previous{};
    double accumulator{0}, bufferedJump{0}, coyote{0}, swimJump{0};
    void tick(const PlayerInput &input, const VoxelCollisionWorld &world)
    {
        body.waitingForTerrain = false;
        if (!recover(world, body, metrics.queries))
        {
            body.grounded = false;
            bufferedJump = coyote = 0;
            previous = body.position;
            return;
        }
        // Revalidate support each tick: deleting a floor must not leave a stale
        // grounded flag, but still permits the intentional coyote interval.
        const Hit support = sweep(world, body.bounds(), {0, -skin * 2, 0}, metrics.queries);
        body.grounded = body.velocity.y <= 0 && support.hit && support.solidGround;
        coyote = body.grounded ? settings.coyoteTime : std::max(0.0, coyote - fixedStep);
        submerged = immersion(world, body.bounds(), metrics.queries);
        const double wet = std::clamp(submerged.water + submerged.lava, 0.0, 1.0);
        const double liquidSpeed = settings.swimSpeed * (submerged.lava > 0 ? 0.5 : 1.0);
        glm::dvec2 direction = input.move;
        const double len = glm::length(direction);
        if (len > 1) direction /= len;
        const double drySpeed = input.sprint ? settings.sprintSpeed : settings.walkSpeed;
        const glm::dvec2 target = direction * glm::mix(drySpeed, liquidSpeed, wet);
        glm::dvec2 horizontal(body.velocity.x, body.velocity.z);
        const glm::dvec2 difference = target - horizontal;
        const double distance = glm::length(difference);
        double acceleration = body.grounded ? (len > 0 ? settings.acceleration : settings.braking)
                                            : settings.airAcceleration;
        acceleration = glm::mix(acceleration, settings.acceleration, wet);
        if (distance > 0)
            horizontal += difference * std::min(1.0, acceleration * fixedStep / distance);
        body.velocity.x = horizontal.x;
        body.velocity.z = horizontal.y;

        swimJump = std::max(0.0, swimJump - fixedStep);
        // Upward swimming settles with roughly 80% of the body immersed.
        // The surface-jump band must include that equilibrium, not require
        // a body already lifted unnaturally far out of the water.
        const bool surfaceJump = submerged.water > 0 && wet < 0.9;
        if (bufferedJump > 0 && (coyote > 0 || surfaceJump))
        {
            body.velocity.y = settings.jumpSpeed;
            body.grounded = false;
            coyote = bufferedJump = 0;
            if (surfaceJump) swimJump = 0.25;
        }
        else bufferedJump = std::max(0.0, bufferedJump - fixedStep);
        body.velocity.y -= glm::mix(settings.gravity, settings.waterGravity, wet) * fixedStep;
        if (wet > 0 && swimJump <= 0)
        {
            const double verticalTarget = (int(input.swimUp) - int(input.swimDown)) * liquidSpeed;
            body.velocity.y = glm::mix(body.velocity.y, verticalTarget,
                1.0 - std::exp(-settings.waterDrag * wet * fixedStep));
        }
        body.velocity.y = std::max(body.velocity.y, -settings.terminalSpeed);
        const auto recovered = body.position;
        move(world, body, body.velocity * fixedStep, metrics.queries);
        // Recovery is a discontinuity, not a path through solid geometry.
        if (recovered != previous) previous = body.position;
    }
};
} // namespace physics
