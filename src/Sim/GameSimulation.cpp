// GameSimulation — CPU-owned simulation facade (issue #208, Epic #204 Phase D).
// The player/mob/world rules implemented here are the historical Engine rules
// moved verbatim behind a render-free API: fixed-step rates, pause/focus
// suspension semantics, spawn/restore validation, reload refusal and the
// issue #180 persistence contracts are unchanged. See the header for the
// ownership / policy boundaries and docs/simulation-render-contracts.md.
#include "Sim/GameSimulation.hpp"

#include <World/WorldCollisionView.hpp>
#include <World/WorldMobView.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>

namespace sim
{

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
    m_mobs.reset(static_cast<uint64_t>(seed));
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
    const WorldPersistence *persistence = m_world->worldPersistence();
    bool ok = true;
    if (!persistence || !persistence->writePlayerState(playerState))
    {
        std::cout << "[world] player state save failed\n";
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
