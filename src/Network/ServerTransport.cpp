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
	SessionIntents intents;
	std::optional<wire::ActionResult> result;
	std::optional<wire::PlayerSnapshot> snapshot;
	std::optional<uint64_t> lastSnapshotTick;
};

struct Peer
{
	Address address;
	uint64_t nonce{};
	Time joinDeadline{};
	Time nextSnapshot{};
	std::optional<Link> link;
	std::optional<wire::Welcome> welcome;
	Rate inbound, outbound;
	uint64_t nextAction{1}, lastInputTick{};
	bool confirmed{}, closing{}, waitingForResult{};
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
		: socket(bind), world(world), peers(checkedCap(cap)), mailboxes(peers.size()) {}

	Sender sender(Peer &peer)
	{
		return {socket, peer.address, wire::Endpoint::Server, peer.outbound, globalOut};
	}

	static void markClosing(Peer &peer, Mailbox &box)
	{
		peer.closing = box.closing = true;
		box.intents = {};
		box.result.reset();
		box.snapshot.reset();
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
		if (action && (peer.waitingForResult || link.pending || action->actionId != peer.nextAction))
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
				markClosing(peer, box);
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
		if ((!peer.confirmed && now >= peer.joinDeadline) || peer.link->expired(now))
			return close(slot);

		std::optional<SessionAdmission> admission;
		std::optional<wire::ActionResult> result;
		bool spawnFailed;
		{
			std::lock_guard lock(mutex);
			admission = mailboxes[slot].admission;
			result = std::exchange(mailboxes[slot].result, std::nullopt);
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
					peer.nonce, wire::RejectReason::Unavailable, wire::kVersion, wire::kVersion}});
				return close(slot);
			}
			peer.welcome = wire::Welcome{peer.nonce, worldId, admission->player, world.seed,
				world.generatorVersion, compatibility, admission->tick, admission->nextEditId};
			sendWelcome(peer, now);
		}
		if (result)
		{
			peer.link->queue(wire::Endpoint::Server, *result, now);
			peer.waitingForResult = false;
		}
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
			{
				peer.link->flush(sender(peer), now);
				flushSnapshot(slot, now);
			}
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
		{
			std::lock_guard lock(mutex);
			ownerTick = completedTick;
			if (discardInputs)
				for (auto &box : mailboxes)
					box.intents.inputs = {};
		}
		for (auto &box : mailboxes)
		{
			SessionIntents intents;
			std::optional<SessionAdmission> admission;
			bool closing, confirmed, active, spawnFailed;
			{
				std::lock_guard lock(mutex);
				if (!box.generation)
					continue;
				admission = box.admission;
				closing = box.closing;
				confirmed = box.confirmed;
				active = box.active;
				spawnFailed = box.spawnFailed;
				intents = std::exchange(box.intents, {});
				// Intake for the tick about to run has closed for this slot.
				box.inputClosedThrough = completedTick + 1;
			}
			if (closing)
			{
				if (admission && admission->player.valid())
					owner.leaveRemote(admission->player);
				std::lock_guard lock(mutex);
				box = {};
				continue;
			}
			if (!admission)
			{
				const auto joined = owner.joinRemote();
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
			if (active && (intents.action || std::ranges::any_of(intents.inputs, &InputSample::sequence)))
			{
				const auto result = owner.receiveRemote(player, intents);
				if (result)
				{
					std::lock_guard lock(mutex);
					if (!box.closing)
						box.result = result;
				}
			}
		}
		if (completedTick % kSnapshotIntervalTicks != 0)
			return;
		// Capture after all activations and retirements at this boundary.
		// The IO worker cannot reuse a slot before the owner retires it.
		for (auto &box : mailboxes)
		{
			sim::PlayerId player;
			{
				std::lock_guard lock(mutex);
				if (!box.active || box.closing || box.lastSnapshotTick == completedTick)
					continue;
				player = box.admission->player;
			}
			auto snapshot = owner.snapshotRemote(player);
			std::lock_guard lock(mutex);
			if (!box.closing)
			{
				box.snapshot = std::move(snapshot);
				box.lastSnapshotTick = completedTick;
			}
		}
	}

	Socket socket;
	SessionWorld world;
	const uint64_t worldId{randomId()}, epoch{randomId()};
	const wire::Compatibility compatibility{wire::localCompatibility()};
	std::vector<Peer> peers;
	std::vector<Mailbox> mailboxes;
	std::array<Discovery, 256> discoveryRates;
	Traffic globalIn, globalOut;
	uint64_t generation{};
	uint64_t ownerTick{}; // published and read under mutex
	size_t firstPeer{};
	std::mutex mutex;
	std::exception_ptr error;
	std::jthread worker; // joins before releasing any socket/IO state
};

ServerTransport::ServerTransport(Address bind, size_t remoteCap, SessionWorld world)
	: m_impl(std::make_unique<Impl>(bind, remoteCap, world)) {}

ServerTransport::~ServerTransport() = default;

Address ServerTransport::address() const { return m_impl->socket.address(); }

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
