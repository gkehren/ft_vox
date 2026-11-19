#include "ServerHost.hpp"

#include <Network/PlayerInput.hpp>
#include <World/WorldPersistence.hpp>

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <utility>

namespace server
{
namespace
{
namespace udp = net::udp;
namespace wire = net::wire;

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

wire::EntityMotion entityMotion(const physics::Body &body)
{
	wire::EntityMotion motion;
	constexpr int32_t limit = wire::kHorizontalLimit * wire::kEntityPositionScale;
	for (int axis = 0; axis < 3; ++axis)
	{
		motion.position[axis] = static_cast<int32_t>(std::clamp(
			std::round(body.position[axis] * wire::kEntityPositionScale), double(-limit), double(limit - 1)));
		const double velocity = body.velocity[axis];
		motion.velocity[axis] = std::isfinite(velocity) ? static_cast<int16_t>(std::clamp(
			std::round(velocity * wire::kEntityVelocityScale), double(INT16_MIN), double(INT16_MAX))) : 0;
	}
	return motion;
}

template<class Record, size_t N, class Entity, class Decorate>
uint8_t captureEntities(std::array<Record, N> &records, const std::vector<Entity> &population,
	glm::dvec3 feet, int radius, uint64_t Entity::*id, Decorate decorate)
{
	struct Candidate
	{
		const Entity *entity;
		double distanceSquared;
	};
	std::array<Candidate, N> nearest;
	size_t count = 0;
	const auto center = chunkAt(feet);
	const auto closer = [id](const Candidate &a, const Candidate &b) {
		if (a.distanceSquared != b.distanceSquared)
			return a.distanceSquared < b.distanceSquared;
		return a.entity->*id < b.entity->*id;
	};
	for (const auto &entity : population)
	{
		const auto position = entity.body.position;
		const auto representable = [](double coordinate) {
			return coordinate >= -wire::kHorizontalLimit && coordinate < wire::kHorizontalLimit;
		};
		if (!representable(position.x) || !representable(position.y) || !representable(position.z))
			continue;
		const auto coord = chunkAt(position);
		if (!inInterest({coord.x, coord.y}, center, radius))
			continue;
		const auto offset = position - feet;
		const Candidate candidate{&entity, offset.x * offset.x + offset.z * offset.z};
		if (count == N)
		{
			if (!closer(candidate, nearest.front()))
				continue;
			std::pop_heap(nearest.begin(), nearest.end(), closer);
			--count;
		}
		nearest[count++] = candidate;
		std::push_heap(nearest.begin(), nearest.begin() + count, closer);
	}
	std::sort(nearest.begin(), nearest.begin() + count, [id](const Candidate &a, const Candidate &b) {
		return a.entity->*id < b.entity->*id;
	});
	for (size_t i = 0; i < count; ++i)
	{
		const auto &entity = *nearest[i].entity;
		records[i].id = entity.*id;
		records[i].motion = entityMotion(entity.body);
		decorate(records[i], entity);
	}
	return static_cast<uint8_t>(count);
}

wire::ActionCode actionCode(sim::VoxelTargetResult result)
{
	using Target = sim::VoxelTargetResult;
	using Code = wire::ActionCode;
	switch (result)
	{
	case Target::Valid: return Code::Accepted;
	case Target::InvalidTarget: return Code::InvalidTarget;
	case Target::OutOfReach: return Code::OutOfReach;
	case Target::TerrainUnavailable: return Code::TerrainUnavailable;
	case Target::StaleRevision: return Code::StaleRevision;
	case Target::Occupied: return Code::Occupied;
	}
	return Code::Unavailable;
}

wire::ActionCode actionCode(sim::BreakResult result)
{
	using Break = sim::BreakResult;
	using Code = wire::ActionCode;
	switch (result)
	{
	case Break::Ok: return Code::Accepted;
	case Break::Unbreakable: return Code::Forbidden;
	case Break::UnknownTerrain: return Code::TerrainUnavailable;
	case Break::WorldRejected: return Code::InvalidTarget;
	case Break::IdExhausted: return Code::Unavailable;
	case Break::InvalidPlayer: return Code::Forbidden;
	}
	return Code::Unavailable;
}

wire::ActionCode actionCode(sim::PlaceResult result)
{
	using Place = sim::PlaceResult;
	using Code = wire::ActionCode;
	switch (result)
	{
	case Place::Ok: return Code::Accepted;
	case Place::EmptySlot:
	case Place::NotPlaceable: return Code::InventoryRejected;
	case Place::PlayerOverlap:
	case Place::WorldRejected: return Code::Occupied;
	case Place::UnknownTerrain: return Code::TerrainUnavailable;
	case Place::InvalidPlayer: return Code::Forbidden;
	}
	return Code::Unavailable;
}

wire::ActionCode actionCode(items::ItemResult result)
{
	using Result = items::ItemResult;
	using Code = wire::ActionCode;
	switch (result)
	{
	case Result::Ok: return Code::Accepted;
	case Result::Partial: return Code::Partial;
	case Result::IdExhausted: return Code::Unavailable;
	case Result::InvalidPlayer: return Code::Forbidden;
	default: return Code::InventoryRejected;
	}
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
	if (!intents.action || intents.action->actionId != remote.nextActionId || !wire::validAction(*intents.action))
		return std::nullopt;
	++remote.nextActionId;
	return applyAction(player, *intents.action);
}

wire::ActionResult ServerHost::applyAction(sim::PlayerId player, const wire::Action &action)
{
	using Kind = wire::ActionKind;
	using Code = wire::ActionCode;
	wire::ActionResult result{action.actionId, m_ticks, m_sim->world()->nextEditId(),
		0, action.operation, Code::Unavailable};
	switch (action.operation)
	{
	case Kind::Select:
		result.code = actionCode(m_sim->selectHotbarSlot(player, action.slot));
		return result;
	case Kind::DropOne:
	case Kind::DropStack:
	{
		const auto count = action.operation == Kind::DropOne ? uint16_t{1}
			: m_sim->inventory(player)->stack(action.slot).count;
		// Slot intents carry no aim. Use a fixed server-owned toss direction.
		result.code = count == 0 ? Code::InventoryRejected
			: actionCode(m_sim->dropFromSlot(player, action.slot, count, {0, 0, 1}));
		return result;
	}
	case Kind::Move:
		result.code = actionCode(m_sim->moveStack(player, action.slot, action.otherSlot));
		return result;
	case Kind::Swap:
		result.code = actionCode(m_sim->swapStacks(player, action.slot, action.otherSlot));
		return result;
	case Kind::SplitHalf:
		result.code = actionCode(m_sim->splitStack(player, action.slot, action.otherSlot));
		return result;
	case Kind::Break:
	case Kind::Place:
		break;
	}

	const bool placing = action.operation == Kind::Place;
	const auto operation = placing ? sim::VoxelInteraction::Place : sim::VoxelInteraction::Break;
	const glm::ivec3 target{action.target.x, action.target.y, action.target.z};
	const auto aim = wire::aimDirection(action.yawTurns, action.pitchTurns);
	const glm::dvec3 direction{aim[0], aim[1], aim[2]};
	result.code = actionCode(m_sim->validateVoxelTarget(player, operation, target,
		{action.expected.generation, action.expected.revision}, direction));
	if (result.code != Code::Accepted)
		return result;

	WorldEditResult edit;
	result.code = placing ? actionCode(m_sim->placeSelectedBlock(player, target, &edit))
	                      : actionCode(m_sim->breakBlock(player, target, &edit));
	if (result.code == Code::Accepted)
	{
		result.editId = edit.editId;
		result.nextEditId = m_sim->world()->nextEditId();
	}
	return result;
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
		record.motionReset = state.motionReset;
		if (id == player)
			snapshot.localTimers = {state.movementTimers.bufferedJump, state.movementTimers.coyote,
				state.movementTimers.swimJump};
	}
	return snapshot;
}

std::optional<wire::Inventory> ServerHost::inventoryRemote(sim::PlayerId player)
{
	auto &last = m_remotePlayers.at(player).inventory;
	sim::InventorySnapshot state;
	if (!m_sim->inventorySnapshot(player, state))
		return std::nullopt;
	if (last.revision == 0 || last.slots != state.slots || last.selected != state.selectedSlot)
	{
		if (last.revision == UINT64_MAX)
			throw std::overflow_error("inventory revision exhausted");
		last = {m_ticks, last.revision + 1, static_cast<uint8_t>(state.selectedSlot), state.slots};
	}
	return last;
}

std::optional<udp::EntitySnapshots> ServerHost::entitySnapshotsRemote(sim::PlayerId player) const
{
	udp::EntitySnapshots snapshot;
	snapshot.items.tick = snapshot.mobs.tick = m_ticks;
	snapshot.items.idHighWater = m_sim->itemIdHighWater();
	const auto feet = m_sim->playerFeet(player);
	const auto radius = m_config.interestRadiusChunks;
	snapshot.items.count = captureEntities(snapshot.items.items, m_sim->itemEntities().entities(), feet,
		radius, &items::ItemEntity::id, [](auto &record, const auto &entity) { record.stack = entity.stack; });
	snapshot.mobs.count = captureEntities(snapshot.mobs.mobs, m_sim->mobs().mobs(), feet,
		radius, &entities::Mob::replicationId, [](auto &record, const auto &entity) {
			record.species = static_cast<uint8_t>(entity.species);
		});
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
			command = net::playerInput(slot.input);
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
	constexpr auto anchorPolicy = sim::GameSimulation::LocalAnchorPolicy::Exclude;
	m_sim->advancePlayerMobs(kTickDt, radiusBlocks, false, anchorPolicy);
	m_sim->advanceItems(kTickDt, false, anchorPolicy);
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
