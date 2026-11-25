#include <Core/ServerStatus.hpp>
#include <World/WorldSave.hpp>

#include <algorithm>

namespace monitoring
{
namespace
{
constexpr std::array<char, 4> kMagic{'F', 'T', 'M', 'N'};
constexpr uint32_t kVersion = 1;
constexpr std::array kStatusFields{&Status::sampledMs, &Status::uptimeMs, &Status::ticks, &Status::droppedTicks,
	&Status::checkpoint, &Status::chunks, &Status::generating, &Status::waitingForRecord,
	&Status::waitingForGeneration, &Status::registeredProfiles, &Status::offlineProfiles};
constexpr std::array kTrafficFields{&TransportSample::receivedBytes, &TransportSample::sentBytes,
	&TransportSample::worldBytes, &TransportSample::retransmits, &TransportSample::resyncs};
constexpr std::array kQueueFields{&TransportSample::count, &TransportSample::capacity,
	&TransportSample::worldSends, &TransportSample::baselines};
static_assert(kStatusFields.size() + kTrafficFields.size() == 16 && kQueueFields.size() == 4);
}

Frame encode(const Status &status)
{
	worldsave::ByteWriter writer;
	writer.raw(kMagic.data(), kMagic.size());
	writer.u32(kVersion);
	for (const auto field : kStatusFields)
		writer.u64(status.*field);
	for (const auto field : kTrafficFields)
		writer.u64(status.transport.*field);
	for (const auto field : kQueueFields)
		writer.u16(status.transport.*field);
	for (size_t i = 0; i < kMaxPlayers; ++i)
	{
		const auto row = i < status.transport.count ? status.transport.players[i] : Player{};
		writer.u32(row.id.value);
		writer.u8(static_cast<uint8_t>(row.state));
	}
	Frame frame{};
	std::ranges::copy(writer.take(), frame.begin());
	return frame;
}

std::optional<Status> decode(const Frame &frame)
{
	worldsave::ByteReader reader(frame.data(), frame.size());
	std::array<char, kMagic.size()> magic{};
	reader.raw(magic.data(), magic.size());
	if (magic != kMagic || reader.u32() != kVersion)
		return std::nullopt;
	Status status;
	auto &transport = status.transport;
	for (const auto field : kStatusFields)
		status.*field = reader.u64();
	for (const auto field : kTrafficFields)
		transport.*field = reader.u64();
	for (const auto field : kQueueFields)
		transport.*field = reader.u16();
	if (transport.capacity > kMaxPlayers || transport.count > transport.capacity ||
		transport.worldSends > transport.count || transport.baselines > transport.worldSends ||
		status.offlineProfiles > status.registeredProfiles || status.uptimeMs > status.sampledMs)
		return std::nullopt;
	uint32_t previous = 1; // ID 1 is the simulation anchor.
	for (size_t i = 0; i < kMaxPlayers; ++i)
	{
		const auto id = reader.u32();
		const auto phase = static_cast<SessionState>(reader.u8());
		if (i >= transport.count)
		{
			if (id || phase != SessionState{})
				return std::nullopt;
			continue;
		}
		if (id <= previous || id > sim::kMaxPlayerId ||
			phase < SessionState::Syncing || phase > SessionState::Disconnecting)
			return std::nullopt;
		transport.players[i] = {{id}, phase};
		previous = id;
	}
	return reader.failed() ? std::nullopt : std::optional{status};
}

uint64_t monotonicMs()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool fresh(uint64_t sampleMs, uint64_t nowMs)
{
	return sampleMs <= nowMs &&
		nowMs - sampleMs <= static_cast<uint64_t>(std::chrono::milliseconds(kMaxAge).count());
}
}
