#pragma once

// =============================================================================
// PlayerId — stable per-session player identity (issue #246, Epic #245
// Phase A). The multiplayer ownership split keeps ONE shared world authority
// (VoxelWorld + TerrainGenerator + MobSystem + ItemEntitySystem) and ONE
// PlayerState per player session (PlayerController + Inventory + hotbar/
// flight/spawn state), indexed by this id. See GameSimulation and
// docs/simulation-render-contracts.md §11 for the ownership design note.
//
// Identity semantics (the items::EntityId discipline, adapted to sessions):
//   * PLAIN VALUE, save/wire stable: a uint32_t ordinal in a strong type so a
//     player handle can never be confused with an item/mob EntityId
//     (uint64_t, a SEPARATE per-family allocator) or an ItemId registry
//     ordinal. No network types — the future session layer (Epic #245 Phase
//     B) maps wire identities onto these simulation-owned handles.
//   * INVALID: value 0 (kInvalidPlayerId) is never assigned. Default-
//     constructed handles are invalid; every per-player API answers a safe
//     refusal/no-op for them.
//   * ALLOCATION: createPlayer() assigns monotonic IDs starting at 1.
//     The dedicated host can reattach an authenticated persistent actor
//     with the original ID. The transport scopes its old and new sessions.
//   * EPOCH / RELOAD: initialize() and reload() are world-epoch boundaries.
//     They reset the shared populations and every session's inventory, but
//     player SESSIONS survive (bodies carry over until the host respawns them
//     — the historical solo contract) and the id allocator NEVER rewinds.
//     A handle therefore addresses the same session across world epochs, and
//     a retired/never-allocated handle can never alias another player's
//     state. See docs/adr/multiplayer-checkpoints.md for persistent identity.
//   * EXHAUSTION: the largest ASSIGNABLE id is kMaxPlayerId; the allocator
//     rests at kMaxNextPlayerId (representable, never assigned) and
//     createPlayer() refuses with kInvalidPlayerId instead of wrapping.
//   * ITERATION: GameSimulation keeps sessions ID-SORTED (monotonic
//     allocation + order-preserving removal), so playerIds() and every bulk
//     step/snapshot traverse a deterministic order — never a hash order.
//
// Dependency rule (tests/check_core_includes.py): std headers only.
// =============================================================================

#include <cstdint>
#include <limits>
#include <type_traits>

namespace sim
{

/// Strong plain-value player session handle. 0 is invalid (never assigned).
struct PlayerId
{
    uint32_t value{0};

    constexpr bool valid() const { return value != 0; }

    constexpr friend bool operator==(const PlayerId &, const PlayerId &) = default;
    constexpr friend bool operator<(const PlayerId &a, const PlayerId &b) { return a.value < b.value; }
};

/// The never-assigned invalid handle (default-constructed PlayerId).
inline constexpr PlayerId kInvalidPlayerId{0};

/// Id-exhaustion guards (the items::kMaxNextEntityId discipline): ids are
/// monotonic uint32_t, so the largest ASSIGNABLE id is UINT32_MAX - 1 — the
/// allocator can then rest at next = UINT32_MAX, which stays representable.
/// Allocation is REFUSED once the allocator reaches kMaxNextPlayerId; it
/// never wraps and never reuses a retired id.
inline constexpr uint32_t kMaxPlayerId = std::numeric_limits<uint32_t>::max() - 1;
inline constexpr uint32_t kMaxNextPlayerId = std::numeric_limits<uint32_t>::max();
static_assert(kMaxPlayerId + 1 == kMaxNextPlayerId,
              "the last player id's next value must stay representable");

static_assert(std::is_trivially_copyable_v<PlayerId>, "PlayerId must stay a plain value");
static_assert(sizeof(PlayerId) == 4, "PlayerId is a compact 4-byte plain value");

} // namespace sim
