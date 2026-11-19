#include "ClientTransport.hpp"
#include "UdpLink.hpp"

#include <Chunk/TerrainGenerator.hpp>

#include <stdexcept>
#include <utility>

namespace net::udp
{
namespace
{
template<class Snapshot>
bool newerSnapshot(const Snapshot &sample, const SnapshotHistory<Snapshot> &history, uint64_t joinedTick)
{
	return sample.tick >= joinedTick && (!history.current || sample.tick > history.current->tick);
}

template<class Snapshot>
void installSnapshot(SnapshotHistory<Snapshot> &history, const Snapshot &sample)
{
	history.previous = std::exchange(history.current, sample);
}
}

struct ClientTransport::Impl
{
	explicit Impl(Address server) : server(server)
	{
		if (server.port == 0 || server.ipv4 == 0)
			throw std::invalid_argument("expected a concrete server endpoint");
	}

	void finish(ClientStatus next)
	{
		status = next;
		link.reset();
		welcome.reset();
		playerSnapshot.reset();
		clearAction();
		nextActionId = 1;
		clearProjection();
	}

	void clearAction()
	{
		pendingAction.reset();
		actionResult.reset();
	}

	void clearProjection()
	{
		chunks.clear();
		inventory.reset();
		itemHistory = {};
		mobHistory = {};
		assembler.reset();
		assemblyOpen = false;
		lastTransferId = lastEditId = 0;
	}

	void connect(Time now)
	{
		finish(ClientStatus::Connecting);
		nonce = randomId();
		rejection.reset();
		heartbeats = 0;
		deadline = now + kJoinTimeout;
		nextHello = now;
	}

	void resync(wire::ResyncReason reason, Time now)
	{
		if (status != ClientStatus::Connected)
			return;
		clearProjection();
		playerSnapshot.reset();
		clearAction();
		resyncReason = reason;
		status = ClientStatus::Resyncing;
		queueDeparture(now);
	}

	void queueDeparture(Time now)
	{
		if (link->pending || link->reliableSent == wire::kMaxSequence)
			return;
		if (status == ClientStatus::Resyncing)
			link->queue(wire::Endpoint::Client, wire::Resync{resyncReason}, now);
		else if (status == ClientStatus::Leaving)
			link->queue(wire::Endpoint::Client, wire::Disconnect{wire::DisconnectReason::Leaving}, now);
	}

	std::optional<uint64_t> sendAction(wire::Action action, Time now)
	{
		if (status != ClientStatus::Connected || link->pending || pendingAction || actionResult ||
		    link->reliableSent == wire::kMaxSequence)
			return std::nullopt;
		action.actionId = nextActionId;
		if (!wire::validAction(action))
			return std::nullopt;
		link->queue(wire::Endpoint::Client, action, now);
		pendingAction = action;
		actionDeadline = now + 2 * kReliableTimeout;
		return nextActionId++;
	}

	void fragment(const wire::ChunkFragment &part, Time now)
	{
		const auto &baseline = part.baseline;
		if (baseline.transferId <= lastTransferId)
			return;
		const auto result = assembler.accept(part, now);
		assemblyOpen = result == wire::AssemblyStatus::InProgress;
		if (assemblyOpen)
			return;
		if (result != wire::AssemblyStatus::Complete)
		{
			const auto reason = result == wire::AssemblyStatus::BadChecksum ? wire::ResyncReason::Checksum :
				result == wire::AssemblyStatus::Expired ? wire::ResyncReason::Timeout : wire::ResyncReason::Baseline;
			return resync(reason, now);
		}
		lastTransferId = baseline.transferId;
		const auto found = chunks.find(baseline.coord);
		if (found != chunks.end())
		{
			const auto &version = found->second.version;
			if (baseline.version.generation < version.generation ||
			    (baseline.version.generation == version.generation && baseline.version.revision < version.revision))
				return;
			if (baseline.nextEditId < found->second.nextEditId)
				return resync(wire::ResyncReason::Baseline, now);
		}
		else if (chunks.size() == wire::kMaxReplicatedChunks)
			return resync(wire::ResyncReason::Baseline, now);
		if (baseline.nextEditId < welcome->nextEditId)
			return resync(wire::ResyncReason::Baseline, now);
		auto &chunk = chunks.try_emplace(baseline.coord).first->second;
		chunk.version = baseline.version;
		chunk.nextEditId = baseline.nextEditId;
		std::ranges::copy(assembler.bytes(), chunk.materials.begin());
	}

	void edit(const wire::WorldEdit &update, Time now)
	{
		const auto &cell = update.cell;
		const wire::ChunkCoord coord{floorDivChunk(cell.x), floorDivChunk(cell.z)};
		const auto found = chunks.find(coord);
		if (found == chunks.end())
			return resync(wire::ResyncReason::Baseline, now);
		auto &chunk = found->second;
		if (update.base.generation < chunk.version.generation)
			return;
		if (update.base.generation == chunk.version.generation && update.editId < chunk.nextEditId &&
		    update.base.revision < chunk.version.revision)
			return; // already represented in the installed baseline or delta history
		if (update.base != chunk.version || update.editId < chunk.nextEditId || update.editId <= lastEditId)
			return resync(wire::ResyncReason::Baseline, now);
		const auto x = cell.x - coord.x * CHUNK_SIZE;
		const auto z = cell.z - coord.z * CHUNK_SIZE;
		chunk.materials[(cell.y * CHUNK_SIZE + z) * CHUNK_SIZE + x] = update.material;
		++chunk.version.revision;
		chunk.nextEditId = update.editId + 1;
		lastEditId = update.editId;
	}

	void project(const wire::Packet &packet, Time now)
	{
		if (const auto *part = std::get_if<wire::ChunkFragment>(&packet.payload))
			return fragment(*part, now);
		if (assemblyOpen)
			return resync(wire::ResyncReason::Baseline, now);
		if (const auto *delta = std::get_if<wire::WorldEdit>(&packet.payload))
			return edit(*delta, now);
		const auto &remove = std::get<wire::ChunkRemove>(packet.payload);
		const auto found = chunks.find(remove.coord);
		if (found != chunks.end() && found->second.version.generation == remove.generation)
			chunks.erase(found);
	}

	Sender sender()
	{
		return {socket, server, wire::Endpoint::Client, outbound, globalOut};
	}

	void receive(const Received &frame, Time now)
	{
		if (frame.from != server || !inbound.traffic.take(now, frame.size) || frame.size > wire::kDatagramBytes)
			return;
		wire::Packet packet;
		if (wire::decode(wire::Endpoint::Server, {frame.bytes.data(), frame.size}, packet) != wire::CodecStatus::Ok ||
		    !inbound.takeKind(now, kindOf(packet)))
			return;
		if (status == ClientStatus::Connecting)
		{
			if (const auto *reply = std::get_if<wire::Welcome>(&packet.payload); reply && reply->nonce == nonce)
			{
				if (reply->content != wire::localCompatibility() ||
				    reply->generatorVersion != TerrainGenerator::kGeneratorVersion)
				{
					rejection = wire::RejectReason::Content;
					return finish(ClientStatus::Rejected);
				}
				link.emplace(packet.scope, now);
				welcome = *reply;
				status = ClientStatus::Connected;
			}
			else if (const auto *reject = std::get_if<wire::Reject>(&packet.payload);
			         reject && reject->nonce == nonce)
			{
				rejection = reject->reason;
				finish(ClientStatus::Rejected);
			}
			return;
		}
		if (!link || packet.scope != link->scope)
			return;
		if (const auto *ack = std::get_if<wire::Ack>(&packet.payload))
		{
			if (!link->pending)
				return;
			const auto kind = link->pending->kind;
			if (!link->acknowledge(ack->through))
				return;
			link->lastReceived = now;
			if (status == ClientStatus::Leaving && (kind == wire::Kind::Disconnect || kind == wire::Kind::Resync))
				finish(ClientStatus::Disconnected);
			else if (status == ClientStatus::Resyncing && kind == wire::Kind::Resync)
				connect(now);
			return;
		}
		const auto kind = kindOf(packet);
		const auto *players = std::get_if<wire::PlayerSnapshot>(&packet.payload);
		const auto *items = std::get_if<wire::ItemSnapshot>(&packet.payload);
		const auto *mobs = std::get_if<wire::MobSnapshot>(&packet.payload);
		if (players && (players->tick < welcome->tick || (playerSnapshot && players->tick <= playerSnapshot->tick) ||
		                std::ranges::none_of(std::span(players->players).first(players->count),
		                     [this](const auto &player) { return player.player == welcome->player; })))
			return;
		if (items && (!newerSnapshot(*items, itemHistory, welcome->tick) ||
		              (itemHistory.current && items->idHighWater < itemHistory.current->idHighWater)))
			return;
		if (mobs && !newerSnapshot(*mobs, mobHistory, welcome->tick))
			return;
		const auto lane = wire::policy(kind)->lane;
		if (!players && !items && !mobs && kind != wire::Kind::Heartbeat && lane != wire::Lane::Reliable)
			return;
		const auto admitted = link->incoming.admit(packet);
		if (lane == wire::Lane::Reliable)
			link->ack = true; // includes replay/gap, so a lost ACK cannot deadlock the sender
		if (admitted != wire::ReceiveStatus::Accepted)
			return;
		link->lastReceived = now;
		if (players && status == ClientStatus::Connected)
			playerSnapshot = *players;
		else if (items && status == ClientStatus::Connected)
			installSnapshot(itemHistory, *items);
		else if (mobs && status == ClientStatus::Connected)
			installSnapshot(mobHistory, *mobs);
		else if (kind == wire::Kind::Heartbeat)
			++heartbeats;
		else if (kind == wire::Kind::Disconnect)
		{
			link->flushAck(sender(), now);
			if (status == ClientStatus::Resyncing)
				connect(now);
			else
				finish(ClientStatus::Disconnected);
		}
		else if (const auto *request = std::get_if<wire::Resync>(&packet.payload))
			resync(request->reason, now);
		else if (const auto *state = std::get_if<wire::Inventory>(&packet.payload))
		{
			if (status == ClientStatus::Connected && state->tick >= welcome->tick &&
			    (!inventory || (state->revision > inventory->revision && state->tick >= inventory->tick)))
				inventory = *state;
		}
		else if (const auto *result = std::get_if<wire::ActionResult>(&packet.payload))
		{
			if (pendingAction && result->actionId == pendingAction->actionId &&
			    result->operation == pendingAction->operation)
			{
				actionResult = *result;
				pendingAction.reset();
			}
		}
		else if (status == ClientStatus::Connected && (kind == wire::Kind::ChunkFragment ||
		         kind == wire::Kind::WorldEdit || kind == wire::Kind::ChunkRemove))
			project(packet, now);
	}

	void poll(Time now)
	{
		if (status == ClientStatus::Resyncing && link->expired(now))
			connect(now);
		else if ((status == ClientStatus::Connecting && now >= deadline) || (link && link->expired(now)) ||
		         (pendingAction && now >= actionDeadline))
			finish(ClientStatus::TimedOut);
		if (status == ClientStatus::Connected && assembler.expire(now))
			resync(wire::ResyncReason::Timeout, now);
		if (status == ClientStatus::Connecting && now >= nextHello &&
		    sender().send(now, {{}, 0, wire::Hello{nonce, wire::kVersion, wire::kVersion, wire::localCompatibility()}}))
			nextHello = now + kHelloInterval;
		Received frame;
		for (unsigned i = 0; i < kReadBatch && socket.receive(frame); ++i)
			receive(frame, now);
		if (link)
		{
			queueDeparture(now);
			link->flush(sender(), now);
		}
	}

	Socket socket{{0, 0}};
	Address server;
	ClientStatus status{ClientStatus::Disconnected};
	std::optional<Link> link;
	std::optional<wire::Welcome> welcome;
	std::optional<wire::PlayerSnapshot> playerSnapshot;
	std::optional<wire::Inventory> inventory;
	SnapshotHistory<wire::ItemSnapshot> itemHistory;
	SnapshotHistory<wire::MobSnapshot> mobHistory;
	std::optional<wire::RejectReason> rejection;
	std::optional<wire::Action> pendingAction;
	std::optional<wire::ActionResult> actionResult;
	uint64_t nextActionId{1};
	Time actionDeadline{};
	wire::ResyncReason resyncReason{};
	ChunkProjection chunks;
	wire::ChunkAssembler assembler;
	uint64_t lastTransferId{}, lastEditId{};
	bool assemblyOpen{};
	Rate inbound, outbound;
	Traffic globalOut;
	Time deadline{}, nextHello{};
	uint64_t nonce{}, heartbeats{};
};

ClientTransport::ClientTransport(Address server) : m_impl(std::make_unique<Impl>(server)) {}
ClientTransport::~ClientTransport() = default;

void ClientTransport::connect(Clock::time_point now)
{
	if (m_impl->status == ClientStatus::Connecting || m_impl->link)
		throw std::logic_error("UDP client must finish the current session before joining");
	m_impl->connect(now);
}

void ClientTransport::poll(Clock::time_point now) { m_impl->poll(now); }

void ClientTransport::requestWorldResync(Clock::time_point now) { m_impl->resync(wire::ResyncReason::Baseline, now); }

void ClientTransport::disconnect(Clock::time_point now)
{
	if (m_impl->status == ClientStatus::Connecting)
		m_impl->finish(ClientStatus::Disconnected);
	if (m_impl->status == ClientStatus::Connected)
	{
		m_impl->clearAction();
		m_impl->status = ClientStatus::Leaving;
		m_impl->queueDeparture(now);
	}
	else if (m_impl->status == ClientStatus::Resyncing)
		m_impl->status = ClientStatus::Leaving; // a queued Resync can complete the departure
}

ClientStatus ClientTransport::status() const { return m_impl->status; }
bool ClientTransport::sendInput(const wire::Input &input, Clock::time_point now)
{
	return m_impl->status == ClientStatus::Connected && wire::validInput(input) &&
		m_impl->link->sendFresh(m_impl->sender(), now, input);
}

std::optional<uint64_t> ClientTransport::sendAction(wire::Action action, Clock::time_point now)
{
	return m_impl->sendAction(action, now);
}

std::optional<wire::ActionResult> ClientTransport::takeActionResult()
{
	return std::exchange(m_impl->actionResult, std::nullopt);
}

std::optional<wire::Welcome> ClientTransport::welcome() const { return m_impl->welcome; }
std::optional<wire::SessionScope> ClientTransport::scope() const
{
	return m_impl->link ? std::optional{m_impl->link->scope} : std::nullopt;
}
std::optional<wire::PlayerSnapshot> ClientTransport::latestPlayerSnapshot() const { return m_impl->playerSnapshot; }
std::optional<wire::Inventory> ClientTransport::inventory() const { return m_impl->inventory; }
const SnapshotHistory<wire::ItemSnapshot> &ClientTransport::itemSnapshots() const { return m_impl->itemHistory; }
const SnapshotHistory<wire::MobSnapshot> &ClientTransport::mobSnapshots() const { return m_impl->mobHistory; }
const ChunkProjection &ClientTransport::chunks() const { return m_impl->chunks; }
std::optional<wire::RejectReason> ClientTransport::rejection() const { return m_impl->rejection; }
uint64_t ClientTransport::heartbeatsReceived() const { return m_impl->heartbeats; }

} // namespace net::udp
