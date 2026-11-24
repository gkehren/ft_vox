#pragma once

#include <Sim/PlayerId.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <span>

// CPU-only values shared by the transport, dedicated host and local launcher.
namespace monitoring
{
inline constexpr size_t kMaxPlayers = 255;
inline constexpr auto kSampleInterval = std::chrono::seconds(1);
inline constexpr auto kMaxAge = std::chrono::seconds(3);

enum class SessionState : uint8_t { Syncing = 1, Connected, Disconnecting };

struct Player
{
	sim::PlayerId id;
	SessionState state{};
};

struct TransportSample
{
	uint64_t receivedBytes{}, sentBytes{}, worldBytes{}, retransmits{}, resyncs{};
	uint16_t count{}, capacity{}, worldSends{}, baselines{};
	std::array<Player, kMaxPlayers> players{};

	std::span<const Player> roster() const { return std::span{players}.first(count); }
};

// Local inherited pipe only. No peer endpoint, key, capability or role data.
struct Status
{
	uint64_t sampledMs{}, uptimeMs{}, ticks{}, droppedTicks{}, checkpoint{};
	uint64_t chunks{}, generating{}, waitingForRecord{}, waitingForGeneration{}, registeredProfiles{}, offlineProfiles{};
	TransportSample transport;
};

// FTMN, u32 version, sixteen u64 values, four u16 values and a fixed
// roster of (u32 ID, u8 phase). Unused roster slots are zero.
inline constexpr size_t kFrameSize = 8 + 16 * 8 + 4 * 2 + kMaxPlayers * 5;
using Frame = std::array<uint8_t, kFrameSize>;
Frame encode(const Status &status);
std::optional<Status> decode(const Frame &frame);

// On Windows and Linux steady_clock uses the machine's monotonic clock, so
// the launcher can also reject samples that sat in the child pipe.
uint64_t monotonicMs();
bool fresh(uint64_t sampleMs, uint64_t nowMs);
}
