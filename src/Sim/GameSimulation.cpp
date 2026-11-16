// GameSimulation — CPU-owned simulation facade (issue #208, Epic #204 Phase D).
// The player/mob/world rules implemented here are the historical Engine rules
// moved verbatim behind a render-free API: fixed-step rates, pause/focus
// suspension semantics, spawn/restore validation, reload refusal and the
// issue #180 persistence contracts are unchanged. See the header for the
// ownership / policy boundaries and docs/simulation-render-contracts.md.
#include "Sim/GameSimulation.hpp"

#include <Items/ItemRegistry.hpp>
#include <Items/ItemStack.hpp>
#include <Physics/BlockPhysics.hpp>
#include <World/WorldCollisionView.hpp>
#include <World/WorldMobView.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>

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
    }
    return "Unknown";
}
} // namespace

GameSimulation::GameSimulation(ThreadPool *threadPool)
    : m_threadPool(threadPool)
{
}

GameSimulation::~GameSimulation()
{
    // Final safety net (mirrors ~VoxelWorld): a host that forgot the explicit
    // shutdown still persists accepted edits. After shutdown() this is a no-op.
    shutdown();
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
    // Fresh session = empty inventory (issue #225). The inventory.state
    // restore hook lands in #228 on the finalizePlayerSpawn seam.
    m_inventory.clear();
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
        if (m_world->openWorld(config.savesRoot.string(), config.worldName, error))
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
                            m_savedPlayer->y + m_player.settings.eyeHeight,
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
        out.eye = m_player.renderEye();
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
    // Successful reload = fresh inventory (issue #225). A REFUSED reload
    // returned above and keeps the old inventory authoritative.
    m_inventory.clear();
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

bool GameSimulation::savePlayerState(float yaw, float pitch)
{
    if (!m_world || !m_world->isWorldOpen())
        return false;
    const PlayerPersistState playerState = capturePlayerState(yaw, pitch);
    WorldPersistence *persistence = m_world->worldPersistence();
    bool ok = true;
    if (!persistence || !persistence->writePlayerState(playerState))
    {
        std::cout << "[world] player state save failed\n";
        ok = false;
    }
    // Full gameplay capture (issue #228): inventory + dropped items +
    // allocator high-water, written BEFORE the flush durability barrier.
    // Each failure is reported and stays retryable (the next capture/flush
    // rewrites it) — never silently dropped.
    if (!persistence || !persistence->writeInventoryState(captureInventoryState()))
    {
        std::cout << "[world] inventory state save failed\n";
        ok = false;
    }
    if (!persistence || !persistence->writeItemsState(captureItemsState()))
    {
        std::cout << "[world] items state save failed\n";
        ok = false;
    }
    if (!m_world->flushWorld())
    {
        const std::string error =
            persistence ? persistence->status().lastError : std::string("(no status)");
        std::cout << "[world] flush failed: " << error << "\n";
        ok = false;
    }
    return ok;
}

void GameSimulation::shutdown()
{
    if (m_world && m_world->isWorldOpen())
    {
        // Gameplay safety net (issue #228): inventory + items are fully
        // authoritative (no host presentation input), so the unconditional
        // close captures them even when the host forgot savePlayerState.
        // player.state still needs the camera orientation — only the
        // explicit savePlayerState path writes it (unchanged).
        if (WorldPersistence *persistence = m_world->worldPersistence())
        {
            if (!persistence->writeInventoryState(captureInventoryState()))
                std::cerr << "[world] final shutdown: inventory state write failed" << std::endl;
            if (!persistence->writeItemsState(captureItemsState()))
                std::cerr << "[world] final shutdown: items state write failed" << std::endl;
        }
        // Unconditional destructor-only close: resolve stranded edits,
        // capture, flush, shutdown. Destruction has no retry point; a partial
        // save must never be reported as success (issue #180 review round 4).
        if (!m_world->forceCloseWorldForShutdown())
            std::cerr << "[world] final shutdown did not persist all accepted edits" << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Per-frame advance
// ---------------------------------------------------------------------------

void GameSimulation::advancePlayer(double dt, const PlayerCommand &command, bool suspended)
{
    if (suspended)
    {
        m_player.suspend();
        return;
    }
    // Flight is host-driven (the observer moves the body via
    // setPlayerPosition); only the walking controller steps here.
    if (m_playerFlight || !m_world)
        return;
    WorldCollisionView world(*m_world);
    m_player.advance(dt, command, world);
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

void GameSimulation::advanceItems(double dt, bool suspended)
{
    if (!m_world)
        return;
    // Main-thread scoped collision view (the advanceMobs pattern). The world
    // is NOT pumped here: publication ordering stays with the bridge /
    // pumpWorld (issue #114). Pickup proximity uses the authoritative feet.
    WorldCollisionView world(*m_world);
    m_items.update(dt, world, m_player.body.position, m_inventory, suspended);
}

void GameSimulation::pumpWorld()
{
    if (m_world)
        m_world->pump();
}

// ---------------------------------------------------------------------------
// Player commands / queries
// ---------------------------------------------------------------------------

void GameSimulation::suspendPlayer()
{
    m_player.suspend();
}

void GameSimulation::clearPlayerInput()
{
    m_player.clearInput();
}

bool GameSimulation::setPlayerFlight(bool enabled, glm::dvec3 feet)
{
    if (!enabled)
    {
        if (!m_world)
            return false;
        WorldCollisionView world(*m_world);
        physics::Body candidate = m_player.body;
        candidate.position = feet;
        physics::QueryStats queries;
        if (!physics::clear(world, candidate.bounds(), queries))
        {
            m_playerStatus = "Walk unavailable: body overlaps solid or unloaded terrain.";
            return false;
        }
    }
    m_playerFlight = enabled;
    m_playerStatus = "";
    m_player.reset(feet);
    return true;
}

void GameSimulation::setPlayerPosition(glm::dvec3 feet)
{
    m_player.reset(feet);
}

glm::dvec3 GameSimulation::spawnPlayerOnSurface(glm::dvec3 around)
{
    if (!m_world)
        return around;
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
        for (int y = CHUNK_HEIGHT - 1; y >= 0; --y)
        {
            const auto cell = world.sample({x, y, z});
            if (!cell.available) break;
            if (!cell.solid) continue;
            physics::Body candidate;
            candidate.position = {x + 0.5, y + 1.0 + physics::skin, z + 0.5};
            if (physics::clear(world, candidate.bounds(), queries))
            {
                m_player.reset(candidate.position);
                m_playerFlight = false;
                m_playerStatus = "";
                return m_player.eye();
            }
        }
    }
    const glm::dvec3 fallbackEye(around.x, 100.0, around.z);
    m_player.reset(fallbackEye - glm::dvec3(0, m_player.settings.eyeHeight, 0));
    m_playerFlight = true;
    m_playerStatus = "No safe spawn in loaded terrain: fly to an open area.";
    return fallbackEye;
}

bool GameSimulation::restorePlayerState(const PlayerPersistState &saved)
{
    if (!m_world)
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

    m_player.reset(restoredFeet);
    if (waitingForTerrain)
        m_player.body.waitingForTerrain = true;
    m_playerFlight = saved.flight;
    if (saved.selectedBlock >= 0 && saved.selectedBlock < static_cast<int32_t>(COUNT))
        m_selectedBlock = static_cast<TextureType>(saved.selectedBlock);
    std::cout << "[world] restored player at (" << restoredFeet.x << ", " << restoredFeet.y
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
    PlayerPersistState state;
    state.x = m_player.body.position.x;
    state.y = m_player.body.position.y;
    state.z = m_player.body.position.z;
    state.yaw = yaw;
    state.pitch = pitch;
    state.flight = m_playerFlight;
    state.selectedBlock = static_cast<int32_t>(m_selectedBlock);
    return state;
}

worldsave::InventoryPersistState GameSimulation::captureInventoryState() const
{
    worldsave::InventoryPersistState state;
    state.selectedSlot = static_cast<uint8_t>(m_inventory.selectedSlot());
    const std::array<items::ItemStack, items::kInventorySize> &slots = m_inventory.slots();
    for (std::size_t i = 0; i < slots.size(); ++i)
    {
        state.slots[i].itemId = static_cast<uint16_t>(slots[i].item);
        state.slots[i].count = slots[i].count;
    }
    return state;
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
        // The save layer validated ids/counts/selection (decodeInventoryState);
        // makeStack re-canonicalizes the empty stacks.
        std::array<items::ItemStack, items::kInventorySize> slots{};
        for (std::size_t i = 0; i < slots.size(); ++i)
            slots[i] = items::makeStack(static_cast<items::ItemId>(m_savedInventory->slots[i].itemId),
                                        m_savedInventory->slots[i].count);
        m_inventory.restore(slots, m_savedInventory->selectedSlot);
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

bool GameSimulation::canPlaceVoxel(const glm::vec3 &worldPos, TextureType type) const
{
    // Historical Engine placement rule, moved verbatim behind the facade
    // (issue #209): flying players and non-solid blocks never collide with
    // the body; a solid block is refused when it overlaps the authoritative
    // player body, so the body cannot be entombed by its own edit. Logical
    // pending solids immediately participate in subsequent collision
    // queries, so the body cannot enter before publication either.
    if (m_playerFlight || !physics::blockCell(type).solid)
        return true;
    physics::Body playerBody;
    playerBody.position = m_player.body.position;
    return !physics::overlaps(playerBody.bounds(),
                              physics::voxelBounds(glm::ivec3(glm::floor(worldPos))));
}

// ---------------------------------------------------------------------------
// Inventory / hotbar gameplay commands (issue #225, Epic #223 Phase B)
// ---------------------------------------------------------------------------
// Instantaneous command applications at the host frame boundary: no buffer,
// no fixed-step work, world-independent. The deterministic rules live in
// items::Inventory (issue #224); the facade is the ownership + logging seam.

items::ItemResult GameSimulation::selectHotbarSlot(int index)
{
    return m_inventory.setSelectedSlot(index);
}

items::ItemResult GameSimulation::moveStack(int from, int to)
{
    return m_inventory.moveStack(from, to);
}

items::ItemResult GameSimulation::swapStacks(int a, int b)
{
    return m_inventory.swapStacks(a, b);
}

items::ItemResult GameSimulation::splitStack(int from, int to)
{
    return m_inventory.splitStack(from, to);
}

items::AddResult GameSimulation::grantItems(items::ItemId item, uint16_t count)
{
    // Normalize FIRST so the audit line reflects what was actually applied
    // (PR #232 review): a {None, N} sentinel input collapses to the canonical
    // empty and grants nothing — the log must not overstate that as N items.
    const items::ItemStack request = items::makeStack(item, count);
    const items::AddResult result = m_inventory.addStack(request);
    const uint32_t applied = static_cast<uint32_t>(request.count) - result.remainder.count;
    // Test/admin seam (epic #223 §3): EVERY grant is explicitly logged so
    // items entering the inventory without a break/pickup cause stay
    // auditable in headless runs and (later) authoritative server logs.
    const std::string_view name = items::itemName(item);
    std::cout << "[items] grant " << count << "x ";
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
// World interaction commands (issues #226/#227, Epic #223 Phase C/D)
// ---------------------------------------------------------------------------

BreakResult GameSimulation::breakBlock(glm::ivec3 cell, WorldEditResult *outEdit)
{
    if (!m_world)
        return BreakResult::UnknownTerrain;
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
    // Existing world edit semantics (issue #114): the delete may defer with
    // its mirror group; the drop spawns immediately at the cell center.
    const glm::vec3 center(cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f);
    WorldEditResult edit = m_world->deleteVoxel(center);
    if (!edit.accepted)
        return BreakResult::WorldRejected;
    const items::ItemStack drop = items::dropForBlock(type);
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
    const items::ItemStack selected = m_inventory.selectedStack();
    if (selected.isEmpty())
        return PlaceResult::EmptySlot;
    const std::optional<TextureType> block = items::placeableBlockOf(selected.item);
    if (!block.has_value())
        return PlaceResult::NotPlaceable; // future non-block items (none in v1)
    const glm::vec3 center(cell.x + 0.5f, cell.y + 0.5f, cell.z + 0.5f);
    // Player-aware placement rule (#209): the SAME query the creative host
    // gates with — walking bodies refuse solid overlap, flight never does.
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
    m_inventory.removeCount(m_inventory.selectedSlot(), 1);
    if (outEdit)
        *outEdit = std::move(edit);
    return PlaceResult::Ok;
}

RaycastHit GameSimulation::raycastVoxel(glm::vec3 origin, glm::vec3 dir, float maxDist) const
{
    // The historical Engine::raycastVoxel march, moved VERBATIM (issue #227):
    // same float arithmetic, same 0.05 step, same first-active-cell rule and
    // adjacent fallback. VoxelWorld::isVoxelActive is the exact seam the
    // render bridge forwarded to, so parity holds by construction. Unknown
    // cells answer inactive — the march passes through them, never hits.
    RaycastHit result{};
    if (!m_world)
        return result;
    dir = glm::normalize(dir);
    const float step = 0.05f;
    glm::vec3 prev = origin;
    for (float t = 0.f; t <= maxDist; t += step)
    {
        const glm::vec3 p = origin + dir * t;
        const glm::vec3 block(std::floor(p.x), std::floor(p.y), std::floor(p.z));
        if (m_world->isVoxelActive(block))
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
        prev = p;
    }
    return result;
}

items::ItemResult GameSimulation::dropFromSlot(int slot, uint16_t count, glm::dvec3 dir)
{
    const items::RemoveResult removed = m_inventory.removeCount(slot, count);
    if (removed.result != items::ItemResult::Ok)
        return removed.result; // InvalidSlot / EmptyStack
    if (removed.removed.isEmpty())
        return items::ItemResult::Ok; // count 0: no-op, no entity
    // Host-supplied presentation direction (the savePlayerState(yaw,pitch)
    // pattern): normalized here; degenerate OR non-finite input falls back
    // to +Y (an inf component would otherwise normalize to NaN and poison
    // the spawn origin/velocity). The launch speed is a deterministic plain
    // setting — no RNG.
    glm::dvec3 direction = dir;
    const bool finite = std::isfinite(direction.x) && std::isfinite(direction.y) &&
                        std::isfinite(direction.z);
    const double length = glm::length(direction);
    direction = (finite && length > 1e-6) ? direction / length : glm::dvec3(0.0, 1.0, 0.0);
    const glm::dvec3 origin = m_player.eye() + direction * 0.3;
    const glm::dvec3 velocity = direction * m_items.settings.dropSpeed + glm::dvec3(0.0, 1.0, 0.0);
    m_items.spawnPlayerDrop(removed.removed, origin, velocity);
    return items::ItemResult::Ok;
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

PlayerSnapshot GameSimulation::playerSnapshot() const
{
    PlayerSnapshot s;
    s.position = m_player.body.position;
    s.velocity = m_player.body.velocity;
    s.renderEye = m_player.renderEye();
    s.grounded = m_player.body.grounded;
    s.waitingForTerrain = m_player.body.waitingForTerrain;
    s.flight = m_playerFlight;
    s.submerged = m_player.submerged;
    s.metrics = m_player.metrics;
    s.selectedBlock = m_selectedBlock;
    s.status = m_playerStatus;
    return s;
}

void GameSimulation::inventorySnapshot(InventorySnapshot &out) const
{
    // In-place fill of the bounded plain value (the mobSnapshots pattern):
    // one fixed-array copy + the selection, no allocation, no registry work.
    out.slots = m_inventory.slots();
    out.selectedSlot = m_inventory.selectedSlot();
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
