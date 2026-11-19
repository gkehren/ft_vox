#include "ServerHost.hpp"

#include <World/WorldPersistence.hpp>

#include <chrono>
#include <cmath>
#include <thread>
#include <utility>

namespace server
{
namespace
{
namespace udp = net::udp;
namespace wire = net::wire;

sim::PlayerCommand commandFor(const wire::Input &input)
{
	return {{input.moveX, input.moveZ},
		(input.flags & wire::Input::sprint) != 0, (input.flags & wire::Input::jump) != 0,
		(input.flags & wire::Input::swimUp) != 0, (input.flags & wire::Input::swimDown) != 0};
}

glm::ivec2 chunkAt(glm::dvec3 feet)
{
	return {floorDivChunk(static_cast<int>(std::floor(feet.x))),
		floorDivChunk(static_cast<int>(std::floor(feet.z)))};
}

bool inInterest(wire::ChunkCoord coord, glm::ivec2 center, int radius)
{
	const int64_t x = static_cast<int64_t>(coord.x) - center.x;
	const int64_t z = static_cast<int64_t>(coord.z) - center.y;
	return x * x + z * z <= radius * radius;
}

uint8_t wireMaterial(uint8_t material)
{
	return material == AIR ? wire::kAirMaterial : material;
}
} // namespace

ServerHost::ServerHost(HostConfig config) : m_config(std::move(config)) {}

net::udp::SessionWorld ServerHost::sessionWorld() const
{
	const auto world = m_sim->worldSnapshot();
	return {static_cast<uint32_t>(world.seed), world.generatorVersion};
}

net::udp::SessionAdmission ServerHost::joinRemote()
{
	const auto player = m_sim->createPlayer();
	if (player.valid())
		m_remotePlayers.try_emplace(player);
	return {player, m_ticks, m_sim->world()->nextEditId()};
}

bool ServerHost::activateRemote(sim::PlayerId player)
{
	return m_remotePlayers.contains(player) &&
		m_sim->setPlayerInterestRadius(player, m_config.interestRadiusChunks) &&
		m_sim->trySpawnWalkingPlayerOnSurface(player, m_spawnCenter);
}

void ServerHost::leaveRemote(sim::PlayerId player)
{
	if (m_remotePlayers.erase(player))
		m_sim->removePlayer(player);
}

std::optional<net::wire::ActionResult> ServerHost::receiveRemote(
	sim::PlayerId player, const net::udp::SessionIntents &intents)
{
	const auto found = m_remotePlayers.find(player);
	if (found == m_remotePlayers.end() || !m_sim->playerPositioned(player))
		return std::nullopt;
	auto &remote = found->second;
	for (uint64_t lead = 1; lead <= udp::kInputLeadTicks; ++lead)
	{
		const auto &sample = intents.inputs[(m_ticks + lead) % udp::kInputLeadTicks];
		const auto tick = sample.input.intendedTick;
		if (!udp::withinInputWindow(tick, m_ticks) || tick < remote.lastQueuedTick ||
		    sample.sequence <= remote.lastQueuedSequence || sample.sequence > wire::kMaxSequence ||
		    !wire::validInput(sample.input))
			continue;
		remote.inputs[tick % udp::kInputLeadTicks] = sample;
		remote.lastQueuedSequence = sample.sequence;
		remote.lastQueuedTick = tick;
	}
	if (!intents.action)
		return std::nullopt;
	return net::wire::ActionResult{intents.action->actionId, m_ticks, m_sim->world()->nextEditId(),
		0, intents.action->operation, net::wire::ActionCode::Unavailable};
}

wire::PlayerSnapshot ServerHost::snapshotRemote(sim::PlayerId player) const
{
	wire::PlayerSnapshot snapshot;
	snapshot.tick = m_ticks;
	snapshot.lastProcessedInput = m_remotePlayers.at(player).lastProcessedInput;
	const auto center = chunkAt(m_sim->playerFeet(player));
	const int radius = m_config.interestRadiusChunks;
	unsigned others = 0;
	for (const auto &[id, remote] : m_remotePlayers)
	{
		if (!m_sim->playerPositioned(id))
			continue;
		if (id != player)
		{
			if (others == wire::kMaxPlayersPerSnapshot - 1)
				continue;
			const auto coord = chunkAt(m_sim->playerFeet(id));
			if (!inInterest({coord.x, coord.y}, center, radius))
				continue;
			++others;
		}
		sim::PlayerSnapshot state;
		m_sim->playerSnapshot(id, state);
		auto &record = snapshot.players[snapshot.count++];
		record.player = id;
		record.feet = {state.position.x, state.position.y, state.position.z};
		record.velocity = {static_cast<float>(state.velocity.x), static_cast<float>(state.velocity.y),
			static_cast<float>(state.velocity.z)};
		record.flags = (state.grounded ? wire::PlayerState::grounded : 0u) |
			(state.waitingForTerrain ? wire::PlayerState::waitingForTerrain : 0u) |
			(state.flight ? wire::PlayerState::flight : 0u);
	}
	return snapshot;
}

void ServerHost::distributeWorldEdits()
{
	const bool complete = m_sim->world()->drainLogicalEdits(m_logicalEdits);
	if (m_logicalEdits.empty() && complete)
		return;
	for (auto &[id, remote] : m_remotePlayers)
	{
		if (remote.chunks.empty() || remote.worldGap)
			continue;
		if (!complete)
		{
			remote.worldGap = true;
			remote.edits.clear();
			continue;
		}
		// Track the installed or scheduled projection until its removal is
		// ordered. The actor may leave and re-enter while a transfer is busy.
		for (const auto &edit : m_logicalEdits)
		{
			const wire::ChunkCoord coord{floorDivChunk(edit.cell.x), floorDivChunk(edit.cell.z)};
			const auto found = remote.chunks.find(coord);
			if (found == remote.chunks.end() ||
			    found->second.generation != edit.base.generation || found->second.revision > edit.base.revision)
				continue;
			if (found->second.revision != edit.base.revision || remote.edits.size() == wire::kReliableWindow)
			{
				remote.worldGap = true;
				remote.edits.clear();
				break;
			}
			remote.edits.push_back({edit.id, {edit.cell.x, edit.cell.z, static_cast<uint8_t>(edit.cell.y)},
				{edit.base.generation, edit.base.revision}, wireMaterial(static_cast<uint8_t>(edit.type))});
			++found->second.revision;
		}
	}
}

std::optional<udp::WorldUpdate> ServerHost::nextWorldUpdate(sim::PlayerId player, bool allowBaseline)
{
	const auto radius = m_config.interestRadiusChunks;
	if (radius == 0)
		return std::nullopt;
	auto &remote = m_remotePlayers.at(player);
	if (remote.worldGap || remote.nextTransferId > wire::kMaxSequence)
		return wire::Resync{wire::ResyncReason::Baseline};
	const auto center = chunkAt(m_sim->playerFeet(player));
	const auto &world = *m_sim->world();
	for (auto it = remote.chunks.begin(); it != remote.chunks.end(); ++it)
	{
		const auto coord = it->first;
		const auto version = world.chunkContentVersion({coord.x, 0, coord.z});
		if (inInterest(coord, center, radius) && version && version->generation == it->second.generation)
			continue;
		const wire::ChunkRemove remove{coord, it->second.generation};
		std::erase_if(remote.edits, [&](const auto &edit) { return edit.base.generation == remove.generation; });
		remote.chunks.erase(it);
		return remove;
	}
	if (!remote.edits.empty())
	{
		const auto edit = remote.edits.front();
		remote.edits.pop_front();
		return edit;
	}
	if (!allowBaseline)
		return std::nullopt;
	for (const auto offset : VoxelWorld::playerInterestOffsets(radius))
	{
		const wire::ChunkCoord coord{center.x + offset.x, center.y + offset.z};
		const glm::ivec3 coreCoord{coord.x, 0, coord.z};
		if (remote.chunks.contains(coord) || !world.chunkContentVersion(coreCoord))
			continue;
		auto transfer = std::make_unique<udp::ChunkTransfer>();
		ChunkContentVersion version;
		const auto watermark = world.captureChunkContent(coreCoord, transfer->materials, version);
		std::ranges::transform(transfer->materials, transfer->materials.begin(), wireMaterial);
		transfer->baseline = {remote.nextTransferId++, coord, {version.generation, version.revision},
			watermark, wire::chunkChecksum(transfer->materials)};
		remote.chunks.emplace(coord, transfer->baseline.version);
		return udp::WorldUpdate{std::move(transfer)};
	}
	return std::nullopt;
}

void ServerHost::suspendPlayerInput()
{
	for (auto &[id, remote] : m_remotePlayers)
		remote.inputs = {};
	m_sim->advancePlayers(0, true);
}

void ServerHost::advancePlayers()
{
	m_sim->advancePlayer(kTickDt, {}, false);
	const auto tick = m_ticks + 1;
	for (auto &[id, remote] : m_remotePlayers)
	{
		if (!m_sim->playerPositioned(id))
			continue;
		auto &slot = remote.inputs[tick % udp::kInputLeadTicks];
		sim::PlayerCommand command{};
		if (slot.sequence != 0 && slot.input.intendedTick == tick)
		{
			command = commandFor(slot.input);
			remote.lastProcessedInput = slot.sequence;
			slot = {};
		}
		m_sim->advancePlayer(id, kTickDt, command, false);
	}
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
	m_spawnCenter = center;
	simulation->world()->observeLogicalEdits();
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
	advancePlayers();
	if (!m_sim->advancePlayerStreaming(m_config.streaming))
	{
		error = "player streaming tick refused";
		return false;
	}
	if (terrainTiming == TerrainTiming::QuiescePerTick && !quiesce(error))
		return false;

	const double radiusBlocks = m_config.interestRadiusChunks * static_cast<double>(CHUNK_SIZE);
	m_sim->advanceMobs(kTickDt, m_sim->playerFeet(), radiusBlocks, false);
	// Only the compatibility anchor's inventory is checkpointed. Keep
	// shared items out of temporary remote inventories until pickup lands.
	m_sim->advanceItems(kTickDt, false, sim::GameSimulation::ItemPickup::LocalOnly);
	m_sim->pumpWorld();

	distributeWorldEdits();
	// The render and item event streams have no consumers in this host.
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
	m_remotePlayers.clear();
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
