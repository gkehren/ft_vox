#pragma once

// =============================================================================
// GameSimulation — the CPU-owned simulation facade (issue #208, Epic #204
// Phase D; issue #246, Epic #245 Phase A splits shared world authority from
// per-player gameplay state). SINGLE owner of the authoritative gameplay
// state, in two ownership layers:
//
// SHARED WORLD AUTHORITY (one instance per simulation, world-epoch scoped):
//   * world identity: the TerrainGenerator (seed + kGeneratorVersion) and the
//     VoxelWorld (chunk residency/streaming, logical edits, generation
//     publication, WorldPersistence — the issue #206 contracts);
//   * passive mobs: entities::MobSystem at the fixed 60 Hz step over the
//     CPU-only WorldMobView (spawn/retire/interest radii are plain values,
//     independent of Camera / RenderSettings);
//   * dropped items: items::ItemEntitySystem at the fixed 60 Hz step
//     (issue #226, Epic #223 Phase C) — break/drop commands, deterministic
//     auto-pickup, id-sorted bounded snapshots + a spawn/remove event stream;
//   * session identity: the world generation / reload counter.
//
// PER-PLAYER SESSIONS (one PlayerState per PlayerId, indexed id-sorted):
//   * physics::PlayerController at the fixed 120 Hz step, flight/walk mode,
//     spawn/restore, creative block selection, transition status;
//   * items::Inventory (36 slots, hotbar 0..8) with explicit gameplay
//     commands + a bounded value snapshot (issue #225, Epic #223 Phase B;
//     the item domain contracts are issue #224 / src/Items/).
//   Sessions are created/removed explicitly (createPlayer/removePlayer);
//   PlayerId allocation is monotonic and never reused (Sim/PlayerId.hpp).
//   The LOCAL player (localPlayerId()) is the solo-compatibility anchor: it
//   exists from construction, is always the first session and cannot be
//   removed. Every legacy single-player API below is a THIN FORWARD to it —
//   one source of truth, no parallel solo state.
//
// A2 SCOPE (issue #247, Epic #245 Phase A): world interactions (break/place/
// drop), the player-aware placement rule and auto-pickup are ACTOR-AWARE.
// Every gameplay command/query has a per-player form taking a PlayerId first;
// the legacy single-player forms are thin forwards to the LOCAL player, so
// solo behavior stays bit-identical (one source of truth). The placement rule
// protects EVERY walking player body (deterministic ascending-id validation),
// and shared pickup evaluates eligible players in ascending-PlayerId order
// with conserved partial merges. Actor-aware targeting derives the ray origin
// from the actor's authoritative eye and clamps reach to kInteractionReach —
// never a client-supplied origin. A1 (#246) split the ownership; A2 makes the
// interactions actor-aware. A3 (#248) adds shared player-driven residency.
// Multi-player persistence is #259 (E1), and server-side reach/rate
// gating of break/place CELLS is #255 (C3). The shared systems' contracts are
// unchanged. Design note: docs/simulation-render-contracts.md §11–§13.
//
// The facade is render-free (enforced by tests/check_core_includes.py): no
// Camera, RenderSettings, SDL, Vulkan or ImGui types, and no engine singleton
// state. The host supplies plain values — elapsed time, normalized player
// commands, an observer/interest point — and consumes read-only, bounded
// VALUE snapshots (player, mob transforms/animation, world identity + edit
// revision). Fixed stepping stays internal to the controllers; the maxSteps
// catch-up cap is unchanged (Epic #204 invariant). All mutation stays on the
// single simulation thread (the host's frame boundary), exactly as before.
//
// State / policy boundaries (issue #208):
//   * SDL key/mouse -> normalized commands is HOST responsibility; camera
//     orientation and render interpolation are presentation. The simulation
//     owns the authoritative feet position, flight mode and block selection.
//   * Pause/focus/benchmark POLICY stays with the host: it decides WHEN to
//     suspend (advancePlayer/advanceMobs take an explicit flag, per player or
//     bulk). The simulation implements the suspension semantics: accumulators
//     cleared, previous/current synced, NO catch-up burst after suspension.
//   * The observer/interest point is a plain value (camera position for the
//     graphical host, a scripted probe for headless tests), so debug flight
//     and headless streaming work without a Camera.
//   * Day/night: the day cycle is a PRESENTATION parameter (the graphical
//     host's ShaderParameters.dayTime), NOT authoritative world time. The
//     simulation owns no wall clock beyond the fixed-step accumulators; the
//     host keeps advancing dayTime exactly as before (pause gates it there).
//
// Ownership / lifetime:
//   * the ThreadPool is REFERENCED (host-owned), exactly like VoxelWorld;
//   * the graphical host attaches a RenderWorldBridge to world() — the bridge
//     only references the world and MUST be destroyed before the simulation;
//   * headless hosts drive bootstrapTerrain() / pumpWorld() themselves; no
//     render handle, mesh or GPU resource is ever created here.
//
// Frame sequencing (graphical host, unchanged from Phase C): the bridge's
// processFinishedJobs() keeps the issue #114 publication ordering (world
// generation -> render mesh/light -> deferred edits). The simulation therefore
// does NOT pump the world in advancePlayer/advanceMobs; pumpWorld() is the
// headless-only publication seam.
// =============================================================================

#include <climits>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <Chunk/TerrainGenerator.hpp>
#include <Core/SimContracts.hpp>
#include <Core/ThreadPool.hpp>
#include <Entities/MobSystem.hpp>
#include <Items/Inventory.hpp>
#include <Items/ItemEntitySystem.hpp>
#include <Physics/PlayerController.hpp>
#include <Sim/PlayerId.hpp>
#include <World/VoxelWorld.hpp>
#include <World/WorldPersistence.hpp>

#include <array>

namespace sim
{

/// Read-only per-frame player snapshot: plain values, bounded, no renderer
/// types. `renderEye` is the interpolated eye the presentation camera follows
/// (previous -> current at the accumulator fraction); `position` is the
/// authoritative feet position. `status` points at static literals only.
/// One snapshot per player session (playerSnapshot(id, out)); the legacy
/// by-value playerSnapshot() answers the local player's.
struct PlayerSnapshot
{
    glm::dvec3 position{0.0};
    glm::dvec3 velocity{0.0};
    glm::dvec3 renderEye{0.0};
    bool grounded{false};
    bool waitingForTerrain{false};
    bool flight{false};
    physics::Immersion submerged{};
    physics::PhysicsMetrics metrics{};
    physics::PlayerController::Timers movementTimers{};
    uint32_t motionReset{};
    TextureType selectedBlock{STONE};
    const char *status{""};
};

/// Read-only world identity / revision snapshot: plain values, bounded
/// (worldName is the validated save name, empty for a transient session).
/// `editRevision` is the world's monotonic logical edit counter (the id the
/// NEXT accepted edit will carry), so consumers can detect edit progression
/// without scanning chunks.
struct WorldSnapshot
{
    int seed{0};
    uint32_t generatorVersion{0};
    uint64_t worldGenerationId{0}; // session reload counter
    uint64_t editRevision{0};
    size_t chunkCount{0};
    bool hasPendingLogicalEdits{false};
    bool persistenceOpen{false};
    std::string worldName;
};

/// Read-only inventory snapshot (issue #225, Epic #223 Phase B): a BOUNDED
/// plain value — the 36 fixed slots plus the selected hotbar index. Filled
/// in place like mobSnapshots (no per-frame allocation, no intermediate
/// cache). Display names are resolved through the item registry on the UI
/// side (#229): the snapshot carries stable ItemIds, never strings.
/// Value equality is defaulted (the ItemStack pattern) so digest/replication
/// comparisons (#228/#230, epic §6) compare whole snapshots directly.
struct InventorySnapshot
{
    std::array<items::ItemStack, items::kInventorySize> slots{};
    int selectedSlot{0};

    constexpr friend bool operator==(const InventorySnapshot &, const InventorySnapshot &) = default;
};

/// Derived dropped-item presentation state (issue #226, Epic #223 Phase C):
/// the items::ItemEntitySystem snapshot, re-exported under the sim contract
/// namespace exactly like sim::MobSnapshot. Bounded by the population cap,
/// ID-SORTED, previous/current for interpolation (mix with
/// itemInterpolationAlpha()), item + count + age. No render types.
using ItemEntitySnapshot = items::ItemEntitySnapshot;

/// Result of breakBlock (issue #226): the interaction rejection vocabulary,
/// distinct from the item-operation codes (items::ItemResult).
enum class BreakResult : uint8_t
{
    Ok,             // block deleted (world edit semantics) + registry drop spawned
    Unbreakable,    // BEDROCK / fluids — not breakable by hand in v1
    UnknownTerrain, // cell unloaded / not generated (never inferred)
    WorldRejected,  // nothing to break (AIR / out of bounds) or the world
                    // refused the edit (no-op / invalid target)
    IdExhausted,    // the entity-id allocator reached its representable bound
                    // (stabilization PR3): the break is refused BEFORE the
                    // world edit so the registry drop is never silently lost
                    // (conservation). Append-only vocabulary.
    InvalidPlayer   // unknown / retired actor handle (issue #247, Epic #245
                    // Phase A2): the per-player break is refused WITHOUT
                    // touching the world or the item population. The actor-
                    // identity rejection the authoritative server validates
                    // first (epic #245 §1). Append-only vocabulary.
};

/// Result of placeSelectedBlock (issue #227, Epic #223 Phase D): the
/// place-from-hotbar rejection vocabulary.
enum class PlaceResult : uint8_t
{
    Ok,             // placed through the world edit semantics; consumed 1 item
    EmptySlot,      // selected hotbar stack is empty
    NotPlaceable,   // selected item has no registry placeableBlock (future
                    // non-block items; every v1 catalog item is placeable)
    PlayerOverlap,  // canPlaceVoxel refused (walking body + solid block, #209)
    UnknownTerrain, // target cell unloaded / not generated (gameplay refuses;
                    // the creative seam may defer into registered records)
    WorldRejected,  // world refused the edit (occupied / out-of-bounds target)
    InvalidPlayer   // unknown / retired actor handle (issue #247, Epic #245
                    // Phase A2): the per-player place is refused WITHOUT
                    // consuming inventory or editing the world. Append-only
                    // vocabulary.
};

enum class VoxelInteraction { Break, Place };
enum class VoxelTargetResult { Valid, InvalidTarget, OutOfReach, TerrainUnavailable, StaleRevision, Occupied };

/// Plain-value targeting result (issue #227): the simulation-owned raycast
/// query answers the hit block plus the adjacent placement cell. `adjacent`
/// is the last marched cell before the hit, with the historical fallback
/// (hit cell minus the ray direction signs) when the ray starts inside a
/// block. No render types — the highlight consumes this (presentation).
struct RaycastHit
{
    bool hit{false};
    glm::vec3 block{0.0f};
    glm::vec3 adjacent{0.0f};
};

struct VoxelTargetTrace
{
    glm::ivec3 block{}, adjacent{};
    bool hit{}, hasPlaceFace{}, terrainUnavailable{};
};

class GameSimulation
{
public:
    /// Bootstrap radius (chunks) shared by the terrain bootstrap and the
    /// surface-spawn scan (the historical Engine::kBootstrapRadius).
    static constexpr int kBootstrapRadiusChunks = 2;

    /// Hard upper bound on the targeting march (stabilization PR1). The
    /// graphical host exposes renderSettings.raycastDistance in [2, 32]
    /// (default 8); 64 leaves 2x headroom over the widest supported
    /// interaction range while keeping the float march accumulator far below
    /// the precision wall where `t += 0.05f` stalls (a non-terminating loop
    /// for unbounded distances). Larger requested distances are CLAMPED, not
    /// rejected — a valid ray never marches farther than this.
    static constexpr float kMaxRaycastDistance = 64.f;

    /// Supported ray-origin envelope (stabilization PR1): every component
    /// must satisfy |c| <= this limit. Chosen so the whole clamped march
    /// stays inside the world's finite collision envelope
    /// (WorldCollisionView's SHRT_MIN/SHRT_MAX walls), which keeps every
    /// floor() -> int coordinate conversion in range. Origins outside the
    /// supported world envelope answer a safe no-hit.
    static constexpr float kRaycastOriginLimit = 32700.f;
    static_assert(kRaycastOriginLimit + kMaxRaycastDistance < static_cast<float>(SHRT_MAX),
                  "the clamped march must stay inside the +X/+Z collision envelope");
    static_assert(-kRaycastOriginLimit - kMaxRaycastDistance > static_cast<float>(SHRT_MIN),
                  "the clamped march must stay inside the -X/-Z collision envelope");

    /// Authoritative gameplay interaction reach (issue #247, Epic #245 Phase
    /// A2): the maximum distance an ACTOR-AWARE targeting query
    /// (raycastVoxel(id, dir, maxDist)) may march, measured from the actor's
    /// authoritative eye. The server owns this value — a client can request a
    /// SHORTER distance but never target beyond it, and can never supply the
    /// ray origin (it is derived from the actor). Matches the historical
    /// default interaction range (renderSettings.raycastDistance default 8).
    /// The legacy raw-origin raycastVoxel(origin, dir, maxDist) is the
    /// privileged creative/editor host seam and keeps the host-supplied
    /// distance (clamped only by kMaxRaycastDistance); it is NOT the
    /// network-facing gameplay query.
    static constexpr float kInteractionReach = 8.f;
    static_assert(kInteractionReach > 0.f && kInteractionReach <= kMaxRaycastDistance,
                  "the gameplay reach must be positive and within the march safety cap");

    /// Simultaneous player-session ceiling (issue #246, Epic #245 Phase A):
    /// a generous bounded population, NOT a two-player architecture — the
    /// future session layer (Phase B) imposes its own policy limit below it.
    /// createPlayer() refuses with kInvalidPlayerId at the cap; removal
    /// frees a slot (the retired id is never reused).
    static constexpr std::size_t kMaxPlayers = 256;

    /// World-open request (issue #180 identity rules apply): an existing save
    /// keeps its stored seed; an explicit seed that contradicts the save runs
    /// the session transient (openWorldError() reports why).
    struct Config
    {
        int seed{0};               // <= 0: stored seed, else a random roll
        std::string worldName;     // empty: transient session (no persistence)
        std::filesystem::path savesRoot{"saves"};
        bool dedicatedServer{false};
    };

    /// Result of finalizePlayerSpawn(): where the host camera should look
    /// from, plus the restored orientation when a saved state was applied.
    struct SpawnOutcome
    {
        glm::dvec3 eye{0.0};
        bool restored{false};
        float yaw{0.f}, pitch{0.f}; // valid when restored (camera convention)
    };

    /// The worker pool is referenced, not owned (host lifetime; may be null
    /// for fully synchronous headless fixtures, like VoxelWorld). The local
    /// player session exists from construction (the solo-compatibility
    /// anchor): the legacy APIs are total before initialize().
    explicit GameSimulation(ThreadPool *threadPool);
    /// Final safety net: force-closes an still-open persistence (the host's
    /// explicit shutdown() is the durable path that also writes player.state).
    ~GameSimulation();

    GameSimulation(const GameSimulation &) = delete;
    GameSimulation &operator=(const GameSimulation &) = delete;

    // --- Player sessions (issue #246, Epic #245 Phase A) -----------------------
    // One shared world, N independent player sessions. Mutation stays on the
    // single simulation thread; every per-player API below answers a safe
    // refusal/no-op for an invalid, retired or never-allocated PlayerId — a
    // stale handle can never address another player's state (ids are never
    // reused, Sim/PlayerId.hpp). Sessions survive initialize()/reload() (the
    // world-epoch boundaries reset every inventory, not the sessions); the
    // allocator never rewinds. Deterministic iteration: playerIds() and the
    // bulk APIs traverse sessions in ascending id order.

    /// Create a fresh session: default controller at the origin, empty
    /// inventory, walking mode, STONE creative selection. The session starts
    /// UNPOSITIONED — it does not join shared pickup or player-body placement
    /// checks until the host places it (setPlayerPosition / spawnPlayerOnSurface
    /// / restorePlayerState / setPlayerFlight). The host places the body
    /// afterwards. Returns kInvalidPlayerId at the population cap or id
    /// exhaustion (nothing is created).
    PlayerId createPlayer();
    /// Dedicated host lifecycle. Reattach an authenticated persistent actor
    /// after its old transport scope and simulation state have been retired.
    bool attachPersistentPlayer(PlayerId id);

    /// Remove a session. REFUSES (returns false, nothing changes) for an
    /// invalid/unknown id and for the LOCAL player — the solo-compatibility
    /// anchor must stay total for the legacy APIs. Removing a session never
    /// touches the shared world, mobs, dropped items or the other sessions;
    /// items the player already dropped stay in the shared world population.
    bool removePlayer(PlayerId id);

    bool hasPlayer(PlayerId id) const;
    std::size_t playerCount() const { return m_players.size(); }
    /// The solo-compatibility anchor: created at construction, always the
    /// first session, never removable. Every legacy single-player API
    /// forwards to it.
    PlayerId localPlayerId() const { return m_players.front()->id; }
    /// Ascending-id session list, filled IN PLACE (the snapshot pattern):
    /// clear + reserve keeps the caller's capacity across frames.
    void playerIds(std::vector<PlayerId> &out) const;

    // --- Lifecycle (explicit reset/open/close/reload) -------------------------

    /// Resolve the seed, create the authoritative world, open persistence
    /// when requested and read the saved player state. Returns the bootstrap
    /// center: the SAVED local-player position when one exists (issue #180
    /// review round 6 — the initial area must generate around it), else
    /// `fallbackCenter`. Terrain is NOT generated here: the graphical host
    /// bootstraps through RenderWorldBridge::generateInitialArea (voxels +
    /// render binding/meshing/upload in one step), a headless host calls
    /// bootstrapTerrain(). Follow with finalizePlayerSpawn().
    /// Player flight mode, block selection and status carry over from a
    /// previous session until finalizePlayerSpawn (restore/spawn) sets them —
    /// the historical Engine behavior, per session. EVERY session's inventory
    /// and the shared dropped-item population + id allocator reset to EMPTY
    /// here; the local player's inventory and the item population are restored
    /// from inventory.state / items.state by finalizePlayerSpawn (issue #228;
    /// missing files = fresh, corrupt = empty with a loud save-error log).
    /// Player sessions themselves survive the epoch boundary (issue #246).
    glm::dvec3 initialize(const Config &config, glm::dvec3 fallbackCenter);

    /// Headless synchronous voxel bootstrap: the world half of the bridge's
    /// generateInitialArea, with no render handles involved.
    void bootstrapTerrain(glm::dvec3 center);

    /// After the terrain bootstrap: restore the validated saved LOCAL player,
    /// or fall back to the surface spawn around `fallbackAround` (the
    /// graphical host passes its camera position — the historical scan
    /// anchor; a headless host passes the bootstrap center). Camera
    /// orientation is presentation — the outcome carries the saved yaw/pitch
    /// for the host. Also restores the saved inventory + dropped items +
    /// entity-id allocator high-water (issue #228), independent of the player
    /// outcome. The gameplay restore is ONCE per session (the payloads are
    /// consumed): repeated calls never overwrite live gameplay state
    /// (PR #235 review). Non-local sessions are spawned/restored explicitly
    /// by the host (issue #246; multi-player persistence is #259).
    SpawnOutcome finalizePlayerSpawn(glm::dvec3 fallbackAround);

    /// Close (refusable) and recreate the world for `newSeed` (<= 0 -> 42,
    /// the historical reload default). Returns FALSE and keeps the OLD world
    /// authoritative when the persistent close is refused (stranded logical
    /// edits — issue #180 review round 4). The graphical host quiesces and
    /// closes through the bridge FIRST (render jobs hold record leases), then
    /// destroys the bridge before calling this. Terrain bootstrap and player
    /// spawn are separate steps (the benchmark host uses its own placement).
    /// Reloaded worlds are transient: persistence stays off afterwards.
    /// A SUCCESSFUL reload resets EVERY session's inventory to empty
    /// (issue #225 generalized by #246) and the shared dropped-item population
    /// + id allocator (issue #226); a REFUSED reload keeps the old world,
    /// inventories AND items authoritative. Player bodies are intentionally
    /// LEFT to the host's spawn step: a headless host that reloads without
    /// re-spawning keeps the previous bodies (possibly in an unloaded region)
    /// until it calls finalizePlayerSpawn / spawnPlayerOnSurface. Sessions
    /// and their ids survive the reload (issue #246).
    bool reload(int newSeed);

    /// Durable GAMEPLAY capture + flush (no close). The host supplies the
    /// presentation orientation (camera yaw/pitch); everything else is
    /// authoritative sim state. Since issue #228 (Epic #223 Phase E) this is
    /// the FULL gameplay capture: player.state + inventory.state +
    /// items.state (population + entity-id allocator high-water), all three
    /// written BEFORE the flush durability barrier. The API name is kept
    /// for host compatibility. Call while the render bridge is still alive,
    /// before shutdown(). Returns false when nothing is open or any write /
    /// the flush failed (failures are logged, exactly like the historical
    /// Engine shutdown; a failed write stays retryable, never dropped).
    /// A1 scope (issue #246): the capture covers the LOCAL player's state —
    /// Dedicated checkpoints include the caller's complete remote roster.
    bool savePlayerState(float yaw, float pitch, const worldsave::PlayerRoster *roster = nullptr);

    /// Destructor-only close: resolve stranded edits, capture, flush and
    /// shut down UNCONDITIONALLY (no retry point at destruction). Idempotent;
    /// the graphical host calls it after destroying the render bridge.
    /// Since issue #228 the safety net also captures inventory.state +
    /// items.state (fully authoritative — no host presentation input needed,
    /// unlike player.state's camera orientation, which only the explicit
    /// savePlayerState path writes). A1 scope: local-player inventory, the
    /// shared item population (issue #246).
    void shutdown();

    // --- Per-frame advance (explicit elapsed time + plain values) -------------

    /// One host frame of the LOCAL player's physics (fixed 120 Hz
    /// internally) — the solo-compatibility forward to
    /// advancePlayer(localPlayerId(), ...). `suspended` implements the host's
    /// pause/focus/benchmark policy: no stepping, accumulators cleared, no
    /// catch-up burst afterwards. In flight mode the body is host-driven
    /// (setPlayerPosition each frame); only the walking controller steps here.
    void advancePlayer(double dt, const PlayerCommand &command, bool suspended);

    /// One host frame of ONE player's physics (fixed 120 Hz internally,
    /// issue #246). Returns false for an invalid/unknown id (no-op). Same
    /// suspension/flight semantics as the legacy forward.
    bool advancePlayer(PlayerId id, double dt, const PlayerCommand &command, bool suspended);

    /// One host frame of EVERY player's physics, in ascending id order
    /// (issue #246), with a neutral (zero) command — the bulk idle/fixture
    /// step. Hosts with per-player input loop playerIds() + advancePlayer(id,
    /// ...) instead. One scoped collision view serves the whole batch;
    /// suspension/flight semantics are identical per player.
    void advancePlayers(double dt, bool suspended);

    /// Host-owned radius in chunks. Zero disables interest. The center is
    /// sampled from the positioned session's authoritative body each tick.
    bool setPlayerInterestRadius(PlayerId id, int radiusChunks);
    /// One CPU-only host tick for the entire interest union. Pumps world
    /// publication and edits before eviction. The first call selects this
    /// streaming mode for the world epoch. Returns false before initialize
    /// while solo terrain jobs or edits are pending, or after transient solo edits.
    bool advancePlayerStreaming(PlayerStreamingBudget budget = {});

    /// One host frame of passive mobs (fixed 60 Hz internally). `observer` is
    /// the interest point (camera position or a headless probe),
    /// `availableRadius` the streamed radius in blocks (plain value — the
    /// graphical host passes its view distance). `suspended` freezes the
    /// population and syncs previous/current for interpolation; the
    /// sim-owned mobsEnabled() toggle suspends it as well.
    void advanceMobs(double dt, glm::dvec3 observer, double availableRadius, bool suspended);

    // Dedicated hosts keep a local compatibility player for the world lifecycle.
    enum class LocalAnchorPolicy { Include, Exclude };

    /// One shared step with positioned participants as population/retirement observers.
    void advancePlayerMobs(double dt, double availableRadius, bool suspended,
                           LocalAnchorPolicy anchorPolicy = LocalAnchorPolicy::Include);

    /// One host frame of dropped item entities (fixed 60 Hz internally,
    /// issue #226). The host calls it NEXT TO advanceMobs (the #227 rewiring
    /// adds the call site; until then only headless hosts/tests drive it).
    /// Actor-aware pickup (issue #247, Epic #245 Phase A2): positioned
    /// participants are evaluated in ascending-PlayerId
    /// order (the deterministic priority); each drop merges hotbar-first into
    /// the first in-range player, a partial remainder is offered to the next
    /// eligible player in the same tick, and the aggregate count is conserved.
    /// Unpositioned sessions (created but not yet spawned/placed) are excluded
    /// so a default-origin body never vacuum-collects shared drops. Solo (one
    /// positioned session) is bit-identical to the historical local-only rule.
    /// Suspension freezes the population and syncs previous/current (no
    /// catch-up burst). The world is NOT pumped here — the issue #114
    /// publication ordering stays with the bridge / pumpWorld().
    void advanceItems(double dt, bool suspended, LocalAnchorPolicy anchorPolicy = LocalAnchorPolicy::Include);

    /// Headless world publication: publish finished generation jobs + apply
    /// deferred logical edits. The graphical host does NOT call this — the
    /// bridge's processFinishedJobs() sequences world and render publication
    /// together (issue #114 ordering).
    void pumpWorld();

    // --- Player commands / queries --------------------------------------------
    // Every command exists in two forms: the legacy single-player form (a
    // thin forward to the LOCAL player) and the per-player form taking a
    // PlayerId first (issue #246). Per-player forms answer a safe refusal /
    // no-op for invalid/unknown ids — never another player's state.

    /// Pause/focus transition: clear accumulators + buffered input so no
    /// accumulated time or input is ever replayed (historical semantics).
    void suspendPlayer();
    void suspendPlayer(PlayerId id);
    /// Drop buffered jump input (host UI captured the keyboard).
    void clearPlayerInput();
    void clearPlayerInput(PlayerId id);

    /// Flight/walk toggle at `feet` (graphical host: camera eye - eyeHeight).
    /// Disabling flight validates body clearance at `feet` and REFUSES when
    /// the body overlaps solid or unloaded terrain (status message set,
    /// returns false). On success the controller resets at `feet`. The
    /// per-player form also returns false for an invalid/unknown id.
    bool setPlayerFlight(bool enabled, glm::dvec3 feet);
    bool setPlayerFlight(PlayerId id, bool enabled, glm::dvec3 feet);

    /// Host-driven reposition (flight follow, benchmark/inspection reset):
    /// resets the controller at `feet` (zero velocity, suspended
    /// accumulators) without changing the flight mode.
    void setPlayerPosition(glm::dvec3 feet);
    void setPlayerPosition(PlayerId id, glm::dvec3 feet);

    /// Host LIFECYCLE restore (benchmark/inspection exit, review §4.8):
    /// sets the flight mode and resets at `feet` WITHOUT the gameplay
    /// clearance validation — a scripted camera can end embedded in
    /// terrain, and the validated setPlayerFlight(false) would refuse,
    /// leaving flight stuck on. Gameplay toggles keep setPlayerFlight.
    void restorePlayerFlight(bool enabled, glm::dvec3 feet);
    void restorePlayerFlight(PlayerId id, bool enabled, glm::dvec3 feet);

    /// Surface spawn scan around `around`: closest columns first, bounded by
    /// the bootstrapped region. Success: walking reset at the safe feet.
    /// Fallback: flight at y=100 with a status message. Returns the eye
    /// position the host camera should take. The per-player form answers
    /// `around` unchanged for an invalid/unknown id (the no-world fallback).
    glm::dvec3 spawnPlayerOnSurface(glm::dvec3 around);
    glm::dvec3 spawnPlayerOnSurface(PlayerId id, glm::dvec3 around);

    /// Walking-only surface spawn for server admissions. Failure leaves the
    /// session untouched, including whether it participates in spatial gameplay.
    bool trySpawnWalkingPlayerOnSurface(PlayerId id, glm::dvec3 around);

    /// Saved-state restore (issue #180 review round 6): exact position when
    /// usable, physics::recover() fixes embedding up to one block, a
    /// waitingForTerrain result keeps the position (the solver holds until
    /// the terrain streams in), a clearly-invalid position returns false so
    /// the caller falls back to the surface spawn. Also restores flight mode
    /// and the selected block. Camera orientation stays with the host.
    /// The per-player form also returns false for an invalid/unknown id.
    bool restorePlayerState(const PlayerPersistState &saved);
    bool restorePlayerState(PlayerId id, const PlayerPersistState &saved);

    /// Read <world>/player.state when a world is open; nullopt for a fresh
    /// world or an unusable save (already reported through the error log).
    /// A1 scope: the solo save format carries the LOCAL player (issue #246).
    std::optional<PlayerPersistState> readSavedPlayerState() const;

    /// Authoritative player state + host-supplied presentation orientation,
    /// in the persisted plain-value format (issue #180 Phase 5). The
    /// per-player form answers a default-constructed state for an
    /// invalid/unknown id.
    PlayerPersistState capturePlayerState(float yaw, float pitch) const;
    PlayerPersistState capturePlayerState(PlayerId id, float yaw, float pitch) const;

    /// Authoritative gameplay state in the persisted plain-value formats
    /// (issue #228): the LOCAL inventory (selection + 36 stacks) and the
    /// shared dropped item population + allocator next-id. Exposed for tests
    /// and the #230 runner round-trips; savePlayerState writes them through
    /// WorldPersistence. A1 scope (issue #246): solo format unchanged.
    worldsave::InventoryPersistState captureInventoryState() const;
    worldsave::RemotePlayerState captureRemotePlayerState(PlayerId id) const;
    bool restoreRemotePlayerState(const worldsave::RemotePlayerState &saved, glm::dvec3 spawnCenter);
    worldsave::ItemsPersistState captureItemsState() const;
    void reserveItemIds(uint64_t next, uint64_t limit) { m_items.reserveIds(next, limit); }

    // --- Voxel interaction commands (headless seam) ----------------------------
    // The graphical host issues edits through RenderWorldBridge (it drains
    // the world event stream and registers geometric commit groups from the
    // same result); these forwards are the render-free path for headless
    // hosts and tests. Semantics are the world's (issue #114 deferred edits).

    WorldEditResult placeVoxel(const glm::vec3 &worldPos, TextureType type);
    WorldEditResult deleteVoxel(const glm::vec3 &worldPos);
    bool isVoxelActive(const glm::vec3 &worldPos) const;

    /// Player-aware placement rule (issue #209, Epic #204 Phase E; generalized
    /// to every session by issue #247, Epic #245 Phase A2): a SOLID block may
    /// not be placed where it would overlap ANY authoritative walking player
    /// body — a flying player is exempt (the creative rule), and non-solid
    /// blocks never collide. Only POSITIONED sessions participate (an
    /// unpositioned default-origin body never blocks placement). Bodies are
    /// validated in deterministic ascending-PlayerId order, so overlapping
    /// players are considered reproducibly and no player can be entombed by
    /// another's edit. This is a PURE QUERY, not enforcement: placeVoxel()
    /// below does NOT call it, so a host that wants player-aware placement
    /// MUST gate its edit with canPlaceVoxel() first (the graphical host does;
    /// placeSelectedBlock gates with it internally). It lives here so the rule
    /// is defined once and shared, not copied. Pure query: it never edits the
    /// world. Non-finite or out-of-envelope positions answer false BEFORE any
    /// float->int conversion (issue #247 review). Solo (one positioned session)
    /// is bit-identical to the historical local-body rule.
    bool canPlaceVoxel(const glm::vec3 &worldPos, TextureType type) const;
    /// Actor-aware form (issue #247): validates the actor handle (an unknown /
    /// retired session answers false — it may not even probe placement), then
    /// applies the SAME all-body rule. The rule itself is actor-independent:
    /// every walking body is protected regardless of who asks.
    bool canPlaceVoxel(PlayerId id, const glm::vec3 &worldPos, TextureType type) const;

    // --- Inventory / hotbar gameplay commands (issue #225, Epic #223 Phase B) ---
    // Plain-value arguments, explicit items::ItemResult codes — the future
    // authoritative server's rejection vocabulary (epic #223 §3/§6).
    //
    // Ordering: commands apply IMMEDIATELY at the host frame boundary (the
    // call itself), BEFORE the next advancePlayer/advanceMobs step. There is
    // no command buffer and no fixed-step inventory work in this phase, so
    // suspension never replays or reorders anything: a command issued while
    // the host is suspended applies exactly once, at call time.
    //
    // World-independent: inventories exist before initialize() and survive
    // until initialize/reload resets them (the #228 restore hook lands with
    // persistence). Mutations go through these commands ONLY — the
    // inventory() accessors below are read-only.
    //
    // Per-player forms (issue #246): an invalid/unknown PlayerId answers
    // items::ItemResult::InvalidPlayer WITHOUT touching any inventory (the
    // grant form echoes the canonicalized request in the remainder, the
    // rejected-input discipline). Legacy forms forward to the local player.
    //
    // Deliberately NOT exposed here: items::Inventory::removeCount. The
    // command set is exactly the epic §3 inventory-management commands plus
    // the grant seam; item CONSUMPTION arrives with place-from-hotbar (#227),
    // which removes from the selected slot only on a successful placement.

    /// Select the gameplay hotbar slot (0..kHotbarSize-1); out of range ->
    /// InvalidSlot, selection unchanged. This is the GAMEPLAY selection; the
    /// creative/editor selectedBlock() seam below is deliberately separate
    /// until #227 wires the host.
    items::ItemResult selectHotbarSlot(int index);
    items::ItemResult selectHotbarSlot(PlayerId id, int index);

    /// Whole-stack move/merge between inventory slots (empty destination
    /// receives, same item merges, different items -> ItemMismatch).
    items::ItemResult moveStack(int from, int to);
    items::ItemResult moveStack(PlayerId id, int from, int to);

    /// Unconditional exchange of two slots (any items, empty included).
    items::ItemResult swapStacks(int a, int b);
    items::ItemResult swapStacks(PlayerId id, int a, int b);

    /// Move the larger half (ceil, odd counts defined) of `from` into `to`
    /// with transfer semantics — the authoritative half-split the UI drag
    /// cursor will drive (#229).
    items::ItemResult splitStack(int from, int to);
    items::ItemResult splitStack(PlayerId id, int from, int to);

    /// TEST/ADMIN SEAM (epic #223 §3): grant items without a break/pickup
    /// cause. Every call is EXPLICITLY LOGGED ("[items] grant ...") so
    /// headless runs and future server audits can account for spawned items.
    /// The log records the requested count and — when it differs — the
    /// APPLIED amount: a {None, N} sentinel input collapses to the canonical
    /// empty (an Ok no-op logged with "applied 0"), partial/full/rejected
    /// grants log their remainder and applied count. Non-local grants carry
    /// the player id in the log line (issue #246); the local line keeps the
    /// historical format.
    /// Same partial semantics as the future pickup path: hotbar-first merge,
    /// then empty slots; AddResult.remainder is what did not fit (Full leaves
    /// the inventory untouched). Counts above maxStack distribute across
    /// slots. Not a gameplay rule — walking-mode survival never grants.
    items::AddResult grantItems(items::ItemId item, uint16_t count);
    items::AddResult grantItems(PlayerId id, items::ItemId item, uint16_t count);

    // --- World interaction commands (issues #226/#227, Epic #223 Phase C/D;
    //     actor-aware since issue #247, Epic #245 Phase A2) -------------------
    // The survival-lite break/drop/place loop; the sim raycast below is the
    // targeting rule. The creative/editor placeVoxel / deleteVoxel seams
    // above stay unchanged (infinite-block semantics).
    //
    // Actor-aware forms (issue #247): each command takes a PlayerId first and
    // derives the interaction context (inventory, eye/feet, occupancy) from
    // that server-owned session. An unknown / retired handle answers
    // InvalidPlayer WITHOUT any side effect (no world edit, no inventory
    // consumption, no item spawn). The legacy single-player forms are thin
    // forwards to the LOCAL player — solo behavior is bit-identical.
    //
    // Ordering (single simulation thread, host frame boundary): simultaneous
    // attempts resolve in COMMAND CALL ORDER. Two breaks of one block: the
    // first accepted break deletes it (effective state immediately AIR), the
    // second sees AIR -> WorldRejected, exactly one drop. Two places of one
    // cell: first Ok, second WorldRejected (occupied). Shared-drop claims
    // resolve in ascending-PlayerId pickup order (advanceItems). Per-player
    // inventories never contend across sessions.
    //
    // `outEdit` (optional): on an ACCEPTED edit the world's WorldEditResult
    // (edit id + geometric members) is moved out so the graphical host can
    // publish through RenderWorldBridge::publishEditResult — the SAME event
    // drain + commit-group path the creative edits use, no second edit path
    // (issue #227). Headless callers pass nullptr.

    /// Break a voxel by hand: validates the cell against the item registry
    /// (BEDROCK / fluids refused, unloaded cells never inferred), deletes
    /// through the EXISTING world edit semantics (issue #114 deferred-edit
    /// ordering preserved) and spawns the registry drop at the cell center
    /// with deterministic id/edit-revision jitter. BreakResult distinguishes
    /// Ok / Unbreakable / UnknownTerrain / WorldRejected / IdExhausted
    /// (stabilization PR3: a break whose drop needs a fresh entity id is
    /// refused BEFORE the world edit when the allocator is exhausted — the
    /// block stays, nothing is lost; empty-drop blocks stay breakable).
    /// The break EFFECT is world-shared (the drop belongs to the shared item
    /// population, not the actor); the actor handle is validated for authority
    /// (InvalidPlayer for an unknown session). The legacy form forwards to the
    /// local player.
    BreakResult breakBlock(glm::ivec3 cell, WorldEditResult *outEdit = nullptr);
    BreakResult breakBlock(PlayerId id, glm::ivec3 cell, WorldEditResult *outEdit = nullptr);

    /// Place the selected hotbar block (issue #227): the ACTOR's selected stack
    /// must hold a PLACEABLE item (registry placeableBlock); gates with the
    /// all-body player-aware canPlaceVoxel rule (#209 / #247); edits through
    /// the existing world semantics; CONSUMES EXACTLY 1 ITEM ON SUCCESS ONLY
    /// from the actor's inventory. PlaceResult distinguishes Ok / EmptySlot /
    /// NotPlaceable / PlayerOverlap / UnknownTerrain / WorldRejected /
    /// InvalidPlayer. The legacy form forwards to the local player.
    PlaceResult placeSelectedBlock(glm::ivec3 cell, WorldEditResult *outEdit = nullptr);
    PlaceResult placeSelectedBlock(PlayerId id, glm::ivec3 cell, WorldEditResult *outEdit = nullptr);

    /// Owner-thread preflight for a versioned interaction intent. Walks from
    /// the actor's eye through effective voxels within kInteractionReach.
    /// A placement needs an empty cell on the entry face of the first hit.
    /// Apply an accepted intent through the corresponding gameplay command
    /// in the same owner turn so its preconditions still hold.
    VoxelTargetResult validateVoxelTarget(PlayerId id, VoxelInteraction operation, glm::ivec3 target,
                                          ChunkContentVersion expected, glm::dvec3 direction) const;

    /// Simulation-owned targeting query (issue #227): the historical
    /// Engine::raycastVoxel march behind a render-free API — 0.05 float
    /// step, first ACTIVE cell, adjacent-cell fallback when the ray starts
    /// inside a block, unknown cells never hit. `dir` is normalized here
    /// (the host passes the raw camera front as a plain value); `maxDist`
    /// stays a host-supplied plain value (renderSettings.raycastDistance).
    /// Pure query: never edits the world.
    ///
    /// Authoritative semantics (stabilization PR1/PR2): the march samples
    /// the EFFECTIVE voxel state through a scoped WorldCollisionView —
    /// stored backing overlaid with pending logical edits, the SAME state
    /// breakBlock()/placeSelectedBlock() and physics collide with. A
    /// deferred deletion is therefore already invisible to targeting, and a
    /// deferred placement already targetable, before the edit physically
    /// lands. Fluids and vegetation hit exactly as before (any non-AIR
    /// effective type); the collision view's solid floor / open sky
    /// boundaries are physics policy and do NOT create targeting hits
    /// (below-world / above-sky cells pass through, the historical rule).
    ///
    /// Input validation (stabilization PR1): non-finite origin/direction
    /// components, non-finite or negative maxDist, a degenerate direction
    /// (squared length < 1e-12, evaluated in double so finite-but-huge
    /// components cannot overflow the gate) and origins outside
    /// kRaycastOriginLimit all answer a safe no-hit. maxDist is clamped to
    /// kMaxRaycastDistance and the march is iteration-bounded, so the query
    /// always terminates. Valid gameplay rays keep the historical float
    /// arithmetic bit for bit.
    ///
    /// PRIVILEGED HOST SEAM (issue #247): this raw-origin form accepts a
    /// host-supplied origin + distance (the graphical host passes its camera).
    /// It is the creative/editor targeting path for the trusted local host and
    /// benchmarks — NOT the network-facing gameplay query. An unprivileged
    /// session must use the actor-aware form below, which derives the origin
    /// from the server-owned actor and clamps reach.
    RaycastHit raycastVoxel(glm::vec3 origin, glm::vec3 dir, float maxDist) const;
    /// Grid-plane targeting shared by server validation and client aiming.
    static VoxelTargetTrace traceVoxelTarget(const WorldCollisionView &view, glm::dvec3 eye, glm::dvec3 direction);

    /// Actor-aware targeting query (issue #247, Epic #245 Phase A2): the ray
    /// ORIGIN is derived from the actor's authoritative eye (never client-
    /// supplied) and maxDist is clamped to the server-owned kInteractionReach
    /// (a client may request a shorter distance, never a longer one). The
    /// march itself is the SAME historical effective-state rule as the legacy
    /// form (input validation, 0.05 step, first-active-cell, adjacent
    /// fallback). An unknown / retired actor answers a safe no-hit. Pure
    /// query: never edits the world. For the local actor with origin == eye
    /// and maxDist <= kInteractionReach this is bit-identical to the legacy
    /// raw-origin form (one march implementation).
    RaycastHit raycastVoxel(PlayerId id, glm::vec3 dir, float maxDist) const;

    /// Drop items from an inventory slot: removes up to `count` (clamped,
    /// the removeCount semantics) and spawns one entity at the ACTOR's eye
    /// with a host-supplied plain direction (presentation input, the
    /// savePlayerState(yaw, pitch) pattern) and deterministic speed. A
    /// degenerate OR non-finite direction falls back to +Y. count 0 is an Ok
    /// no-op; invalid slot / empty slot answer InvalidSlot / EmptyStack.
    /// Manual drops carry the longer pickup delay (no instant re-pickup).
    /// An exhausted entity-id allocator answers IdExhausted WITHOUT touching
    /// the inventory (stabilization PR3 conservation). Actor-aware since
    /// issue #247: the per-player form uses the actor's inventory/eye and
    /// answers InvalidPlayer for an unknown session; the legacy form forwards
    /// to the local player.
    items::ItemResult dropFromSlot(int slot, uint16_t count, glm::dvec3 dir);
    items::ItemResult dropFromSlot(PlayerId id, int slot, uint16_t count, glm::dvec3 dir);

    // --- Read-only bounded snapshots -------------------------------------------

    /// The LOCAL player's snapshot (solo-compatibility forward).
    PlayerSnapshot playerSnapshot() const;
    /// One player's snapshot, filled IN PLACE (issue #246). Returns false for
    /// an invalid/unknown id, leaving `out` UNTOUCHED.
    bool playerSnapshot(PlayerId id, PlayerSnapshot &out) const;
    /// Derived mob presentation snapshots (bounded by entities::kMaxMobCount),
    /// filled IN PLACE like MobSystem::renderStates — no intermediate cache or
    /// per-frame vector copy. previous/current interpolation and the #129
    /// animation fields are produced by MobSystem::renderStates; localSkylight
    /// stays UNASSIGNED (< 0) — the graphical host samples the bridge light
    /// caches afterwards, headless consumers ignore it.
    void mobSnapshots(std::vector<MobSnapshot> &out) const { m_mobs.renderStates(out); }
    /// Bounded LOCAL inventory snapshot, filled IN PLACE (issue #225): the
    /// fixed 36-slot array + selected hotbar index, copied from the
    /// authoritative items::Inventory. No allocation, no registry strings —
    /// the UI resolves ItemId display names itself.
    void inventorySnapshot(InventorySnapshot &out) const;
    /// One player's inventory snapshot, filled IN PLACE (issue #246). Returns
    /// false for an invalid/unknown id, leaving `out` UNTOUCHED.
    bool inventorySnapshot(PlayerId id, InventorySnapshot &out) const;
    /// Id-sorted bounded dropped-item snapshots, filled IN PLACE (issue
    /// #226, the mobSnapshots pattern). Interpolate previous -> current with
    /// itemInterpolationAlpha(); counts/ages are authoritative tick values.
    void itemSnapshots(std::vector<ItemEntitySnapshot> &out) const { m_items.snapshots(out); }
    /// Drain the bounded spawn/remove lifecycle stream (the WorldEvent
    /// takeEvents pattern): appends into `out`, empties the stream. Partial
    /// pickups are not events — snapshot counts carry them.
    void takeItemEvents(std::vector<items::ItemEntityEvent> &out) { m_items.takeEvents(out); }
    /// Render interpolation fraction for the item snapshots (accumulator /
    /// fixed step, clamped 0..1).
    double itemInterpolationAlpha() const { return m_items.interpolationAlpha(); }
    WorldSnapshot worldSnapshot() const;

    // --- Identity / accessors ----------------------------------------------------

    bool isInitialized() const { return m_world != nullptr; }
    /// The authoritative world (bridge attach seam; null before initialize).
    VoxelWorld *world() { return m_world.get(); }
    const VoxelWorld *world() const { return m_world.get(); }
    TerrainGenerator *terrainGenerator() { return m_terrainGenerator.get(); }
    const TerrainGenerator *terrainGenerator() const { return m_terrainGenerator.get(); }
    int seed() const { return m_seed; }
    uint64_t worldGenerationId() const { return m_worldGenerationId; }
    /// Why persistence is off despite a requested world (seed mismatch / open
    /// failure); empty otherwise. Surfaced by the host's World UI panel.
    const std::string &openWorldError() const { return m_openWorldError; }

    // Legacy single-player accessors: thin forwards to the LOCAL player
    // session (issue #246 — one source of truth). Per-player queries use
    // playerSnapshot(id, out) / playerFeet(id) / playerFlight(id).
    // Reference/pointer LIFETIME: session storage is heap-backed and
    // pointer-stable — references survive createPlayer()/removePlayer() of
    // OTHER sessions and world epochs (initialize/reload); they are
    // invalidated only when the referenced session itself is removed (or the
    // simulation is destroyed). The bounded snapshots remain the copy-out
    // read seam for consumers that must outlive a session.
    bool playerFlight() const { return localState().flight; }
    glm::dvec3 playerFeet() const { return localState().controller.body.position; }
    glm::dvec3 playerEye() const { return localState().controller.eye(); }
    glm::dvec3 playerRenderEye() const { return localState().controller.renderEye(); }
    const physics::PlayerSettings &playerSettings() const { return localState().controller.settings; }
    const char *playerStatus() const { return localState().status; }
    /// Per-player feet position (issue #246); the zero vector for an
    /// invalid/unknown id (never another player's position).
    glm::dvec3 playerFeet(PlayerId id) const;
    /// Per-player authoritative eye position (issue #247): feet + eyeHeight.
    /// The actor-aware targeting query derives its ray origin from this; the
    /// zero vector for an invalid/unknown id.
    glm::dvec3 playerEye(PlayerId id) const;
    /// Whether a session has been positioned and thus participates in spatial
    /// gameplay (shared pickup + player-body placement checks). The local
    /// anchor is always positioned; non-local sessions become positioned on
    /// the first successful setPlayerPosition / setPlayerFlight / spawn /
    /// restore. False for an invalid/unknown id (issue #247 review).
    bool playerPositioned(PlayerId id) const;
    /// Per-player flight mode (issue #246); false for an invalid/unknown id.
    bool playerFlight(PlayerId id) const;
    /// CREATIVE/EDITOR block selection (flight mode, benchmarks, the `T`
    /// cycle, headless place/delete scripts). Authoritative and persisted
    /// (player.state). Deliberate DUALITY with the gameplay hotbar selection
    /// (issue #225, epic #223 §3): selectHotbarSlot()/inventorySnapshot() are
    /// the survival-lite selection, this stays the infinite-block creative
    /// seam. The two selections never synchronize.
    /// Commands-only mutation (review M6): the UI/host used to write through
    /// a mutable reference, leaking authoritative state across the
    /// sim/render boundary — every mutation now goes through a command, the
    /// inventory discipline. Per-player state since issue #246.
    const TextureType &selectedBlock() const { return localState().selectedBlock; }
    void setSelectedBlock(TextureType type) { localState().selectedBlock = type; }
    void setSelectedBlock(PlayerId id, TextureType type);

    /// Spawn/retire/interest settings — plain values, no Camera/RenderSettings.
    entities::MobSettings &mobSettings() { return m_mobs.settings; }
    const entities::MobSettings &mobSettings() const { return m_mobs.settings; }
    /// Gameplay toggle (authoritative): disabled mobs do not populate/step.
    /// Commands-only mutation (review M6, the selectedBlock pattern).
    bool mobsEnabled() const { return m_mobsEnabled; }
    void setMobsEnabled(bool enabled) { m_mobsEnabled = enabled; }
    /// Diagnostics seam (dropped steps / query stats / population).
    const entities::MobSystem &mobs() const { return m_mobs; }
    /// Read-only LOCAL inventory seam (issue #225): queries/inspection only —
    /// every mutation goes through the gameplay commands above.
    const items::Inventory &inventory() const { return localState().inventory; }
    /// Read-only per-player inventory seam (issue #246); nullptr for an
    /// invalid/unknown id. The pointed-to inventory is pointer-stable until
    /// THAT session is removed (heap-backed storage). Same commands-only
    /// mutation discipline.
    const items::Inventory *inventory(PlayerId id) const;
    /// Read-only dropped-item diagnostics seam (issue #226, the mobs()
    /// pattern): population, dropped steps/events, id-sorted entities.
    const items::ItemEntitySystem &itemEntities() const { return m_items; }
    /// Dropped-item tunables — plain values, no Camera/RenderSettings (the
    /// mobSettings() pattern). Tests tune them for bounded fixtures.
    items::ItemEntitySettings &itemSettings() { return m_items.settings; }
    const items::ItemEntitySettings &itemSettings() const { return m_items.settings; }
    /// Monotonic entity-id high-water mark (issue #226): the #228 persistence
    /// hook captures/restores it so ids are never reused across save/reload.
    uint64_t itemIdHighWater() const { return m_items.idHighWater(); }

    // --- Persistence passthrough (issue #180 contracts; world-owned) -------------

    bool isWorldOpen() const { return m_world && m_world->isWorldOpen(); }
    bool flushWorld() { return m_world && m_world->flushWorld(); }
    WorldPersistence *worldPersistence() { return m_world ? m_world->worldPersistence() : nullptr; }
    const WorldPersistence *worldPersistence() const { return m_world ? m_world->worldPersistence() : nullptr; }

private:
    /// One player session's authoritative gameplay state (issue #246): the
    /// 120 Hz controller, the 36-slot inventory, flight/walk mode, the
    /// creative block selection and the transition status (static literals
    /// only). Plain values + the controller's own fixed-step accumulators —
    /// no references, no heap churn per frame. The shared world/mobs/items
    /// live OUTSIDE this record: removing a session never touches them.
    struct PlayerState
    {
        physics::PlayerController controller;
        uint32_t motionReset{};
        void resetMotion(glm::dvec3 feet)
        {
            controller.reset(feet);
            ++motionReset;
        }
        items::Inventory inventory;
        bool flight{false};
        // Spatial-participation gate (issue #247 review): a session joins
        // shared pickup and player-body placement checks only once the host
        // has placed it (setPlayerPosition / setPlayerFlight / spawn / restore).
        // The local anchor is positioned from construction (solo lifecycle);
        // non-local sessions start false so a default-origin body never
        // vacuum-collects drops or blocks placement before it is spawned.
        // Persists across world epochs like the body itself (initialize/reload
        // carry bodies over); not persisted in the solo save format.
        bool positioned{false};
        int interestRadiusChunks{0};
        TextureType selectedBlock{STONE};
        const char *status{""}; // static literals only
    };
    struct PlayerEntry
    {
        PlayerId id;
        PlayerState state;
    };

    bool isGameplayParticipant(const PlayerEntry &entry, LocalAnchorPolicy anchorPolicy) const;
    worldsave::InventoryPersistState captureInventoryState(PlayerId id) const;
    void restoreInventoryState(PlayerId id, const worldsave::InventoryPersistState &saved);

    /// The local player is always the FIRST entry (created at construction,
    /// never removable, ids monotonic) — O(1) access for the legacy forwards.
    PlayerState &localState() { return m_players.front()->state; }
    const PlayerState &localState() const { return m_players.front()->state; }
    /// Id-sorted binary search; nullptr for invalid/unknown IDs. A persistent
    /// actor can reattach only from the dedicated host.
    PlayerState *findPlayer(PlayerId id);
    const PlayerState *findPlayer(PlayerId id) const;
    /// Build the bounded snapshot value from a session (shared by the legacy
    /// by-value form and the per-player in-place form — one source of truth).
    static PlayerSnapshot snapshotOf(const PlayerState &state);

    RaycastHit raycastMarch(glm::vec3 origin, glm::vec3 dir, float maxDist) const;

    /// Integer-only test for the supported editable-world envelope (issue
    /// #247 review): X/Z in [SHRT_MIN, SHRT_MAX), Y in [0, WORLD_HEIGHT).
    /// Gameplay placement/break validate cells with this BEFORE constructing
    /// any float position, so extreme coordinates never reach a float->int
    /// floor conversion (which would be UB outside the int range).
    static bool cellInWorldEnvelope(glm::ivec3 cell);

    /// Read the persisted gameplay state when a world is open (issue #228,
    /// the readSavedPlayerState pattern): nullopt for a fresh world / old
    /// save (quiet) or an unusable file (already reported through the
    /// persistence error log). finalizePlayerSpawn restores what was read;
    /// a corrupt file degrades to empty, never refuses the world open.
    std::optional<worldsave::InventoryPersistState> readSavedInventoryState() const;
    std::optional<worldsave::ItemsPersistState> readSavedItemsState() const;
    /// Restore the LOCAL inventory + the shared dropped items + allocator
    /// from the saved payloads (validated by the save layer). Called by
    /// finalizePlayerSpawn after the player restore/spawn, independent of its
    /// outcome.
    void restoreGameplayState();

    ThreadPool *m_threadPool{nullptr};

    // Declaration order is destruction order reversed: the world (which
    // references the generator) tears down before the generator it uses.
    std::unique_ptr<TerrainGenerator> m_terrainGenerator;
    std::unique_ptr<VoxelWorld> m_world;

    // Player sessions (issue #246): ID-SORTED by construction (monotonic
    // allocation + order-preserving removal); the local player is entry 0.
    // HEAP-BACKED entries (unique_ptr, PR #261 review): vector growth/erase
    // moves only the pointers, so a PlayerState address is stable for the
    // session's lifetime — accessor references survive creation/removal of
    // OTHER sessions and are invalidated only by removal of the referenced
    // session itself. Entries are allocated at createPlayer() ONLY — the
    // per-frame physics paths never allocate. Sessions survive
    // initialize/reload (world-epoch boundaries reset every inventory, not
    // the sessions); the allocator never rewinds, so retired ids are never
    // reused. Bounded by kMaxPlayers.
    std::vector<std::unique_ptr<PlayerEntry>> m_players;
    uint32_t m_nextPlayerId{1};

    // Reusable pickup-candidate buffer (issue #247): rebuilt in ascending
    // PlayerId order each advanceItems (the deterministic pickup priority).
    // clear() + refill keeps the reserved capacity — NO per-frame allocation
    // after the first frame (the snapshot buffer-reuse pattern). The pointers
    // reference the heap-backed sessions, valid for the whole advance.
    std::vector<items::PickupCandidate> m_pickupCandidates;
    std::vector<VoxelWorld::PlayerInterest> m_streamingInterests;

    entities::MobSystem m_mobs;
    // Dropped item entities (issue #226): owns the monotonic id allocator,
    // the fixed-step accumulator and the bounded event stream. Plain values
    // + owning containers, no references — destruction-order independent.
    // initialize/reload reset it (session boundary); #228 persists the
    // id high-water mark and the population. SHARED across player sessions.
    items::ItemEntitySystem m_items;

    bool m_mobsEnabled{true};

    int m_seed{0};
    uint64_t m_worldGenerationId{1};
    std::string m_openWorldError;
    std::optional<PlayerPersistState> m_savedPlayer;
    // Gameplay restore payloads (issue #228), read at initialize alongside
    // m_savedPlayer and applied to the LOCAL player by finalizePlayerSpawn.
    std::optional<worldsave::InventoryPersistState> m_savedInventory;
    std::optional<worldsave::ItemsPersistState> m_savedItems;
};

} // namespace sim
