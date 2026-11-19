#pragma once

#include "UdpSocket.hpp"

#include <map>
#include <memory>

namespace net::udp
{

enum class ClientStatus { Disconnected, Connecting, Connected, Leaving, Resyncing, TimedOut, Rejected };

struct ReplicatedChunk
{
	wire::ChunkVersion version;
	uint64_t nextEditId{};
	// Canonical y/z/x order using wire material ordinals and wire AIR (255).
	std::array<uint8_t, wire::kChunkBytes> materials;
};

using ChunkProjection = std::map<wire::ChunkCoord, ReplicatedChunk>;

template<class Snapshot>
struct SnapshotHistory
{
	std::optional<Snapshot> previous;
	std::optional<Snapshot> current;
};

// v2 client IO adapter. The graphical host owns the bounded nonblocking pump
// and its connection state on the frame thread.
class ClientTransport
{
public:
	using Clock = std::chrono::steady_clock;
	explicit ClientTransport(Address server);
	~ClientTransport();
	void connect(Clock::time_point now);
	void poll(Clock::time_point now);
	void disconnect(Clock::time_point now);
	// Retires the current projection and rejoins with a fresh session.
	void requestWorldResync(Clock::time_point now);
	// Sends one fresh input sample while connected. False means no datagram
	// was sent (invalid payload, rate limit, or socket backpressure).
	bool sendInput(const wire::Input &input, Clock::time_point now);
	// Queues one reliable action using the next ID in this session. The
	// caller supplies the observed chunk version for a voxel action.
	// Empty means busy, disconnected or invalid. Retried by poll().
	std::optional<uint64_t> sendAction(wire::Action action, Clock::time_point now);
	// Consume the result to free the action slot. The projection is updated
	// independently by the authoritative world stream.
	std::optional<wire::ActionResult> takeActionResult();

	ClientStatus status() const;
	std::optional<wire::Welcome> welcome() const;
	std::optional<wire::SessionScope> scope() const;
	// Latest authoritative state for the current scope. Cleared on departure.
	std::optional<wire::PlayerSnapshot> latestPlayerSnapshot() const;
	const SnapshotHistory<wire::PlayerSnapshot> &playerSnapshots() const;
	std::optional<wire::Inventory> inventory() const;
	// Current defines the active set. Interpolate only IDs also in previous.
	// Read on the pump owner thread. Both histories clear on scope retirement.
	const SnapshotHistory<wire::ItemSnapshot> &itemSnapshots() const;
	const SnapshotHistory<wire::MobSnapshot> &mobSnapshots() const;
	// Only verified, fully installed chunks. Read on the pump owner thread.
	const ChunkProjection &chunks() const;
	std::optional<wire::RejectReason> rejection() const;
	uint64_t heartbeatsReceived() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace net::udp
