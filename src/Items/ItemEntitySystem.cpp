// ItemEntitySystem — dropped item entities (issue #226, Epic #223 Phase C).
// See ItemEntitySystem.hpp for the contract overview. The fixed-step /
// suspension discipline mirrors MobSystem::update; movement reuses the
// shared swept-collision primitive (physics::move) — no new solver.

#include <Items/ItemEntitySystem.hpp>
#include <Items/ItemStack.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>

namespace items
{
namespace
{
// splitmix64 finalizer — the same deterministic mixer MobSystem uses for
// per-entity randomness. Spawn jitter derives ONLY from stable inputs
// (entity id, edit revision): no RNG state, no wall clock (epic #223 §2/§6).
uint64_t mix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
double random01(uint64_t &state)
{
    state = mix(state);
    return double(state >> 11) * 0x1.0p-53;
}
// Only invariants the fixed-step loop and pickup arithmetic rely on.
void validateSettings([[maybe_unused]] const ItemEntitySettings &s)
{
    assert(std::isfinite(s.fixedStep) && s.fixedStep > 0.0);
    assert(s.maxSteps > 0);
    assert(std::isfinite(s.gravity) && std::isfinite(s.terminalVelocity));
    assert(std::isfinite(s.pickupRadius) && s.pickupRadius >= 0.0);
    assert(std::isfinite(s.groundFriction) && s.groundFriction >= 0.0 && s.groundFriction <= 1.0);
}
} // namespace

ItemEntitySystem::ItemEntitySystem()
{
    m_entities.reserve(ItemEntitySettings::capacity);
}

void ItemEntitySystem::reset()
{
    m_entities.clear();
    m_events.clear();
    m_nextId = 1;
    m_dropped = 0;
    m_droppedEvents = 0;
    m_accumulator = 0;
    m_queries = {};
}

void ItemEntitySystem::restore(uint64_t nextEntityId, const std::vector<ItemEntity> &entities)
{
    // The save layer validated ids/counts/transforms (decodeItemsState);
    // debug-assert the invariants the tick rules rely on. nextEntityId ==
    // kMaxNextEntityId is the VALID exhausted state (stabilization PR3): the
    // allocator resumes refused, never wrapping; every uint64 >= 1 is <= it.
    assert(nextEntityId >= 1 && nextEntityId <= kMaxNextEntityId);
    assert(entities.size() <= ItemEntitySettings::capacity);
    uint64_t prevId = 0;
    for (const ItemEntity &e : entities)
    {
        assert(e.id > prevId && e.id < nextEntityId);
        prevId = e.id;
    }
    m_entities = entities;
    for (ItemEntity &e : m_entities)
        e.previous = e.body.position; // no interpolation jump on frame one
    m_nextId = nextEntityId;
    m_events.clear();
    m_dropped = 0;
    m_droppedEvents = 0;
    m_accumulator = 0;
    m_queries = {};
}

// ---------------------------------------------------------------------------
// Spawn seams
// ---------------------------------------------------------------------------

EntityId ItemEntitySystem::spawn(ItemStack stack, glm::dvec3 position, glm::dvec3 velocity,
                                 uint32_t pickupDelayTicks)
{
    if (stack.isEmpty())
        return 0;
    // Id-exhaustion guard (stabilization PR3): refuse BEFORE the overflow
    // despawn — a spawn that cannot assign an id must not destroy an existing
    // entity either. The allocator never wraps (kMaxNextEntityId bound) and
    // id 0 stays permanently invalid.
    if (idsExhausted())
        return 0;
    // Bounded population: deterministic oldest-first overflow despawn BEFORE
    // the new entity takes the slot (the Removed event precedes the Spawned).
    if (m_entities.size() >= ItemEntitySettings::capacity)
        despawnOldest();
    ItemEntity e{};
    e.id = m_nextId++;
    e.stack = stack;
    e.body.position = position;
    e.body.velocity = velocity;
    e.body.size = settings.bodySize;
    e.previous = position;
    e.pickupDelay = pickupDelayTicks;
    m_entities.push_back(e); // monotonic ids keep the vector id-sorted
    emit({ItemEventKind::Spawned, e.id, stack, ItemRemoveReason::PickedUp, position});
    return e.id;
}

EntityId ItemEntitySystem::spawnBreakDrop(ItemStack drop, glm::dvec3 cellCenter, uint64_t editId)
{
    if (drop.isEmpty())
        return 0;
    // Deterministic scatter/pop from the id this spawn WILL assign (spawn
    // order is single-threaded command order) and the world edit revision.
    uint64_t h = mix(m_nextId ^ mix(editId));
    const double angle = random01(h) * 2.0 * std::numbers::pi;
    const double speed = settings.breakScatter * (0.5 + 0.5 * random01(h));
    const glm::dvec3 velocity{std::cos(angle) * speed, settings.breakPopUp, std::sin(angle) * speed};
    return spawn(drop, cellCenter, velocity, settings.pickupDelayTicks);
}

EntityId ItemEntitySystem::spawnPlayerDrop(ItemStack stack, glm::dvec3 origin, glm::dvec3 velocity)
{
    return spawn(stack, origin, velocity, settings.playerDropDelayTicks);
}

void ItemEntitySystem::despawnOldest()
{
    if (m_entities.empty())
        return;
    auto oldest = m_entities.begin();
    for (auto it = std::next(m_entities.begin()); it != m_entities.end(); ++it)
        if (it->ageTicks > oldest->ageTicks)
            oldest = it;
    // Strict > keeps the FIRST (smallest id) on age ties: deterministic.
    emit({ItemEventKind::Removed, oldest->id, oldest->stack, ItemRemoveReason::Overflow,
          oldest->body.position});
    m_entities.erase(oldest);
}

// ---------------------------------------------------------------------------
// Fixed-step advance (the MobSystem discipline)
// ---------------------------------------------------------------------------

void ItemEntitySystem::update(double dt, const physics::VoxelCollisionWorld &world,
                              glm::dvec3 playerFeet, Inventory &inventory, bool suspended)
{
    // Legacy single-player forward (issue #247): a one-candidate batch. The
    // candidate lives on the stack — no heap churn for the solo path.
    const PickupCandidate candidate{playerFeet, &inventory};
    update(dt, world, std::span<const PickupCandidate>(&candidate, 1), suspended);
}

void ItemEntitySystem::update(double dt, const physics::VoxelCollisionWorld &world,
                              std::span<const PickupCandidate> candidates, bool suspended)
{
    validateSettings(settings);
    m_queries = {};
    if (suspended)
    {
        m_accumulator = 0;
        for (ItemEntity &e : m_entities)
            e.previous = e.body.position;
        return;
    }
    dt = std::isfinite(dt) ? std::max(dt, 0.0) : 0.0;
    m_accumulator += dt;
    uint32_t steps = 0;
    while (m_accumulator + 1e-12 >= settings.fixedStep && steps < settings.maxSteps)
    {
        step(world, candidates);
        m_accumulator -= settings.fixedStep;
        ++steps;
    }
    if (m_accumulator >= settings.fixedStep)
    {
        const uint64_t dropped = uint64_t(m_accumulator / settings.fixedStep);
        m_dropped += dropped;
        m_accumulator = std::fmod(m_accumulator, settings.fixedStep);
    }
}

void ItemEntitySystem::step(const physics::VoxelCollisionWorld &world,
                            std::span<const PickupCandidate> candidates)
{
    const double dt = settings.fixedStep;
    // Id order (the vector is id-sorted by construction): pickup merges into
    // the inventories in a deterministic order when several items qualify.
    for (ItemEntity &e : m_entities)
    {
        e.previous = e.body.position;
        ++e.ageTicks;
        if (e.pickupDelay > 0)
            --e.pickupDelay;
        // Gravity + ground settle, then the shared swept-collision move.
        // Unknown terrain holds the body (waitingForTerrain) — solidity is
        // never inferred, exactly like the player/mob solvers.
        e.body.velocity.y = std::max(e.body.velocity.y - settings.gravity * dt, -settings.terminalVelocity);
        if (e.body.grounded)
        {
            e.body.velocity.x *= settings.groundFriction;
            e.body.velocity.z *= settings.groundFriction;
        }
        physics::move(world, e.body, e.body.velocity * dt, m_queries);
        // Auto-pickup (issue #247 multi-player): elapsed delay + proximity to
        // an eligible player's authoritative feet, hotbar-first merge through
        // the Phase A stack math. Candidates are evaluated in the caller's
        // deterministic priority order (ascending PlayerId from the facade);
        // the first in-range player merges, a PARTIAL remainder is offered to
        // the next eligible candidate in the SAME tick, and a FULL inventory
        // skips to the next candidate. The aggregate count is conserved — an
        // item is never duplicated nor silently lost. With one candidate this
        // is bit-identical to the historical single-player rule.
        if (e.pickupDelay == 0 && !e.stack.isEmpty())
        {
            for (const PickupCandidate &candidate : candidates)
            {
                if (e.stack.isEmpty())
                    break; // fully claimed by an earlier (higher-priority) player
                if (candidate.inventory == nullptr)
                    continue;
                if (glm::distance(e.body.position, candidate.feet) <= settings.pickupRadius)
                {
                    const AddResult added = candidate.inventory->addStack(e.stack);
                    if (added.result == ItemResult::Ok || added.result == ItemResult::Partial)
                        e.stack = added.remainder; // Ok -> canonical empty
                }
            }
        }
        // Any future rule producing NaN must trip here in Debug, not leak
        // into AABB voxel queries (the tickMob guard).
        assert(std::isfinite(e.body.position.x) && std::isfinite(e.body.position.y) &&
               std::isfinite(e.body.position.z));
    }
    // Removal pass (id order): pickup resolves BEFORE age despawn within a
    // tick. Events are emitted before the erase so consumers see the exact
    // spawn/remove order.
    for (const ItemEntity &e : m_entities)
    {
        if (e.stack.isEmpty())
            emit({ItemEventKind::Removed, e.id, ItemStack{}, ItemRemoveReason::PickedUp, e.body.position});
        else if (e.ageTicks >= settings.despawnAgeTicks)
            emit({ItemEventKind::Removed, e.id, e.stack, ItemRemoveReason::Aged, e.body.position});
    }
    std::erase_if(m_entities, [&](const ItemEntity &e) {
        return e.stack.isEmpty() || e.ageTicks >= settings.despawnAgeTicks;
    });
}

// ---------------------------------------------------------------------------
// Derived outputs
// ---------------------------------------------------------------------------

void ItemEntitySystem::snapshots(std::vector<ItemEntitySnapshot> &out) const
{
    out.clear();
    out.reserve(m_entities.size()); // keeps the caller's capacity across frames
    for (const ItemEntity &e : m_entities)
        out.push_back({e.id, e.stack, e.previous, e.body.position, e.ageTicks});
}

void ItemEntitySystem::takeEvents(std::vector<ItemEntityEvent> &out)
{
    out.insert(out.end(), m_events.begin(), m_events.end());
    m_events.clear();
}

std::vector<ItemEntityEvent> ItemEntitySystem::takeEvents()
{
    std::vector<ItemEntityEvent> out;
    takeEvents(out);
    return out;
}

double ItemEntitySystem::interpolationAlpha() const
{
    return std::clamp(m_accumulator / settings.fixedStep, 0.0, 1.0);
}

void ItemEntitySystem::emit(const ItemEntityEvent &event)
{
    // Bounded stream (the WorldEvent contract): a consumer that falls behind
    // drops the OLDEST events at the cap — O(1) deque pop, counted so the
    // loss is observable rather than silent.
    constexpr std::size_t kMaxBufferedItemEvents = 4096;
    if (m_events.size() >= kMaxBufferedItemEvents)
    {
        m_events.pop_front();
        ++m_droppedEvents;
    }
    m_events.push_back(event);
}

} // namespace items
