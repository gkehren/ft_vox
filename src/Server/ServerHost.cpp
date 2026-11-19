#include "ServerHost.hpp"

#include <World/WorldPersistence.hpp>

#include <chrono>
#include <thread>
#include <utility>

namespace server
{

ServerHost::ServerHost(HostConfig config) : m_config(std::move(config)) {}

net::udp::SessionWorld ServerHost::sessionWorld() const
{
	const auto world = m_sim->worldSnapshot();
	return {static_cast<uint32_t>(world.seed), world.generatorVersion};
}

net::udp::SessionAdmission ServerHost::joinRemote()
{
	// Until movement authority lands, remote sessions have no positioned
	// body, so joining cannot affect pickup, placement or streaming.
	return {m_sim->createPlayer(), m_ticks, m_sim->world()->nextEditId()};
}

void ServerHost::leaveRemote(sim::PlayerId player)
{
	m_sim->removePlayer(player);
}

std::optional<net::wire::ActionResult> ServerHost::receiveRemote(
	sim::PlayerId player, const net::udp::SessionIntents &intents)
{
	if (!intents.action || !m_sim->hasPlayer(player))
		return std::nullopt;
	return net::wire::ActionResult{intents.action->actionId, m_ticks, m_sim->world()->nextEditId(),
		0, intents.action->operation, net::wire::ActionCode::Unavailable};
}

bool ServerHost::start(std::string &error)
{
	if (m_sim)
	{
		error = "server is already started";
		return false;
	}
	auto worldConfig = m_config.world;
	if (worldConfig.worldName == "." || !WorldPersistence::isValidWorldName(worldConfig.worldName))
	{
		error = "invalid world name";
		return false;
	}
	if (worldConfig.seed == 0 && !WorldPersistence::worldExists(worldConfig.savesRoot, worldConfig.worldName))
		worldConfig.seed = 42; // reproducible default for new worlds

	if (m_config.workers > 0)
		m_workers = std::make_unique<ThreadPool>(m_config.workers);
	auto simulation = std::make_unique<sim::GameSimulation>(m_workers.get());
	const glm::dvec3 center = simulation->initialize(worldConfig, {8.0, 100.0, 8.0});
	if (!simulation->isWorldOpen())
	{
		error = simulation->openWorldError();
		return false;
	}

	simulation->bootstrapTerrain(center);
	const auto spawn = simulation->finalizePlayerSpawn(center);
	if (!simulation->setPlayerInterestRadius(simulation->localPlayerId(), m_config.interestRadiusChunks) ||
		!simulation->advancePlayerStreaming({0, 0, 0}))
	{
		error = "could not initialize player streaming";
		return false;
	}

	m_yaw = spawn.yaw;
	m_pitch = spawn.pitch;
	m_restoredPlayer = spawn.restored;
	m_ticks = 0;
	m_sim = std::move(simulation);
	return true;
}

bool ServerHost::quiesce(std::string &error)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	for (;;)
	{
		m_sim->pumpWorld();
		if (!m_sim->world()->asyncBusy())
			return true;
		if (std::chrono::steady_clock::now() >= deadline)
		{
			error = "terrain publication timed out while quiescing";
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

bool ServerHost::step(TerrainTiming terrainTiming, std::string &error)
{
	if (!m_sim)
	{
		error = "server is not started";
		return false;
	}
	m_sim->advancePlayers(kTickDt, false);
	if (!m_sim->advancePlayerStreaming(m_config.streaming))
	{
		error = "player streaming tick refused";
		return false;
	}
	if (terrainTiming == TerrainTiming::QuiescePerTick && !quiesce(error))
		return false;

	const double radiusBlocks = m_config.interestRadiusChunks * static_cast<double>(CHUNK_SIZE);
	m_sim->advanceMobs(kTickDt, m_sim->playerFeet(), radiusBlocks, false);
	m_sim->advanceItems(kTickDt, false);
	m_sim->pumpWorld();

	// The host owns publication. Phase B1 drains streams with no consumers
	// attached and reuses the storage at each tick boundary.
	m_sim->world()->takeEvents(m_worldEvents);
	m_sim->takeItemEvents(m_itemEvents);
	m_worldEvents.clear();
	m_itemEvents.clear();
	++m_ticks;
	return true;
}

bool ServerHost::save(std::string &error)
{
	if (!quiesce(error))
		return false;
	auto &world = *m_sim->world();
	if (world.hasPendingLogicalEdits())
	{
		// Stop intake and finish accepted edits even when their chunks had
		// no observer or the generation budget was zero.
		world.resolvePendingEditsForPersistenceShutdown();
		if (world.hasPendingLogicalEdits())
		{
			error = "accepted edits still pending after shutdown resolution";
			return false;
		}
	}
	if (!m_sim->savePlayerState(m_yaw, m_pitch))
	{
		error = "save barrier or checkpoint commit failed";
		return false;
	}
	return true;
}

bool ServerHost::stop(std::string &error)
{
	if (!m_sim)
		return true;
	const bool committed = save(error);
	m_finalMetrics = metrics();
	const bool closed = committed && m_sim->world()->closeWorld();
	if (committed && !closed)
		error = "persistent world refused close";
	// Use the existing unconditional recovery path for a refused close. The
	// caller still receives the explicit failure even if that retry succeeds.
	m_sim->shutdown();
	m_sim.reset();
	m_workers.reset();
	return closed;
}

HostMetrics ServerHost::metrics() const
{
	if (!m_sim)
		return m_finalMetrics;
	const auto world = m_sim->worldSnapshot();
	const auto streaming = m_sim->world()->playerStreamingStats();
	return {.ticks = m_ticks,
			.worldEpoch = world.worldGenerationId,
			.nextEditId = world.editRevision,
			.checkpointGeneration = m_sim->worldPersistence()->checkpointGeneration(),
			.chunks = world.chunkCount,
			.players = m_sim->playerCount(),
			.mobs = m_sim->mobs().mobs().size(),
			.items = m_sim->itemEntities().entities().size(),
			.pendingGeneration = m_sim->world()->pendingGenJobs(),
			.waitingForRecord = streaming.waitingForRecord,
			.waitingForGeneration = streaming.waitingForGeneration,
			.seed = world.seed};
}

} // namespace server
