#pragma once

#include <Items/ItemTypes.hpp>
#include <Sim/PlayerId.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace net::wire
{

// The authoritative protocol, separate from the test-only UDP v1 prototype.
// See docs/adr/authoritative-wire-protocol.md for byte layouts and delivery.
inline constexpr uint16_t kVersion = 2;
inline constexpr uint16_t kItemSchema = 1;
inline constexpr size_t kDatagramBytes = 1200;
inline constexpr size_t kHeaderBytes = 36;
inline constexpr size_t kMaxPlayersPerSnapshot = 24;
inline constexpr size_t kChunkBytes = 65536;
inline constexpr size_t kMaxReplicatedChunks = 197; // radius-eight disk
inline constexpr size_t kFragmentBytes = 1100;
inline constexpr size_t kFragmentsPerChunk = (kChunkBytes + kFragmentBytes - 1) / kFragmentBytes;
inline constexpr int32_t kHorizontalLimit = 1 << 23;
inline constexpr uint8_t kAirMaterial = 255;
inline constexpr uint64_t kMaxSequence = UINT64_MAX - 1;
inline constexpr size_t kReliableWindow = 64;
inline constexpr auto kChunkTimeout = std::chrono::seconds(5);

constexpr size_t fragmentSize(uint8_t index)
{
    if (index >= kFragmentsPerChunk)
        return 0;
    return index + 1u == kFragmentsPerChunk
        ? kChunkBytes - index * kFragmentBytes
        : kFragmentBytes;
}

enum class Kind : uint8_t
{
    Hello = 1, Welcome = 2, Reject = 3, Input = 4, PlayerSnapshot = 5,
    Action = 6, ActionResult = 7, Inventory = 8, WorldEdit = 9,
    ChunkFragment = 10, ChunkRemove = 11, Resync = 12, Ack = 13,
    Heartbeat = 14, Disconnect = 15
};
enum class Endpoint : uint8_t { Client, Server };
enum class Direction : uint8_t { ClientToServer, ServerToClient, Both };
enum class Lane : uint8_t { Handshake, Fresh, Reliable, Ack };

struct Policy
{
    Direction direction;
    Lane lane;
    uint16_t maxPayload;
    uint8_t messagesPerSecond;
    uint8_t burst;
};

// Null for a tag outside this version's vocabulary. Budgets are per peer
// per direction, with aggregate limits and retransmission rules in the ADR.
const Policy *policy(Kind kind) noexcept;

struct SessionScope
{
    uint64_t sessionId{};
    uint64_t epoch{};
    bool operator==(const SessionScope &) const = default;
};

struct Compatibility
{
    uint16_t itemSchema{};
    uint64_t catalog{};
    uint64_t generator{};
    bool operator==(const Compatibility &) const = default;
};

Compatibility localCompatibility() noexcept;
uint64_t chunkChecksum(std::span<const uint8_t> bytes) noexcept;

struct Hello
{
    static constexpr Kind kind = Kind::Hello;
    uint64_t nonce{};
    uint16_t minVersion{}, maxVersion{};
    Compatibility content;
    bool operator==(const Hello &) const = default;
};

struct Welcome
{
    static constexpr Kind kind = Kind::Welcome;
    uint64_t nonce{}, worldId{};
    sim::PlayerId player;
    uint32_t seed{}, generatorVersion{};
    Compatibility content;
    uint64_t tick{}, nextEditId{};
    bool operator==(const Welcome &) const = default;
};

enum class RejectReason : uint8_t { Version = 1, Content = 2, Full = 3, Unavailable = 4 };
struct Reject
{
    static constexpr Kind kind = Kind::Reject;
    uint64_t nonce{};
    RejectReason reason{};
    uint16_t minVersion{}, maxVersion{};
    bool operator==(const Reject &) const = default;
};

// Negotiation on a syntactically valid Hello. An empty result selects v2.
std::optional<RejectReason> rejectHello(const Hello &hello, Compatibility local) noexcept;

struct Input
{
    static constexpr Kind kind = Kind::Input;
    static constexpr uint8_t sprint = 1, jump = 2, swimUp = 4, swimDown = 8;
    uint64_t intendedTick{};
    float moveX{}, moveZ{};
    uint8_t flags{};
    bool operator==(const Input &) const = default;
};

bool validInput(const Input &input) noexcept;

struct PlayerState
{
    static constexpr uint8_t grounded = 1, waitingForTerrain = 2, flight = 4;
    sim::PlayerId player;
    std::array<double, 3> feet{};
    std::array<float, 3> velocity{};
    uint8_t flags{};
    bool operator==(const PlayerState &) const = default;
};

struct PlayerSnapshot
{
    static constexpr Kind kind = Kind::PlayerSnapshot;
    uint64_t tick{}, lastProcessedInput{};
    uint8_t count{};
    std::array<PlayerState, kMaxPlayersPerSnapshot> players{};

    // Only the active prefix represents player state on the wire.
    constexpr bool operator==(const PlayerSnapshot &other) const noexcept
    {
        const auto active = std::min<size_t>(count, players.size());
        return tick == other.tick && lastProcessedInput == other.lastProcessedInput &&
               count == other.count &&
               std::equal(players.begin(), players.begin() + active, other.players.begin());
    }
};

struct Cell
{
    int32_t x{}, z{};
    uint8_t y{};
    bool operator==(const Cell &) const = default;
};

struct ChunkCoord
{
    int32_t x{}, z{};
    auto operator<=>(const ChunkCoord &) const = default;
};

struct ChunkVersion
{
    uint64_t generation{}, revision{};
    bool operator==(const ChunkVersion &) const = default;
};

enum class ActionKind : uint8_t
{
    Break = 1, Place = 2, Select = 3, DropOne = 4, DropStack = 5,
    Move = 6, Swap = 7, SplitHalf = 8
};

// Only the fields for the selected kind are populated. The fixed layout
// requires zero in unused fields. Slot operations express intent, not counts.
struct Action
{
    static constexpr Kind kind = Kind::Action;
    uint64_t actionId{};
    ActionKind operation{};
    uint8_t slot{}, otherSlot{};
    Cell target{};
    ChunkVersion expected{};
    float yawTurns{}, pitchTurns{};
    bool operator==(const Action &) const = default;
};

enum class ActionCode : uint8_t
{
    Accepted = 0, Partial = 1, InvalidTarget = 2, OutOfReach = 3,
    TerrainUnavailable = 4, StaleRevision = 5, InventoryRejected = 6,
    Occupied = 7, RateLimited = 8, Forbidden = 9, Unavailable = 10
};
struct ActionResult
{
    static constexpr Kind kind = Kind::ActionResult;
    uint64_t actionId{}, tick{}, nextEditId{}, editId{};
    ActionKind operation{};
    ActionCode code{};
    bool operator==(const ActionResult &) const = default;
};

struct Inventory
{
    static constexpr Kind kind = Kind::Inventory;
    uint64_t tick{}, revision{};
    uint8_t selected{};
    std::array<items::ItemStack, items::kInventorySize> slots{};
    bool operator==(const Inventory &) const = default;
};

struct WorldEdit
{
    static constexpr Kind kind = Kind::WorldEdit;
    uint64_t editId{};
    Cell cell;
    ChunkVersion base;
    uint8_t material{};
    bool operator==(const WorldEdit &) const = default;
};

struct ChunkBaseline
{
    uint64_t transferId{};
    ChunkCoord coord;
    ChunkVersion version;
    uint64_t nextEditId{}, checksum{};
    bool operator==(const ChunkBaseline &) const = default;
};

struct ChunkFragment
{
    static constexpr Kind kind = Kind::ChunkFragment;
    ChunkBaseline baseline;
    uint8_t index{};
    std::array<uint8_t, kFragmentBytes> materials{};

    // The final fragment uses only the prefix defined by its index.
    constexpr bool operator==(const ChunkFragment &other) const noexcept
    {
        const auto size = fragmentSize(index);
        const auto active = size == 0 ? materials.size() : size;
        return baseline == other.baseline && index == other.index &&
               std::equal(materials.begin(), materials.begin() + active, other.materials.begin());
    }
};

struct ChunkRemove
{
    static constexpr Kind kind = Kind::ChunkRemove;
    ChunkCoord coord;
    uint64_t generation{};
    bool operator==(const ChunkRemove &) const = default;
};

enum class ResyncReason : uint8_t { Baseline = 1, Checksum = 2, Timeout = 3, EpochChanged = 4 };
struct Resync
{
    static constexpr Kind kind = Kind::Resync;
    ResyncReason reason{};
    bool operator==(const Resync &) const = default;
};

struct Ack
{
    static constexpr Kind kind = Kind::Ack;
    uint64_t through{};
    bool operator==(const Ack &) const = default;
};

struct Heartbeat
{
    static constexpr Kind kind = Kind::Heartbeat;
    bool operator==(const Heartbeat &) const = default;
};

enum class DisconnectReason : uint8_t { Leaving = 1, Shutdown = 2, Resync = 3 };
struct Disconnect
{
    static constexpr Kind kind = Kind::Disconnect;
    DisconnectReason reason{};
    bool operator==(const Disconnect &) const = default;
};

using Payload = std::variant<Hello, Welcome, Reject, Input, PlayerSnapshot, Action,
                             ActionResult, Inventory, WorldEdit, ChunkFragment,
                             ChunkRemove, Resync, Ack, Heartbeat, Disconnect>;

struct Packet
{
    SessionScope scope;
    uint64_t sequence{};
    Payload payload;
    bool operator==(const Packet &) const = default;
};

enum class CodecStatus : uint8_t
{
    Ok, Truncated, Oversized, BadMagic, VersionMismatch, UnknownKind,
    WrongDirection, InvalidHeader, BadLength, InvalidPayload
};

class Datagram
{
public:
    std::span<const uint8_t> view() const { return {bytes_.data(), size_}; }

private:
    friend CodecStatus encode(Endpoint, const Packet &, Datagram &) noexcept;
    std::array<uint8_t, kDatagramBytes> bytes_{};
    size_t size_{};
};

// Fixed storage, no heap allocation or exceptions. Failure leaves out untouched.
CodecStatus encode(Endpoint sender, const Packet &packet, Datagram &out) noexcept;
CodecStatus decode(Endpoint sender, std::span<const uint8_t> bytes, Packet &out) noexcept;

enum class ReceiveStatus : uint8_t
{
    Accepted, WrongSession, WrongEpoch, Unsequenced, InvalidSequence, Replay, Gap
};

// One per authenticated inbound direction. Call after decoding and after
// reserving queue space. Advance an ordered ACK only after admitting the packet.
class ReceiveWindow
{
public:
    explicit ReceiveWindow(SessionScope scope) : scope_(scope) {}
    ReceiveStatus admit(const Packet &packet) noexcept;
    uint64_t reliableThrough() const { return reliableThrough_; }

private:
    SessionScope scope_;
    uint64_t reliableThrough_{};
    uint64_t freshest_{};
};

enum class AssemblyStatus : uint8_t { InProgress, Complete, InvalidFragment, Gap, Expired, BadChecksum };

// One buffer per authenticated connection. Deliver reliably admitted
// fragments here, in order, and reset when the connection scope changes.
class ChunkAssembler
{
public:
    using Clock = std::chrono::steady_clock;
    AssemblyStatus accept(const ChunkFragment &fragment, Clock::time_point now) noexcept;
    bool expire(Clock::time_point now) noexcept;
    void reset() noexcept;
    // Install only on Complete. The view is valid until the next accept/reset.
    std::span<const uint8_t> bytes() const noexcept;

private:
    std::array<uint8_t, kChunkBytes> bytes_{};
    ChunkBaseline baseline_;
    Clock::time_point started_{};
    size_t nextIndex_{};
};

} // namespace net::wire
