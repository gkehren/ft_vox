// GameSimulation — CPU-owned simulation facade (issue #208, Epic #204 Phase D;
// issue #246, Epic #245 Phase A: shared world authority + per-player sessions).
// The player/mob/world rules implemented here are the historical Engine rules
// moved verbatim behind a render-free API: fixed-step rates, pause/focus
// suspension semantics, spawn/restore validation, reload refusal and the
// issue #180 persistence contracts are unchanged. The legacy single-player
// APIs are thin forwards to the LOCAL player session — one source of truth.
// See the header for the ownership / policy boundaries and
// docs/simulation-render-contracts.md (§11 for the multiplayer split).
#include "Sim/GameSimulation.hpp"

#include <Items/ItemRegistry.hpp>
#include <Items/ItemStack.hpp>
#include <Physics/BlockPhysics.hpp>
#include <World/WorldCollisionView.hpp>
#include <World/WorldMobView.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <utility>

namespace sim
{
namespace
{
// Stable result names for the grant audit log (the items::ItemResult
// vocabulary is the future server rejection code set, epic #223 §6).
const char *itemResultName(items::ItemResult result)
{
    switch (result)
    {
    case items::ItemResult::Ok:
        return "Ok";
    case items::ItemResult::Partial:
        return "Partial";
    case items::ItemResult::Full:
        return "Full";
    case items::ItemResult::InvalidSlot:
        return "InvalidSlot";
    case items::ItemResult::EmptyStack:
        return "EmptyStack";
    case items::ItemResult::InvalidItem:
        return "InvalidItem";
    case items::ItemResult::ItemMismatch:
        return "ItemMismatch";
    case items::ItemResult::NotPlaceable:
        return "NotPlaceable";
    case items::ItemResult::IdExhausted:
        return "IdExhausted";
    case items::ItemResult::InvalidPlayer:
        return "InvalidPlayer";
    }
    return "Unknown";
}
} // namespace

GameSimulation::GameSimulation(ThreadPool *threadPool)
    : m_threadPool(threadPool)
{
    // The local player is the solo-compatibility anchor (issue #246): created
    // at construction so every legacy API is total before initialize(), always
    // the first session (ids are monotonic) and never removable. Heap-backed
    // entry: its address stays stable for the simulation's lifetime.
    m_players.push_back(std::make_unique<PlayerEntry>(PlayerEntry{PlayerId{m_nextPlayerId++}, PlayerState{}}));
    // The local anchor participates in spatial gameplay from construction (the
    // historical solo lifecycle — its body is authoritative before initialize).
    // Non-local sessions start UNPOSITIONED and join pickup / placement checks
    // only once the host places them (issue #247 review).
    m_players.front()->state.positioned = true;
    // Pickup-candidate buffer (issue #247): reserve the session ceiling once so
    // the per-frame advanceItems refill never allocates (the snapshot pattern).
    m_pickupCandidates.reserve(kMaxPlayers);
    m_streamingInterests.reserve(kMaxPlayers);
}

GameSimulation::~GameSimulation()
{
    // Final safety net (mirrors ~VoxelWorld): a host that forgot the explicit
    // shutdown still persists accepted edits. After shutdown() this is a no-op.
    shutdown();
}

// ---------------------------------------------------------------------------
// Player sessions (issue #246, Epic #245 Phase A)
// ---------------------------------------------------------------------------

PlayerId GameSimulation::createPlayer()
{
    // Population cap + id-exhaustion guard (the items spawn discipline): a
    // refused creation allocates NOTHING — the id counter does not advance.
    if (m_players.size() >= kMaxPlayers || m_nextPlayerId >= kMaxNextPlayerId)
        return kInvalidPlayerId;
    const PlayerId id{m_nextPlayerId++};
    // Monotonic ids + append = id-sorted order by construction (the
    // ItemEntitySystem pattern); no re-sort on the session lifecycle path.
    // The heap-backed entry keeps its address across later vector growth —
    // accessor references to THIS session stay valid (PR #261 review).
    m_players.push_back(std::make_unique<PlayerEntry>(PlayerEntry{id, PlayerState{}}));
    return id;
}

bool GameSimulation::attachPersistentPlayer(PlayerId id)
{
    if (!m_world || !m_world->dedicatedPersistence() || id.value <= localPlayerId().value ||
        id.value > kMaxPlayerId || m_players.size() >= kMaxPlayers)
        return false;
    const auto it = std::lower_bound(m_players.begin(), m_players.end(), id,
                                     [](const std::unique_ptr<PlayerEntry> &entry, PlayerId wanted)
                                     { return entry->id < wanted; });
    if (it != m_players.end() && (*it)->id == id)
        return false;
    m_players.insert(it, std::make_unique<PlayerEntry>(PlayerEntry{id, PlayerState{}}));
    m_nextPlayerId = std::max(m_nextPlayerId, id.value + 1);
    return true;
}

bool GameSimulation::removePlayer(PlayerId id)
{
    // The local player is the legacy-API anchor: removing it would leave the
    // solo forwards dangling, so it is refused. Invalid/unknown ids are
    // refused without any change.
    if (!id.valid() || id == localPlayerId())
        return false;
    const auto it = std::lower_bound(m_players.begin(), m_players.end(), id,
                                     [](const std::unique_ptr<PlayerEntry> &entry, PlayerId wanted)
                                     { return entry->id < wanted; });
    if (it == m_players.end() || !((*it)->id == id))
        return false;
    // Order-preserving erase: the shared world/mobs/items and every other
    // session are untouched (their heap entries do not move — only this
    // session's references are invalidated). Items this player dropped stay
    // in the shared population (they belong to the world, not the session).
    m_players.erase(it);
    return true;
}

bool GameSimulation::hasPlayer(PlayerId id) const
{
    return findPlayer(id) != nullptr;
}

void GameSimulation::playerIds(std::vector<PlayerId> &out) const
{
    // In-place fill (the snapshot pattern): ascending id order, deterministic.
    out.clear();
    out.reserve(m_players.size());
    for (const std::unique_ptr<PlayerEntry> &entry : m_players)
        out.push_back(entry->id);
}

const GameSimulation::PlayerState *GameSimulation::findPlayer(PlayerId id) const
{
    if (!id.valid())
        return nullptr;
    const auto it = std::lower_bound(m_players.begin(), m_players.end(), id,
                                     [](const std::unique_ptr<PlayerEntry> &entry, PlayerId wanted)
                                     { return entry->id < wanted; });
    if (it == m_players.end() || !((*it)->id == id))
        return nullptr;
    return &(*it)->state;
}

GameSimulation::PlayerState *GameSimulation::findPlayer(PlayerId id)
{
    // Single lookup implementation (the const overload); the cast is safe —
    // the non-const caller holds a non-const simulation.
    return const_cast<PlayerState *>(std::as_const(*this).findPlayer(id));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

glm::dvec3 GameSimulation::initialize(const Config &config, glm::dvec3 fallbackCenter)
{
    int seed = config.seed;
    if (seed <= 0)
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(100000, 999999);
        seed = dis(gen);
    }

    // Persistent-world seed resolution (issue #180, Phase 5): must run BEFORE
    // the generator and mobs consume the seed. An existing world keeps its
    // stored identity; the rolled seed is only a default for new worlds.
    bool wantPersistence = !config.worldName.empty();
    m_openWorldError.clear();
    if (wantPersistence)
    {
        int storedSeed = 0;
        std::string seedError;
        if (WorldPersistence::peekStoredSeed(config.savesRoot.string(), config.worldName, storedSeed, seedError))
        {
            if (config.seed > 0 && storedSeed != config.seed)
            {
                // Explicit seed contradicts the save: never overwrite identity
                // — run this session transient instead.
                m_openWorldError = "stored seed " + std::to_string(storedSeed) +
                                   " != requested seed " + std::to_string(config.seed) +
                                   " (running this session without persistence)";
                std::cout << "[world] refusing to open '" << config.worldName << "': "
                          << m_openWorldError << "\n";
                wantPersistence = false;
            }
            else
            {
                seed = storedSeed;
            }
        }
        // else: the world does not exist yet (created with the current seed
        // below) or its meta is unreadable — openWorld() reports the exact
        // reason either way; the session keeps the current seed.
    }

    ++m_worldGenerationId;
    m_seed = seed;
    m_savedPlayer.reset();
    m_savedInventory.reset();
    m_savedItems.reset();
    m_mobs.reset(static_cast<uint64_t>(seed));
    // Fresh world epoch = empty inventories for EVERY session (issue #225
    // generalized by #246). The local inventory.state restore hook lands in
    // #228 on the finalizePlayerSpawn seam. Sessions themselves survive the
    // epoch boundary; the player-id allocator never rewinds.
    for (std::unique_ptr<PlayerEntry> &entry : m_players)
        entry->state.inventory.clear();
    // Fresh session = no dropped items, id allocator restarted (issue #226).
    // The items.state hook (population + id high-water) lands in #228.
    m_items.reset();
    // Tear the old world down BEFORE replacing the generator it references
    // (~VoxelWorld's shutdown safety net can still generate stranded-edit
    // records through the generator).
    m_world.reset();
    m_terrainGenerator = std::make_unique<TerrainGenerator>(seed);
    m_world = std::make_unique<VoxelWorld>(m_terrainGenerator.get(), m_threadPool);

    // Open (or create) the save AFTER the world exists but BEFORE any
    // generation, so chunk edits from the first frame are already tracked.
    // Failure is not fatal: the session continues without persistence.
    if (wantPersistence)
    {
        std::string error;
        if (m_world->openWorld(config.savesRoot.string(), config.worldName, error,
                              config.dedicatedServer, config.existingWorldOnly))
        {
            std::cout << "[world] opened '" << config.worldName << "' (seed " << seed << ")\n";
        }
        else
        {
            m_openWorldError = error;
            std::cout << "[world] failed to open '" << config.worldName << "': " << error << "\n";
        }
    }

    // Restored-player bootstrap (issue #180 review round 6): player.state is
    // read BEFORE the terrain bootstrap so the initial area generates around
    // the SAVED position — a player who logged out at chunk (100, -80) must
    // not bootstrap around the default origin and wait for streaming to walk
    // the whole distance.
    m_savedPlayer = readSavedPlayerState();
    // Gameplay restore payloads (issue #228): read BEFORE the terrain
    // bootstrap, alongside the player state. Missing files (fresh world /
    // old save) are quiet; corrupt files are already reported through the
    // persistence error log and degrade to empty at restore.
    m_savedInventory = readSavedInventoryState();
    m_savedItems = readSavedItemsState();
    glm::dvec3 center = fallbackCenter;
    if (m_savedPlayer)
    {
        center = glm::dvec3(m_savedPlayer->x,
                            m_savedPlayer->y + localState().controller.settings.eyeHeight,
                            m_savedPlayer->z);
    }
    return center;
}

void GameSimulation::bootstrapTerrain(glm::dvec3 center)
{
    if (m_world)
        m_world->generateInitialAreaVoxels(glm::vec3(center), kBootstrapRadiusChunks);
}

GameSimulation::SpawnOutcome GameSimulation::finalizePlayerSpawn(glm::dvec3 fallbackAround)
{
    SpawnOutcome out;
    if (m_savedPlayer && restorePlayerState(*m_savedPlayer))
    {
        out.restored = true;
        out.yaw = m_savedPlayer->yaw;
        out.pitch = m_savedPlayer->pitch;
        out.eye = localState().controller.renderEye();
    }
    else
    {
        out.eye = spawnPlayerOnSurface(fallbackAround);
    }
    // Gameplay restore (issue #228) is independent of the player outcome:
    // a player fallback to the surface spawn still keeps the saved items.
    restoreGameplayState();
    return out;
}

bool GameSimulation::reload(int newSeed)
{
    // Deliberate (issue #180): benchmark / reload worlds are transient — close
    // (and flush) any world opened for interactive play so the run can never
    // write into the user's save. The close can be REFUSED (stranded logical
    // edits — issue #180 review round 3): aborting the reload keeps the old
    // world authoritative. The graphical host has already quiesced + closed
    // through the bridge (render jobs hold record leases), so this world-side
    // close is the headless path / a no-op there.
    if (m_world && m_world->isWorldOpen())
    {
        if (!m_world->closeWorld())
        {
            std::cerr << "[world] reload aborted: persistent world could not be closed safely"
                      << std::endl;
            return false;
        }
    }

    const int seed = newSeed > 0 ? newSeed : 42;
    ++m_worldGenerationId;
    m_seed = seed;
    m_openWorldError.clear();
    m_savedPlayer.reset();
    m_savedInventory.reset();
    m_savedItems.reset();
    // Successful reload = fresh inventories for EVERY session (issue #225
    // generalized by #246). A REFUSED reload returned above and keeps the old
    // inventories authoritative. Sessions + player ids survive the reload.
    for (std::unique_ptr<PlayerEntry> &entry : m_players)
        entry->state.inventory.clear();
    // Successful reload = fresh dropped-item session (issue #226): the
    // population, events and id allocator reset with the world.
    m_items.reset();

    // The bridge (if any) was destroyed by the host before this call; the
    // world references the generator, so tear it down first.
    m_world.reset();
    m_terrainGenerator.reset();
    m_mobs.reset(static_cast<uint64_t>(seed));
    m_terrainGenerator = std::make_unique<TerrainGenerator>(seed);
    m_world = std::make_unique<VoxelWorld>(m_terrainGenerator.get(), m_threadPool);
    return true;
}

bool GameSimulation::savePlayerState(float yaw, float pitch, const worldsave::PlayerRoster *roster)
{
    if (!m_world || !m_world->isWorldOpen() || m_world->dedicatedPersistence() != (roster != nullptr))
        return false;
    WorldPersistence *persistence = m_world->worldPersistence();
    if (!persistence)
        return false;
    // Stabilization PR5b (Phase 3): the durability barrier runs FIRST, then
    // the FULL gameplay capture (player + inventory + items + allocator
    // high-water) is committed as ONE transactional checkpoint. The capture
    // and the flushed chunk overrides belong to the same logical boundary —
    // a crash recovers either the previous complete checkpoint or this one,
    // never a mixture. Success is reported ONLY when the manifest committed.
    if (!m_world->flushWorld())
    {
        std::cout << "[world] flush failed: " << persistence->status().lastError << "\n";
        return false;
    }
    const PlayerPersistState playerState = capturePlayerState(yaw, pitch);
    worldsave::CheckpointPayloads payloads;
    payloads.playerPresent = true;
    payloads.player = worldsave::encodePlayerState(playerState);
    payloads.inventory = worldsave::encodeInventoryState(captureInventoryState());
    payloads.items = worldsave::encodeItemsState(captureItemsState());
    payloads.worldEditRevision = m_world->nextEditId();
    if (roster)
        payloads.roster = worldsave::encodePlayerRoster(*roster);
    if (!persistence->commitCheckpoint(payloads))
    {
        std::cout << "[world] checkpoint commit failed: " << persistence->status().lastError << "\n";
        return false;
    }
    if (roster)
        return true;
    // Legacy compatibility shims (rollback support, ADR §2.7): best-effort —
    // the committed checkpoint is authoritative, so a shim write failure is
    // logged but is NOT a save failure. Failures stay retryable (the next
    // save rewrites them).
    if (!persistence->writePlayerState(playerState))
        std::cout << "[world] player state shim write failed\n";
    if (!persistence->writeInventoryState(captureInventoryState()))
        std::cout << "[world] inventory state shim write failed\n";
    if (!persistence->writeItemsState(captureItemsState()))
        std::cout << "[world] items state shim write failed\n";
    return true;
}

void GameSimulation::shutdown()
{
    if (m_world && m_world->isWorldOpen())
    {
        if (m_world->dedicatedPersistence())
        {
            m_world->forceCloseWorldForShutdown();
            return;
        }
        // Gameplay safety net (issue #228): inventory + items are fully
        // authoritative (no host presentation input), so the unconditional
        // close captures them even when the host forgot savePlayerState.
        // player.state still needs the camera orientation — only the
        // explicit savePlayerState path captures it; the checkpoint CARRIES
        // the previous player payload forward (stabilization PR5b).
        if (WorldPersistence *persistence = m_world->worldPersistence())
        {
            // Legacy compatibility shims (best-effort, rollback support).
            if (!persistence->writeInventoryState(captureInventoryState()))
                std::cerr << "[world] final shutdown: inventory state write failed" << std::endl;
            if (!persistence->writeItemsState(captureItemsState()))
                std::cerr << "[world] final shutdown: items state write failed" << std::endl;
        }
        // Unconditional destructor-only close: resolve stranded edits,
        // capture, flush, commit the checkpoint with the captured gameplay
        // payloads, shutdown. Destruction has no retry point; a partial
        // save must never be reported as success (issue #180 review round 4).
        worldsave::CheckpointPayloads payloads;
        payloads.playerPresent = false; // carried forward by the store
        payloads.inventory = worldsave::encodeInventoryState(captureInventoryState());
        payloads.items = worldsave::encodeItemsState(captureItemsState());
        payloads.worldEditRevision = m_world->nextEditId();
        if (!m_world->forceCloseWorldForShutdown(&payloads))
            std::cerr << "[world] final shutdown did not persist all accepted edits" << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Per-frame advance
// ---------------------------------------------------------------------------

void GameSimulation::advancePlayer(double dt, const PlayerCommand &command, bool suspended)
{
    // Solo-compatibility forward (issue #246): the local player session.
    advancePlayer(localPlayerId(), dt, command, suspended);
}

bool GameSimulation::advancePlayer(PlayerId id, double dt, const PlayerCommand &command, bool suspended)
{
    PlayerState *player = findPlayer(id);
    if (!player)
        return false;
    if (suspended)
    {
        player->controller.suspend();
        return true;
    }
    // Flight is host-driven (the observer moves the body via
    // setPlayerPosition); only the walking controller steps here.
    if (player->flight || !m_world)
        return true;
    WorldCollisionView world(*m_world);
    player->controller.advance(dt, command, world);
    return true;
}

void GameSimulation::advancePlayers(double dt, bool suspended)
{
    // Bulk idle step (issue #246): every session in ascending id order with a
    // neutral command. Per-player semantics are identical to advancePlayer —
    // suspension first (even without a world), flight bodies never step. ONE
    // scoped collision view serves the whole batch (the shared lock is taken
    // once per frame, not once per player).
    if (suspended)
    {
        for (std::unique_ptr<PlayerEntry> &entry : m_players)
            entry->state.controller.suspend();
        return;
    }
    if (!m_world)
        return;
    // Skip the shared lock entirely when nobody walks (all-flight frames):
    // the pre-scan is N bool reads, the view acquisition is not free.
    bool anyWalking = false;
    for (const std::unique_ptr<PlayerEntry> &entry : m_players)
        if (!entry->state.flight)
        {
            anyWalking = true;
            break;
        }
    if (!anyWalking)
        return;
    WorldCollisionView world(*m_world);
    const PlayerCommand idle{};
    for (std::unique_ptr<PlayerEntry> &entry : m_players)
    {
        if (!entry->state.flight)
            entry->state.controller.advance(dt, idle, world);
    }
}

bool GameSimulation::setPlayerInterestRadius(PlayerId id, int radiusChunks)
{
    PlayerState *player = findPlayer(id);
    if (!player || radiusChunks < 0 || radiusChunks > kMaxPlayerInterestRadiusChunks)
        return false;
    player->interestRadiusChunks = radiusChunks;
    return true;
}

bool GameSimulation::advancePlayerStreaming(PlayerStreamingBudget budget)
{
    if (!m_world)
        return false;
    m_streamingInterests.clear();
    for (const auto &entry : m_players)
    {
        const PlayerState &state = entry->state;
        if (!state.positioned || state.interestRadiusChunks == 0)
            continue;
        const glm::dvec3 &feet = state.controller.body.position;
        if (!(feet.x >= SHRT_MIN && feet.x < SHRT_MAX &&
              feet.z >= SHRT_MIN && feet.z < SHRT_MAX && std::isfinite(feet.y)))
            continue;
        const glm::ivec3 center{floorDivChunk(static_cast<int>(std::floor(feet.x))), 0,
                               floorDivChunk(static_cast<int>(std::floor(feet.z)))};
        m_streamingInterests.push_back({entry->id, center, state.interestRadiusChunks});
    }
    return m_world->advancePlayerStreaming(m_streamingInterests, budget);
}

void GameSimulation::advanceMobs(double dt, glm::dvec3 observer, double availableRadius, bool suspended)
{
    if (!m_world || !m_terrainGenerator)
        return;
    WorldMobView world(*m_world, *m_terrainGenerator);
    // The gameplay toggle is authoritative sim state: disabled mobs never
    // populate or step, regardless of the host's suspension flag.
    m_mobs.update(dt, world, observer, availableRadius, suspended || !m_mobsEnabled);
}

bool GameSimulation::isGameplayParticipant(const PlayerEntry &entry, LocalAnchorPolicy anchorPolicy) const
{
    return entry.state.positioned && (anchorPolicy == LocalAnchorPolicy::Include || entry.id != localPlayerId());
}

void GameSimulation::advancePlayerMobs(double dt, double availableRadius, bool suspended, LocalAnchorPolicy anchorPolicy)
{
    if (!m_world || !m_terrainGenerator)
        return;
    std::array<glm::dvec3, kMaxPlayers> observers;
    size_t count = 0;
    for (const auto &entry : m_players)
        if (isGameplayParticipant(*entry, anchorPolicy))
            observers[count++] = entry->state.controller.body.position;
    WorldMobView world(*m_world, *m_terrainGenerator);
    m_mobs.update(dt, world, std::span(observers).first(count), availableRadius, suspended || !m_mobsEnabled);
}

void GameSimulation::advanceItems(double dt, bool suspended, LocalAnchorPolicy anchorPolicy)
{
    if (!m_world)
        return;
    // Main-thread scoped collision view (the advanceMobs pattern). The world
    // is NOT pumped here: publication ordering stays with the bridge /
    // pumpWorld (issue #114).
    WorldCollisionView world(*m_world);
    // Actor-aware pickup (issue #247): m_players is id-sorted, so the
    // participant filter preserves deterministic pickup priority. The buffer
    // is reserved once and refilled in place (no per-frame allocation). A
    // suspended frame skips the refill (the system ignores candidates and
    // only syncs previous/current).
    m_pickupCandidates.clear();
    if (!suspended)
    {
        for (const std::unique_ptr<PlayerEntry> &entry : m_players)
            if (isGameplayParticipant(*entry, anchorPolicy))
                m_pickupCandidates.push_back({entry->state.controller.body.position, &entry->state.inventory});
    }
    m_items.update(dt, world, m_pickupCandidates, suspended);
}

void GameSimulation::pumpWorld()
{
    if (m_world)
        m_world->pump();
}

// ---------------------------------------------------------------------------
// Player commands / queries (legacy forwards + per-player forms, issue #246)
// ---------------------------------------------------------------------------

void GameSimulation::suspendPlayer()
{
    suspendPlayer(localPlayerId());
}

void GameSimulation::suspendPlayer(PlayerId id)
{
    if (PlayerState *player = findPlayer(id))
        player->controller.suspend();
}

void GameSimulation::clearPlayerInput()
{
    clearPlayerInput(localPlayerId());
}

void GameSimulation::clearPlayerInput(PlayerId id)
{
    if (PlayerState *player = findPlayer(id))
        player->controller.clearInput();
}

bool GameSimulation::setPlayerFlight(bool enabled, glm::dvec3 feet)
{
    return setPlayerFlight(localPlayerId(), enabled, feet);
}

bool GameSimulation::setPlayerFlight(PlayerId id, bool enabled, glm::dvec3 feet)
{
    PlayerState *player = findPlayer(id);
    if (!player)
        return false;
    if (!enabled)
    {
        if (!m_world)
            return false;
        WorldCollisionView world(*m_world);
        physics::Body candidate = player->controller.body;
        candidate.position = feet;
        physics::QueryStats queries;
        if (!physics::clear(world, candidate.bounds(), queries))
        {
            player->status = "Walk unavailable: body overlaps solid or unloaded terrain.";
            return false;
        }
    }
    player->flight = enabled;
    player->status = "";
    player->resetMotion(feet);
    player->positioned = true; // a successful toggle places the body (issue #247 review)
    return true;
}

void GameSimulation::setPlayerPosition(glm::dvec3 feet)
{
    setPlayerPosition(localPlayerId(), feet);
}

void GameSimulation::setPlayerPosition(PlayerId id, glm::dvec3 feet)
{
    if (PlayerState *player = findPlayer(id))
    {
        player->resetMotion(feet);
        player->positioned = true; // host reposition activates the session (issue #247 review)
    }
}

void GameSimulation::restorePlayerFlight(bool enabled, glm::dvec3 feet)
{
    restorePlayerFlight(localPlayerId(), enabled, feet);
}

void GameSimulation::restorePlayerFlight(PlayerId id, bool enabled, glm::dvec3 feet)
{
    // Host lifecycle restore (review §4.8): benchmark/inspection exit must
    // recover the EXACT pre-benchmark mode even when the scripted camera
    // ends embedded in terrain — the gameplay clearance validation of
    // setPlayerFlight(false) would refuse and leave flight stuck on. This
    // is a lifecycle seam (the restore pattern), not a gameplay command.
    if (PlayerState *player = findPlayer(id))
    {
        player->flight = enabled;
        player->status = "";
        player->resetMotion(feet);
        player->positioned = true; // lifecycle restore places the body (issue #247 review)
    }
}

glm::dvec3 GameSimulation::spawnPlayerOnSurface(glm::dvec3 around)
{
    return spawnPlayerOnSurface(localPlayerId(), around);
}

glm::dvec3 GameSimulation::spawnPlayerOnSurface(PlayerId id, glm::dvec3 around)
{
    PlayerState *player = findPlayer(id);
    if (!player || !m_world)
        return around;
    if (trySpawnWalkingPlayerOnSurface(id, around))
        return player->controller.eye();
    const glm::dvec3 fallbackEye(around.x, 100.0, around.z);
    player->resetMotion(fallbackEye - glm::dvec3(0, player->controller.settings.eyeHeight, 0));
    player->flight = true;
    player->status = "No safe spawn in loaded terrain: fly to an open area.";
    player->positioned = true;
    return fallbackEye;
}

bool GameSimulation::trySpawnWalkingPlayerOnSurface(PlayerId id, glm::dvec3 around)
{
    PlayerState *player = findPlayer(id);
    if (!player || !m_world)
        return false;
    WorldCollisionView world(*m_world);
    physics::QueryStats queries;
    // Closest columns first, bounded by the already bootstrapped region.
    for (int radius = 0; radius < kBootstrapRadiusChunks * int(CHUNK_SIZE); ++radius)
    for (int dx = -radius; dx <= radius; ++dx)
    for (int dz = -radius; dz <= radius; ++dz)
    {
        if (std::max(std::abs(dx), std::abs(dz)) != radius) continue;
        const int x = static_cast<int>(std::floor(around.x)) + dx;
        const int z = static_cast<int>(std::floor(around.z)) + dz;
        if (x < SHRT_MIN || x >= SHRT_MAX || z < SHRT_MIN || z >= SHRT_MAX) continue;
        for (int y = CHUNK_HEIGHT - 1; y >= 0; --y)
        {
            const auto cell = world.sample({x, y, z});
            if (!cell.available) break;
            if (!cell.solid) continue;
            physics::Body candidate;
            candidate.position = {x + 0.5, y + 1.0 + physics::skin, z + 0.5};
            if (physics::clear(world, candidate.bounds(), queries))
            {
                player->resetMotion(candidate.position);
                player->flight = false;
                player->status = "";
                player->positioned = true; // surface spawn places the body (issue #247 review)
                return true;
            }
        }
    }
    return false;
}

bool GameSimulation::restorePlayerState(const PlayerPersistState &saved)
{
    return restorePlayerState(localPlayerId(), saved);
}

bool GameSimulation::restorePlayerState(PlayerId id, const PlayerPersistState &saved)
{
    PlayerState *player = findPlayer(id);
    if (!player || !m_world)
        return false;
    // Terrain validation (issue #180 review round 6): the saved file is
    // structurally valid, but the position itself must be usable against the
    // freshly bootstrapped terrain.
    WorldCollisionView world(*m_world);
    physics::QueryStats queries;
    physics::Body candidate;
    candidate.position = glm::dvec3(saved.x, saved.y, saved.z);
    glm::dvec3 restoredFeet = candidate.position;
    bool waitingForTerrain = false;
    if (physics::recover(world, candidate, queries))
    {
        // Valid as-is or embedded by at most one block (recover() corrected
        // it) — use the (possibly corrected) feet position.
        restoredFeet = candidate.position;
    }
    else if (candidate.waitingForTerrain)
    {
        // Terrain not fully available around the saved spot: keep the
        // position verbatim and let the solver hold the body until the
        // terrain streams in — never an aggressive fallback because one
        // neighbor is not loaded yet.
        waitingForTerrain = true;
    }
    else
    {
        // Clearly invalid (embedded deeper than recover() can fix): fall back
        // to the surface spawn.
        std::cout << "[world] saved player position is not usable; falling back to the surface spawn\n";
        return false;
    }

    player->resetMotion(restoredFeet);
    if (waitingForTerrain)
        player->controller.body.waitingForTerrain = true;
    player->flight = saved.flight;
    player->positioned = true; // a usable restore places the body (issue #247 review)
    if (saved.selectedBlock >= 0 && saved.selectedBlock < static_cast<int32_t>(COUNT))
        player->selectedBlock = static_cast<TextureType>(saved.selectedBlock);
    // The local line keeps the historical format (issue #246 solo
    // compatibility); non-local restores carry the session id.
    const std::string who = (id == localPlayerId())
                                ? std::string("player")
                                : "player#" + std::to_string(id.value);
    std::cout << "[world] restored " << who << " at (" << restoredFeet.x << ", " << restoredFeet.y
              << ", " << restoredFeet.z << ")" << (waitingForTerrain ? " (waiting for terrain)" : "")
              << "\n";
    return true;
}

std::optional<PlayerPersistState> GameSimulation::readSavedPlayerState() const
{
    if (!m_world || !m_world->isWorldOpen() || !m_world->worldPersistence())
        return std::nullopt;
    PlayerPersistState saved;
    if (!m_world->worldPersistence()->readPlayerState(saved))
        return std::nullopt; // fresh world, or already reported as corrupt
    return saved;
}

PlayerPersistState GameSimulation::capturePlayerState(float yaw, float pitch) const
{
    return capturePlayerState(localPlayerId(), yaw, pitch);
}

PlayerPersistState GameSimulation::capturePlayerState(PlayerId id, float yaw, float pitch) const
{
    const PlayerState *player = findPlayer(id);
    if (!player)
        return PlayerPersistState{}; // invalid/unknown id: no state to capture
    PlayerPersistState state;
    state.x = player->controller.body.position.x;
    state.y = player->controller.body.position.y;
    state.z = player->controller.body.position.z;
    state.yaw = yaw;
    state.pitch = pitch;
    state.flight = player->flight;
    state.selectedBlock = static_cast<int32_t>(player->selectedBlock);
    return state;
}

worldsave::InventoryPersistState GameSimulation::captureInventoryState() const
{
    return captureInventoryState(localPlayerId());
}

worldsave::InventoryPersistState GameSimulation::captureInventoryState(PlayerId id) const
{
    worldsave::InventoryPersistState state;
    const auto &inventory = findPlayer(id)->inventory;
    state.selectedSlot = static_cast<uint8_t>(inventory.selectedSlot());
    const std::array<items::ItemStack, items::kInventorySize> &slots = inventory.slots();
    for (std::size_t i = 0; i < slots.size(); ++i)
    {
        state.slots[i].itemId = static_cast<uint16_t>(slots[i].item);
        state.slots[i].count = slots[i].count;
    }
    return state;
}

void GameSimulation::restoreInventoryState(PlayerId id, const worldsave::InventoryPersistState &saved)
{
    std::array<items::ItemStack, items::kInventorySize> slots{};
    for (std::size_t i = 0; i < slots.size(); ++i)
        slots[i] = items::makeStack(static_cast<items::ItemId>(saved.slots[i].itemId), saved.slots[i].count);
    findPlayer(id)->inventory.restore(slots, saved.selectedSlot);
}

worldsave::RemotePlayerState GameSimulation::captureRemotePlayerState(PlayerId id) const
{
    const auto velocity = findPlayer(id)->controller.body.velocity;
    return {id, capturePlayerState(id, 0, 0), captureInventoryState(id),
            {velocity.x, velocity.y, velocity.z}};
}

bool GameSimulation::restoreRemotePlayerState(const worldsave::RemotePlayerState &saved, glm::dvec3 spawnCenter)
{
    auto *player = findPlayer(saved.id);
    if (!player || player->positioned)
        return false;
    if (restorePlayerState(saved.id, saved.pose))
    {
        if (player->controller.body.position == glm::dvec3(saved.pose.x, saved.pose.y, saved.pose.z))
            player->controller.body.velocity = {saved.velocity[0], saved.velocity[1], saved.velocity[2]};
    }
    else
    {
        if (!trySpawnWalkingPlayerOnSurface(saved.id, spawnCenter))
            return false;
        player->flight = saved.pose.flight;
        player->selectedBlock = static_cast<TextureType>(saved.pose.selectedBlock);
    }
    restoreInventoryState(saved.id, saved.inventory);
    return true;
}

worldsave::ItemsPersistState GameSimulation::captureItemsState() const
{
    worldsave::ItemsPersistState state;
    // The allocator's NEXT id = persisted high-water + 1: restoring it
    // guarantees no id reuse across sessions (epic #223 §2/§6).
    state.nextEntityId = m_items.idHighWater() + 1;
    const std::vector<items::ItemEntity> &entities = m_items.entities(); // id-sorted
    state.entities.reserve(entities.size());
    for (const items::ItemEntity &e : entities)
    {
        worldsave::ItemEntityPersistState r;
        r.id = e.id;
        r.itemId = static_cast<uint16_t>(e.stack.item);
        r.count = e.stack.count;
        r.px = e.body.position.x;
        r.py = e.body.position.y;
        r.pz = e.body.position.z;
        r.vx = e.body.velocity.x;
        r.vy = e.body.velocity.y;
        r.vz = e.body.velocity.z;
        r.ageTicks = e.ageTicks;
        r.pickupDelayTicks = e.pickupDelay;
        state.entities.push_back(r);
    }
    return state;
}

std::optional<worldsave::InventoryPersistState> GameSimulation::readSavedInventoryState() const
{
    if (!m_world || !m_world->isWorldOpen() || !m_world->worldPersistence())
        return std::nullopt;
    worldsave::InventoryPersistState state;
    if (!m_world->worldPersistence()->readInventoryState(state))
        return std::nullopt; // fresh world / old save, or already reported as corrupt
    return state;
}

std::optional<worldsave::ItemsPersistState> GameSimulation::readSavedItemsState() const
{
    if (!m_world || !m_world->isWorldOpen() || !m_world->worldPersistence())
        return std::nullopt;
    worldsave::ItemsPersistState state;
    if (!m_world->worldPersistence()->readItemsState(state))
        return std::nullopt; // fresh world / old save, or already reported as corrupt
    return state;
}

void GameSimulation::restoreGameplayState()
{
    if (m_savedInventory)
    {
        restoreInventoryState(localPlayerId(), *m_savedInventory);
        std::cout << "[world] restored inventory (selected slot "
                  << static_cast<int>(m_savedInventory->selectedSlot) << ")\n";
    }
    if (m_savedItems)
    {
        std::vector<items::ItemEntity> entities;
        entities.reserve(m_savedItems->entities.size());
        for (const worldsave::ItemEntityPersistState &r : m_savedItems->entities)
        {
            items::ItemEntity e{};
            e.id = r.id;
            e.stack = items::makeStack(static_cast<items::ItemId>(r.itemId), r.count);
            e.body.position = glm::dvec3(r.px, r.py, r.pz);
            e.body.velocity = glm::dvec3(r.vx, r.vy, r.vz);
            e.body.size = m_items.settings.bodySize;
            e.previous = e.body.position;
            e.ageTicks = r.ageTicks;
            e.pickupDelay = r.pickupDelayTicks;
            entities.push_back(e);
        }
        m_items.restore(m_savedItems->nextEntityId, entities);
        std::cout << "[world] restored " << entities.size() << " dropped items (next entity id "
                  << m_savedItems->nextEntityId << ")\n";
    }
    // Consume the payloads (PR #235 review): the restore seam is idempotent
    // — a repeated finalizePlayerSpawn (future respawn flows, #230 runner
    // round-trips) must never overwrite live gameplay state with the saved
    // snapshot. The player restore keeps its historical re-restore behavior.
    m_savedInventory.reset();
    m_savedItems.reset();
}

// ---------------------------------------------------------------------------
// Voxel interaction (headless seam; the graphical host uses the bridge)
// ---------------------------------------------------------------------------

WorldEditResult GameSimulation::placeVoxel(const glm::vec3 &worldPos, TextureType type)
{
    return m_world ? m_world->placeVoxel(worldPos, type) : WorldEditResult{};
}

WorldEditResult GameSimulation::deleteVoxel(const glm::vec3 &worldPos)
{
    return m_world ? m_world->deleteVoxel(worldPos) : WorldEditResult{};
}

bool GameSimulation::isVoxelActive(const glm::vec3 &worldPos) const
{
    return m_world && m_world->isVoxelActive(worldPos);
}

bool GameSimulation::cellInWorldEnvelope(glm::ivec3 cell)
{
    // The supported editable-world envelope (issue #247 review): the finite
    // SHRT_MIN/SHRT_MAX horizontal camera envelope of WorldCollisionView plus
    // the fixed vertical world [0, WORLD_HEIGHT). Pure integer comparisons —
    // no float conversion, so extreme cells (near the INT32 bounds) are
    // rejected safely before any float position / AABB is constructed.
    return cell.x >= SHRT_MIN && cell.x < SHRT_MAX &&
           cell.z >= SHRT_MIN && cell.z < SHRT_MAX &&
           cell.y >= 0 && cell.y < WORLD_HEIGHT;
}

bool GameSimulation::canPlaceVoxel(const glm::vec3 &worldPos, TextureType type) const
{
    // Historical Engine placement rule (issue #209), generalized to EVERY
    // session by issue #247: flying players and non-solid blocks never
    // collide with a body; a solid block is refused when it overlaps ANY
    // authoritative WALKING player body, so no player can be entombed by
    // their own or another's edit. Bodies are checked in ascending-PlayerId
    // order (m_players is id-sorted) — deterministic validation. Logical
    // pending solids immediately participate in subsequent collision
    // queries, so a body cannot enter before publication either. Solo (one
    // session) reproduces the historical local-body rule bit for bit.
    if (!physics::blockCell(type).solid)
        return true;
    // Coordinate validation BEFORE floor->int (issue #247 review): a non-finite
    // or out-of-envelope position would make glm::floor -> int conversion UB
    // (e.g. a cell near INT32_MAX rounds up to 2^31 as float). Non-finite input
    // and positions outside the supported world envelope answer a safe refusal.
    // The bounds are exactly representable floats, so a position passing this
    // gate floors to an in-range int cell.
    if (!std::isfinite(worldPos.x) || !std::isfinite(worldPos.y) || !std::isfinite(worldPos.z))
        return false;
    if (worldPos.x < static_cast<float>(SHRT_MIN) || worldPos.x >= static_cast<float>(SHRT_MAX) ||
        worldPos.z < static_cast<float>(SHRT_MIN) || worldPos.z >= static_cast<float>(SHRT_MAX) ||
        worldPos.y < 0.f || worldPos.y >= static_cast<float>(WORLD_HEIGHT))
        return false;
    const physics::Aabb cell = physics::voxelBounds(glm::ivec3(glm::floor(worldPos)));
    for (const std::unique_ptr<PlayerEntry> &entry : m_players)
    {
        const PlayerState &player = entry->state;
        // Unpositioned sessions (default-origin bodies) do not block placement
        // until the host places them (issue #247 review); flying bodies are
        // exempt (the creative rule).
        if (!player.positioned || player.flight)
            continue;
        physics::Body playerBody;
        playerBody.position = player.controller.body.position;
        if (physics::overlaps(playerBody.bounds(), cell))
            return false;
    }
    return true;
}

bool GameSimulation::canPlaceVoxel(PlayerId id, const glm::vec3 &worldPos, TextureType type) const
{
    // Actor-aware form (issue #247): an unknown/retired session may not even
    // probe placement. The rule itself is actor-independent — every walking
    // body is protected regardless of who asks.
    if (!findPlayer(id))
        return false;
    return canPlaceVoxel(worldPos, type);
}

// ---------------------------------------------------------------------------
// Inventory / hotbar gameplay commands (issue #225, Epic #223 Phase B)
// ---------------------------------------------------------------------------
// Instantaneous command applications at the host frame boundary: no buffer,
// no fixed-step work, world-independent. The deterministic rules live in
// items::Inventory (issue #224); the facade is the ownership + logging seam.
// Legacy forms forward to the local player; per-player forms (issue #246)
// refuse invalid/unknown ids with items::ItemResult::InvalidPlayer.

items::ItemResult GameSimulation::selectHotbarSlot(int index)
{
    return selectHotbarSlot(localPlayerId(), index);
}

items::ItemResult GameSimulation::selectHotbarSlot(PlayerId id, int index)
{
    PlayerState *player = findPlayer(id);
    return player ? player->inventory.setSelectedSlot(index) : items::ItemResult::InvalidPlayer;
}

items::ItemResult GameSimulation::moveStack(int from, int to)
{
    return moveStack(localPlayerId(), from, to);
}

items::ItemResult GameSimulation::moveStack(PlayerId id, int from, int to)
{
    PlayerState *player = findPlayer(id);
    return player ? player->inventory.moveStack(from, to) : items::ItemResult::InvalidPlayer;
}

items::ItemResult GameSimulation::swapStacks(int a, int b)
{
    return swapStacks(localPlayerId(), a, b);
}

items::ItemResult GameSimulation::swapStacks(PlayerId id, int a, int b)
{
    PlayerState *player = findPlayer(id);
    return player ? player->inventory.swapStacks(a, b) : items::ItemResult::InvalidPlayer;
}

items::ItemResult GameSimulation::splitStack(int from, int to)
{
    return splitStack(localPlayerId(), from, to);
}

items::ItemResult GameSimulation::splitStack(PlayerId id, int from, int to)
{
    PlayerState *player = findPlayer(id);
    return player ? player->inventory.splitStack(from, to) : items::ItemResult::InvalidPlayer;
}

items::AddResult GameSimulation::grantItems(items::ItemId item, uint16_t count)
{
    return grantItems(localPlayerId(), item, count);
}

items::AddResult GameSimulation::grantItems(PlayerId id, items::ItemId item, uint16_t count)
{
    // Normalize FIRST so the audit line reflects what was actually applied
    // (PR #232 review): a {None, N} sentinel input collapses to the canonical
    // empty and grants nothing — the log must not overstate that as N items.
    const items::ItemStack request = items::makeStack(item, count);
    PlayerState *player = findPlayer(id);
    // Invalid/unknown session: refuse WITHOUT touching any inventory; the
    // canonicalized request is echoed in the remainder (the rejected-input
    // discipline, issue #246).
    const items::AddResult result = player ? player->inventory.addStack(request)
                                           : items::AddResult{items::ItemResult::InvalidPlayer, request};
    const uint32_t applied = static_cast<uint32_t>(request.count) - result.remainder.count;
    // Test/admin seam (epic #223 §3): EVERY grant is explicitly logged so
    // items entering the inventory without a break/pickup cause stay
    // auditable in headless runs and (later) authoritative server logs.
    // Non-local grants carry the session id (issue #246); the local line
    // keeps the historical format byte for byte.
    const std::string_view name = items::itemName(item);
    std::cout << "[items] grant ";
    if (!(id == localPlayerId()))
        std::cout << "player#" << id.value << " ";
    std::cout << count << "x ";
    if (name.empty())
        std::cout << "item#" << static_cast<uint16_t>(item);
    else
        std::cout << name;
    std::cout << " (admin seam): " << itemResultName(result.result);
    if (!result.remainder.isEmpty())
        std::cout << ", remainder " << result.remainder.count;
    if (applied != count)
        std::cout << ", applied " << applied;
    std::cout << "\n";
    return result;
}

// ---------------------------------------------------------------------------
// World interaction commands (issues #226/#227, Epic #223 Phase C/D;
// actor-aware since issue #247, Epic #245 Phase A2)
// ---------------------------------------------------------------------------
// Each command has a legacy single-player form (a thin forward to the LOCAL
// player) and an actor-aware form taking a PlayerId first. The actor-aware
// form validates the handle (InvalidPlayer for an unknown/retired session,
// WITHOUT side effects) and derives the interaction context from that
// session. Solo behavior is bit-identical through both paths — one source of
// truth. The world edit semantics are unchanged.

BreakResult GameSimulation::breakBlock(glm::ivec3 cell, WorldEditResult *outEdit)
{
    return breakBlock(localPlayerId(), cell, outEdit);
}

BreakResult GameSimulation::breakBlock(PlayerId id, glm::ivec3 cell, WorldEditResult *outEdit)
{
    // Actor identity first (issue #247): an unknown/retired session refuses
    // without touching the world or the item population. The break EFFECT is
    // world-shared (the drop belongs to the shared population), so no actor
    // body/inventory is consumed here — the handle is validated for authority.
    if (!findPlayer(id))
        return BreakResult::InvalidPlayer;
    if (!m_world)
        return BreakResult::UnknownTerrain;
    // Out-of-envelope cells are boundary walls, not editable terrain (issue
    // #247 review): reject them with integer comparisons before any float
    // position is built. (The int-first effective sample below already answers
    // AIR for them; this guard makes the coordinate invariant explicit.)
    if (!cellInWorldEnvelope(cell))
        return BreakResult::WorldRejected;
    // Sample the EFFECTIVE voxel (storage + pending edits) through the CPU
    // collision seam. The view's shared lock MUST be released before the
    // exclusive edit lock below — scope it tightly.
    TextureType type = AIR;
    bool available = false;
    {
        WorldCollisionView view(*m_world);
        const physics::Cell sampled = view.sampleWithType(cell, &type);
        available = sampled.available;
    }
    if (!available)
        return BreakResult::UnknownTerrain; // unloaded / unreadable: never inferred
    if (type == AIR)
        return BreakResult::WorldRejected; // nothing to break (incl. out-of-bounds cells)
    if (!items::isBreakableBlock(type))
        return BreakResult::Unbreakable; // BEDROCK / WATER / LAVA by hand in v1
    // Id-exhaustion guard (stabilization PR3): a break whose registry drop
    // needs a fresh entity id is refused BEFORE the world edit, so the drop
    // is never silently lost (conservation). Blocks with an empty drop table
    // entry need no entity and stay breakable. The allocator never wraps.
    const items::ItemStack drop = items::dropForBlock(type);
    if (!drop.isEmpty() && m_items.idsExhausted())
        return BreakResult::IdExhausted;
    // Existing world edit semantics (issue #114): the delete may defer with
    // its mirror group; the drop spawns immediately at the cell center.
    const glm::vec3 center(cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f);
    WorldEditResult edit = m_world->deleteVoxel(center);
    if (!edit.accepted)
        return BreakResult::WorldRejected;
    if (!drop.isEmpty())
        m_items.spawnBreakDrop(drop, glm::dvec3(cell.x + 0.5, cell.y + 0.5, cell.z + 0.5), edit.editId);
    // The graphical host publishes the accepted edit through the bridge
    // (event drain + commit groups) — no second edit path (issue #227).
    if (outEdit)
        *outEdit = std::move(edit);
    return BreakResult::Ok;
}

PlaceResult GameSimulation::placeSelectedBlock(glm::ivec3 cell, WorldEditResult *outEdit)
{
    return placeSelectedBlock(localPlayerId(), cell, outEdit);
}

PlaceResult GameSimulation::placeSelectedBlock(PlayerId id, glm::ivec3 cell, WorldEditResult *outEdit)
{
    // Actor identity first (issue #247): refuse without consuming inventory
    // or editing the world for an unknown/retired session.
    PlayerState *player = findPlayer(id);
    if (!player)
        return PlaceResult::InvalidPlayer;
    items::Inventory &inventory = player->inventory;
    const items::ItemStack selected = inventory.selectedStack();
    if (selected.isEmpty())
        return PlaceResult::EmptySlot;
    const std::optional<TextureType> block = items::placeableBlockOf(selected.item);
    if (!block.has_value())
        return PlaceResult::NotPlaceable; // future non-block items (none in v1)
    // Coordinate validation BEFORE any float conversion (issue #247 review):
    // an extreme cell (near the INT32 bounds) would overflow the float->int
    // floor in canPlaceVoxel / the world edit (UB). Reject out-of-envelope
    // cells with integer comparisons; the world refuses them anyway as
    // out-of-bounds targets. Valid gameplay cells are unaffected.
    if (!cellInWorldEnvelope(cell))
        return PlaceResult::WorldRejected;
    const glm::vec3 center(cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f);
    // Player-aware placement rule (#209 / #247): the SAME query the creative
    // host gates with — now protecting EVERY walking body, not just the
    // actor's. Flight never collides; a solid overlapping any walking player
    // is refused.
    if (!canPlaceVoxel(center, *block))
        return PlaceResult::PlayerOverlap;
    // Gameplay refuses unknown targets (the creative seam may defer edits
    // into registered-but-unloaded records; survival placement needs terrain).
    if (!m_world)
        return PlaceResult::UnknownTerrain;
    {
        WorldCollisionView view(*m_world);
        if (!view.sample(cell).available)
            return PlaceResult::UnknownTerrain;
    }
    WorldEditResult edit = m_world->placeVoxel(center, *block);
    if (!edit.accepted)
        return PlaceResult::WorldRejected; // occupied / out-of-bounds target
    // Consume exactly 1 item, ONLY on success (the survival contract).
    inventory.removeCount(inventory.selectedSlot(), 1);
    if (outEdit)
        *outEdit = std::move(edit);
    return PlaceResult::Ok;
}

VoxelTargetResult GameSimulation::validateVoxelTarget(PlayerId id, VoxelInteraction operation, glm::ivec3 target,
                                                       ChunkContentVersion expected, glm::dvec3 direction) const
{
    using Result = VoxelTargetResult;
    const double length = glm::length(direction);
    if (!playerPositioned(id) || !cellInWorldEnvelope(target) || !(length > 0.0) || !std::isfinite(length))
        return Result::InvalidTarget;
    const glm::dvec3 eye = playerEye(id);
    if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z))
        return Result::InvalidTarget;
    const glm::dvec3 corner(target);
    const auto offset = eye - glm::clamp(eye, corner, corner + 1.0);
    if (glm::dot(offset, offset) > kInteractionReach * kInteractionReach)
        return Result::OutOfReach;

    if (!m_world)
        return Result::TerrainUnavailable;
    const auto version = m_world->chunkContentVersion({floorDivChunk(target.x), 0, floorDivChunk(target.z)});
    if (!version)
        return Result::TerrainUnavailable;
    if (version->generation != expected.generation || version->revision != expected.revision)
        return Result::StaleRevision;

    WorldCollisionView view(*m_world);
    TextureType targetType = AIR;
    if (!view.sampleWithType(target, &targetType).available)
        return Result::TerrainUnavailable;
    const bool placing = operation == VoxelInteraction::Place;
    if (placing && targetType != AIR)
        return Result::Occupied;

    const auto hit = traceVoxelTarget(view, eye, direction);
    if (hit.terrainUnavailable)
        return Result::TerrainUnavailable;
    const bool matches = placing ? hit.hasPlaceFace && hit.adjacent == target : hit.block == target;
    return hit.hit && matches ? Result::Valid : Result::InvalidTarget;
}

VoxelTargetTrace GameSimulation::traceVoxelTarget(const WorldCollisionView &view,
                                                  glm::dvec3 eye, glm::dvec3 direction)
{
    const double length = glm::length(direction);
    // Keep integer grid-plane conversions and the bounded march well
    // inside the representable range, including at the world boundary.
    constexpr double originLimit = double(INT32_MAX) / 2.0;
    if (!(length > 0.0) || !std::isfinite(length) ||
        !(std::abs(eye.x) <= originLimit && std::abs(eye.y) <= originLimit &&
          std::abs(eye.z) <= originLimit))
        return {};
    // Step at grid planes so even a short passage through an intervening
    // voxel can occlude the target. A tied crossing enters the diagonal
    // cell without crossing a single face suitable for placement.
    direction /= length;
    glm::ivec3 cell = glm::floor(eye);
    glm::ivec3 step(0);
    const double infinity = std::numeric_limits<double>::infinity();
    glm::dvec3 next(infinity), stride(infinity);
    for (int axis = 0; axis < 3; ++axis)
    {
        const double component = direction[axis];
        if (std::abs(component) <= std::numeric_limits<double>::epsilon())
            continue;
        step[axis] = component > 0.0 ? 1 : -1;
        if (step[axis] < 0 && eye[axis] == cell[axis])
            --cell[axis];
        const int plane = cell[axis] + (step[axis] > 0 ? 1 : 0);
        next[axis] = (plane - eye[axis]) / component;
        stride[axis] = std::abs(1.0 / component);
    }
    glm::ivec3 previous = cell;
    int crossed = 0;
    for (;;)
    {
        TextureType material = AIR;
        const auto sample = view.sampleWithType(cell, &material);
        if (!sample.available)
            return {.terrainUnavailable = true};
        if (material != AIR)
            return {cell, previous, true, crossed == 1, false};
        if (sample.solid)
            return {}; // virtual world boundary

        const double distance = std::min({next.x, next.y, next.z});
        if (distance > kInteractionReach)
            return {};
        previous = cell;
        crossed = 0;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (next[axis] != distance)
                continue;
            cell[axis] += step[axis];
            next[axis] += stride[axis];
            ++crossed;
        }
    }
}

RaycastHit GameSimulation::raycastVoxel(glm::vec3 origin, glm::vec3 dir, float maxDist) const
{
    // Privileged raw-origin host seam (issue #247): the trusted local host /
    // creative editor supplies the camera origin + distance. The march itself
    // is shared with the actor-aware form (one implementation).
    return raycastMarch(origin, dir, maxDist);
}

RaycastHit GameSimulation::raycastVoxel(PlayerId id, glm::vec3 dir, float maxDist) const
{
    // Actor-aware targeting (issue #247): the origin is the actor's
    // authoritative eye — NEVER client-supplied — and the distance is clamped
    // to the server-owned gameplay reach. An unknown/retired actor hits
    // nothing. std::min propagates a NaN maxDist into the march's non-finite
    // gate (safe no-hit); a negative request stays negative (also rejected).
    const PlayerState *player = findPlayer(id);
    if (!player)
        return RaycastHit{};
    const glm::vec3 origin(player->controller.eye());
    return raycastMarch(origin, dir, std::min(maxDist, kInteractionReach));
}

RaycastHit GameSimulation::raycastMarch(glm::vec3 origin, glm::vec3 dir, float maxDist) const
{
    // The historical Engine::raycastVoxel march (issue #227): same float
    // arithmetic, same 0.05 step, same first-active-cell rule and adjacent
    // fallback. Stabilization PR1 hardened the inputs and bounded the loop;
    // PR2 unified the cell query with the authoritative EFFECTIVE state
    // (WorldCollisionView: stored backing + pending logical edits) that
    // breakBlock/placeSelectedBlock and physics already observe. Unknown
    // cells answer unavailable — the march passes through them, never hits.
    RaycastHit result{};
    if (!m_world)
        return result;
    // Non-finite inputs would poison floor() -> int conversion (UB) or the
    // loop control: NaN comparisons silently skip the march, +Inf distances
    // never terminate. An invalid ray simply hits nothing.
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z))
        return result;
    if (!std::isfinite(maxDist) || maxDist < 0.f)
        return result;
    if (!std::isfinite(dir.x) || !std::isfinite(dir.y) || !std::isfinite(dir.z))
        return result;
    // Origin envelope: with the clamped distance below, every marched cell
    // stays inside the supported world-coordinate envelope, so all float ->
    // int coordinate conversions are in range (no UB at extreme positions).
    if (std::fabs(origin.x) > kRaycastOriginLimit ||
        std::fabs(origin.y) > kRaycastOriginLimit ||
        std::fabs(origin.z) > kRaycastOriginLimit)
        return result;
    // Degenerate-direction guard (review §4.4), symmetric to dropFromSlot.
    // The squared length is evaluated in DOUBLE: finite-but-huge float
    // components (e.g. 1e30) overflow a float dot to +Inf, which would slip
    // past a float-only gate and normalize to a zero direction.
    const double dx = dir.x, dy = dir.y, dz = dir.z;
    const double len2 = dx * dx + dy * dy + dz * dz;
    if (len2 < 1e-12)
        return result;
    // Normalize: the float path keeps the historical bit-exact arithmetic
    // for every ray a float dot can represent; the double fallback only
    // engages when the float dot overflows to infinity.
    const float dotF = glm::dot(dir, dir);
    if (std::isfinite(dotF))
        dir = glm::normalize(dir);
    else
        dir = glm::vec3(glm::normalize(glm::dvec3(dir)));
    const float step = 0.05f;
    const float dist = std::min(maxDist, kMaxRaycastDistance);
    // Bounded march: an integer iteration cap derived from the clamped
    // distance. The float accumulator is unchanged (historical semantics),
    // but t can never stall at the float precision wall: dist <= 64 keeps
    // every increment representable, and the cap terminates the loop even
    // if that invariant ever broke.
    const int maxSteps = static_cast<int>(std::ceil(dist / step)) + 1;
    // ONE scoped view for the whole march: the shared lock is taken once
    // (cheaper than the historical per-cell lock), chunk lookups are cached,
    // and the pending-edit overlay matches the authoritative effective state
    // the gameplay commands and collision observe (stabilization PR2).
    WorldCollisionView view(*m_world);
    glm::vec3 prev = origin;
    float t = 0.f;
    for (int i = 0; i < maxSteps && t <= dist; ++i)
    {
        const glm::vec3 p = origin + dir * t;
        const glm::vec3 block(std::floor(p.x), std::floor(p.y), std::floor(p.z));
        // Historical vertical pass-through: below-world / above-sky cells
        // never hit (isVoxelActive answered false there). The collision
        // view's solid floor / open sky are PHYSICS boundaries, not targets;
        // the horizontal envelope check is belt-and-braces for the origin
        // limit above. Cells outside it cannot hold resident chunks.
        if (block.y >= 0.f && block.y < static_cast<float>(WORLD_HEIGHT) &&
            block.x >= static_cast<float>(SHRT_MIN) && block.x < static_cast<float>(SHRT_MAX) &&
            block.z >= static_cast<float>(SHRT_MIN) && block.z < static_cast<float>(SHRT_MAX))
        {
            TextureType type = AIR;
            const physics::Cell cell = view.sampleWithType(
                glm::ivec3(static_cast<int>(block.x), static_cast<int>(block.y),
                           static_cast<int>(block.z)),
                &type);
            // Any non-AIR effective type hits — solids AND fluids/vegetation,
            // exactly the historical isVoxelActive rule, now pending-aware.
            if (cell.available && type != AIR)
            {
                result.hit = true;
                result.block = block;
                result.adjacent = glm::vec3(std::floor(prev.x), std::floor(prev.y), std::floor(prev.z));
                if (result.adjacent == result.block)
                    result.adjacent = block - glm::vec3(dir.x > 0 ? 1.f : (dir.x < 0 ? -1.f : 0.f),
                                                        dir.y > 0 ? 1.f : (dir.y < 0 ? -1.f : 0.f),
                                                        dir.z > 0 ? 1.f : (dir.z < 0 ? -1.f : 0.f));
                return result;
            }
        }
        prev = p;
        t += step;
    }
    return result;
}

items::ItemResult GameSimulation::dropFromSlot(int slot, uint16_t count, glm::dvec3 dir)
{
    return dropFromSlot(localPlayerId(), slot, count, dir);
}

items::ItemResult GameSimulation::dropFromSlot(PlayerId id, int slot, uint16_t count, glm::dvec3 dir)
{
    // Actor identity first (issue #247): refuse without consuming inventory
    // or spawning an entity for an unknown/retired session.
    PlayerState *player = findPlayer(id);
    if (!player)
        return items::ItemResult::InvalidPlayer;
    items::Inventory &inventory = player->inventory;
    // Id-exhaustion guard (stabilization PR3): refuse BEFORE any inventory
    // mutation so a failed drop never consumes items (conservation). Only
    // checked when an entity would actually spawn: count 0 stays an Ok no-op
    // and invalid/empty slots keep their historical result codes below.
    if (count > 0 && slot >= 0 && slot < items::kInventorySize &&
        !inventory.stack(slot).isEmpty() && m_items.idsExhausted())
        return items::ItemResult::IdExhausted;
    const items::RemoveResult removed = inventory.removeCount(slot, count);
    if (removed.result != items::ItemResult::Ok)
        return removed.result; // InvalidSlot / EmptyStack
    if (removed.removed.isEmpty())
        return items::ItemResult::Ok; // count 0: no-op, no entity
    // Host-supplied presentation direction (the savePlayerState(yaw,pitch)
    // pattern): normalized here; degenerate OR non-finite input falls back
    // to +Y (an inf component would otherwise normalize to NaN and poison
    // the spawn origin/velocity). The launch speed is a deterministic plain
    // setting — no RNG. The spawn origin is the ACTOR's authoritative eye.
    glm::dvec3 direction = dir;
    const bool finite = std::isfinite(direction.x) && std::isfinite(direction.y) &&
                        std::isfinite(direction.z);
    const double length = glm::length(direction);
    direction = (finite && length > 1e-6) ? direction / length : glm::dvec3(0.0, 1.0, 0.0);
    const glm::dvec3 origin = player->controller.eye() + direction * 0.3;
    const glm::dvec3 velocity = direction * m_items.settings.dropSpeed + glm::dvec3(0.0, 1.0, 0.0);
    m_items.spawnPlayerDrop(removed.removed, origin, velocity);
    return items::ItemResult::Ok;
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

PlayerSnapshot GameSimulation::snapshotOf(const PlayerState &state)
{
    PlayerSnapshot s;
    s.position = state.controller.body.position;
    s.velocity = state.controller.body.velocity;
    s.renderEye = state.controller.renderEye();
    s.grounded = state.controller.body.grounded;
    s.waitingForTerrain = state.controller.body.waitingForTerrain;
    s.flight = state.flight;
    s.submerged = state.controller.submerged;
    s.metrics = state.controller.metrics;
    s.movementTimers = state.controller.timers();
    s.motionReset = state.motionReset;
    s.selectedBlock = state.selectedBlock;
    s.status = state.status;
    return s;
}

PlayerSnapshot GameSimulation::playerSnapshot() const
{
    return snapshotOf(localState());
}

bool GameSimulation::playerSnapshot(PlayerId id, PlayerSnapshot &out) const
{
    const PlayerState *player = findPlayer(id);
    if (!player)
        return false; // invalid/unknown id: `out` stays untouched
    out = snapshotOf(*player);
    return true;
}

void GameSimulation::inventorySnapshot(InventorySnapshot &out) const
{
    inventorySnapshot(localPlayerId(), out);
}

bool GameSimulation::inventorySnapshot(PlayerId id, InventorySnapshot &out) const
{
    const PlayerState *player = findPlayer(id);
    if (!player)
        return false; // invalid/unknown id: `out` stays untouched
    // In-place fill of the bounded plain value (the mobSnapshots pattern):
    // one fixed-array copy + the selection, no allocation, no registry work.
    out.slots = player->inventory.slots();
    out.selectedSlot = player->inventory.selectedSlot();
    return true;
}

glm::dvec3 GameSimulation::playerFeet(PlayerId id) const
{
    const PlayerState *player = findPlayer(id);
    return player ? player->controller.body.position : glm::dvec3(0.0);
}

glm::dvec3 GameSimulation::playerEye(PlayerId id) const
{
    const PlayerState *player = findPlayer(id);
    return player ? player->controller.eye() : glm::dvec3(0.0);
}

bool GameSimulation::playerPositioned(PlayerId id) const
{
    const PlayerState *player = findPlayer(id);
    return player ? player->positioned : false;
}

bool GameSimulation::playerFlight(PlayerId id) const
{
    const PlayerState *player = findPlayer(id);
    return player ? player->flight : false;
}

void GameSimulation::setSelectedBlock(PlayerId id, TextureType type)
{
    if (PlayerState *player = findPlayer(id))
        player->selectedBlock = type;
}

const items::Inventory *GameSimulation::inventory(PlayerId id) const
{
    const PlayerState *player = findPlayer(id);
    return player ? &player->inventory : nullptr;
}

WorldSnapshot GameSimulation::worldSnapshot() const
{
    WorldSnapshot s;
    s.seed = m_seed;
    s.generatorVersion = TerrainGenerator::kGeneratorVersion;
    s.worldGenerationId = m_worldGenerationId;
    if (m_world)
    {
        s.editRevision = m_world->nextEditId();
        s.chunkCount = m_world->chunkCount();
        s.hasPendingLogicalEdits = m_world->hasPendingLogicalEdits();
        s.persistenceOpen = m_world->isWorldOpen();
        if (const WorldPersistence *persistence = m_world->worldPersistence())
            s.worldName = persistence->name();
    }
    return s;
}

} // namespace sim
