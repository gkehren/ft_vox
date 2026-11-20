#include "WorldIdentities.hpp"

#include <Network/UdpSocket.hpp>
#include <World/MultiplayerSave.hpp>
#include <World/WorldCheckpoint.hpp>

#include <algorithm>
#include <cstring>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace server
{
namespace
{
namespace fs = std::filesystem;
namespace wire = net::wire;
constexpr char kMagic[4] = {'F', 'T', 'V', 'A'};
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderBytes = 41;
constexpr size_t kRecordBytes = 4 + wire::PlayerKey{}.size();
constexpr size_t kChecksumBytes = 8;
constexpr uint32_t kActorReservation = 256;
constexpr uint64_t kItemReservation = 1u << 20;
}

WorldIdentities::~WorldIdentities()
{
	if (m_lock == -1)
		return;
#ifdef _WIN32
	CloseHandle(reinterpret_cast<HANDLE>(m_lock));
#else
	close(static_cast<int>(m_lock));
#endif
}

bool WorldIdentities::lock(const fs::path &worldDir, std::string &error)
{
	std::error_code ec;
	fs::create_directories(worldDir, ec);
	if (ec)
	{
		error = "could not create server world directory: " + ec.message();
		return false;
	}
	const auto lockPath = worldDir / "server.lock";
#ifdef _WIN32
	const HANDLE handle = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
									  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle != INVALID_HANDLE_VALUE)
		m_lock = reinterpret_cast<intptr_t>(handle);
#else
	const int fd = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (fd != -1)
	{
		if (flock(fd, LOCK_EX | LOCK_NB) == 0)
			m_lock = fd;
		else
			close(fd);
	}
#endif
	if (m_lock == -1)
	{
		error = "could not lock server world (already in use or lock file unavailable)";
		return false;
	}
	m_path = worldDir / worldsave::kIdentityFile;
	return true;
}

bool WorldIdentities::load(int seed, uint32_t generator, uint64_t checkpointWorldId, bool hasCheckpoint,
						   std::string &error)
{
	std::vector<uint8_t> bytes;
	const auto status = worldsave::readFileInto(m_path, bytes,
		kHeaderBytes + kRecordBytes * worldsave::kMaxPersistentPlayers + kChecksumBytes);
	if (status == worldsave::SaveStatus::NotFound && !checkpointWorldId)
	{
		m_worldId = net::udp::randomId();
		m_seed = seed;
		m_generator = generator;
		// reserveItems publishes this new registry before the server can bind.
		return true;
	}
	if (status != worldsave::SaveStatus::Ok || !decode(bytes))
	{
		error = "server identity registry missing, corrupt or unsupported";
		return false;
	}
	if (m_seed != seed || m_generator != generator || (checkpointWorldId && m_worldId != checkpointWorldId))
	{
		error = "server identity registry belongs to another world or generator";
		return false;
	}
	if (m_checkpointRequired && !hasCheckpoint)
	{
		error = "server identity registry requires a committed world checkpoint";
		return false;
	}
	m_nextActor = m_actorLimit;
	return true;
}

bool WorldIdentities::decode(const std::vector<uint8_t> &bytes)
{
	if (bytes.size() < kHeaderBytes + kChecksumBytes)
		return false;
	worldsave::ByteReader r(bytes.data(), bytes.size());
	char magic[sizeof(kMagic)];
	r.raw(magic, sizeof(magic));
	if (std::memcmp(magic, kMagic, sizeof(magic)) != 0 || r.u32() != kVersion)
		return false;
	m_worldId = r.u64();
	m_seed = r.i32();
	m_generator = r.u32();
	const auto required = r.u8();
	m_checkpointRequired = required != 0;
	m_actorLimit = r.u32();
	m_itemLimit = r.u64();
	const auto count = r.u32();
	if (!m_worldId || required > 1 || m_actorLimit < 2 || !m_itemLimit ||
		count > worldsave::kMaxPersistentPlayers ||
		bytes.size() != kHeaderBytes + kRecordBytes * count + kChecksumBytes)
		return false;
	uint32_t previous = 1;
	std::set<wire::PlayerKey> keys;
	for (uint32_t i = 0; i < count; ++i)
	{
		const sim::PlayerId id{r.u32()};
		wire::PlayerKey key;
		r.raw(key.data(), key.size());
		if (id.value <= previous || id.value >= m_actorLimit || key == wire::PlayerKey{} ||
			!keys.insert(key).second)
			return false;
		m_profiles.emplace(id, key);
		previous = id.value;
	}
	return r.u64() == worldsave::fnv1a64(bytes.data(), bytes.size() - kChecksumBytes) && !r.failed();
}

bool WorldIdentities::save() const
{
	worldsave::ByteWriter w;
	w.raw(kMagic, sizeof(kMagic));
	w.u32(kVersion);
	w.u64(m_worldId);
	w.i32(m_seed);
	w.u32(m_generator);
	w.u8(m_checkpointRequired ? 1 : 0);
	w.u32(m_actorLimit);
	w.u64(m_itemLimit);
	w.u32(static_cast<uint32_t>(m_profiles.size()));
	for (const auto &[id, key] : m_profiles)
	{
		w.u32(id.value);
		w.raw(key.data(), key.size());
	}
	auto bytes = w.take();
	worldsave::ByteWriter checksumWriter;
	checksumWriter.u64(worldsave::fnv1a64(bytes.data(), bytes.size()));
	const auto checksum = checksumWriter.take();
	bytes.insert(bytes.end(), checksum.begin(), checksum.end());
	return worldsave::writeCheckpointFileAtomic(m_path, bytes, true);
}

bool WorldIdentities::markCheckpointRequired()
{
	if (m_checkpointRequired)
		return true;
	m_checkpointRequired = true;
	if (save())
		return true;
	m_checkpointRequired = false;
	return false;
}

sim::PlayerId WorldIdentities::find(const wire::PlayerKey &key) const
{
	const auto it = std::ranges::find_if(m_profiles, [&](const auto &entry) { return entry.second == key; });
	return it == m_profiles.end() ? sim::kInvalidPlayerId : it->first;
}

sim::PlayerId WorldIdentities::allocatePlayer()
{
	if (m_nextActor == m_actorLimit)
	{
		if (m_actorLimit == sim::kMaxNextPlayerId)
			return {};
		const auto available = sim::kMaxNextPlayerId - m_actorLimit;
		m_actorLimit += std::min(kActorReservation, available);
		if (!save())
		{
			m_actorLimit = m_nextActor;
			return {};
		}
	}
	return {m_nextActor++};
}

bool WorldIdentities::enroll(sim::PlayerId id, const wire::PlayerKey &key)
{
	if (size() >= worldsave::kMaxPersistentPlayers || !m_profiles.emplace(id, key).second)
		return false;
	if (save())
		return true;
	m_profiles.erase(id);
	return false;
}

bool WorldIdentities::reserveItems(uint64_t checkpointNext, uint64_t &next, uint64_t &limit)
{
	next = std::max(checkpointNext, m_itemLimit);
	const auto available = UINT64_MAX - next;
	limit = next + std::min(kItemReservation, available);
	const auto previous = m_itemLimit;
	m_itemLimit = limit;
	if (save())
		return true;
	m_itemLimit = previous;
	return false;
}

} // namespace server
