#pragma once

// =============================================================================
// GameSimulation — the CPU-owned simulation facade (issue #208, Epic #204
// Phase D). SINGLE owner of the authoritative gameplay state:
//
//   * world identity: the TerrainGenerator (seed + kGeneratorVersion) and the
//     VoxelWorld (chunk residency/streaming, logical edits, generation
//     publication, WorldPersistence — the issue #206 contracts);
//   * player: physics::PlayerController at the fixed 120 Hz step, flight/walk
//     mode, spawn/restore, selected block, transition status;
//   * passive mobs: entities::MobSystem at the fixed 60 Hz step over the
//     CPU-only WorldMobView (spawn/retire/interest radii are plain values,
//     independent of Camera / RenderSettings);
//   * inventory: items::Inventory (36 slots, hotbar 0..8) with explicit
//     gameplay commands + a bounded value snapshot (issue #225, Epic #223
//     Phase B; the item domain contracts are issue #224 / src/Items/);
//   * session identity: the world generation / reload counter.
//
// The facade is render-free (enforced by tests/check_core_includes.py): no
// Camera, RenderSettings, SDL, Vulkan or ImGui types, and no engine singleton
// state. The host supplies plain values — elapsed time, normalized player
// commands, an observer/interest point — and consumes read-only, bounded
// VALUE snapshots (player, mob transforms/animation, world identity + edit
// revision). Fixed stepping stays internal to the controllers; the maxSteps
// catch-up cap is unchanged (Epic #204 invariant).
//
// State / policy boundaries (issue #208):
//   * SDL key/mouse -> normalized commands is HOST responsibility; camera
//     orientation and render interpolation are presentation. The simulation
//     owns the authoritative feet position, flight mode and block selection.
//   * Pause/focus/benchmark POLICY stays with the host: it decides WHEN to
//     suspend (advancePlayer/advanceMobs take an explicit flag). The
//     simulation implements the suspension semantics: accumulators cleared,
//     previous/current synced, NO catch-up burst after suspension.
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
#include <Physics/PlayerController.hpp>
#include <World/VoxelWorld.hpp>
#include <World/WorldPersistence.hpp>

#include <array>

namespace sim
{

/// Read-only per-frame player snapshot: plain values, bounded, no renderer
/// types. `renderEye` is the interpolated eye the presentation camera follows
/// (previous -> current at the accumulator fraction); `position` is the
/// authoritative feet position. `status` points at static literals only.
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

class GameSimulation
{
public:
    /// Bootstrap radius (chunks) shared by the terrain bootstrap and the
    /// surface-spawn scan (the historical Engine::kBootstrapRadius).
    static constexpr int kBootstrapRadiusChunks = 2;

    /// World-open request (issue #180 identity rules apply): an existing save
    /// keeps its stored seed; an explicit seed that contradicts the save runs
    /// the session transient (openWorldError() reports why).
    struct Config
    {
        int seed{0};               // <= 0: stored seed, else a random roll
        std::string worldName;     // empty: transient session (no persistence)
        std::filesystem::path savesRoot{"saves"};
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
    /// for fully synchronous headless fixtures, like VoxelWorld).
    explicit GameSimulation(ThreadPool *threadPool);
    /// Final safety net: force-closes an still-open persistence (the host's
    /// explicit shutdown() is the durable path that also writes player.state).
    ~GameSimulation();

    GameSimulation(const GameSimulation &) = delete;
    GameSimulation &operator=(const GameSimulation &) = delete;

    // --- Lifecycle (explicit reset/open/close/reload) -------------------------

    /// Resolve the seed, create the authoritative world, open persistence
    /// when requested and read the saved player state. Returns the bootstrap
    /// center: the SAVED player position when one exists (issue #180 review
    /// round 6 — the initial area must generate around it), else
    /// `fallbackCenter`. Terrain is NOT generated here: the graphical host
    /// bootstraps through RenderWorldBridge::generateInitialArea (voxels +
    /// render binding/meshing/upload in one step), a headless host calls
    /// bootstrapTerrain(). Follow with finalizePlayerSpawn().
    /// Player flight mode, block selection and status carry over from a
    /// previous session until finalizePlayerSpawn (restore/spawn) sets them —
    /// the historical Engine behavior. The inventory resets to EMPTY here
    /// (issue #225; the inventory.state restore hook lands in #228).
    glm::dvec3 initialize(const Config &config, glm::dvec3 fallbackCenter);

    /// Headless synchronous voxel bootstrap: the world half of the bridge's
    /// generateInitialArea, with no render handles involved.
    void bootstrapTerrain(glm::dvec3 center);

    /// After the terrain bootstrap: restore the validated saved player, or
    /// fall back to the surface spawn around `fallbackAround` (the graphical
    /// host passes its camera position — the historical scan anchor; a
    /// headless host passes the bootstrap center). Camera orientation is
    /// presentation — the outcome carries the saved yaw/pitch for the host.
    SpawnOutcome finalizePlayerSpawn(glm::dvec3 fallbackAround);

    /// Close (refusable) and recreate the world for `newSeed` (<= 0 -> 42,
    /// the historical reload default). Returns FALSE and keeps the OLD world
    /// authoritative when the persistent close is refused (stranded logical
    /// edits — issue #180 review round 4). The graphical host quiesces and
    /// closes through the bridge FIRST (render jobs hold record leases), then
    /// destroys the bridge before calling this. Terrain bootstrap and player
    /// spawn are separate steps (the benchmark host uses its own placement).
    /// Reloaded worlds are transient: persistence stays off afterwards.
    /// A SUCCESSFUL reload resets the inventory to empty (issue #225); a
    /// REFUSED reload keeps the old world AND the old inventory authoritative.
    /// The player body is intentionally LEFT to the host's spawn step: a
    /// headless host that reloads without re-spawning keeps the previous body
    /// (possibly in an unloaded region) until it calls finalizePlayerSpawn /
    /// spawnPlayerOnSurface.
    bool reload(int newSeed);

    /// Durable player-state capture + flush (no close). The host supplies the
    /// presentation orientation (camera yaw/pitch); everything else is
    /// authoritative sim state. Call while the render bridge is still alive,
    /// before shutdown(). Returns false when nothing is open or I/O failed
    /// (failures are logged, exactly like the historical Engine shutdown).
    bool savePlayerState(float yaw, float pitch);

    /// Destructor-only close: resolve stranded edits, capture, flush and
    /// shut down UNCONDITIONALLY (no retry point at destruction). Idempotent;
    /// the graphical host calls it after destroying the render bridge.
    void shutdown();

    // --- Per-frame advance (explicit elapsed time + plain values) -------------

    /// One host frame of player physics (fixed 120 Hz internally).
    /// `suspended` implements the host's pause/focus/benchmark policy: no
    /// stepping, accumulators cleared, no catch-up burst afterwards. In
    /// flight mode the body is host-driven (setPlayerPosition each frame);
    /// only the walking controller steps here.
    void advancePlayer(double dt, const PlayerCommand &command, bool suspended);

    /// One host frame of passive mobs (fixed 60 Hz internally). `observer` is
    /// the interest point (camera position or a headless probe),
    /// `availableRadius` the streamed radius in blocks (plain value — the
    /// graphical host passes its view distance). `suspended` freezes the
    /// population and syncs previous/current for interpolation; the
    /// sim-owned mobsEnabled() toggle suspends it as well.
    void advanceMobs(double dt, glm::dvec3 observer, double availableRadius, bool suspended);

    /// Headless world publication: publish finished generation jobs + apply
    /// deferred logical edits. The graphical host does NOT call this — the
    /// bridge's processFinishedJobs() sequences world and render publication
    /// together (issue #114 ordering).
    void pumpWorld();

    // --- Player commands / queries --------------------------------------------

    /// Pause/focus transition: clear accumulators + buffered input so no
    /// accumulated time or input is ever replayed (historical semantics).
    void suspendPlayer();
    /// Drop buffered jump input (host UI captured the keyboard).
    void clearPlayerInput();

    /// Flight/walk toggle at `feet` (graphical host: camera eye - eyeHeight).
    /// Disabling flight validates body clearance at `feet` and REFUSES when
    /// the body overlaps solid or unloaded terrain (status message set,
    /// returns false). On success the controller resets at `feet`.
    bool setPlayerFlight(bool enabled, glm::dvec3 feet);

    /// Host-driven reposition (flight follow, benchmark/inspection reset):
    /// resets the controller at `feet` (zero velocity, suspended
    /// accumulators) without changing the flight mode.
    void setPlayerPosition(glm::dvec3 feet);

    /// Surface spawn scan around `around`: closest columns first, bounded by
    /// the bootstrapped region. Success: walking reset at the safe feet.
    /// Fallback: flight at y=100 with a status message. Returns the eye
    /// position the host camera should take.
    glm::dvec3 spawnPlayerOnSurface(glm::dvec3 around);

    /// Saved-state restore (issue #180 review round 6): exact position when
    /// usable, physics::recover() fixes embedding up to one block, a
    /// waitingForTerrain result keeps the position (the solver holds until
    /// the terrain streams in), a clearly-invalid position returns false so
    /// the caller falls back to the surface spawn. Also restores flight mode
    /// and the selected block. Camera orientation stays with the host.
    bool restorePlayerState(const PlayerPersistState &saved);

    /// Read <world>/player.state when a world is open; nullopt for a fresh
    /// world or an unusable save (already reported through the error log).
    std::optional<PlayerPersistState> readSavedPlayerState() const;

    /// Authoritative player state + host-supplied presentation orientation,
    /// in the persisted plain-value format (issue #180 Phase 5).
    PlayerPersistState capturePlayerState(float yaw, float pitch) const;

    // --- Voxel interaction commands (headless seam) ----------------------------
    // The graphical host issues edits through RenderWorldBridge (it drains
    // the world event stream and registers geometric commit groups from the
    // same result); these forwards are the render-free path for headless
    // hosts and tests. Semantics are the world's (issue #114 deferred edits).

    WorldEditResult placeVoxel(const glm::vec3 &worldPos, TextureType type);
    WorldEditResult deleteVoxel(const glm::vec3 &worldPos);
    bool isVoxelActive(const glm::vec3 &worldPos) const;

    /// Player-aware placement rule (issue #209, Epic #204 Phase E): a SOLID
    /// block may not be placed where it would overlap the authoritative
    /// player body — unless the player is flying or the block is non-solid.
    /// This is a PURE QUERY, not enforcement: placeVoxel() below does NOT call
    /// it, so a host that wants player-aware placement MUST gate its edit with
    /// canPlaceVoxel() first (the graphical host does; headless hosts/tests may
    /// deliberately place inside the body by calling placeVoxel() directly).
    /// It lives here so the rule is defined once and shared, not copied.
    /// Pure query: it never edits the world.
    bool canPlaceVoxel(const glm::vec3 &worldPos, TextureType type) const;

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
    // World-independent: the inventory exists before initialize() and
    // survives until initialize/reload resets it (the #228 restore hook lands
    // with persistence). Mutations go through these commands ONLY — the
    // inventory() accessor below is read-only.
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

    /// Whole-stack move/merge between inventory slots (empty destination
    /// receives, same item merges, different items -> ItemMismatch).
    items::ItemResult moveStack(int from, int to);

    /// Unconditional exchange of two slots (any items, empty included).
    items::ItemResult swapStacks(int a, int b);

    /// Move the larger half (ceil, odd counts defined) of `from` into `to`
    /// with transfer semantics — the authoritative half-split the UI drag
    /// cursor will drive (#229).
    items::ItemResult splitStack(int from, int to);

    /// TEST/ADMIN SEAM (epic #223 §3): grant items without a break/pickup
    /// cause. Every call is EXPLICITLY LOGGED ("[items] grant ...") so
    /// headless runs and future server audits can account for spawned items.
    /// The log records the requested count and — when it differs — the
    /// APPLIED amount: a {None, N} sentinel input collapses to the canonical
    /// empty (an Ok no-op logged with "applied 0"), partial/full/rejected
    /// grants log their remainder and applied count.
    /// Same partial semantics as the future pickup path: hotbar-first merge,
    /// then empty slots; AddResult.remainder is what did not fit (Full leaves
    /// the inventory untouched). Counts above maxStack distribute across
    /// slots. Not a gameplay rule — walking-mode survival never grants.
    items::AddResult grantItems(items::ItemId item, uint16_t count);

    // --- Read-only bounded snapshots -------------------------------------------

    PlayerSnapshot playerSnapshot() const;
    /// Derived mob presentation snapshots (bounded by entities::kMaxMobCount),
    /// filled IN PLACE like MobSystem::renderStates — no intermediate cache or
    /// per-frame vector copy. previous/current interpolation and the #129
    /// animation fields are produced by MobSystem::renderStates; localSkylight
    /// stays UNASSIGNED (< 0) — the graphical host samples the bridge light
    /// caches afterwards, headless consumers ignore it.
    void mobSnapshots(std::vector<MobSnapshot> &out) const { m_mobs.renderStates(out); }
    /// Bounded inventory snapshot, filled IN PLACE (issue #225): the fixed
    /// 36-slot array + selected hotbar index, copied from the authoritative
    /// items::Inventory. No allocation, no registry strings — the UI resolves
    /// ItemId display names itself.
    void inventorySnapshot(InventorySnapshot &out) const;
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

    bool playerFlight() const { return m_playerFlight; }
    glm::dvec3 playerFeet() const { return m_player.body.position; }
    glm::dvec3 playerEye() const { return m_player.eye(); }
    glm::dvec3 playerRenderEye() const { return m_player.renderEye(); }
    const physics::PlayerSettings &playerSettings() const { return m_player.settings; }
    const char *playerStatus() const { return m_playerStatus; }
    /// CREATIVE/EDITOR block selection (flight mode, benchmarks, the `T`
    /// cycle, headless place/delete scripts). Authoritative and persisted
    /// (player.state); the UI mutates it through this reference. Deliberate
    /// DUALITY with the gameplay hotbar selection (issue #225, epic #223
    /// §3): selectHotbarSlot()/inventorySnapshot() are the survival-lite
    /// selection, this reference stays the infinite-block creative seam
    /// until #227 wires the host; #228 migrates the persistence. The two
    /// selections never synchronize in this phase.
    TextureType &selectedBlock() { return m_selectedBlock; }
    const TextureType &selectedBlock() const { return m_selectedBlock; }

    /// Spawn/retire/interest settings — plain values, no Camera/RenderSettings.
    entities::MobSettings &mobSettings() { return m_mobs.settings; }
    const entities::MobSettings &mobSettings() const { return m_mobs.settings; }
    /// Gameplay toggle (authoritative): disabled mobs do not populate/step.
    bool &mobsEnabled() { return m_mobsEnabled; }
    bool mobsEnabled() const { return m_mobsEnabled; }
    /// Diagnostics seam (dropped steps / query stats / population).
    const entities::MobSystem &mobs() const { return m_mobs; }
    /// Read-only inventory seam (issue #225): queries/inspection only —
    /// every mutation goes through the gameplay commands above.
    const items::Inventory &inventory() const { return m_inventory; }

    // --- Persistence passthrough (issue #180 contracts; world-owned) -------------

    bool isWorldOpen() const { return m_world && m_world->isWorldOpen(); }
    bool flushWorld() { return m_world && m_world->flushWorld(); }
    WorldPersistence *worldPersistence() { return m_world ? m_world->worldPersistence() : nullptr; }
    const WorldPersistence *worldPersistence() const { return m_world ? m_world->worldPersistence() : nullptr; }

private:
    ThreadPool *m_threadPool{nullptr};

    // Declaration order is destruction order reversed: the world (which
    // references the generator) tears down before the generator it uses.
    std::unique_ptr<TerrainGenerator> m_terrainGenerator;
    std::unique_ptr<VoxelWorld> m_world;

    physics::PlayerController m_player;
    entities::MobSystem m_mobs;
    // Plain value (no references, no heap): destruction-order independent,
    // declared with the other owned gameplay state. Reset to empty by
    // initialize/reload; the #228 restore hook will refill it from
    // inventory.state on the same lifecycle seams as player.state.
    items::Inventory m_inventory;

    bool m_playerFlight{false};
    bool m_mobsEnabled{true};
    TextureType m_selectedBlock{STONE};
    const char *m_playerStatus{""}; // static literals only

    int m_seed{0};
    uint64_t m_worldGenerationId{1};
    std::string m_openWorldError;
    std::optional<PlayerPersistState> m_savedPlayer;
};

} // namespace sim
