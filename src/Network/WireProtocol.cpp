#include "WireProtocol.hpp"

#include <Chunk/TerrainGenerator.hpp>
#include <Entities/MobTypes.hpp>
#include <Items/ItemRegistry.hpp>
#include <Physics/PlayerController.hpp>

#include <algorithm>
#include <bit>
#include <numbers>
#include <cmath>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace net::wire
{
namespace
{

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
static_assert(CHUNK_VOLUME == kChunkBytes && CHUNK_SIZE == 16 && WORLD_HEIGHT == 256);
static_assert(items::kInventorySize == 36 && items::kHotbarSize == 9);
static_assert(COUNT < kAirMaterial);

constexpr uint32_t kMagic = 0x4654564e; // FTVN

template <class T>
using Bits = std::conditional_t<sizeof(T) == 1, uint8_t,
             std::conditional_t<sizeof(T) == 2, uint16_t,
             std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>>>;

// Reject all-ones exponents before incoming float bits enter FP arithmetic.
template <class T>
bool finiteBits(Bits<T> bits)
{
    if constexpr (std::is_same_v<T, float>)
        return (bits & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000);
    else if constexpr (std::is_same_v<T, double>)
        return (bits & UINT64_C(0x7ff0000000000000)) != UINT64_C(0x7ff0000000000000);
    else
        return true;
}

class Reader
{
public:
    explicit Reader(std::span<const uint8_t> bytes) : remaining_(bytes) {}

    template <class T>
    bool field(T &value)
    {
        if (remaining_.size() < sizeof(T))
            return false;
        uint64_t bits = 0;
        for (uint8_t byte : remaining_.first(sizeof(T)))
            bits = (bits << 8) | byte;
        const auto valueBits = static_cast<Bits<T>>(bits);
        if (!finiteBits<T>(valueBits))
            return false;
        value = std::bit_cast<T>(valueBits);
        remaining_ = remaining_.subspan(sizeof(T));
        return true;
    }

    template <class... T>
    bool fields(T &...values) { return (field(values) && ...); }

    bool raw(std::span<uint8_t> value)
    {
        if (remaining_.size() < value.size())
            return false;
        std::copy_n(remaining_.begin(), value.size(), value.begin());
        remaining_ = remaining_.subspan(value.size());
        return true;
    }

    bool empty() const { return remaining_.empty(); }

private:
    std::span<const uint8_t> remaining_;
};

class Writer
{
public:
    explicit Writer(std::span<uint8_t> bytes) : remaining_(bytes) {}

    template <class T>
    bool field(T value)
    {
        if (remaining_.size() < sizeof(T))
            return false;
        const auto valueBits = std::bit_cast<Bits<T>>(value);
        if (!finiteBits<T>(valueBits))
            return false;
        const auto bits = static_cast<uint64_t>(valueBits);
        for (size_t i = 0; i < sizeof(T); ++i)
            remaining_[i] = static_cast<uint8_t>(bits >> (8 * (sizeof(T) - i - 1)));
        remaining_ = remaining_.subspan(sizeof(T));
        return true;
    }

    template <class... T>
    bool fields(const T &...values) { return (field(values) && ...); }

    bool raw(std::span<const uint8_t> value)
    {
        if (remaining_.size() < value.size())
            return false;
        std::copy(value.begin(), value.end(), remaining_.begin());
        remaining_ = remaining_.subspan(value.size());
        return true;
    }

    size_t remaining() const { return remaining_.size(); }

private:
    std::span<uint8_t> remaining_;
};

class Fingerprint
{
public:
    void byte(uint8_t value) { value_ = (value_ ^ value) * UINT64_C(1099511628211); }
    void number(uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
            byte(static_cast<uint8_t>(value >> shift));
    }
    void string(std::string_view value)
    {
        for (unsigned char ch : value)
            byte(ch);
        byte(0);
    }
    uint64_t value() const { return value_; }

private:
    uint64_t value_ = UINT64_C(14695981039346656037);
};

constexpr std::array kPolicies{
    Policy{Direction::ClientToServer, Lane::Handshake, 62, 2, 2},   // Hello
    Policy{Direction::ServerToClient, Lane::Handshake, 62, 2, 2},   // Welcome
    Policy{Direction::ServerToClient, Lane::Handshake, 13, 2, 2},   // Reject
    Policy{Direction::ClientToServer, Lane::Fresh, 17, 60, 8},      // Input
    Policy{Direction::ServerToClient, Lane::Fresh, 41 + 45 * kMaxPlayersPerSnapshot, 20, 4},
    Policy{Direction::ClientToServer, Lane::Reliable, 44, 16, 8},   // Action
    Policy{Direction::ServerToClient, Lane::Reliable, 34, 16, 8},   // ActionResult
    Policy{Direction::ServerToClient, Lane::Reliable, 17 + 4 * items::kInventorySize, 10, 4},
    Policy{Direction::ServerToClient, Lane::Reliable, 34, 32, 16},  // WorldEdit
    Policy{Direction::ServerToClient, Lane::Reliable, 49 + kFragmentBytes, 60, 16},
    Policy{Direction::ServerToClient, Lane::Reliable, 16, 16, 8},   // ChunkRemove
    Policy{Direction::Both, Lane::Reliable, 1, 1, 1},              // Resync
    Policy{Direction::Both, Lane::Ack, 8, 60, 16},                  // Ack
    Policy{Direction::Both, Lane::Fresh, 0, 2, 2},                 // Heartbeat
    Policy{Direction::Both, Lane::Reliable, 1, 1, 1},               // Disconnect
    Policy{Direction::ServerToClient, Lane::Fresh, 17 + 30 * kMaxItemsPerSnapshot, 10, 3},
    Policy{Direction::ServerToClient, Lane::Fresh, 9 + 27 * kMaxMobsPerSnapshot, 10, 3},
};
static_assert(kPolicies.size() == std::variant_size_v<Payload>);
static_assert(std::all_of(kPolicies.begin(), kPolicies.end(), [](const Policy &rules) {
    return kHeaderBytes + rules.maxPayload <= kDatagramBytes;
}));

bool allows(Direction direction, Endpoint sender)
{
    return direction == Direction::Both ||
           (direction == Direction::ClientToServer && sender == Endpoint::Client) ||
           (direction == Direction::ServerToClient && sender == Endpoint::Server);
}

constexpr bool validSequence(uint64_t value) { return value != 0 && value <= kMaxSequence; }
constexpr bool validPlayer(sim::PlayerId value) { return value.valid() && value.value <= sim::kMaxPlayerId; }
constexpr bool validVersions(uint16_t min, uint16_t max) { return min != 0 && min <= max; }
constexpr bool validMaterial(uint8_t material) { return material < COUNT || material == kAirMaterial; }
bool validStack(items::ItemStack stack)
{
    return stack.isEmpty() ? stack.item == items::ItemId::None : stack.count <= items::maxStackOf(stack.item);
}

template<class IO, class Motion>
bool entityMotion(IO &io, Motion &motion)
{
    constexpr int32_t limit = kHorizontalLimit * kEntityPositionScale;
    for (auto &coordinate : motion.position)
        if (!io.field(coordinate) || coordinate < -limit || coordinate >= limit)
            return false;
    for (auto &velocity : motion.velocity)
        if (!io.field(velocity))
            return false;
    return true;
}
constexpr bool validCell(const Cell &cell)
{
    return cell.x >= -kHorizontalLimit && cell.x < kHorizontalLimit &&
           cell.z >= -kHorizontalLimit && cell.z < kHorizontalLimit;
}

constexpr bool validChunk(const ChunkCoord &coord)
{
    constexpr int32_t limit = kHorizontalLimit / CHUNK_SIZE;
    return coord.x >= -limit && coord.x < limit && coord.z >= -limit && coord.z < limit;
}

template <class E>
constexpr bool inRange(E value, E first, E last) { return value >= first && value <= last; }

constexpr bool validActionKind(ActionKind kind) { return inRange(kind, ActionKind::Break, ActionKind::SplitHalf); }
constexpr bool voxelAction(ActionKind kind) { return kind == ActionKind::Break || kind == ActionKind::Place; }

bool validHeader(Kind kind, Lane lane, SessionScope scope, uint64_t sequence)
{
    if (kind == Kind::Hello || kind == Kind::Reject)
        return scope == SessionScope{} && sequence == 0;
    if (scope.sessionId == 0 || scope.epoch == 0)
        return false;
    if (lane == Lane::Handshake || lane == Lane::Ack)
        return sequence == 0;
    return validSequence(sequence);
}

bool validBaseline(const ChunkBaseline &p)
{
    return validSequence(p.transferId) && validChunk(p.coord) &&
           p.version.generation != 0 && p.nextEditId != 0;
}

bool validFragment(const ChunkFragment &p)
{
    const auto size = fragmentSize(p.index);
    return size != 0 && validBaseline(p.baseline) &&
           std::all_of(p.materials.begin(), p.materials.begin() + size, validMaterial);
}

template <class IO, class T>
bool encodeFields(IO &io, T &p)
{
    // The same field order and validation run in both directions.
    using P = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<P, Hello>)
    {
        return io.fields(p.nonce, p.minVersion, p.maxVersion, p.content.itemSchema,
                         p.content.catalog, p.content.generator) && io.raw(p.playerKey) &&
               std::ranges::any_of(p.playerKey, [](uint8_t byte) { return byte != 0; }) &&
               p.nonce != 0 && validVersions(p.minVersion, p.maxVersion) && p.content.itemSchema != 0;
    }
    else if constexpr (std::is_same_v<P, Welcome>)
    {
        return io.fields(p.nonce, p.worldId, p.player.value, p.seed, p.generatorVersion,
                         p.content.itemSchema, p.content.catalog, p.content.generator, p.tick, p.nextEditId) &&
               p.nonce != 0 && p.worldId != 0 && validPlayer(p.player) &&
               p.seed != 0 && p.seed <= INT32_MAX && p.generatorVersion != 0 &&
               p.content.itemSchema == kItemSchema && p.nextEditId != 0;
    }
    else if constexpr (std::is_same_v<P, Reject>)
    {
        return io.fields(p.nonce, p.reason, p.minVersion, p.maxVersion) && p.nonce != 0 &&
               inRange(p.reason, RejectReason::Version, RejectReason::ProfileLimit) &&
               validVersions(p.minVersion, p.maxVersion);
    }
    else if constexpr (std::is_same_v<P, Input>)
    {
        return io.fields(p.intendedTick, p.moveX, p.moveZ, p.flags) && validInput(p);
    }
    else if constexpr (std::is_same_v<P, PlayerSnapshot>)
    {
        if (!io.fields(p.tick, p.lastProcessedInput, p.count) || p.lastProcessedInput > kMaxSequence ||
            p.count == 0 || p.count > kMaxPlayersPerSnapshot)
            return false;
        uint32_t previousId = 0;
        for (auto &state : std::span(p.players).first(p.count))
        {
            if (!io.fields(state.player.value, state.feet[0], state.feet[1], state.feet[2],
                           state.velocity[0], state.velocity[1], state.velocity[2], state.flags, state.motionReset) ||
                !validPlayer(state.player) || state.player.value <= previousId || state.flags > 0x07)
                return false;
            for (double coordinate : state.feet)
                if (coordinate < -kHorizontalLimit || coordinate >= kHorizontalLimit)
                    return false;
            for (float velocity : state.velocity)
                if (std::abs(velocity) > 1024.f)
                    return false;
            previousId = state.player.value;
        }
        auto &timers = p.localTimers;
        constexpr physics::PlayerSettings settings;
        return io.fields(timers[0], timers[1], timers[2]) &&
            timers[0] >= 0 && timers[0] <= settings.jumpBuffer &&
            timers[1] >= 0 && timers[1] <= settings.coyoteTime &&
            timers[2] >= 0 && timers[2] <= physics::PlayerController::swimJumpDuration;
    }
    else if constexpr (std::is_same_v<P, Action>)
    {
        return io.fields(p.actionId, p.operation, p.slot, p.otherSlot, p.target.x, p.target.z, p.target.y,
                         p.expected.generation, p.expected.revision, p.yawTurns, p.pitchTurns) && validAction(p);
    }
    else if constexpr (std::is_same_v<P, ActionResult>)
    {
        if (!io.fields(p.actionId, p.tick, p.nextEditId, p.editId, p.operation, p.code) ||
            !validSequence(p.actionId) || p.nextEditId == 0 || !validActionKind(p.operation) ||
            !inRange(p.code, ActionCode::Accepted, ActionCode::Unavailable))
            return false;
        if (p.code == ActionCode::Partial && p.operation != ActionKind::Move && p.operation != ActionKind::SplitHalf)
            return false;
        if (p.code == ActionCode::Accepted && voxelAction(p.operation))
            return p.editId != 0 && p.editId < p.nextEditId;
        return p.editId == 0;
    }
    else if constexpr (std::is_same_v<P, Inventory>)
    {
        if (!io.fields(p.tick, p.revision, p.selected) || p.revision == 0 || p.selected >= items::kHotbarSize)
            return false;
        for (auto &stack : p.slots)
        {
            if (!io.fields(stack.item, stack.count) || !validStack(stack))
                return false;
        }
        return true;
    }
    else if constexpr (std::is_same_v<P, WorldEdit>)
    {
        return io.fields(p.editId, p.cell.x, p.cell.z, p.cell.y, p.base.generation, p.base.revision, p.material) &&
               validSequence(p.editId) && validCell(p.cell) && p.base.generation != 0 &&
               p.base.revision != UINT64_MAX && validMaterial(p.material);
    }
    else if constexpr (std::is_same_v<P, ChunkFragment>)
    {
        if (!io.fields(p.baseline.transferId, p.baseline.coord.x, p.baseline.coord.z,
                       p.baseline.version.generation, p.baseline.version.revision,
                       p.baseline.nextEditId, p.baseline.checksum, p.index) ||
            fragmentSize(p.index) == 0)
            return false;
        return io.raw(std::span(p.materials).first(fragmentSize(p.index))) && validFragment(p);
    }
    else if constexpr (std::is_same_v<P, ChunkRemove>)
    {
        return io.fields(p.coord.x, p.coord.z, p.generation) && validChunk(p.coord) && p.generation != 0;
    }
    else if constexpr (std::is_same_v<P, Resync>)
    {
        return io.field(p.reason) && inRange(p.reason, ResyncReason::Baseline, ResyncReason::EpochChanged);
    }
    else if constexpr (std::is_same_v<P, Ack>)
    {
        return io.field(p.through) && validSequence(p.through);
    }
    else if constexpr (std::is_same_v<P, Heartbeat>)
    {
        return true;
    }
    else if constexpr (std::is_same_v<P, Disconnect>)
    {
        return io.field(p.reason) && inRange(p.reason, DisconnectReason::Leaving, DisconnectReason::Resync);
    }
    else if constexpr (std::is_same_v<P, ItemSnapshot>)
    {
        if (!io.fields(p.tick, p.idHighWater, p.count) || p.idHighWater > kMaxSequence || p.count > p.items.size())
            return false;
        uint64_t previousId = 0;
        for (auto &state : std::span(p.items).first(p.count))
        {
            if (!io.field(state.id) || state.id <= previousId || state.id > p.idHighWater ||
                !entityMotion(io, state.motion) || !io.fields(state.stack.item, state.stack.count) ||
                state.stack.isEmpty() || !validStack(state.stack))
                return false;
            previousId = state.id;
        }
        return true;
    }
    else if constexpr (std::is_same_v<P, MobSnapshot>)
    {
        if (!io.fields(p.tick, p.count) || p.count > p.mobs.size())
            return false;
        uint64_t previousId = 0;
        for (auto &state : std::span(p.mobs).first(p.count))
        {
            if (!io.field(state.id) || state.id <= previousId || state.id > kMaxSequence ||
                !entityMotion(io, state.motion) || !io.field(state.species) || state.species >= entities::kMobSpeciesCount)
                return false;
            previousId = state.id;
        }
        return true;
    }
}

template <class P>
bool readPayload(Reader &reader, Payload &payload)
{
    return encodeFields(reader, payload.emplace<P>());
}

using PayloadReader = bool (*)(Reader &, Payload &);

template <size_t... Index>
constexpr auto payloadReaders(std::index_sequence<Index...>)
{
    static_assert(((static_cast<uint8_t>(std::variant_alternative_t<Index, Payload>::kind) == Index + 1) && ...));
    return std::array<PayloadReader, sizeof...(Index)>{readPayload<std::variant_alternative_t<Index, Payload>>...};
}

constexpr auto kPayloadReaders = payloadReaders(std::make_index_sequence<std::variant_size_v<Payload>>{});

Kind kindOf(const Packet &packet)
{
    return static_cast<Kind>(packet.payload.index() + 1);
}

} // namespace

const Policy *policy(Kind kind) noexcept
{
    const auto tag = static_cast<uint8_t>(kind);
    return tag == 0 || tag > kPolicies.size() ? nullptr : &kPolicies[tag - 1];
}

Compatibility localCompatibility() noexcept
{
    static const Compatibility result = [] {
        Fingerprint catalog;
        catalog.string("FTVN catalog 1");
        catalog.number(COUNT);
        for (size_t i = 0; i < textureTypeString.size(); ++i)
        {
            const auto material = static_cast<TextureType>(i);
            const auto drop = items::dropForBlock(material);
            catalog.string(textureTypeString[i]);
            catalog.number(static_cast<uint16_t>(items::itemForBlock(material)));
            catalog.number(static_cast<uint16_t>(drop.item));
            catalog.number(drop.count);
        }
        catalog.number(kAirMaterial);
        catalog.number(items::kItemCount);
        for (const auto &item : items::itemDefinitions())
        {
            catalog.number(static_cast<uint16_t>(item.id));
            catalog.string(item.name);
            catalog.number(item.maxStack);
            catalog.number(item.placeableBlock ? static_cast<uint32_t>(*item.placeableBlock) : UINT32_MAX);
        }

        Fingerprint generator;
        generator.string("FTVN generator 1");
        generator.number(TerrainGenerator::kGeneratorVersion);
        generator.number(CHUNK_SIZE);
        generator.number(WORLD_HEIGHT);
        return Compatibility{kItemSchema, catalog.value(), generator.value()};
    }();
    return result;
}

bool validAction(const Action &p) noexcept
{
    if (!validSequence(p.actionId) || !validActionKind(p.operation))
        return false;
    if (voxelAction(p.operation))
    {
        return p.slot == 0 && p.otherSlot == 0 && validCell(p.target) && p.expected.generation != 0 &&
               p.yawTurns >= 0.f && p.yawTurns < 1.f && std::abs(p.pitchTurns) <= 0.25f;
    }
    if (p.target != Cell{} || p.expected != ChunkVersion{} ||
        std::bit_cast<uint32_t>(p.yawTurns) != 0 || std::bit_cast<uint32_t>(p.pitchTurns) != 0)
        return false;
    if (p.operation == ActionKind::Select)
        return p.slot < items::kHotbarSize && p.otherSlot == 0;
    if (p.operation == ActionKind::DropOne || p.operation == ActionKind::DropStack)
        return p.slot < items::kInventorySize && p.otherSlot == 0;
    return p.slot < items::kInventorySize && p.otherSlot < items::kInventorySize && p.slot != p.otherSlot;
}

std::array<double, 3> aimDirection(float yawTurns, float pitchTurns) noexcept
{
    constexpr double radiansPerTurn = 2.0 * std::numbers::pi;
    const double yaw = yawTurns * radiansPerTurn;
    const double pitch = pitchTurns * radiansPerTurn;
    return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
}

bool validInput(const Input &input) noexcept
{
    return input.flags <= (Input::sprint | Input::jump | Input::swimUp | Input::swimDown) &&
           double(input.moveX) * input.moveX + double(input.moveZ) * input.moveZ <= 1.0;
}

std::optional<RejectReason> rejectHello(const Hello &hello, Compatibility local) noexcept
{
    if (hello.minVersion > kVersion || hello.maxVersion < kVersion)
        return RejectReason::Version;
    if (hello.content != local)
        return RejectReason::Content;
    return std::nullopt;
}

uint64_t chunkChecksum(std::span<const uint8_t> bytes) noexcept
{
    Fingerprint checksum;
    for (uint8_t byte : bytes)
        checksum.byte(byte);
    return checksum.value();
}

CodecStatus encode(Endpoint sender, const Packet &packet, Datagram &out) noexcept
{
    const auto kind = kindOf(packet);
    const auto *rules = policy(kind);
    if (!rules)
        return CodecStatus::UnknownKind;
    if (!allows(rules->direction, sender))
        return CodecStatus::WrongDirection;
    if (!validHeader(kind, rules->lane, packet.scope, packet.sequence))
        return CodecStatus::InvalidHeader;

    Datagram frame;
    Writer payload(std::span(frame.bytes_).subspan(kHeaderBytes, rules->maxPayload));
    if (!std::visit([&payload](const auto &value) { return encodeFields(payload, value); }, packet.payload))
        return CodecStatus::InvalidPayload;
    const auto length = static_cast<uint16_t>(rules->maxPayload - payload.remaining());
    Writer header(std::span(frame.bytes_).first(kHeaderBytes));
    if (!header.fields(kMagic, kVersion, kind, uint8_t{0}, length, uint16_t{0},
                       packet.scope.sessionId, packet.scope.epoch, packet.sequence))
        return CodecStatus::InvalidHeader;
    frame.size_ = kHeaderBytes + length;
    out = frame;
    return CodecStatus::Ok;
}

CodecStatus decode(Endpoint sender, std::span<const uint8_t> bytes, Packet &out) noexcept
{
    if (bytes.size() < kHeaderBytes)
        return CodecStatus::Truncated;
    if (bytes.size() > kDatagramBytes)
        return CodecStatus::Oversized;

    uint32_t magic{};
    uint16_t version{}, length{}, reserved{};
    uint8_t flags{};
    Kind kind{};
    Packet packet;
    Reader reader(bytes);
    if (!reader.fields(magic, version, kind, flags, length, reserved,
                       packet.scope.sessionId, packet.scope.epoch, packet.sequence))
        return CodecStatus::Truncated;
    if (magic != kMagic)
        return CodecStatus::BadMagic;
    if (version != kVersion)
        return CodecStatus::VersionMismatch;
    const auto *rules = policy(kind);
    if (!rules)
        return CodecStatus::UnknownKind;
    if (!allows(rules->direction, sender))
        return CodecStatus::WrongDirection;
    if (flags != 0 || reserved != 0 || !validHeader(kind, rules->lane, packet.scope, packet.sequence))
        return CodecStatus::InvalidHeader;
    if (length > rules->maxPayload || length != bytes.size() - kHeaderBytes)
        return CodecStatus::BadLength;
    if (!kPayloadReaders[static_cast<uint8_t>(kind) - 1](reader, packet.payload))
        return CodecStatus::InvalidPayload;
    if (!reader.empty())
        return CodecStatus::BadLength;
    out = packet;
    return CodecStatus::Ok;
}

ReceiveStatus ReceiveWindow::admit(const Packet &packet) noexcept
{
    const auto kind = kindOf(packet);
    const auto *rules = policy(kind);
    if (!rules || (rules->lane != Lane::Reliable && rules->lane != Lane::Fresh))
        return ReceiveStatus::Unsequenced;
    if (packet.scope.sessionId == 0 || packet.scope.sessionId != scope_.sessionId)
        return ReceiveStatus::WrongSession;
    if (packet.scope.epoch == 0 || packet.scope.epoch != scope_.epoch)
        return ReceiveStatus::WrongEpoch;
    if (!validSequence(packet.sequence))
        return ReceiveStatus::InvalidSequence;

    auto &through = rules->lane == Lane::Reliable ? reliableThrough_ :
                    kind == Kind::ItemSnapshot ? freshestItems_ :
                    kind == Kind::MobSnapshot ? freshestMobs_ : freshest_;
    if (packet.sequence <= through)
        return ReceiveStatus::Replay;
    if (rules->lane == Lane::Reliable && packet.sequence != through + 1)
        return ReceiveStatus::Gap;
    through = packet.sequence;
    return ReceiveStatus::Accepted;
}

AssemblyStatus ChunkAssembler::accept(const ChunkFragment &fragment, Clock::time_point now) noexcept
{
    if (expire(now))
        return AssemblyStatus::Expired;
    if (nextIndex_ == kFragmentsPerChunk)
        reset();
    if (!validFragment(fragment))
    {
        reset();
        return AssemblyStatus::InvalidFragment;
    }
    if (fragment.index != nextIndex_ || (nextIndex_ != 0 && fragment.baseline != baseline_))
    {
        reset();
        return AssemblyStatus::Gap;
    }
    if (nextIndex_ == 0)
    {
        baseline_ = fragment.baseline;
        started_ = now;
    }
    std::copy_n(fragment.materials.begin(), fragmentSize(fragment.index),
                bytes_.begin() + fragment.index * kFragmentBytes);
    progressed_ = now;
    ++nextIndex_;
    if (nextIndex_ != kFragmentsPerChunk)
        return AssemblyStatus::InProgress;
    if (chunkChecksum(bytes_) == baseline_.checksum)
        return AssemblyStatus::Complete;
    reset();
    return AssemblyStatus::BadChecksum;
}

bool ChunkAssembler::expire(Clock::time_point now) noexcept
{
    if (nextIndex_ == 0 || nextIndex_ == kFragmentsPerChunk ||
        (now - progressed_ < kChunkIdleTimeout && now - started_ < kChunkTransferTimeout))
        return false;
    reset();
    return true;
}

void ChunkAssembler::reset() noexcept { nextIndex_ = 0; }

std::span<const uint8_t> ChunkAssembler::bytes() const noexcept
{
    if (nextIndex_ != kFragmentsPerChunk)
        return {};
    return bytes_;
}

} // namespace net::wire
