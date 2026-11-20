#include "ServerTransport.hpp"
#include "UdpLink.hpp"

#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace net::udp
{
namespace
{
struct Mailbox
{
	uint64_t generation{};
	bool closing{}, confirmed{}, active{}, spawnFailed{};
	uint64_t inputClosedThrough{};
	std::optional<SessionAdmission> admission;
	wire::PlayerKey playerKey{};
	SessionIntents intents;
	std::optional<wire::ActionResult> result;
	std::optional<wire::Inventory> inventory;
	uint64_t stagedInventoryRevision{}, queuedInventoryRevision{}, resultInventoryRevision{};
	std::optional<wire::PlayerSnapshot> snapshot;
	std::optional<uint64_t> lastSnapshotTick;
	std::optional<EntitySnapshots> entities;
	std::optional<uint64_t> lastEntityTick;
	std::optional<WorldUpdate> worldUpdate;
	bool worldBusy{}, baselineBusy{};
	WorldTraffic worldTraffic;

	void stageInventory(const std::optional<wire::Inventory> &update)
	{
		if (update && update->revision > stagedInventoryRevision)
		{
			inventory = update;
			stagedInventoryRevision = update->revision;
		}
	}
};

struct Peer
{
	Address address;
	uint64_t nonce{};
	Time joinDeadline{};
	Time nextSnapshot{};
	Time nextEntitySnapshot{}, nextInventory{};
	std::optional<Link> link;
	std::optional<wire::Welcome> welcome;
	Rate inbound, outbound;
	uint64_t nextAction{1}, lastInputTick{};
	std::unique_ptr<ChunkTransfer> transfer;
	uint8_t nextFragment{};
	bool worldInFlight{};
	std::optional<Time> resyncDeadline;
	bool confirmed{}, closing{}, waitingForResult{};
};

struct OwnerInbox
{
	size_t slot{};
	std::optional<SessionAdmission> admission;
	wire::PlayerKey playerKey{};
	SessionIntents intents;
	bool closing{}, confirmed{}, active{}, spawnFailed{};
};

struct Discovery
{
	Rate inbound, outbound;
};

size_t checkedCap(size_t cap)
{
	if (cap > 255)
		throw std::invalid_argument("remote session cap exceeds 255");
	return cap;
}
} // namespace

struct ServerTransport::Impl
{
	Impl(Address bind, size_t cap, SessionWorld world)
		: socket(bind), world(world), worldId(world.id ? world.id : randomId()),
		  peers(checkedCap(cap)), mailboxes(peers.size())
	{
		ownerInbox.reserve(peers.size());
	}

	Sender sender(Peer &peer)
	{
		return {socket, peer.address, wire::Endpoint::Server, peer.outbound, globalOut};
	}

	void releaseBaseline(Mailbox &box)
	{
		if (std::exchange(box.baselineBusy, false))
			--activeBaselines;
	}

	void markClosing(Peer &peer, Mailbox &box)
	{
		peer.closing = box.closing = true;
		box.intents = {};
		box.result.reset();
		box.inventory.reset();
		box.snapshot.reset();
		box.entities.reset();
		box.worldUpdate.reset();
		peer.transfer.reset();
		releaseBaseline(box);
	}

	void close(size_t slot)
	{
		std::lock_guard lock(mutex);
		markClosing(peers[slot], mailboxes[slot]);
	}

	void sendWelcome(Peer &peer, Time now)
	{
		if (peer.welcome)
			sender(peer).send(now, {peer.link->scope, 0, *peer.welcome});
	}

	Discovery &discovery(Address address)
	{
		// Fixed hash buckets prevent per-source allocation under address churn.
		// Colliding IPv4 sources share the same conservative discovery budget.
		const uint32_t hash = address.ipv4 * 0x9e3779b9u;
		return discoveryRates[hash >> 24];
	}

	void hello(Address from, Peer *existing, const wire::Hello &msg, Time now)
	{
		auto &rates = discovery(from);
		if (!rates.inbound.takeKind(now, wire::Kind::Hello))
			return;
		const auto reject = [&](wire::RejectReason reason) {
			Sender{socket, from, wire::Endpoint::Server, rates.outbound, globalOut}.send(now,
				{{}, 0, wire::Reject{msg.nonce, reason, wire::kVersion, wire::kVersion}});
		};
		if (const auto reason = wire::rejectHello(msg, compatibility))
			return reject(*reason);
		if (existing)
		{
			if (!existing->closing && existing->nonce == msg.nonce)
				sendWelcome(*existing, now);
			// A different nonce alone cannot displace a scoped session.
			return;
		}
		const auto available = std::find_if(peers.begin(), peers.end(), [](const Peer &p) { return !p.link; });
		if (available == peers.end())
			return reject(wire::RejectReason::Full);

		if (generation == UINT64_MAX)
			throw std::runtime_error("UDP session generation exhausted");
		uint64_t capability;
		do
		{
			capability = randomId();
		}
		while (std::any_of(peers.begin(), peers.end(), [capability](const Peer &p) {
			return p.link && p.link->scope.sessionId == capability;
		}));
		available->address = from;
		available->nonce = msg.nonce;
		available->joinDeadline = now + kJoinTimeout;
		available->link.emplace(wire::SessionScope{capability, epoch}, now);
		const auto slot = static_cast<size_t>(available - peers.begin());
		std::lock_guard lock(mutex);
		mailboxes[slot].generation = ++generation;
		mailboxes[slot].playerKey = msg.playerKey;
	}

	void receive(const Received &frame, Time now)
	{
		if (!globalIn.take(now, frame.size, true) || frame.from.port == 0)
			return;
		auto it = std::find_if(peers.begin(), peers.end(), [&](const Peer &p) {
			return p.link && p.address == frame.from;
		});
		auto &rate = it == peers.end() ? discovery(frame.from).inbound : it->inbound;
		if (!rate.traffic.take(now, frame.size) || frame.size > wire::kDatagramBytes)
			return;
		wire::Packet packet;
		if (wire::decode(wire::Endpoint::Client, {frame.bytes.data(), frame.size}, packet) != wire::CodecStatus::Ok)
			return;
		if (const auto *msg = std::get_if<wire::Hello>(&packet.payload))
			return hello(frame.from, it == peers.end() ? nullptr : &*it, *msg, now);
		if (it == peers.end() || !it->welcome || it->closing || it->link->scope != packet.scope)
			return;
		const auto kind = kindOf(packet);
		if (!rate.takeKind(now, kind))
			return;

		const size_t slot = static_cast<size_t>(it - peers.begin());
		auto &peer = *it;
		auto &link = *peer.link;
		if (const auto *ack = std::get_if<wire::Ack>(&packet.payload))
		{
			if (link.acknowledge(ack->through))
				link.lastReceived = now;
			return;
		}
		const auto *action = std::get_if<wire::Action>(&packet.payload);
		const bool departing = kind == wire::Kind::Disconnect || kind == wire::Kind::Resync;
		if (action && (peer.waitingForResult || action->actionId != peer.nextAction ||
		              (link.pending && link.pending->kind == wire::Kind::ActionResult)))
		{
			// Capacity/content refusal cannot advance the receive cursor. An
			// earlier admission can still be acknowledged if its ACK was lost.
			link.ack = true;
			return;
		}
		{
			std::lock_guard lock(mutex);
			auto &box = mailboxes[slot];
			const auto *input = std::get_if<wire::Input>(&packet.payload);
			if (input && (!withinInputWindow(input->intendedTick, ownerTick) ||
			              input->intendedTick <= box.inputClosedThrough ||
			              input->intendedTick < peer.lastInputTick))
				return;
			const auto status = link.incoming.admit(packet);
			if (wire::policy(kind)->lane == wire::Lane::Reliable)
				link.ack = true;
			if (status != wire::ReceiveStatus::Accepted)
				return;
			link.lastReceived = now;
			if (departing)
			{
			if (kind == wire::Kind::Resync && !peer.resyncDeadline)
				{
					++worldTraffic.resyncs;
					++box.worldTraffic.resyncs;
				}
				markClosing(peer, box);
			}
			else
				peer.confirmed = box.confirmed = true;

			if (input)
			{
				// Within the six-tick window different ticks cannot collide.
				// A newer sequence for the same tick replaces the sample.
				box.intents.inputs[input->intendedTick % kInputLeadTicks] = {packet.sequence, *input};
				peer.lastInputTick = input->intendedTick;
			}
			if (action)
			{
				box.intents.action = *action;
				peer.waitingForResult = true;
				++peer.nextAction;
			}
		}
		if (departing)
			link.flushAck(sender(peer), now);
	}

	void refresh(size_t slot, Time now)
	{
		auto &peer = peers[slot];
		if (!peer.link)
			return;
		if (peer.closing)
		{
			std::lock_guard lock(mutex);
			if (mailboxes[slot].generation == 0)
				peer = {};
			return;
		}
		if ((!peer.confirmed && now >= peer.joinDeadline) || peer.link->expired(now) ||
		    (peer.resyncDeadline && now >= *peer.resyncDeadline))
			return close(slot);

		std::optional<SessionAdmission> admission;
		bool spawnFailed;
		{
			std::lock_guard lock(mutex);
			admission = mailboxes[slot].admission;
			spawnFailed = mailboxes[slot].spawnFailed;
		}
		if (spawnFailed)
		{
			sender(peer).send(now, {peer.link->scope, peer.link->reliableSent + 1,
				wire::Disconnect{wire::DisconnectReason::Resync}});
			return close(slot);
		}
		if (!peer.welcome && admission)
		{
			if (!admission->player.valid())
			{
				sender(peer).send(now, {{}, 0, wire::Reject{
					peer.nonce, admission->refusal, wire::kVersion, wire::kVersion}});
				return close(slot);
			}
			peer.welcome = wire::Welcome{peer.nonce, worldId, admission->player, world.seed,
				world.generatorVersion, compatibility, admission->tick, admission->nextEditId};
			sendWelcome(peer, now);
		}
	}

	void queueReliable(size_t slot, Time now)
	{
		auto &peer = peers[slot];
		auto &link = *peer.link;
		if (link.pending || peer.resyncDeadline)
			return;
		std::optional<wire::ActionResult> result;
		std::optional<wire::Inventory> inventory;
		std::optional<WorldUpdate> update;
		{
			std::lock_guard lock(mutex);
			auto &box = mailboxes[slot];
			if (peer.worldInFlight && (!peer.transfer || peer.nextFragment == wire::kFragmentsPerChunk))
			{
				peer.transfer.reset();
				releaseBaseline(box);
				peer.worldInFlight = box.worldBusy = false;
			}
			// The reliable ACK for the required inventory precedes the action
			// result. Unrelated inventory changes cannot starve that result.
			if (box.result && box.resultInventoryRevision <= box.queuedInventoryRevision)
				result = std::exchange(box.result, std::nullopt);
			else if (box.inventory && now >= peer.nextInventory)
			{
				inventory = std::exchange(box.inventory, std::nullopt);
				box.queuedInventoryRevision = inventory->revision;
			}
			if (!result && !inventory && !peer.worldInFlight)
				update = std::exchange(box.worldUpdate, std::nullopt);
		}
		if (result)
		{
			link.queue(wire::Endpoint::Server, *result, now);
			peer.waitingForResult = false;
			return;
		}
		if (inventory)
		{
			link.queue(wire::Endpoint::Server, *inventory, now);
			peer.nextInventory = now + 100ms;
			return;
		}
		if (update)
		{
			peer.worldInFlight = true;
			std::visit([&](auto &value) {
				if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::unique_ptr<ChunkTransfer>>)
				{
					peer.transfer = std::move(value);
					peer.nextFragment = 0;
				}
				else
				{
				if constexpr (std::is_same_v<std::decay_t<decltype(value)>, wire::Resync>)
				{
					std::lock_guard lock(mutex);
					++worldTraffic.resyncs;
					++mailboxes[slot].worldTraffic.resyncs;
					peer.resyncDeadline = now + kReliableTimeout;
				}
					link.queue(wire::Endpoint::Server, value, now);
				}
			}, *update);
		}
		if (peer.transfer)
		{
			wire::ChunkFragment fragment;
			fragment.baseline = peer.transfer->baseline;
			fragment.index = peer.nextFragment++;
			std::copy_n(peer.transfer->materials.begin() + fragment.index * wire::kFragmentBytes,
				wire::fragmentSize(fragment.index), fragment.materials.begin());
			link.queue(wire::Endpoint::Server, fragment, now);
		}
	}

	void flushReliable(size_t slot, Time now)
	{
		queueReliable(slot, now);
		auto &peer = peers[slot];
		auto &link = *peer.link;
		const bool retransmit = link.pending && link.pending->sent;
		if (link.flush(sender(peer), now) &&
		    (link.pending->kind == wire::Kind::ChunkFragment || link.pending->kind == wire::Kind::ChunkRemove ||
		     link.pending->kind == wire::Kind::WorldEdit || link.pending->kind == wire::Kind::Resync))
		{
			std::lock_guard lock(mutex);
			for (auto *traffic : {&worldTraffic, &mailboxes[slot].worldTraffic})
			{
				++traffic->frames;
				traffic->bytes += link.pending->datagram.view().size();
				traffic->retransmits += retransmit;
			}
		}
	}

	void flushEntities(size_t slot, Time now)
	{
		auto &peer = peers[slot];
		if (now < peer.nextEntitySnapshot)
			return;
		std::optional<EntitySnapshots> entities;
		{
			std::lock_guard lock(mutex);
			entities = std::exchange(mailboxes[slot].entities, std::nullopt);
		}
		if (!entities)
			return;
		// Each family has its own budget, leaving global headroom for
		// player snapshots, terrain and reliable controls under load.
		if (itemCapacity.take(now, 1, 256, 24))
			peer.link->sendFresh(sender(peer), now, entities->items);
		if (mobCapacity.take(now, 1, 256, 24))
			peer.link->sendFresh(sender(peer), now, entities->mobs);
		peer.nextEntitySnapshot = now + 100ms;
	}

	void flushSnapshot(size_t slot, Time now)
	{
		auto &peer = peers[slot];
		if (now < peer.nextSnapshot)
			return;
		std::optional<wire::PlayerSnapshot> snapshot;
		{
			std::lock_guard lock(mutex);
			snapshot = std::exchange(mailboxes[slot].snapshot, std::nullopt);
		}
		if (snapshot)
		{
			// Preserve aggregate headroom for ordered world data and controls
			// when more peers are active than can sustain 20 snapshots/s.
			if (snapshotCapacity.take(now, 1, 768, 64))
				peer.link->sendFresh(sender(peer), now, *snapshot);
			peer.nextSnapshot = now + 50ms;
		}
	}

	void poll(Time now)
	{
		for (size_t i = 0; i < peers.size(); ++i)
			refresh((firstPeer + i) % peers.size(), now);
		Received frame;
		for (unsigned i = 0; i < kReadBatch && socket.receive(frame); ++i)
			receive(frame, now);
		for (size_t i = 0; i < peers.size(); ++i)
		{
			const auto slot = (firstPeer + i) % peers.size();
			auto &peer = peers[slot];
			if (peer.confirmed && !peer.closing)
				flushReliable(slot, now);
		}
		for (size_t i = 0; i < peers.size(); ++i)
		{
			const auto slot = (firstPeer + i) % peers.size();
			if (peers[slot].confirmed && !peers[slot].closing)
				flushSnapshot(slot, now);
		}
		for (size_t i = 0; i < peers.size(); ++i)
		{
			const auto slot = (firstPeer + i) % peers.size();
			if (peers[slot].confirmed && !peers[slot].closing)
				flushEntities(slot, now);
		}
		if (!peers.empty())
			firstPeer = (firstPeer + 1) % peers.size();
	}

	void shutdownIo()
	{
		const auto now = Clock::now();
		for (auto &peer : peers)
		{
			if (peer.confirmed && !peer.closing && !peer.link->pending &&
			    peer.link->reliableSent < wire::kMaxSequence)
				sender(peer).send(now, {peer.link->scope, peer.link->reliableSent + 1,
					wire::Disconnect{wire::DisconnectReason::Shutdown}});
		}
	}

	void serviceOwner(SessionOwner &owner, uint64_t completedTick, bool discardInputs)
	{
		ownerInbox.clear();
		{
			std::lock_guard lock(mutex);
			ownerTick = completedTick;
			for (size_t slot = 0; slot < mailboxes.size(); ++slot)
			{
				auto &box = mailboxes[slot];
				if (!box.generation)
					continue;
				if (discardInputs)
					box.intents.inputs = {};
				ownerInbox.push_back({slot, box.admission, std::exchange(box.playerKey, {}),
					std::exchange(box.intents, {}),
					box.closing, box.confirmed, box.active, box.spawnFailed});
				box.inputClosedThrough = completedTick + 1;
			}
		}
		// Freeze one tick's inbox before resolving joins, departures and
		// actions. Slot reuse must not decide which actor wins a conflict.
		for (const auto &work : ownerInbox)
		{
			if (!work.closing)
				continue;
			if (work.admission && work.admission->player.valid())
				owner.leaveRemote(work.admission->player);
			std::lock_guard lock(mutex);
			mailboxes[work.slot] = {};
		}
		for (auto &[slot, admission, playerKey, intents, closing, confirmed, active, spawnFailed] : ownerInbox)
		{
			if (closing)
				continue;
			auto &box = mailboxes[slot];
			if (!admission)
			{
				const auto joined = owner.joinRemote(playerKey);
				{
					std::lock_guard lock(mutex);
					if (!box.closing)
					{
						box.admission = joined;
						continue;
					}
				}
				if (joined.player.valid())
					owner.leaveRemote(joined.player);
				continue;
			}
			if (!confirmed || spawnFailed)
				continue;
			const auto player = admission->player;
			if (!active)
			{
				active = owner.activateRemote(player);
				std::lock_guard lock(mutex);
				box.active = active;
				box.spawnFailed = !active;
			}
		}
		std::erase_if(ownerInbox, [](const auto &work) {
			return work.closing || !work.active ||
				(!work.intents.action && std::ranges::none_of(work.intents.inputs, &InputSample::sequence));
		});
		std::ranges::sort(ownerInbox, {}, [](const auto &work) { return work.admission->player.value; });
		for (const auto &work : ownerInbox)
		{
			if (const auto result = owner.receiveRemote(work.admission->player, work.intents))
			{
				const auto inventory = owner.inventoryRemote(work.admission->player);
				std::lock_guard lock(mutex);
				auto &box = mailboxes[work.slot];
				if (!box.closing)
				{
					box.result = result;
					box.stageInventory(inventory);
					box.resultInventoryRevision = inventory ? inventory->revision : 0;
				}
			}
		}
		// Capture after all activations and retirements at this boundary.
		// The IO worker cannot reuse a slot before the owner retires it.
		const auto start = firstWorldPeer;
		for (size_t i = 0; i < mailboxes.size(); ++i)
		{
			const auto slot = (start + i) % mailboxes.size();
			auto &box = mailboxes[slot];
			sim::PlayerId player;
			bool snapshotDue, entitiesDue, worldReady, allowBaseline;
			{
				std::lock_guard lock(mutex);
				if (!box.active || box.closing)
					continue;
				player = box.admission->player;
				snapshotDue = completedTick % kSnapshotIntervalTicks == 0 && box.lastSnapshotTick != completedTick;
				entitiesDue = completedTick % kEntitySnapshotIntervalTicks == 0 && box.lastEntityTick != completedTick;
				worldReady = !box.worldBusy;
				allowBaseline = activeBaselines < kMaxConcurrentChunkTransfers;
			}
			std::optional<wire::PlayerSnapshot> snapshot;
			if (snapshotDue)
				snapshot = owner.snapshotRemote(player);
			const auto inventory = owner.inventoryRemote(player);
			auto entities = entitiesDue ? owner.entitySnapshotsRemote(player) : std::nullopt;
			auto worldUpdate = worldReady ? owner.nextWorldUpdate(player, allowBaseline) : std::nullopt;
			if (!snapshot && !inventory && !entities && !worldUpdate)
				continue;
			std::lock_guard lock(mutex);
			if (box.closing)
				continue;
			box.stageInventory(inventory);
			if (entities)
			{
				box.entities = std::move(entities);
				box.lastEntityTick = completedTick;
			}
			if (snapshot)
			{
				box.snapshot = std::move(snapshot);
				box.lastSnapshotTick = completedTick;
			}
			if (worldUpdate)
			{
				box.worldBusy = true;
				if (std::holds_alternative<std::unique_ptr<ChunkTransfer>>(*worldUpdate))
				{
					if (!allowBaseline)
						throw std::logic_error("chunk baseline produced without a transfer slot");
					box.baselineBusy = true;
					++activeBaselines;
					firstWorldPeer = (slot + 1) % mailboxes.size();
				}
				box.worldUpdate = std::move(worldUpdate);
			}
		}
	}

	Socket socket;
	SessionWorld world;
	const uint64_t worldId, epoch{randomId()};
	const wire::Compatibility compatibility{wire::localCompatibility()};
	std::vector<Peer> peers;
	std::vector<Mailbox> mailboxes;
	std::vector<OwnerInbox> ownerInbox;
	std::array<Discovery, 256> discoveryRates;
	Traffic globalIn, globalOut;
	Bucket snapshotCapacity, itemCapacity, mobCapacity;
	uint64_t generation{};
	uint64_t ownerTick{}; // published and read under mutex
	size_t firstPeer{}, firstWorldPeer{};
	size_t activeBaselines{}; // shared under mutex
	std::mutex mutex;
	WorldTraffic worldTraffic; // lifetime totals, shared under mutex
	std::exception_ptr error;
	std::jthread worker; // joins before releasing any socket/IO state
};

ServerTransport::ServerTransport(Address bind, size_t remoteCap, SessionWorld world)
	: m_impl(std::make_unique<Impl>(bind, remoteCap, world)) {}

ServerTransport::~ServerTransport() = default;

Address ServerTransport::address() const { return m_impl->socket.address(); }

WorldTraffic ServerTransport::worldTraffic() const
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->worldTraffic;
}

std::optional<WorldTraffic> ServerTransport::worldTraffic(sim::PlayerId player) const
{
	std::lock_guard lock(m_impl->mutex);
	const auto found = std::ranges::find_if(m_impl->mailboxes, [player](const Mailbox &box) {
		return box.active && !box.closing && box.admission->player == player;
	});
	return found == m_impl->mailboxes.end() ? std::nullopt : std::optional{found->worldTraffic};
}

void ServerTransport::start()
{
	if (m_impl->worker.joinable())
		throw std::logic_error("UDP worker already started");
	m_impl->worker = std::jthread([impl = m_impl.get()](std::stop_token token) {
		try
		{
			while (!token.stop_requested())
			{
				impl->poll(Clock::now());
				std::this_thread::sleep_for(5ms);
			}
			impl->shutdownIo();
		}
		catch (...)
		{
			std::lock_guard lock(impl->mutex);
			impl->error = std::current_exception();
		}
	});
}

void ServerTransport::poll(Clock::time_point now)
{
	if (m_impl->worker.joinable())
		throw std::logic_error("cannot pump UDP while its worker is running");
	m_impl->poll(now);
}

void ServerTransport::rethrowIoError()
{
	std::exception_ptr error;
	{
		std::lock_guard lock(m_impl->mutex);
		error = m_impl->error;
	}
	if (error)
		std::rethrow_exception(error);
}

void ServerTransport::serviceOwner(SessionOwner &owner, uint64_t completedTick, bool discardInputs)
{
	m_impl->serviceOwner(owner, completedTick, discardInputs);
}

void ServerTransport::stop(SessionOwner &owner)
{
	if (m_impl->worker.joinable())
	{
		m_impl->worker.request_stop();
		m_impl->worker.join();
	}
	for (size_t slot = 0; slot < m_impl->peers.size(); ++slot)
	{
		if (m_impl->peers[slot].link && !m_impl->peers[slot].closing)
			m_impl->close(slot);
	}
	serviceOwner(owner, m_impl->ownerTick);
}

} // namespace net::udp
