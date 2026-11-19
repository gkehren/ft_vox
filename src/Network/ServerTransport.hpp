#pragma once

#include "UdpSocket.hpp"

#include <memory>

namespace net::udp
{

struct SessionWorld
{
	uint32_t seed, generatorVersion;
};

struct SessionAdmission
{
	sim::PlayerId player;
	uint64_t tick, nextEditId;
};

inline constexpr uint64_t kInputLeadTicks = wire::kInputLeadTicks;
inline constexpr uint64_t kSnapshotIntervalTicks = wire::kPlayerSnapshotIntervalTicks;
inline constexpr uint64_t kEntitySnapshotIntervalTicks = 6;
inline constexpr size_t kMaxConcurrentChunkTransfers = 16;

constexpr bool withinInputWindow(uint64_t intended, uint64_t completed)
{
	return intended > completed && intended - completed <= kInputLeadTicks;
}

struct InputSample
{
	uint64_t sequence{};
	wire::Input input;
};

// One slot per eligible tick. Zero sequence marks a vacant slot.
using InputQueue = std::array<InputSample, kInputLeadTicks>;

struct SessionIntents
{
	InputQueue inputs{};
	std::optional<wire::Action> action;
};

struct ChunkTransfer
{
	wire::ChunkBaseline baseline;
	std::array<uint8_t, wire::kChunkBytes> materials;
};

using WorldUpdate = std::variant<std::unique_ptr<ChunkTransfer>, wire::WorldEdit, wire::ChunkRemove, wire::Resync>;

struct WorldTraffic
{
	uint64_t bytes{}, frames{}, retransmits{}, resyncs{};
};

struct EntitySnapshots
{
	wire::ItemSnapshot items;
	wire::MobSnapshot mobs;
};

class SessionOwner
{
public:
	virtual ~SessionOwner() = default;
	virtual SessionAdmission joinRemote() = 0;
	virtual bool activateRemote(sim::PlayerId player) = 0;
	virtual void leaveRemote(sim::PlayerId player) = 0;
	// Every admitted action gets a result. Inputs are acknowledged via snapshots.
	virtual std::optional<wire::ActionResult> receiveRemote(sim::PlayerId player, const SessionIntents &intents) = 0;
	virtual wire::PlayerSnapshot snapshotRemote(sim::PlayerId player) const = 0;
	virtual std::optional<wire::Inventory> inventoryRemote(sim::PlayerId) { return std::nullopt; }
	virtual std::optional<EntitySnapshots> entitySnapshotsRemote(sim::PlayerId) const { return std::nullopt; }
	// Called only when the peer's single world send slot is available.
	virtual std::optional<WorldUpdate> nextWorldUpdate(sim::PlayerId, bool /*allowBaseline*/) { return std::nullopt; }
};

class ServerTransport
{
public:
	using Clock = std::chrono::steady_clock;
	ServerTransport(Address bind, size_t remoteCap, SessionWorld world);
	~ServerTransport();
	Address address() const;
	WorldTraffic worldTraffic() const;
	std::optional<WorldTraffic> worldTraffic(sim::PlayerId player) const;

	// Production runs poll on its own worker. Tests may pump the same IO
	// implementation with an injected monotonic clock instead of starting it.
	void start();
	void poll(Clock::time_point now);
	void rethrowIoError();
	void stop(SessionOwner &owner);

	// Call before stepping the next simulation tick. Drain one batch of
	// intents in PlayerId order and publish snapshots of the completed state.
	// A host stall may discard buffered movement without discarding actions.
	void serviceOwner(SessionOwner &owner, uint64_t completedTick, bool discardInputs = false);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace net::udp
