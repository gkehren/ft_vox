#pragma once

// =============================================================================
// ItemEntitySystem — dropped item entities (issue #226, Epic #223 Phase C).
//
// The epic's first non-mob entity family, designed for future network
// replication (epic #223 §2/§6): stable ids, plain-value state, deterministic
// fixed-step rules, id-sorted bounded snapshots. MobSystem is the reference
// pattern — same accumulator/suspension discipline, same swept-collision
// primitives (physics::move), NO new solver and NO ECS (epic §7).
//
// Rules (all deterministic, no wall clock, no time-seeded RNG):
//   * fixed 60 Hz step, own accumulator, maxSteps catch-up cap + dropped-step
//     counter (the mob discipline); suspension freezes and syncs
//     previous/current, never replays a burst;
//   * gravity + voxel collision through the CPU collision seam; unknown
//     terrain HOLDS the body (physics::move waitingForTerrain semantics —
//     solidity is never inferred); ground friction settles horizontal motion;
//   * pickup-delay countdown, then proximity pickup into the inventory
//     HOTBAR-FIRST via the Phase A stack math: partial pickup leaves the
//     remainder in the world, a full inventory leaves the entity untouched.
//     Since issue #247 (Epic #245 Phase A2) pickup evaluates MULTIPLE eligible
//     players per entity in a caller-supplied deterministic priority order
//     (the facade passes ascending sim::PlayerId): the first in-range player
//     merges hotbar-first, a partial remainder is offered to the next eligible
//     player in the SAME tick, and the aggregate count is conserved (no
//     duplication). The single-player update() is a thin forward to the
//     one-candidate case, so solo behavior is bit-identical;
//   * age-based despawn (default 18000 ticks = 5 min sim time) and a bounded
//     population cap (default 1024) with deterministic OLDEST-FIRST overflow
//     despawn (ties break on the smallest id);
//   * spawn velocity jitter derives from deterministic inputs only (entity
//     id / edit revision — the splitmix64 finalizer MobSystem uses).
//
// Ids: simulation-owned monotonic allocator, starts at 1, NEVER reused
// within a session (reset() is the session boundary; initialize/reload call
// it). The high-water mark is exposed for the #228 persistence hook.
// PER-FAMILY NAMESPACE: item ids and mob ids are separate allocators (both
// start at 1, the established MobSystem pattern) — consumers must pair an id
// with its entity family, and the #228 persistence format keeps the two
// namespaces distinct.
//
// Dependency rule (tests/check_core_includes.py): Items + Entities/MobTypes
// (the shared EntityId) + Physics/VoxelCollision + glm + std only. No
// world/sim/render header is included; the facade supplies the collision
// view and the pickup candidates (feet + inventory pointers, priority order)
// per advance. The candidate ORDER encodes player priority — this module
// never knows about sim::PlayerId and never reorders the candidates.
// =============================================================================

#include <Entities/MobTypes.hpp>
#include <Items/Inventory.hpp>
#include <Items/ItemTypes.hpp>
#include <Physics/VoxelCollision.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

namespace items
{

/// The shared entity id type (entities::EntityId, re-exported by
/// sim::EntityId): uint64_t, plain value, save/wire stable.
using EntityId = entities::EntityId;

/// Id-exhaustion guards (stabilization PR3). Ids are monotonic uint64_t and
/// the persistence format stores the allocator's NEXT id (high-water + 1)
/// as uint64_t. The largest ASSIGNABLE id is therefore UINT64_MAX - 1: the
/// allocator can then rest at next = UINT64_MAX, which stays representable
/// and valid in items.state (decode requires next >= 1 and id < next).
/// Allocation is REFUSED once the allocator reaches kMaxNextEntityId — it
/// never wraps, never reuses an id, and never assigns the invalid id 0.
/// A refused spawn consumes nothing: the facade commands pre-check
/// idsExhausted() so inventory items and block drops are conserved.
inline constexpr EntityId kMaxEntityId = std::numeric_limits<uint64_t>::max() - 1;
inline constexpr uint64_t kMaxNextEntityId = std::numeric_limits<uint64_t>::max();
static_assert(kMaxEntityId + 1 == kMaxNextEntityId, "the last id's next value must stay representable");

/// Plain-value tunables (the MobSettings pattern). Defaults implement the
/// epic #223 §2 rules; tests tune them for bounded fixtures.
struct ItemEntitySettings
{
    static constexpr std::size_t capacity = 1024; // hard population ceiling

    double fixedStep = 1.0 / 60.0; // 60 Hz (epic invariant, kItemTickHz)
    uint32_t maxSteps = 8;         // catch-up cap per advance (mob discipline)

    double gravity = physics::constants::gravity; // blocks/s^2 (single definition)
    double terminalVelocity = 30.0;   // downward clamp
    double groundFriction = 0.5;      // per-tick horizontal decay when grounded
    glm::dvec3 bodySize{0.25, 0.25, 0.25};

    double pickupRadius = 1.5;        // blocks, 3D distance to the player feet
    uint32_t pickupDelayTicks = 10;   // break drops before pickup is allowed
    uint32_t playerDropDelayTicks = 40; // manual drops (no instant re-pickup)
    uint32_t despawnAgeTicks = 18000; // 5 minutes of sim time at 60 Hz

    double dropSpeed = 3.0;   // manual-drop launch speed along the host dir
    double breakPopUp = 2.0;  // upward pop of a break drop
    double breakScatter = 0.8; // horizontal scatter scale of a break drop
};

/// One authoritative dropped item. Plain values only (no pointers): the
/// physics Body carries position/velocity/size/grounded/waitingForTerrain;
/// `previous` is the last tick's position for render interpolation.
struct ItemEntity
{
    EntityId id{0};
    ItemStack stack{};
    physics::Body body{};
    glm::dvec3 previous{};
    uint32_t ageTicks{0};
    uint32_t pickupDelay{0};
};

/// Bounded spawn/remove lifecycle stream (the WorldEvent pattern): derived
/// consumers (the #229 renderer, headless tests) track population without
/// scanning. Partial pickups are NOT events — the count change is visible in
/// the per-frame snapshot; only spawn/remove transitions are streamed.
enum class ItemEventKind : uint8_t
{
    Spawned,
    Removed
};

/// Why a Removed entity left the world.
enum class ItemRemoveReason : uint8_t
{
    PickedUp, // fully merged into the inventory (event stack is empty)
    Aged,     // despawnAgeTicks reached (event stack = the lost remainder)
    Overflow  // population cap: oldest-first despawn (event stack = lost)
};

struct ItemEntityEvent
{
    ItemEventKind kind{ItemEventKind::Spawned};
    EntityId id{0};
    /// Spawned: the full spawn stack. Removed: the stack at removal —
    /// canonical empty for PickedUp (the items moved into the inventory),
    /// the lost remainder for Aged/Overflow.
    ItemStack stack{};
    ItemRemoveReason reason{ItemRemoveReason::PickedUp}; // Removed only
    glm::dvec3 position{};

    constexpr friend bool operator==(const ItemEntityEvent &, const ItemEntityEvent &) = default;
};

/// Derived per-entity presentation state (epic #223 §2): previous/current
/// positions for interpolation, item + count, age. Bounded by the population
/// cap, ID-SORTED, filled in place like mobSnapshots. The consumer mixes
/// previous -> current with ItemEntitySystem::interpolationAlpha().
struct ItemEntitySnapshot
{
    EntityId id{0};
    ItemStack stack{};
    glm::dvec3 previous{};
    glm::dvec3 current{};
    uint32_t ageTicks{0};

    constexpr friend bool operator==(const ItemEntitySnapshot &, const ItemEntitySnapshot &) = default;
};

/// One eligible pickup receiver (issue #247, Epic #245 Phase A2): the
/// authoritative feet position used for the proximity test and the inventory
/// merged into hotbar-first. A plain non-owning value — the facade keeps the
/// sessions alive for the whole advance. Candidates are supplied in
/// DETERMINISTIC PRIORITY ORDER (the facade passes ascending sim::PlayerId);
/// the system evaluates them in the given order and never sorts or reorders
/// them, so the winner/tie-break is reproducible. A null inventory is skipped
/// defensively (the facade only passes live sessions). This module knows
/// nothing about player identity — the ORDER is the contract.
struct PickupCandidate
{
    glm::dvec3 feet{};
    Inventory *inventory{nullptr};
};

static_assert(std::is_trivially_copyable_v<PickupCandidate>,
              "PickupCandidate must stay a plain non-owning value");

class ItemEntitySystem
{
  public:
    ItemEntitySystem();

    /// Session boundary: clears entities/events/accumulators and restarts
    /// the id allocator at 1 (ids are never reused WITHIN a session; the
    /// #228 hook restores the persisted high-water mark across save/reload).
    void reset();

    /// Persistence restore seam (issue #228, Epic #223 Phase E): replace
    /// the population with save-layer-VALIDATED id-sorted entities and
    /// resume the allocator at `nextEntityId` (the persisted high-water + 1)
    /// so ids are NEVER reused across sessions (replication requirement).
    /// LIFECYCLE ONLY — the facade calls it on finalizePlayerSpawn. Events,
    /// accumulator and dropped counters reset (fresh presentation session);
    /// each entity's previous position is set to its position (no
    /// interpolation jump on the first frame). grounded/waitingForTerrain
    /// are solver transients, not persisted: physics::move re-derives them
    /// on the first tick (the items.state spec notes the one-tick friction
    /// transient).
    void restore(uint64_t nextEntityId, const std::vector<ItemEntity> &entities);

    /// True when the allocator reached kMaxNextEntityId and can no longer
    /// assign a fresh id (stabilization PR3). Facade commands pre-check this
    /// BEFORE mutating inventory/world state so a refused spawn never
    /// consumes items or silently discards a block drop. The population cap
    /// is independent: overflow despawn still applies while ids remain.
    bool idsExhausted() const { return m_nextId >= kMaxNextEntityId; }

    // --- Spawn seams (facade commands + deterministic fixtures) ---------------
    // Ids are assigned at CALL order — command/tick order is the deterministic
    // spawn order. An empty stack spawns nothing and answers id 0; an
    // exhausted allocator also answers id 0 WITHOUT touching the population
    // (no overflow despawn on a refused spawn — conservation).

    /// Generic spawn (fixture seam, the MobSystem::add pattern).
    EntityId spawn(ItemStack stack, glm::dvec3 position, glm::dvec3 velocity, uint32_t pickupDelayTicks);

    /// Break drop at a cell center: deterministic scatter/pop jitter derived
    /// from the new entity id and the world edit id (no RNG state).
    EntityId spawnBreakDrop(ItemStack drop, glm::dvec3 cellCenter, uint64_t editId);

    /// Manual player drop: host-supplied origin/velocity (presentation input
    /// normalized by the facade), longer pickup delay.
    EntityId spawnPlayerDrop(ItemStack stack, glm::dvec3 origin, glm::dvec3 velocity);

    // --- Fixed-step advance ----------------------------------------------------

    /// One host frame with MULTIPLE pickup receivers (issue #247). `candidates`
    /// are the eligible players in deterministic priority order (the facade
    /// passes ascending sim::PlayerId); each entity offers its stack to the
    /// in-range candidates in that order, hotbar-first, a partial remainder
    /// going to the next eligible candidate in the same tick. The aggregate
    /// count is conserved. Suspension clears the accumulator and syncs
    /// previous/current (no catch-up burst afterwards). The caller owns the
    /// collision view's lifetime (main-thread only).
    void update(double dt, const physics::VoxelCollisionWorld &world,
                std::span<const PickupCandidate> candidates, bool suspended);

    /// Legacy single-player forward (kept for system-level fixtures): a
    /// one-candidate batch, bit-identical to the historical pickup rule.
    void update(double dt, const physics::VoxelCollisionWorld &world, glm::dvec3 playerFeet,
                Inventory &inventory, bool suspended);

    // --- Derived outputs ---------------------------------------------------------

    /// Id-sorted bounded snapshots, filled in place (buffer reuse: clear +
    /// reserve keeps the caller's capacity across frames).
    void snapshots(std::vector<ItemEntitySnapshot> &out) const;

    /// Drain the lifecycle stream (WorldEvent takeEvents pattern): appends
    /// into `out` and empties the stream; the by-value overload matches.
    void takeEvents(std::vector<ItemEntityEvent> &out);
    std::vector<ItemEntityEvent> takeEvents();

    // --- Identity / diagnostics ----------------------------------------------------

    /// Last assigned id (0 before the first spawn): the #228 persistence
    /// high-water mark. Monotonic within a session, never reused.
    uint64_t idHighWater() const { return m_nextId - 1; }
    uint64_t droppedSteps() const { return m_dropped; }
    /// Collision-query diagnostics (the MobSystem::queryStats() pattern):
    /// cells/iterations touched by the last update's swept moves.
    const physics::QueryStats &queryStats() const { return m_queries; }
    /// Events dropped at the stream cap because a consumer fell behind
    /// (zero while draining at frame cadence — the WorldEvent contract).
    uint64_t droppedEventCount() const { return m_droppedEvents; }
    std::size_t size() const { return m_entities.size(); }
    /// Render interpolation fraction (accumulator / fixedStep, clamped).
    double interpolationAlpha() const;
    /// Read-only authoritative population, ID-SORTED by construction
    /// (monotonic spawn ids + order-preserving removals).
    const std::vector<ItemEntity> &entities() const { return m_entities; }

    ItemEntitySettings settings;

  private:
    void step(const physics::VoxelCollisionWorld &world, std::span<const PickupCandidate> candidates);
    void despawnOldest(); // population-cap overflow policy
    void emit(const ItemEntityEvent &event);

    std::vector<ItemEntity> m_entities; // id-sorted by construction
    std::deque<ItemEntityEvent> m_events;
    uint64_t m_nextId{1};
    uint64_t m_dropped{0};
    uint64_t m_droppedEvents{0};
    double m_accumulator{0};
    physics::QueryStats m_queries{};
};

} // namespace items
