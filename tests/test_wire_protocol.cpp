#include <Network/WireProtocol.hpp>

#include "TestMain.hpp"

#include <algorithm>
#include <array>
#include <random>
#include <string_view>
#include <vector>

namespace
{
using namespace net::wire;
using S = CodecStatus;

constexpr SessionScope kScope{0x0102030405060708, 0x1112131415161718};
constexpr Compatibility kContent{1, 0x3132333435363738, 0x4142434445464748};
constexpr Packet kSentinel{{}, 0, Reject{5, RejectReason::Unavailable, 2, 2}};

struct Fixture
{
    Endpoint sender;
    Packet packet;
    std::string_view tagLength;
    std::string_view scopeAndSequence;
    std::string_view payload;
    size_t zeroTail{};
};

std::vector<uint8_t> bytesOf(std::string_view hex)
{
    std::vector<uint8_t> bytes;
    constexpr std::string_view digits = "0123456789abcdef";
    unsigned value = 0;
    bool high = true;
    for (char c : hex)
    {
        if (c == ' ')
            continue;
        const auto nibble = digits.find(c);
        CHECK_EXPR(nibble != std::string_view::npos);
        value = (value << 4) | static_cast<unsigned>(nibble);
        if (!high)
        {
            bytes.push_back(static_cast<uint8_t>(value));
            value = 0;
        }
        high = !high;
    }
    CHECK_EXPR(high);
    return bytes;
}

std::vector<uint8_t> encoded(Endpoint sender, const Packet &packet)
{
    Datagram frame;
    CHECK_EXPR(encode(sender, packet, frame) == S::Ok);
    return {frame.view().begin(), frame.view().end()};
}

void checkMalformed(Endpoint sender, std::span<const uint8_t> bytes, S expected)
{
    Packet out = kSentinel;
    CHECK_EXPR(decode(sender, bytes, out) == expected);
    CHECK_EXPR(out == kSentinel);
}

constexpr auto kNoScope = "0000000000000000 0000000000000000 0000000000000000";
constexpr auto kScopedZero = "0102030405060708 1112131415161718 0000000000000000";
constexpr auto kScopedTwo = "0102030405060708 1112131415161718 0000000000000002";

std::array<Fixture, 17> fixtures()
{
    constexpr auto client = Endpoint::Client;
    constexpr auto server = Endpoint::Server;
    PlayerSnapshot players{12, 7, 1, {}};
    players.players[0] = {{3}, {-1.5, 2.0, 0.0}, {0.5f, -0.25f, 1.0f}, 3, 6};
    players.localTimers[2] = 0.25;
    Inventory inventory{13, 2, 8, {}};
    inventory.slots[0] = {items::ItemId::Dirt, 64};
    ChunkFragment fragment{{18, {-3, 4}, {1, 2}, 32, 0x2122232425262728}, 59, {}};
    fragment.data[0] = kAirMaterial;
    ItemSnapshot items{13, 9, 1, {}};
    items.items[0] = {9, {{-32, 64, 0}, {16, -8, 0}}, {items::ItemId::Glass, 3}};
    MobSnapshot mobs{13, 1, {}};
    mobs.mobs[0] = {5, {{-32, 64, 0}, {16, -8, 0}}, 2};

    return {{
        {client, {{}, 0, Hello{1, 2, 5, kContent, {1}}}, "0100 003e", kNoScope,
         "0000000000000001 0002 0005 0001 3132333435363738 4142434445464748 01", 31},
        {server, {kScope, 0, Welcome{1, 9, {3}, 42, 1, kContent, 4, 5}}, "0200 003e", kScopedZero,
         "0000000000000001 0000000000000009 00000003 0000002a 00000001 0001"
         " 3132333435363738 4142434445464748 0000000000000004 0000000000000005"},
        {server, {{}, 0, Reject{1, RejectReason::Version, 3, 3}}, "0300 000d", kNoScope,
         "0000000000000001 01 0003 0003"},
        {client, {kScope, 2, Input{5, 0.5f, -0.25f, 9}}, "0400 0011", kScopedTwo,
         "0000000000000005 3f000000 be800000 09"},
        {server, {kScope, 2, players}, "0500 0056", kScopedTwo,
         "000000000000000c 0000000000000007 01 00000003 bff8000000000000 4000000000000000"
         " 0000000000000000 3f000000 be800000 3f800000 03 00000006"
         " 0000000000000000 0000000000000000 3fd0000000000000"},
        {client, {kScope, 2, Action{9, ActionKind::Place, 0, 0, {-2, 0x010203, 255}, {1, 2}, 0.25f, -0.125f}},
         "0600 002c", kScopedTwo,
         "0000000000000009 02 00 00 fffffffe 00010203 ff 0000000000000001 0000000000000002 3e800000 be000000"},
        {server, {kScope, 2, ActionResult{9, 13, 5, 4, ActionKind::Place, ActionCode::Accepted}},
         "0700 0022", kScopedTwo, "0000000000000009 000000000000000d 0000000000000005 0000000000000004 02 00"},
        {server, {kScope, 2, inventory}, "0800 00a1", kScopedTwo,
         "000000000000000d 0000000000000002 08 0004 0040", 140},
        {server, {kScope, 2, WorldEdit{4, {-2, 0x010203, 255}, {1, 2}, kAirMaterial}},
         "0900 0022", kScopedTwo, "0000000000000004 fffffffe 00010203 ff 0000000000000001 0000000000000002 ff"},
        {server, {kScope, 2, fragment}, "0a00 02af", kScopedTwo,
         "0000000000000012 fffffffd 00000004 0000000000000001 0000000000000002"
         " 0000000000000020 2122232425262728 0000 3b ff", 635},
        {server, {kScope, 2, ChunkRemove{{-3, 4}, 1}}, "0b00 0010", kScopedTwo,
         "fffffffd 00000004 0000000000000001"},
        {client, {kScope, 2, Resync{ResyncReason::Checksum}}, "0c00 0001", kScopedTwo, "02"},
        {client, {kScope, 0, Ack{2}}, "0d00 0008", kScopedZero, "0000000000000002"},
        {server, {kScope, 2, Heartbeat{}}, "0e00 0000", kScopedTwo, ""},
        {client, {kScope, 2, Disconnect{DisconnectReason::Leaving}}, "0f00 0001", kScopedTwo, "01"},
        {server, {kScope, 2, items}, "1000 002f", kScopedTwo,
         "000000000000000d 0000000000000009 01 0000000000000009 ffffffe0 00000040 00000000 0010 fff8 0000 0005 0003"},
        {server, {kScope, 2, mobs}, "1100 0024", kScopedTwo,
         "000000000000000d 01 0000000000000005 ffffffe0 00000040 00000000 0010 fff8 0000 02"},
    }};
}

void testGoldens(const std::array<Fixture, 17> &cases)
{
    for (const auto &fixture : cases)
    {
        auto golden = bytesOf("4654564e 0005");
        for (auto part : {fixture.tagLength, std::string_view("0000"), fixture.scopeAndSequence, fixture.payload})
        {
            const auto bytes = bytesOf(part);
            golden.insert(golden.end(), bytes.begin(), bytes.end());
        }
        golden.resize(golden.size() + fixture.zeroTail, 0);
        CHECK_EXPR(encoded(fixture.sender, fixture.packet) == golden);
        Packet decoded;
        CHECK_EXPR(decode(fixture.sender, golden, decoded) == S::Ok);
        CHECK_EXPR(decoded == fixture.packet);
        for (size_t size = 0; size < golden.size(); ++size)
            checkMalformed(fixture.sender, std::span(golden).first(size),
                           size < kHeaderBytes ? S::Truncated : S::BadLength);
    }
    const auto hello = std::get<Hello>(cases[0].packet.payload);
    CHECK_EXPR(!rejectHello(hello, kContent));
    const Packet noKey{{}, 0, Hello{1, kVersion, kVersion, kContent, {}}};
    Datagram invalid;
    CHECK_EXPR(encode(Endpoint::Client, noKey, invalid) == S::InvalidPayload);
    auto zeroKeyBytes = encoded(Endpoint::Client, cases[0].packet);
    std::ranges::fill(std::span(zeroKeyBytes).last(PlayerKey{}.size()), uint8_t{0});
    checkMalformed(Endpoint::Client, zeroKeyBytes, S::InvalidPayload);
    auto incompatible = hello;
    incompatible.maxVersion = incompatible.minVersion = 1;
    CHECK_EXPR(rejectHello(incompatible, kContent) == RejectReason::Version);
    incompatible.maxVersion = incompatible.minVersion = 6;
    CHECK_EXPR(rejectHello(incompatible, kContent) == RejectReason::Version);
    CHECK_EXPR(rejectHello(hello, {2, kContent.catalog, kContent.generator}) == RejectReason::Content);
    CHECK_EXPR(rejectHello(hello, {1, 0, kContent.generator}) == RejectReason::Content);
    CHECK_EXPR(rejectHello(hello, {1, kContent.catalog, 0}) == RejectReason::Content);
    const auto local = localCompatibility();
    CHECK_EXPR(local.itemSchema == 1 && local.catalog != 0 && local.generator != 0);
    const std::array<uint8_t, 5> helloBytes{'h', 'e', 'l', 'l', 'o'};
    CHECK_EXPR(chunkChecksum(helloBytes) == UINT64_C(0xa430d84680aabd0b));
}

void testMalformed(const std::array<Fixture, 17> &cases)
{
    constexpr auto client = Endpoint::Client;
    constexpr auto server = Endpoint::Server;
    auto mutate = [&](size_t fixture, size_t offset, std::string_view replacement, S expected) {
        const auto &base = cases[fixture];
        auto bytes = encoded(base.sender, base.packet);
        const auto changed = bytesOf(replacement);
        if (offset > bytes.size() || changed.size() > bytes.size() - offset)
        {
            CHECK(false, "malformed fixture patch is outside the datagram");
            return;
        }
        std::copy(changed.begin(), changed.end(), bytes.begin() + offset);
        checkMalformed(base.sender, bytes, expected);
    };
    checkMalformed(client, std::array<uint8_t, 1201>{}, S::Oversized);
    auto input = encoded(client, cases[3].packet);
    checkMalformed(server, input, S::WrongDirection);
    checkMalformed(client, encoded(server, cases[8].packet), S::WrongDirection);
    checkMalformed(client, encoded(server, cases[7].packet), S::WrongDirection);
    mutate(3, 0, "01", S::BadMagic);
    mutate(3, 4, "0001", S::VersionMismatch);
    mutate(3, 4, "0002", S::VersionMismatch);
    mutate(3, 4, "0004", S::VersionMismatch);
    mutate(3, 4, "ffff", S::VersionMismatch);
    mutate(3, 6, "00", S::UnknownKind);
    mutate(3, 6, "ff", S::UnknownKind);
    mutate(3, 7, "01", S::InvalidHeader);
    mutate(3, 8, "ffff", S::BadLength);
    mutate(3, 10, "0001", S::InvalidHeader);
    mutate(3, 12, "0000000000000000", S::InvalidHeader);
    mutate(3, 20, "0000000000000000", S::InvalidHeader);
    mutate(3, 28, "0000000000000000", S::InvalidHeader);
    mutate(3, 28, "ffffffffffffffff", S::InvalidHeader);
    mutate(0, 19, "01", S::InvalidHeader);
    mutate(0, 46, "0001", S::InvalidPayload);
    mutate(1, 56, "80000000", S::InvalidPayload);
    mutate(2, 44, "00", S::InvalidPayload);
    mutate(3, 44, "7fc00000", S::InvalidPayload);
    mutate(3, 44, "7f800001", S::InvalidPayload);
    mutate(3, 48, "ff800000", S::InvalidPayload);
    mutate(3, 44, "3f800000 3f800000", S::InvalidPayload);
    mutate(3, 52, "80", S::InvalidPayload);
    mutate(4, 52, "ff", S::InvalidPayload);
    mutate(4, 53, "ffffffff", S::InvalidPayload);
    mutate(4, 57, "7ff8000000000000", S::InvalidPayload);
    mutate(4, 81, "7f800000", S::InvalidPayload);
    mutate(4, 98, "7ff8000000000000", S::InvalidPayload);
    mutate(4, 106, "bff0000000000000", S::InvalidPayload);
    mutate(4, 114, "3fe0000000000000", S::InvalidPayload);
    mutate(5, 44, "ff", S::InvalidPayload);
    mutate(5, 47, "80000000", S::InvalidPayload);
    mutate(5, 72, "7fc00000", S::InvalidPayload);
    mutate(6, 69, "fe", S::InvalidPayload);
    mutate(7, 53, "ffff", S::InvalidPayload);
    mutate(7, 55, "0041", S::InvalidPayload);
    mutate(7, 55, "0000", S::InvalidPayload);
    mutate(8, 61, "ffffffffffffffff", S::InvalidPayload);
    mutate(8, 69, "fe", S::InvalidPayload);
    mutate(9, 84, "0001", S::InvalidPayload); // index exceeds the encoded payload
    mutate(9, 86, "ff", S::InvalidPayload);
    mutate(9, 86, "00", S::InvalidPayload); // non-final fragment with final fragment's length
    mutate(9, 87, "fe", S::InvalidPayload);
    mutate(10, 36, "7fffffff", S::InvalidPayload);
    mutate(11, 36, "ff", S::InvalidPayload);
    mutate(12, 43, "00", S::InvalidPayload);
    mutate(14, 36, "ff", S::InvalidPayload);
    checkMalformed(client, encoded(server, cases[15].packet), S::WrongDirection);
    checkMalformed(client, encoded(server, cases[16].packet), S::WrongDirection);
    mutate(15, 51, "08", S::InvalidPayload); // id past allocator
    mutate(15, 52, "ff", S::InvalidPayload); // excessive count
    mutate(15, 61, "20000000", S::InvalidPayload); // position outside quantized bounds
    mutate(15, 81, "0000", S::InvalidPayload); // empty live item
    mutate(16, 71, "04", S::InvalidPayload); // invalid species

    // A legal envelope length can still contain extra bytes for a short variant.
    auto snapshot = encoded(server, cases[4].packet);
    ++snapshot[9];
    snapshot.push_back(0);
    checkMalformed(server, snapshot, S::BadLength);
    // A declared record count cannot read past a shorter payload.
    snapshot[52] = 2;
    checkMalformed(server, snapshot, S::InvalidPayload);

    Datagram frame;
    CHECK_EXPR(encode(client, cases[3].packet, frame) == S::Ok);
    CHECK_EXPR(encode(client, {kScope, 2, Action{1, ActionKind::Select, 9}}, frame) == S::InvalidPayload);
    CHECK_EXPR(encode(client, {kScope, 2, Action{1, ActionKind::DropOne, 0, 1}}, frame) == S::InvalidPayload);
    CHECK_EXPR(std::ranges::equal(frame.view(), input));

    const Packet partial{kScope, 2, ActionResult{1, 2, 1, 0, ActionKind::SplitHalf, ActionCode::Partial}};
    CHECK_EXPR(encode(server, partial, frame) == S::Ok);
    Packet decoded;
    CHECK_EXPR(decode(server, frame.view(), decoded) == S::Ok);
    CHECK_EXPR(decoded == partial);
}

void testSequence()
{
    using R = ReceiveStatus;
    ReceiveWindow inbound(kScope);
    Packet action{kScope, 2, Action{1, ActionKind::Select}};
    CHECK_EXPR(inbound.admit(action) == R::Gap);
    CHECK_EXPR(inbound.reliableThrough() == 0);
    action.sequence = 1;
    auto old = action;
    --old.scope.epoch;
    CHECK_EXPR(inbound.admit(old) == R::WrongEpoch);
    ++old.scope.sessionId;
    CHECK_EXPR(inbound.admit(old) == R::WrongSession);
    CHECK_EXPR(inbound.admit({kScope, 0, Ack{1}}) == R::Unsequenced);
    CHECK_EXPR(inbound.admit(action) == R::Accepted);
    CHECK_EXPR(inbound.admit(action) == R::Replay);
    CHECK_EXPR(inbound.reliableThrough() == 1);
    action.sequence = 2;
    CHECK_EXPR(inbound.admit(action) == R::Accepted);
    CHECK_EXPR(inbound.reliableThrough() == 2);
    CHECK_EXPR(inbound.admit({kScope, 4, Input{}}) == R::Accepted);
    CHECK_EXPR(inbound.admit({kScope, 3, Input{}}) == R::Replay);
    CHECK_EXPR(inbound.admit({kScope, 4, Heartbeat{}}) == R::Replay);
    CHECK_EXPR(inbound.admit({kScope, 6, MobSnapshot{}}) == R::Accepted);
    CHECK_EXPR(inbound.admit({kScope, 5, ItemSnapshot{}}) == R::Accepted);
    CHECK_EXPR(inbound.admit({kScope, 4, ItemSnapshot{}}) == R::Replay);
    CHECK_EXPR(inbound.admit({kScope, 4, MobSnapshot{}}) == R::Replay);
    CHECK_EXPR(inbound.admit({kScope, 5, Input{}}) == R::Accepted);
    CHECK_EXPR(inbound.admit({kScope, 0, Input{}}) == R::InvalidSequence);
    CHECK_EXPR(inbound.admit({kScope, kMaxSequence, Input{}}) == R::Accepted);
    CHECK_EXPR(inbound.admit({kScope, UINT64_MAX, Input{}}) == R::InvalidSequence);
    CHECK_EXPR(inbound.admit({kScope, 3, Resync{ResyncReason::Baseline}}) == R::Accepted);
    ReceiveWindow reconnected({kScope.sessionId + 1, kScope.epoch});
    CHECK_EXPR(reconnected.admit(action) == R::WrongSession);
    CHECK_EXPR(reconnected.reliableThrough() == 0);
}

void testInactiveStorage(const std::array<Fixture, 17> &cases)
{
    constexpr auto server = Endpoint::Server;
    const auto &snapshot = cases[4].packet;
    auto reusedSnapshot = snapshot;
    auto &players = std::get<PlayerSnapshot>(reusedSnapshot.payload);
    players.players[players.count] = players.players[0];
    players.players.back().player = {27};

    auto bytes = encoded(server, reusedSnapshot);
    CHECK_EXPR(bytes == encoded(server, snapshot));
    Packet decoded;
    CHECK_EXPR(decode(server, bytes, decoded) == S::Ok);
    CHECK_EXPR(decoded == reusedSnapshot);
    CHECK_EXPR(std::get<PlayerSnapshot>(decoded.payload).players[players.count] == PlayerState{});
    ++players.players[0].flags;
    CHECK_EXPR(decoded != reusedSnapshot);

    const auto &fragment = cases[9].packet;
    auto reusedFragment = fragment;
    auto &part = std::get<ChunkFragment>(reusedFragment.payload);
    const auto active = fragmentSize(part.index, part.baseline.encodedSize);
    part.data[active] = 254;
    part.data.back() = kAirMaterial;

    bytes = encoded(server, reusedFragment);
    CHECK_EXPR(bytes == encoded(server, fragment));
    CHECK_EXPR(decode(server, bytes, decoded) == S::Ok);
    CHECK_EXPR(decoded == reusedFragment);
    CHECK_EXPR(std::get<ChunkFragment>(decoded.payload).data[active] == 0);
    part.data[active - 1] = kAirMaterial;
    CHECK_EXPR(decoded != reusedFragment);
}

void testAssembly()
{
    using A = AssemblyStatus;
    std::array<uint8_t, kChunkBytes> chunk{};
    for (size_t i = 0; i < chunk.size(); ++i)
        chunk[i] = (i % 7 == 0) ? kAirMaterial : static_cast<uint8_t>(i % 32);
    const ChunkBaseline baseline{1, {-5, 7}, {1, 0}, 42, chunkChecksum(chunk)};
    auto fragment = [&chunk, &baseline](uint8_t index) {
        ChunkFragment result{baseline, index, {}};
        std::copy_n(chunk.begin() + index * kFragmentBytes, fragmentSize(index, 0), result.data.begin());
        return result;
    };
    ChunkAssembler assembler;
    const ChunkAssembler::Clock::time_point now{};
    CHECK_EXPR(assembler.accept(fragment(1), now) == A::Gap);
    CHECK_EXPR(assembler.accept(fragment(0), now) == A::InProgress);
    auto mismatch = fragment(1);
    ++mismatch.baseline.version.revision;
    CHECK_EXPR(assembler.accept(mismatch, now) == A::Gap);
    CHECK_EXPR(assembler.bytes().empty());
    CHECK_EXPR(assembler.accept(fragment(0), now) == A::InProgress);
    CHECK_EXPR(assembler.accept(fragment(0), now) == A::Gap);
    CHECK_EXPR(assembler.accept(fragment(0), now) == A::InProgress);
    CHECK_EXPR(!assembler.expire(now + kChunkIdleTimeout - std::chrono::milliseconds(1)));
    CHECK_EXPR(assembler.accept(fragment(1), now + kChunkIdleTimeout) == A::Expired);
    CHECK_EXPR(assembler.accept(fragment(0), now) == A::InProgress);
    for (uint8_t i = 1; i < 5; ++i)
        CHECK_EXPR(assembler.accept(fragment(i), now + std::chrono::seconds(i * 4)) == A::InProgress);
    CHECK_EXPR(assembler.accept(fragment(5), now + kChunkTransferTimeout) == A::Expired);
    CHECK_EXPR(assembler.bytes().empty());
    auto invalid = fragment(0);
    invalid.data[0] = 254;
    CHECK_EXPR(assembler.accept(invalid, now) == A::InvalidFragment);

    for (bool corrupt : {false, true})
    {
        for (uint8_t index = 0; index < kMaxFragmentsPerChunk; ++index)
        {
            auto message = fragment(index);
            if (corrupt && index == 0)
                message.data[0] = 1;
            constexpr auto server = Endpoint::Server;
            const auto bytes = encoded(server, {kScope, uint64_t{index} + 1, message});
            CHECK_EXPR(bytes.size() == (index == 59 ? 723u : 1187u));
            Packet decoded;
            CHECK_EXPR(decode(server, bytes, decoded) == S::Ok);
            const auto status = assembler.accept(std::get<ChunkFragment>(decoded.payload), now);
            CHECK_EXPR(status == (index != 59 ? A::InProgress : (corrupt ? A::BadChecksum : A::Complete)));
            if (status != A::Complete)
                CHECK_EXPR(assembler.bytes().empty());
        }
        if (!corrupt)
            CHECK_EXPR(std::ranges::equal(assembler.bytes(), chunk));
        assembler.reset();
        CHECK_EXPR(assembler.bytes().empty());
    }
}

void testCompressedAssembly()
{
    using A = AssemblyStatus;
    std::array<uint8_t, kChunkBytes> chunk, payload;
    for (size_t i = 0; i < chunk.size(); ++i)
        chunk[i] = static_cast<uint8_t>(i % 32);
    CHECK_EXPR(packChunk(chunk, payload) == 0 && chunk == payload);

    ChunkAssembler assembler;
    const ChunkAssembler::Clock::time_point now{};
    chunk.fill(kAirMaterial);
    for (bool mixed : {false, true})
    {
        if (mixed)
            for (size_t i = 111; i < 3000; ++i)
                chunk[i] = static_cast<uint8_t>(i % 32);
        const ChunkBaseline baseline{1, {-5, 7}, {1, 0}, 42, chunkChecksum(chunk), packChunk(chunk, payload)};
        CHECK_EXPR(mixed ? fragmentCount(baseline.encodedSize) > 1 : baseline.encodedSize == 1024);
        for (uint8_t index = 0; index < fragmentCount(baseline.encodedSize); ++index)
        {
            ChunkFragment part{baseline, index, {}};
            const auto size = fragmentSize(index, baseline.encodedSize);
            std::copy_n(payload.begin() + index * kFragmentBytes, size, part.data.begin());
            const auto bytes = encoded(Endpoint::Server, {kScope, uint64_t{index} + 1, part});
            if (!mixed)
            {
                const auto prefix = bytesOf("0400 00 ffff");
                CHECK_EXPR(std::ranges::equal(std::span(bytes).subspan(84, prefix.size()), prefix));
            }
            Packet decoded;
            CHECK_EXPR(decode(Endpoint::Server, bytes, decoded) == S::Ok);
            if (index == 1)
            {
                auto mismatch = part;
                ++mismatch.baseline.encodedSize;
                CHECK_EXPR(assembler.accept(mismatch, now) == A::Gap && assembler.bytes().empty());
                ChunkFragment first{baseline, 0, {}};
                std::copy_n(payload.begin(), kFragmentBytes, first.data.begin());
                CHECK_EXPR(assembler.accept(first, now) == A::InProgress);
            }
            const bool complete = index + 1u == fragmentCount(baseline.encodedSize);
            CHECK_EXPR(assembler.accept(std::get<ChunkFragment>(decoded.payload), now) ==
                       (complete ? A::Complete : A::InProgress));
            CHECK_EXPR(complete ? std::ranges::equal(assembler.bytes(), chunk) : assembler.bytes().empty());
        }
    }

    ChunkFragment uniform{{2, {-5, 7}, {1, 0}, 42, 0, 1024}, 0, {}};
    uniform.data.fill(0xff);
    const auto expect = [&](const ChunkFragment &part, A result) {
        CHECK_EXPR(assembler.accept(part, now) == result && assembler.bytes().empty());
    };
    expect(uniform, A::BadChecksum);
    auto invalid = uniform;
    --invalid.baseline.encodedSize; // missing repeated material
    expect(invalid, A::InvalidFragment);
    invalid.data[1022] = 0x02; // missing literal materials
    expect(invalid, A::InvalidFragment);
    invalid = uniform;
    invalid.baseline.encodedSize -= 2; // too little decoded output
    expect(invalid, A::InvalidFragment);
    invalid = uniform;
    invalid.baseline.encodedSize += 2; // run after output is full
    expect(invalid, A::InvalidFragment);
    invalid = uniform;
    invalid.data[1] = 254; // reserved material byte in a repeat
    expect(invalid, A::InvalidFragment);
    invalid.data[0] = 0; // reserved material byte in a literal
    expect(invalid, A::InvalidFragment);
}

void testProperties(const std::array<Fixture, 17> &cases)
{
    std::mt19937 rng(0x251245);
    auto canonical = [](Endpoint sender, std::span<const uint8_t> bytes) {
        Packet out = kSentinel;
        if (decode(sender, bytes, out) == S::Ok)
            CHECK_EXPR(std::ranges::equal(encoded(sender, out), bytes));
        else
            CHECK_EXPR(out == kSentinel);
    };

    std::array<uint8_t, kDatagramBytes + 1> noise{};
    for (size_t i = 0; i < 20000; ++i)
    {
        const auto size = rng() % noise.size();
        for (auto &byte : std::span(noise).first(size))
            byte = static_cast<uint8_t>(rng());
        canonical(Endpoint::Client, std::span(noise).first(size));

        const auto &fixture = cases[rng() % cases.size()];
        auto mutated = encoded(fixture.sender, fixture.packet);
        for (size_t edits = 1 + rng() % 5; edits != 0; --edits)
            mutated[rng() % mutated.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
        canonical(fixture.sender, mutated);

        const auto axis = [](uint32_t bits) { return static_cast<float>(int(bits % 1025) - 512) / 1024.f; };
        const Packet input{kScope, i + 1, Input{rng(), axis(rng()), axis(rng()), static_cast<uint8_t>(rng() % 16)}};
        const auto bytes = encoded(Endpoint::Client, input);
        Packet decoded;
        CHECK_EXPR(decode(Endpoint::Client, bytes, decoded) == S::Ok);
        CHECK_EXPR(decoded == input);
    }

    PlayerSnapshot full{42, 41, kMaxPlayersPerSnapshot, {}};
    for (uint32_t i = 0; i < full.count; ++i)
        full.players[i].player.value = i + 1;
    Packet packet{kScope, 2, full};
    const auto bytes = encoded(Endpoint::Server, packet);
    CHECK_EXPR(bytes.size() == 1157);
    Packet decoded;
    CHECK_EXPR(decode(Endpoint::Server, bytes, decoded) == S::Ok);
    CHECK_EXPR(decoded == packet);
    auto unsorted = bytes;
    unsorted[101] = 1; // second player ID duplicates the first
    checkMalformed(Endpoint::Server, unsorted, S::InvalidPayload);

    ItemSnapshot fullItems{42, kMaxItemsPerSnapshot, kMaxItemsPerSnapshot, {}};
    for (size_t i = 0; i < fullItems.count; ++i)
        fullItems.items[i] = {i + 1, {}, {items::ItemId::Stone, 1}};
    packet.payload = fullItems;
    const auto itemBytes = encoded(Endpoint::Server, packet);
    CHECK_EXPR(itemBytes.size() == 1193);
    CHECK_EXPR(decode(Endpoint::Server, itemBytes, decoded) == S::Ok && decoded == packet);
    fullItems.items.back().id = fullItems.items[fullItems.count - 2].id;
    Datagram frame;
    CHECK_EXPR(encode(Endpoint::Server, {kScope, 2, fullItems}, frame) == S::InvalidPayload);

    MobSnapshot fullMobs{42, kMaxMobsPerSnapshot, {}};
    for (size_t i = 0; i < fullMobs.count; ++i)
        fullMobs.mobs[i].id = i + 1;
    packet.payload = fullMobs;
    const auto mobBytes = encoded(Endpoint::Server, packet);
    CHECK_EXPR(mobBytes.size() == 1179);
    CHECK_EXPR(decode(Endpoint::Server, mobBytes, decoded) == S::Ok && decoded == packet);
}
} // namespace

int main()
{
    const auto cases = fixtures();
    testGoldens(cases);
    testMalformed(cases);
    testSequence();
    testInactiveStorage(cases);
    testAssembly();
    testCompressedAssembly();
    testProperties(cases);
    std::cout << "WireProtocol: " << testmain::checks << " checks, " << testmain::fails << " failures\n";
    return testmain::exitCode();
}
