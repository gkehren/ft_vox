// WorldCheckpoint — generation-based gameplay persistence checkpoints
// (stabilization Phase 3). See WorldCheckpoint.hpp and
// docs/adr/gameplay-persistence-transactions.md for the contract.
//
// Every filesystem mutation here is main-thread and stays OUTSIDE the world
// simulation lock: the caller establishes the snapshot boundary + flush
// barrier first. Committed generations are immutable; the live working set
// is never mutated by this layer (recovery re-seeding is the facade's job).

#include <World/WorldCheckpoint.hpp>
#include <World/MultiplayerSave.hpp>
#include <Items/ItemEntitySystem.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace worldsave
{
namespace
{
	namespace fs = std::filesystem;

	constexpr uint64_t kMaxManifestChunks = 1u << 20; // bounded decode
	constexpr size_t kMaxManifestBytes = 77 + 16 * kMaxManifestChunks;
	constexpr char kRosterMagic[4] = {'F', 'T', 'V', 'R'};
	constexpr size_t kRosterHeaderBytes = 20;
	constexpr size_t kRosterRecordBytes = 4 + kPlayerStateBytes + kInventoryStateBytes + 24;

	fs::path generationDirName(uint64_t generation)
	{
		return fs::path("generation-" + std::to_string(generation));
	}

	bool parseGenerationDirName(const std::string &name, uint64_t &out)
	{
		constexpr std::string_view prefix = "generation-";
		if (name.compare(0, prefix.size(), prefix) != 0 || name.size() == prefix.size())
			return false;
		uint64_t value = 0;
		for (size_t i = prefix.size(); i < name.size(); ++i)
		{
			const char c = name[i];
			if (c < '0' || c > '9')
				return false;
			const auto digit = static_cast<uint64_t>(c - '0');
			if (value > (UINT64_MAX - digit) / 10)
				return false;
			value = value * 10 + digit;
		}
		if (value == 0)
			return false;
		out = value;
		return true;
	}

	// Sorted (x, z) order for deterministic manifests and set comparison.
	bool chunkRefLess(const CheckpointChunkRef &a, const CheckpointChunkRef &b)
	{
		if (a.chunkX != b.chunkX)
			return a.chunkX < b.chunkX;
		return a.chunkZ < b.chunkZ;
	}

	bool writeBytesExact(const fs::path &path, const std::vector<uint8_t> &bytes)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;
		if (!bytes.empty())
			out.write(reinterpret_cast<const char *>(bytes.data()),
					  static_cast<std::streamsize>(bytes.size()));
		out.flush();
		return out.good();
	}
} // namespace

// ---------------------------------------------------------------------------
// Manifest codec (the ByteWriter/ByteReader discipline; strict exact size)
// ---------------------------------------------------------------------------

std::vector<uint8_t> encodeCheckpointManifest(const CheckpointManifest &manifest)
{
	ByteWriter w;
	w.raw(kCheckpointManifestMagic, sizeof(kCheckpointManifestMagic));
	w.u32(manifest.worldId ? kCheckpointManifestVersion : 1);
	w.u64(manifest.committedGeneration);
	w.u64(manifest.fallbackGeneration);
	w.u64(manifest.worldEditRevision);
	w.u8(manifest.playerPresent ? 1u : 0u);
	w.u64(manifest.playerChecksum);
	w.u64(manifest.inventoryChecksum);
	w.u64(manifest.itemsChecksum);
	w.u32(static_cast<uint32_t>(manifest.chunks.size()));
	for (const CheckpointChunkRef &ref : manifest.chunks)
	{
		w.i32(ref.chunkX);
		w.i32(ref.chunkZ);
		w.u64(ref.checksum);
	}
	if (manifest.worldId)
	{
		w.u64(manifest.worldId);
		w.u64(manifest.rosterChecksum);
	}
	return w.take();
}

SaveStatus decodeCheckpointManifest(const uint8_t *data, size_t size, CheckpointManifest &out)
{
	out = CheckpointManifest{};
	constexpr size_t kHeaderBytes = 4 + 4 + 8 + 8 + 8 + 1 + 8 + 8 + 8 + 4; // 61
	if (size < kHeaderBytes)
		return SaveStatus::Corrupt;
	ByteReader r(data, size);
	char magic[sizeof(kCheckpointManifestMagic)];
	r.raw(magic, sizeof(magic));
	const uint32_t version = r.u32();
	if (std::memcmp(magic, kCheckpointManifestMagic, sizeof(magic)) != 0)
		return SaveStatus::BadMagic;
	if (version != 1 && version != kCheckpointManifestVersion)
		return SaveStatus::UnsupportedVersion;
	CheckpointManifest m;
	m.committedGeneration = r.u64();
	m.fallbackGeneration = r.u64();
	m.worldEditRevision = r.u64();
	const uint8_t playerPresent = r.u8();
	m.playerPresent = playerPresent != 0;
	m.playerChecksum = r.u64();
	m.inventoryChecksum = r.u64();
	m.itemsChecksum = r.u64();
	const uint32_t count = r.u32();
	if (m.committedGeneration == 0 || m.committedGeneration == UINT64_MAX ||
		m.fallbackGeneration >= m.committedGeneration || count > kMaxManifestChunks || playerPresent > 1)
		return SaveStatus::Corrupt;
	// Each chunk ref is i32 x + i32 z + u64 checksum = 16 bytes.
	if (size != kHeaderBytes + static_cast<size_t>(count) * 16 + (version == 1 ? 0 : 16))
		return SaveStatus::Corrupt; // exact-size format
	m.chunks.reserve(count);
	for (uint32_t i = 0; i < count; ++i)
	{
		CheckpointChunkRef ref;
		ref.chunkX = r.i32();
		ref.chunkZ = r.i32();
		ref.checksum = r.u64();
		m.chunks.push_back(ref);
	}
	if (version == kCheckpointManifestVersion)
	{
		m.worldId = r.u64();
		m.rosterChecksum = r.u64();
		if (!m.worldId)
			return SaveStatus::Corrupt;
	}
	if (r.failed())
		return SaveStatus::Corrupt;
	std::sort(m.chunks.begin(), m.chunks.end(), chunkRefLess);
	out = std::move(m);
	return SaveStatus::Ok;
}

std::vector<uint8_t> encodePlayerRoster(const PlayerRoster &roster)
{
	ByteWriter w;
	w.raw(kRosterMagic, sizeof(kRosterMagic));
	w.u32(1);
	w.u64(roster.worldId);
	w.u32(static_cast<uint32_t>(roster.players.size()));
	for (const auto &player : roster.players)
	{
		w.u32(player.id.value);
		const auto pose = encodePlayerState(player.pose);
		w.raw(pose.data(), pose.size());
		const auto inventory = encodeInventoryState(player.inventory);
		w.raw(inventory.data(), inventory.size());
		for (double axis : player.velocity)
			w.f64(axis);
	}
	return w.take();
}

SaveStatus decodePlayerRoster(const uint8_t *data, size_t size, PlayerRoster &out)
{
	if (size < kRosterHeaderBytes)
		return SaveStatus::Corrupt;
	ByteReader r(data, size);
	char magic[sizeof(kRosterMagic)];
	r.raw(magic, sizeof(magic));
	if (std::memcmp(magic, kRosterMagic, sizeof(magic)) != 0)
		return SaveStatus::BadMagic;
	if (r.u32() != 1)
		return SaveStatus::UnsupportedVersion;
	PlayerRoster roster;
	roster.worldId = r.u64();
	const auto count = r.u32();
	if (!roster.worldId || count > kMaxPersistentPlayers ||
		size != kRosterHeaderBytes + count * kRosterRecordBytes)
		return SaveStatus::Corrupt;
	roster.players.reserve(count);
	uint32_t previous = 1; // ID 1 belongs to the local/legacy anchor
	for (uint32_t i = 0; i < count; ++i)
	{
		RemotePlayerState player;
		player.id.value = r.u32();
		std::array<uint8_t, kPlayerStateBytes> pose;
		std::array<uint8_t, kInventoryStateBytes> inventory;
		r.raw(pose.data(), pose.size());
		r.raw(inventory.data(), inventory.size());
		for (double &axis : player.velocity)
			axis = r.f64();
		if (player.id.value <= previous || player.id.value > sim::kMaxPlayerId ||
			decodePlayerState(pose.data(), pose.size(), player.pose) != SaveStatus::Ok ||
			decodeInventoryState(inventory.data(), inventory.size(), player.inventory) != SaveStatus::Ok ||
			!std::ranges::all_of(player.velocity, [](double axis) { return std::abs(axis) <= 128; }))
			return SaveStatus::Corrupt;
		constexpr double limit = 1 << 23; // dedicated wire coordinate envelope
		if (player.pose.x < -limit || player.pose.x >= limit || player.pose.z < -limit ||
			player.pose.z >= limit || player.pose.selectedBlock < 0 || player.pose.selectedBlock >= COUNT)
			return SaveStatus::Corrupt;
		previous = player.id.value;
		roster.players.push_back(player);
	}
	out = std::move(roster);
	return SaveStatus::Ok;
}

SaveStatus readPlayerRoster(const fs::path &path, PlayerRoster &out, uint64_t *checksum)
{
	std::vector<uint8_t> bytes;
	const auto status = readFileInto(path, bytes, kRosterHeaderBytes + kMaxPersistentPlayers * kRosterRecordBytes);
	if (status != SaveStatus::Ok)
		return status;
	if (checksum)
		*checksum = fnv1a64(bytes.data(), bytes.size());
	return decodePlayerRoster(bytes.data(), bytes.size(), out);
}

// ---------------------------------------------------------------------------
// Path + sync helpers
// ---------------------------------------------------------------------------

fs::path checkpointsDir(const fs::path &worldDir) { return worldDir / "checkpoints"; }
fs::path checkpointGenerationDir(const fs::path &worldDir, uint64_t generation)
{
	return checkpointsDir(worldDir) / generationDirName(generation);
}
fs::path checkpointManifestPath(const fs::path &worldDir)
{
	return checkpointsDir(worldDir) / "manifest";
}
fs::path checkpointManifestPrevPath(const fs::path &worldDir)
{
	return checkpointsDir(worldDir) / "manifest.prev";
}

bool syncFileForCheckpoint(const fs::path &path)
{
#ifdef _WIN32
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
						   FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	const bool ok = FlushFileBuffers(h) != 0;
	CloseHandle(h);
	return ok;
#else
	const int fd = ::open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return false;
	const bool ok = ::fsync(fd) == 0;
	::close(fd);
	return ok;
#endif
}

bool syncDirForCheckpoint(const fs::path &path)
{
#ifdef _WIN32
	// Windows directory-handle flushing is not reliably supported across
	// filesystems (ADR §2.9): the file sync already ran; power-loss
	// durability is documented best-effort on this platform.
	(void)path;
	return true;
#else
	const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		return false;
	const bool ok = ::fsync(fd) == 0;
	::close(fd);
	return ok;
#endif
}

bool writeCheckpointFileAtomic(const fs::path &path, const std::vector<uint8_t> &bytes, bool privateFile)
{
	fs::path tmp = path;
	tmp += ".tmp";
#ifndef _WIN32
	if (privateFile)
	{
		const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
		if (fd < 0)
			return false;
		const bool secured = ::fchmod(fd, 0600) == 0;
		::close(fd);
		if (!secured)
			return false;
	}
#else
	(void)privateFile; // inherits the ACL of the world/profile directory
#endif
	if (!writeBytesExact(tmp, bytes) || !syncFileForCheckpoint(tmp))
		return false;
	std::error_code ec;
	fs::rename(tmp, path, ec);
	if (ec)
		return false;
	// The rename is the commit point. Directory sync is best effort.
	syncDirForCheckpoint(path.parent_path());
	return true;
}

// ---------------------------------------------------------------------------
// CheckpointStore
// ---------------------------------------------------------------------------

CheckpointStore::CheckpointStore(fs::path worldDir) : m_worldDir(std::move(worldDir)) {}

fs::path CheckpointStore::activeDir() const
{
	if (m_activeGeneration == 0)
		return {};
	return checkpointGenerationDir(m_worldDir, m_activeGeneration);
}

bool CheckpointStore::failAt(CheckpointFaultPoint point, std::string &outError) const
{
	if (m_fault != point)
		return false;
	outError = "checkpoint fault injection at point " + std::to_string(static_cast<int>(point));
	return true;
}

bool CheckpointStore::validateGeneration(uint64_t generation, const CheckpointManifest &manifest,
										 std::string &outError) const
{
	const fs::path dir = checkpointGenerationDir(m_worldDir, generation);
	std::error_code ec;
	if (!fs::is_directory(dir, ec))
	{
		outError = "generation directory missing";
		return false;
	}

	// Gameplay payloads: strict codec + manifest checksum. A missing or
	// corrupt referenced payload invalidates the WHOLE generation (never
	// combined with another generation's data).
	const auto checkPayload = [&](const char *name, uint64_t wantChecksum, size_t maxBytes, auto decode) {
		std::vector<uint8_t> bytes;
		const SaveStatus read = readFileInto(dir / name, bytes, maxBytes);
		if (read != SaveStatus::Ok)
			return false;
		if (fnv1a64(bytes.data(), bytes.size()) != wantChecksum)
			return false;
		return decode(bytes);
	};
	if (!checkPayload("inventory.state", manifest.inventoryChecksum, kInventoryStateBytes, [](const std::vector<uint8_t> &b) {
			InventoryPersistState s;
			return decodeInventoryState(b.data(), b.size(), s) == SaveStatus::Ok;
		}))
	{
		outError = "inventory.state missing/corrupt/checksum-mismatch";
		return false;
	}
	if (!checkPayload("items.state", manifest.itemsChecksum,
			kItemsStateHeaderBytes + items::ItemEntitySettings::capacity * kItemEntityRecordBytes,
			[](const std::vector<uint8_t> &b) {
			ItemsPersistState s;
			return decodeItemsState(b.data(), b.size(), s) == SaveStatus::Ok;
		}))
	{
		outError = "items.state missing/corrupt/checksum-mismatch";
		return false;
	}
	if (manifest.playerPresent &&
		!checkPayload("player.state", manifest.playerChecksum, kPlayerStateBytes, [](const std::vector<uint8_t> &b) {
			PlayerPersistState s;
			return decodePlayerState(b.data(), b.size(), s) == SaveStatus::Ok;
		}))
	{
		outError = "player.state missing/corrupt/checksum-mismatch";
		return false;
	}
	if (manifest.worldId)
	{
		PlayerRoster roster;
		uint64_t checksum{};
		if (readPlayerRoster(dir / kRosterFile, roster, &checksum) != SaveStatus::Ok ||
			checksum != manifest.rosterChecksum || roster.worldId != manifest.worldId)
		{
			outError = "players.state missing/corrupt/world or checksum mismatch";
			return false;
		}
	}

	// Chunk set: EXACTLY the referenced files (no unreferenced leftovers —
	// an incomplete staging must not validate), each codec-valid and
	// checksum-matching.
	const fs::path chunksDir = dir / "chunks";
	const ScanResult scan = scanChunkFiles(chunksDir);
	if (scan.status != SaveStatus::Ok)
	{
		outError = "chunk directory scan failed";
		return false;
	}
	if (scan.files.size() != manifest.chunks.size())
	{
		outError = "chunk file count differs from the manifest";
		return false;
	}
	std::vector<CheckpointChunkRef> actual;
	actual.reserve(scan.files.size());
	for (const fs::path &file : scan.files)
	{
		int32_t x = 0, z = 0;
		if (!parseChunkFileName(file.filename().string(), x, z))
		{
			outError = "unparseable chunk file name";
			return false;
		}
		std::vector<uint8_t> bytes;
		if (readFileInto(file, bytes) != SaveStatus::Ok)
		{
			outError = "chunk file unreadable";
			return false;
		}
		std::vector<ChunkEdit> edits;
		if (readChunkFile(file, x, z, edits) != SaveStatus::Ok)
		{
			outError = "chunk file failed codec validation";
			return false;
		}
		actual.push_back({x, z, fnv1a64(bytes.data(), bytes.size())});
	}
	std::sort(actual.begin(), actual.end(), chunkRefLess);
	for (size_t i = 0; i < actual.size(); ++i)
	{
		if (actual[i].chunkX != manifest.chunks[i].chunkX ||
			actual[i].chunkZ != manifest.chunks[i].chunkZ ||
			actual[i].checksum != manifest.chunks[i].checksum)
		{
			outError = "chunk reference mismatch (coordinate or checksum)";
			return false;
		}
	}
	return true;
}

SaveStatus CheckpointStore::tryRecoverManifest(const fs::path &manifestPath, uint64_t &namedGeneration,
											   std::string &outError)
{
	std::vector<uint8_t> bytes;
	const auto status = readFileInto(manifestPath, bytes, kMaxManifestBytes);
	if (status != SaveStatus::Ok)
	{
		outError = "manifest unreadable";
		return status;
	}
	CheckpointManifest manifest;
	const auto decoded = decodeCheckpointManifest(bytes.data(), bytes.size(), manifest);
	if (decoded != SaveStatus::Ok)
	{
		outError = decoded == SaveStatus::UnsupportedVersion ? "unsupported checkpoint manifest version"
															: "manifest corrupt";
		return decoded;
	}
	namedGeneration = manifest.committedGeneration;
	std::string validationError;
	if (!validateGeneration(manifest.committedGeneration, manifest, validationError))
	{
		outError = "committed generation invalid: " + validationError;
		return SaveStatus::Corrupt;
	}
	m_activeGeneration = manifest.committedGeneration;
	m_fallbackGeneration = manifest.fallbackGeneration;
	m_activePlayerPresent = manifest.playerPresent;
	m_activeWorldId = manifest.worldId;
	m_activeManifest = std::move(bytes);
	m_nextGeneration = manifest.committedGeneration + 1;
	return SaveStatus::Ok;
}

CheckpointStore::RecoveryStatus CheckpointStore::recover(std::string &outError)
{
	outError.clear();
	m_activeGeneration = 0;
	m_fallbackGeneration = 0;
	m_activePlayerPresent = false;
	m_activeWorldId = 0;
	m_activeManifest.clear();

	const fs::path cpDir = checkpointsDir(m_worldDir);
	std::error_code ec;
	if (!fs::is_directory(cpDir, ec))
		return RecoveryStatus::Legacy;

	// Inventory the generation directories: they distinguish "never
	// migrated" (Legacy) from "checkpoints exist but none validate"
	// (Unrecoverable), and bound the next generation number.
	bool anyGeneration = false;
	uint64_t maxGeneration = 0;
	for (fs::directory_iterator it(cpDir, ec), end; it != end; it.increment(ec))
	{
		if (ec)
			break;
		uint64_t gen = 0;
		if (it->is_directory(ec) && parseGenerationDirName(it->path().filename().string(), gen))
		{
			anyGeneration = true;
			maxGeneration = std::max(maxGeneration, gen);
		}
	}
	m_nextGeneration = maxGeneration == UINT64_MAX ? UINT64_MAX : maxGeneration + 1;

	const bool manifestExists = fs::exists(checkpointManifestPath(m_worldDir), ec);
	const bool prevExists = fs::exists(checkpointManifestPrevPath(m_worldDir), ec);
	if (!manifestExists && !prevExists)
	{
		// Nothing was ever published: at most orphaned staging from failed
		// first commits (cleanup removes it). Legacy mode — a staging
		// directory alone never migrates a world.
		return RecoveryStatus::Legacy;
	}
	if (!anyGeneration)
	{
		// Manifest debris without any generation directory. A structurally
		// valid manifest still names a committed generation whose payloads
		// are gone -> unrecoverable (never silently downgrade a migrated
		// world to the live working set). Undecodable debris -> legacy.
		for (const fs::path &p : {checkpointManifestPath(m_worldDir),
								  checkpointManifestPrevPath(m_worldDir)})
		{
			std::vector<uint8_t> bytes;
			CheckpointManifest m;
			const auto status = readFileInto(p, bytes, kMaxManifestBytes);
			const auto decoded = status == SaveStatus::Ok
				? decodeCheckpointManifest(bytes.data(), bytes.size(), m) : status;
			if (decoded == SaveStatus::Ok || decoded == SaveStatus::UnsupportedVersion)
			{
				outError = "unrecoverable checkpoint corruption: committed generation directories are missing";
				return RecoveryStatus::Unrecoverable;
			}
		}
		return RecoveryStatus::Legacy;
	}

	uint64_t currentGeneration = 0;
	std::string firstError;
	const auto current = tryRecoverManifest(checkpointManifestPath(m_worldDir), currentGeneration, firstError);
	if (current == SaveStatus::Ok)
		return RecoveryStatus::Recovered;
	if (current == SaveStatus::UnsupportedVersion)
	{
		outError = firstError;
		return RecoveryStatus::Unrecoverable;
	}
	// Manifest-level fault tolerance: the rotated previous manifest names
	// the prior committed generation with its OWN checksums (a crash during
	// rotation/publication never loses both copies).
	std::string prevError;
	uint64_t previousGeneration = 0;
	const auto unusedGeneration = m_nextGeneration;
	if (tryRecoverManifest(checkpointManifestPrevPath(m_worldDir), previousGeneration, prevError) == SaveStatus::Ok)
	{
		// The current manifest may still point at a corrupt or missing
		// generation. Restaging that ID could accidentally validate it
		// before publication. Skip both the named ID and any existing dirs.
		m_nextGeneration = std::max({m_nextGeneration, unusedGeneration, currentGeneration + 1});
		outError = "recovered via manifest.prev (" + firstError + ")";
		return RecoveryStatus::Recovered;
	}
	outError = "unrecoverable checkpoint corruption: " + firstError + "; " + prevError;
	return RecoveryStatus::Unrecoverable;
}

bool CheckpointStore::stagePayload(const fs::path &finalPath, const std::vector<uint8_t> &bytes,
								   std::string &outError) const
{
	if (!writeFileAtomic(finalPath, bytes))
	{
		outError = "failed to stage " + finalPath.filename().string();
		return false;
	}
	if (!syncFileForCheckpoint(finalPath))
	{
		outError = "failed to sync staged " + finalPath.filename().string();
		return false;
	}
	return true;
}

bool CheckpointStore::publishManifest(const std::vector<uint8_t> &bytes, std::string &outError)
{
	const fs::path cpDir = checkpointsDir(m_worldDir);
	const fs::path finalPath = checkpointManifestPath(m_worldDir);
	const fs::path tmpPath = finalPath.string() + ".tmp";
	if (!writeBytesExact(tmpPath, bytes))
	{
		outError = "failed to write manifest.tmp";
		return false;
	}
	if (!syncFileForCheckpoint(tmpPath))
	{
		outError = "failed to sync manifest.tmp";
		return false;
	}
	// Fault point 8: the tmp exists but the atomic rename has not happened —
	// recovery still sees the previous manifest (or none).
	if (failAt(CheckpointFaultPoint::DuringManifestPublish, outError))
		return false;
	std::error_code ec;
	fs::rename(tmpPath, finalPath, ec);
	if (ec)
	{
		outError = "manifest rename failed: " + ec.message();
		return false;
	}
	if (!syncDirForCheckpoint(cpDir))
	{
		// The rename IS the commit point (process-crash consistency holds);
		// a directory-sync failure only weakens power-loss durability.
		// Report it but keep the commit (ADR §2.9).
		outError = "checkpoint committed; directory sync failed (power-loss durability best-effort)";
	}
	return true;
}

bool CheckpointStore::commit(const fs::path &liveChunksDir, const CheckpointPayloads &payloads,
							 std::string &outError)
{
	outError.clear();
	if (failAt(CheckpointFaultPoint::BeforeStaging, outError))
		return false;
	if (m_nextGeneration == UINT64_MAX)
	{
		outError = "checkpoint generation IDs exhausted";
		return false;
	}
	PlayerRoster roster;
	if ((!payloads.roster.empty() &&
		 decodePlayerRoster(payloads.roster.data(), payloads.roster.size(), roster) != SaveStatus::Ok) ||
		(m_activeWorldId && roster.worldId != m_activeWorldId))
	{
		outError = "checkpoint player roster failed validation or belongs to another world";
		return false;
	}

	// Pre-validate the captured payloads BEFORE touching the filesystem: a
	// malformed capture must never produce a staging directory.
	{
		InventoryPersistState inv;
		ItemsPersistState items;
		if (decodeInventoryState(payloads.inventory.data(), payloads.inventory.size(), inv) != SaveStatus::Ok ||
			decodeItemsState(payloads.items.data(), payloads.items.size(), items) != SaveStatus::Ok)
		{
			outError = "checkpoint payloads failed codec validation";
			return false;
		}
		if (payloads.playerPresent)
		{
			PlayerPersistState player;
			if (decodePlayerState(payloads.player.data(), payloads.player.size(), player) != SaveStatus::Ok)
			{
				outError = "checkpoint player payload failed codec validation";
				return false;
			}
		}
	}

	// Resolve the player payload: explicit capture, else carry forward from
	// the current recovery source (active generation, else legacy root).
	bool playerPresent = payloads.playerPresent;
	std::vector<uint8_t> playerBytes = payloads.player;
	if (!playerPresent)
	{
		// Carry forward the last authoritative player payload: the active
		// generation's when it has one, else the legacy root file (a world
		// whose checkpoints never carried a player but whose root shim does).
		// NotFound in BOTH sources = no player ever saved (quiet fresh-spawn).
		std::vector<fs::path> sources;
		if (m_activeGeneration != 0)
			sources.push_back(activeDir() / "player.state");
		sources.push_back(m_worldDir / "player.state");
		for (const fs::path &source : sources)
		{
			std::vector<uint8_t> carried;
			const SaveStatus read = readFileInto(source, carried);
			if (read == SaveStatus::Ok)
			{
				PlayerPersistState player;
				if (decodePlayerState(carried.data(), carried.size(), player) != SaveStatus::Ok)
				{
					outError = "carry-forward player.state is corrupt; refusing the commit";
					return false;
				}
				playerPresent = true;
				playerBytes = std::move(carried);
				break;
			}
			if (read != SaveStatus::NotFound)
			{
				outError = "carry-forward player.state unreadable";
				return false;
			}
		}
	}

	const uint64_t generation = m_nextGeneration;
	const fs::path genDir = checkpointGenerationDir(m_worldDir, generation);
	std::error_code ec;
	// Remove any orphaned staging for this number (a previous failed commit
	// or a crashed publication). Committed generations are NEVER touched:
	// m_nextGeneration is always ahead of every committed number.
	fs::remove_all(genDir, ec);
	fs::create_directories(genDir / "chunks", ec);
	if (ec)
	{
		outError = "failed to create staging directory: " + ec.message();
		return false;
	}

	if (playerPresent && !stagePayload(genDir / "player.state", playerBytes, outError))
		return false;
	if (failAt(CheckpointFaultPoint::AfterPlayerStaged, outError))
		return false;
	if (!stagePayload(genDir / "inventory.state", payloads.inventory, outError))
		return false;
	if (failAt(CheckpointFaultPoint::AfterInventoryStaged, outError))
		return false;
	if (!stagePayload(genDir / "items.state", payloads.items, outError))
		return false;
	if (failAt(CheckpointFaultPoint::AfterItemsStaged, outError))
		return false;
	if (roster.worldId && (!stagePayload(genDir / kRosterFile, payloads.roster, outError) ||
						   failAt(CheckpointFaultPoint::AfterRosterStaged, outError)))
		return false;

	// Copy-on-write chunk snapshot: byte copies of the LIVE override files
	// (small sparse files — no terrain regeneration, no full-world
	// serialization). The caller's flush barrier guarantees the live set is
	// stable (worker drained, captures are main-thread).
	CheckpointManifest manifest;
	manifest.committedGeneration = generation;
	manifest.fallbackGeneration = m_activeGeneration;
	manifest.worldEditRevision = payloads.worldEditRevision;
	manifest.playerPresent = playerPresent;
	manifest.playerChecksum = playerPresent ? fnv1a64(playerBytes.data(), playerBytes.size()) : 0;
	manifest.inventoryChecksum = fnv1a64(payloads.inventory.data(), payloads.inventory.size());
	manifest.itemsChecksum = fnv1a64(payloads.items.data(), payloads.items.size());
	manifest.worldId = roster.worldId;
	if (manifest.worldId)
		manifest.rosterChecksum = fnv1a64(payloads.roster.data(), payloads.roster.size());

	const ScanResult live = scanChunkFiles(liveChunksDir);
	if (live.status != SaveStatus::Ok)
	{
		outError = "live chunk scan failed";
		return false;
	}
	if (live.files.size() > kMaxManifestChunks)
	{
		outError = "live chunk count exceeds the checkpoint format limit";
		return false;
	}
	bool firstChunk = true;
	for (const fs::path &file : live.files)
	{
		if (!firstChunk && failAt(CheckpointFaultPoint::DuringChunkCopy, outError))
			return false;
		firstChunk = false;
		int32_t x = 0, z = 0;
		if (!parseChunkFileName(file.filename().string(), x, z))
		{
			outError = "unparseable live chunk file name";
			return false;
		}
		std::vector<uint8_t> bytes;
		if (readFileInto(file, bytes) != SaveStatus::Ok)
		{
			outError = "live chunk file unreadable";
			return false;
		}
		if (!writeFileAtomic(genDir / "chunks" / file.filename(), bytes))
		{
			outError = "failed to copy chunk file into staging";
			return false;
		}
		if (!syncFileForCheckpoint(genDir / "chunks" / file.filename()))
		{
			outError = "failed to sync staged chunk file";
			return false;
		}
		manifest.chunks.push_back({x, z, fnv1a64(bytes.data(), bytes.size())});
	}
	std::sort(manifest.chunks.begin(), manifest.chunks.end(), chunkRefLess);

	// Validate the staged generation AS A WHOLE before publication (payload
	// re-read + every chunk codec/checksum + exact file set).
	std::string validationError;
	if (!validateGeneration(generation, manifest, validationError))
	{
		outError = "staged generation failed validation: " + validationError;
		return false;
	}
	if (!syncDirForCheckpoint(genDir / "chunks") || !syncDirForCheckpoint(genDir) ||
		!syncDirForCheckpoint(checkpointsDir(m_worldDir)))
	{
		outError = "staged checkpoint directory sync failed";
		return false;
	}
	if (failAt(CheckpointFaultPoint::AfterStagingBeforePublish, outError))
		return false;

	auto manifestBytes = encodeCheckpointManifest(manifest);
	// Rotate the previous manifest (fault tolerance). The old committed
	// manifest stays valid until the new one is published.
	// Rotate the validated active manifest. After recovery from manifest.prev,
	// copying a corrupt manifest over it would destroy the only recovery point.
	if (!m_activeManifest.empty())
	{
		if (!writeCheckpointFileAtomic(checkpointManifestPrevPath(m_worldDir), m_activeManifest))
		{
			outError = "manifest rotation failed";
			return false;
		}
	}

	if (!publishManifest(manifestBytes, outError))
		return false;

	// PUBLISHED: the new generation is the authoritative recovery point.
	const uint64_t previousActive = m_activeGeneration;
	m_activeGeneration = generation;
	m_fallbackGeneration = previousActive;
	m_activePlayerPresent = playerPresent;
	m_activeWorldId = manifest.worldId;
	m_activeManifest = std::move(manifestBytes);
	m_nextGeneration = generation + 1;

	// Fault point 9: a crash immediately after publication — the commit is
	// durable; cleanup simply has not run (recovery ignores the orphans).
	if (m_fault == CheckpointFaultPoint::AfterManifestPublish)
		return true;

	std::string cleanupError;
	cleanup(cleanupError);
	if (!cleanupError.empty())
		outError = cleanupError; // commit succeeded; cleanup is retryable
	return true;
}

void CheckpointStore::cleanup(std::string &outError)
{
	outError.clear();
	const fs::path cpDir = checkpointsDir(m_worldDir);
	std::error_code ec;
	if (!fs::is_directory(cpDir, ec))
		return;

	// Retain the active + fallback generations; remove every other
	// generation directory (older committed generations and orphaned
	// staging alike). Best-effort: a removal failure is reported for the
	// next retry and can never invalidate committed state. The DuringCleanup
	// fault fires AFTER the first removal (partial progress, the exact
	// mid-cleanup crash shape).
	for (fs::directory_iterator it(cpDir, ec), end; it != end; it.increment(ec))
	{
		if (ec)
		{
			outError = "checkpoint cleanup scan failed: " + ec.message();
			return;
		}
		uint64_t gen = 0;
		if (!it->is_directory(ec) || !parseGenerationDirName(it->path().filename().string(), gen))
			continue;
		if (gen == m_activeGeneration || gen == m_fallbackGeneration)
			continue;
		std::error_code removeEc;
		fs::remove_all(it->path(), removeEc);
		if (removeEc)
			outError = "checkpoint cleanup failed for " + it->path().filename().string() +
					   ": " + removeEc.message();
		if (m_fault == CheckpointFaultPoint::DuringCleanup)
		{
			outError = "checkpoint fault injection during cleanup (commit stays durable)";
			return;
		}
	}
	// Orphaned manifest tmp (a crashed publication): never authoritative.
	std::error_code tmpEc;
	fs::remove(checkpointManifestPath(m_worldDir).string() + ".tmp", tmpEc);
}

} // namespace worldsave
