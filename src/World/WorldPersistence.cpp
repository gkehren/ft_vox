#include <World/WorldPersistence.hpp>

#include <Chunk/TerrainGenerator.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point from)
{
	return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}

std::string statusName(worldsave::SaveStatus status)
{
	switch (status)
	{
	case worldsave::SaveStatus::Ok: return "ok";
	case worldsave::SaveStatus::NotFound: return "not found";
	case worldsave::SaveStatus::BadMagic: return "bad magic";
	case worldsave::SaveStatus::UnsupportedVersion: return "unsupported version";
	case worldsave::SaveStatus::Corrupt: return "corrupt";
	case worldsave::SaveStatus::CoordinateMismatch: return "coordinate mismatch";
	case worldsave::SaveStatus::IoError: return "I/O error";
	}
	return "unknown";
}

// ChunkEdit is a plain aggregate (no operator==); both vectors are kept in
// ascending localIndex order by every producer, so a zip compare is exact.
bool sameEditList(const std::vector<worldsave::ChunkEdit> &a,
                  const std::vector<worldsave::ChunkEdit> &b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (a[i].localIndex != b[i].localIndex || a[i].blockType != b[i].blockType)
			return false;
	return true;
}

#ifdef _WIN32
// Debugging nicety only: name the I/O thread so stacks are readable.
void nameCurrentThread(const char *name)
{
#pragma pack(push, 2)
	struct
	{
		ULONG dwType;
		LPCWSTR szName;
		ULONG dwThreadId;
		ULONG dwFlags;
	} info{};
#pragma pack(pop)
	info.dwType = 0x1000;
	info.szName = nullptr;
	WCHAR wide[64];
	if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64) > 0)
	{
		info.szName = wide;
		// Raise 0x406D1388 = thread-name-attachment exception; MSVC debuggers
		// and Windows 10+ SetThreadDescription semantics.
		__try
		{
			RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG),
			               reinterpret_cast<const ULONG_PTR *>(&info));
		}
		__except (EXCEPTION_CONTINUE_EXECUTION)
		{
		}
	}
}
#else
void nameCurrentThread(const char *) {}
#endif
} // namespace

// ---------------------------------------------------------------------------
// SaveService
// ---------------------------------------------------------------------------

SaveService::SaveService(std::filesystem::path chunksDir, int seed)
	: m_chunksDir(std::move(chunksDir)), m_seed(seed)
{
	m_worker = std::jthread([this](std::stop_token stop) {
		nameCurrentThread("ft-vox-save-io");
		workerLoop(std::move(stop));
	});
}

SaveService::~SaveService()
{
	shutdown();
}

uint64_t SaveService::coordKey(int32_t chunkX, int32_t chunkZ)
{
	return (static_cast<uint64_t>(static_cast<uint32_t>(chunkX)) << 32) |
	       static_cast<uint64_t>(static_cast<uint32_t>(chunkZ));
}

void SaveService::setCompletionCallback(CompletionCallback callback)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_completion = std::move(callback);
}

bool SaveService::enqueue(ChunkSaveRequest &&request)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	if (m_shuttingDown)
		return false;
	// Bounded FIFO back-pressure: >1024 DISTINCT modified chunks pending is
	// the only way to get here; blocking the capturer is the safe policy
	// (edits stay recorded on their chunks until space frees up).
	m_queueCv.wait(lock, [&] { return m_shuttingDown || m_queue.size() < kMaxPendingRequests; });
	if (m_shuttingDown)
		return false;
	Job job;
	job.request = std::move(request);
	job.revision = m_nextRevision++;
	m_latestRevision[coordKey(job.request.chunkX, job.request.chunkZ)] = job.revision;
	m_queue.push_back(std::move(job));
	++m_enqueued;
	m_queueCv.notify_one();
	return true;
}

bool SaveService::flush()
{
	std::unique_lock<std::mutex> lock(m_mutex);
	if (m_shuttingDown)
		return false;
	// Failure state is sampled at flush start: this return value describes
	// exactly the requests covered by this barrier.
	m_anyFailed = false;
	// Everything enqueued so far carries a revision < m_nextRevision; wait
	// until the worker has popped (processed or superseded) all of them.
	const uint64_t target = m_nextRevision;
	m_doneCv.wait(lock, [&] { return m_shuttingDown || m_lastFinishedRevision + 1 >= target; });
	if (m_shuttingDown)
		return false;
	return !m_anyFailed;
}

void SaveService::shutdown()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_shuttingDown)
		{
			// Already stopped (or stopping): just make sure a concurrent
			// first-time caller's join below is skipped via joinable().
		}
		else
		{
			m_shuttingDown = true;
			m_queue.clear();
			m_latestRevision.clear();
			m_queueCv.notify_all();
			m_doneCv.notify_all();
		}
	}
	if (m_worker.joinable())
	{
		m_worker.request_stop();
		m_worker.join();
	}
}

SaveService::Stats SaveService::stats() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	Stats s;
	s.enqueued = m_enqueued;
	s.completed = m_completed;
	s.superseded = m_superseded;
	s.failed = m_failed;
	s.deleted = m_deleted;
	s.bytesWritten = m_bytesWritten;
	s.queueDepth = m_queue.size();
	const uint64_t processed = m_completed + m_deleted;
	s.avgSerializeMs = processed ? m_serializeTotalMs / static_cast<double>(processed) : 0.0;
	s.maxSerializeMs = m_serializeMaxMs;
	s.avgWriteMs = processed ? m_writeTotalMs / static_cast<double>(processed) : 0.0;
	s.maxWriteMs = m_writeMaxMs;
	s.lastError = m_lastError;
	return s;
}

void SaveService::workerLoop(std::stop_token stop)
{
	// Defensive wake-up: a raw jthread destruction path requests stop without
	// notifying the condition_variable - the stop callback covers it.
	const std::stop_callback<std::function<void()>> onStop(
		stop, [this] { m_queueCv.notify_all(); });

	for (;;)
	{
		Job job;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_queueCv.wait(lock, [&] { return stop.stop_requested() || !m_queue.empty(); });
			if (stop.stop_requested())
			{
				// Shutdown discards pending work by contract (flush() first
				// for durability).
				m_queue.clear();
				m_latestRevision.clear();
				m_doneCv.notify_all();
				return;
			}
			job = std::move(m_queue.front());
			m_queue.pop_front();
		}

		processJob(job);

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_lastFinishedRevision = job.revision;
		// Converge the supersede map: erase only when no newer request
		// for this coordinate was enqueued while this one ran.
		const auto it = m_latestRevision.find(coordKey(job.request.chunkX, job.request.chunkZ));
		if (it != m_latestRevision.end() && it->second == job.revision)
			m_latestRevision.erase(it);
	}
	// Both the flush waiter and any enqueue blocked on the queue bound wake.
	m_queueCv.notify_all();
	m_doneCv.notify_all();
	}
}

void SaveService::processJob(Job &job)
{
	const int32_t chunkX = job.request.chunkX;
	const int32_t chunkZ = job.request.chunkZ;

	// (1) Supersede check at pop time: a newer revision was enqueued for this
	// coordinate after this job -> drop without any I/O. Serialize time is
	// deliberately NOT spent on doomed work.
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		const auto it = m_latestRevision.find(coordKey(chunkX, chunkZ));
		if (it != m_latestRevision.end() && it->second != job.revision)
		{
			++m_superseded;
			return;
		}
	}

	bool ok = false;
	bool deleted = false;
	std::string error;
	std::vector<worldsave::ChunkEdit> overrides;
	double serializeMs = 0.0;
	double writeMs = 0.0;

	try
	{
		// (2) Regenerate the deterministic base (serialize time = base regen
		// + diff). The payload is immutable, so the source chunk was free to
		// recycle the moment it was captured.
		const Clock::time_point t0 = Clock::now();
		auto base = TerrainGenerator::getThreadLocal(m_seed).generateChunk(chunkX, chunkZ);
		if (base.voxels.size() < static_cast<size_t>(CHUNK_VOLUME))
			throw std::runtime_error("regenerated base has wrong voxel count");
		static_assert(sizeof(Voxel) == 1, "diff walks the voxel bytes as u8");
		overrides = worldsave::diffEditsAgainstBase(
			reinterpret_cast<const uint8_t *>(base.voxels.data()), job.request.currentValues);
		serializeMs = elapsedMs(t0);

		// (3) Persist: empty diff = all overrides reverted -> remove the file.
		const Clock::time_point t1 = Clock::now();
		if (overrides.empty())
		{
			deleted = true;
			const worldsave::SaveStatus st =
				worldsave::removeChunkFile(m_chunksDir, chunkX, chunkZ);
			// removeChunkFile is Ok for both "removed" and "was already
			// absent" (idempotent delete).
			ok = (st == worldsave::SaveStatus::Ok);
			if (!ok)
				error = "removeChunkFile failed (" + statusName(st) + ")";
		}
		else
		{
			const worldsave::SaveStatus st =
				worldsave::writeChunkFile(m_chunksDir, chunkX, chunkZ, overrides);
			ok = (st == worldsave::SaveStatus::Ok);
			if (!ok)
				error = "writeChunkFile failed (" + statusName(st) + ")";
		}
		writeMs = elapsedMs(t1);
	}
	catch (const std::exception &e)
	{
		ok = false;
		error = std::string("serialize exception: ") + e.what();
	}

	// (4) Commit accounting under the mutex (failure bookkeeping is atomic
	// with the revision progress flush() waits on), then run the completion
	// callback WITHOUT holding the service mutex (it locks the persistence
	// index; no lock-order cycle).
	CompletionCallback completion;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (ok)
		{
			if (deleted)
				++m_deleted;
			else
			{
				++m_completed;
				// 28-byte header + 5 bytes per record (see WorldSave.hpp).
				m_bytesWritten += 28u + 5u * static_cast<uint64_t>(overrides.size());
			}
			m_serializeTotalMs += serializeMs;
			m_serializeMaxMs = std::max(m_serializeMaxMs, serializeMs);
			m_writeTotalMs += writeMs;
			m_writeMaxMs = std::max(m_writeMaxMs, writeMs);
		}
		else
		{
			++m_failed;
			m_anyFailed = true;
			m_lastError = "chunk (" + std::to_string(chunkX) + ", " + std::to_string(chunkZ) +
			              "): " + error;
		}
		completion = m_completion;
	}
	if (ok && completion)
	{
		if (deleted)
		{
			static const std::vector<worldsave::ChunkEdit> kEmpty;
			completion(chunkX, chunkZ, kEmpty);
		}
		else
		{
			completion(chunkX, chunkZ, overrides);
		}
	}
}

// ---------------------------------------------------------------------------
// WorldPersistence
// ---------------------------------------------------------------------------

WorldPersistence::~WorldPersistence()
{
	shutdown();
}

uint64_t WorldPersistence::coordKey(int32_t chunkX, int32_t chunkZ)
{
	return (static_cast<uint64_t>(static_cast<uint32_t>(chunkX)) << 32) |
	       static_cast<uint64_t>(static_cast<uint32_t>(chunkZ));
}

void WorldPersistence::recordError(const std::string &error) const
{
	std::lock_guard<std::mutex> lock(m_indexMutex);
	if (!m_errorLog.empty())
		m_errorLog += "\n";
	m_errorLog += error;
	// Bounded log: keep the tail so a long session cannot grow it forever.
	constexpr size_t kMaxErrorLogBytes = 8192;
	if (m_errorLog.size() > kMaxErrorLogBytes)
		m_errorLog.erase(0, m_errorLog.size() - kMaxErrorLogBytes);
}

bool WorldPersistence::worldExists(const std::filesystem::path &savesRoot, const std::string &name)
{
	std::error_code ec;
	return std::filesystem::exists(savesRoot / name / "world.meta", ec) && !ec;
}

bool WorldPersistence::peekStoredSeed(const std::filesystem::path &savesRoot,
                                      const std::string &name, int &outSeed, std::string &error)
{
	worldsave::WorldMeta meta;
	const worldsave::SaveStatus st =
		worldsave::readWorldMeta(savesRoot / name / "world.meta", meta);
	switch (st)
	{
	case worldsave::SaveStatus::Ok:
		outSeed = meta.seed;
		return true;
	case worldsave::SaveStatus::NotFound:
		error = "world '" + name + "' not found";
		break;
	case worldsave::SaveStatus::BadMagic:
		error = "world '" + name + "' has a corrupt world.meta (bad magic)";
		break;
	case worldsave::SaveStatus::UnsupportedVersion:
		error = "world '" + name + "' uses unsupported format version " +
		        std::to_string(meta.formatVersion);
		break;
	default:
		error = "world '" + name + "' world.meta unreadable (" + statusName(st) + ")";
		break;
	}
	return false;
}

WorldPersistence::OpenInfo WorldPersistence::openOrCreate(const std::filesystem::path &savesRoot,
                                                          const std::string &name,
                                                          int currentSeed,
                                                          uint32_t currentGeneratorVersion)
{
	// A prior open keeps its service alive until shutdown; reopening on top
	// would leak a running worker.
	shutdown();

	OpenInfo info;
	const std::filesystem::path worldDir = savesRoot / name;
	const std::filesystem::path metaPath = worldDir / "world.meta";
	const std::filesystem::path chunksDir = worldDir / "chunks";

	worldsave::WorldMeta meta;
	const worldsave::SaveStatus read = worldsave::readWorldMeta(metaPath, meta);
	if (read == worldsave::SaveStatus::Ok)
	{
		info.created = false;
		info.storedFormatVersion = meta.formatVersion;
		info.seed = meta.seed;
		if (meta.generatorVersion != currentGeneratorVersion)
		{
			info.error = "generator version mismatch: world '" + name + "' was generated with version " +
			             std::to_string(meta.generatorVersion) + ", this build provides version " +
			             std::to_string(currentGeneratorVersion) +
			             " (no migration available in save format v1)";
			return info;
		}
		if (meta.seed != currentSeed)
		{
			info.error = "seed mismatch: world '" + name + "' has seed " +
			             std::to_string(meta.seed) + ", requested seed " +
			             std::to_string(currentSeed) + " (opening never overwrites identity)";
			return info;
		}
	}
	else if (read == worldsave::SaveStatus::NotFound)
	{
		info.created = true;
		std::error_code ec;
		std::filesystem::create_directories(chunksDir, ec);
		if (ec)
		{
			info.error = "failed to create world directories: " + ec.message();
			return info;
		}
		meta = worldsave::WorldMeta{worldsave::kWorldSaveFormatVersion, currentSeed,
		                            currentGeneratorVersion};
		if (worldsave::writeWorldMeta(metaPath, meta) != worldsave::SaveStatus::Ok)
		{
			info.error = "failed to write " + metaPath.string();
			return info;
		}
		info.storedFormatVersion = meta.formatVersion;
		info.seed = meta.seed;
	}
	else
	{
		// BadMagic / UnsupportedVersion / Corrupt / IoError: identity is
		// unreadable, refuse to touch anything.
		info.storedFormatVersion = meta.formatVersion;
		info.seed = meta.seed;
		if (read == worldsave::SaveStatus::BadMagic)
			info.error = "world '" + name + "' has a corrupt world.meta (bad magic)";
		else if (read == worldsave::SaveStatus::UnsupportedVersion)
			info.error = "world '" + name + "' uses unsupported format version " +
			             std::to_string(meta.formatVersion) + " (this build supports up to " +
			             std::to_string(worldsave::kWorldSaveFormatVersion) + ")";
		else
			info.error = "world '" + name + "' world.meta unreadable (" + statusName(read) + ")";
		return info;
	}

	// Interrupted atomic writes are never authoritative.
	worldsave::cleanTempFiles(chunksDir);

	// Load every chunk file into the in-memory override index. A file that
	// fails to read is loudly reported and SKIPPED (procedural fallback for
	// that chunk); opening continues.
	{
		std::lock_guard<std::mutex> lock(m_indexMutex);
		m_overrides.clear();
		m_errorLog.clear();
		for (const std::filesystem::path &path : worldsave::scanChunkFiles(chunksDir))
		{
			int32_t cx = 0;
			int32_t cz = 0;
			(void)worldsave::parseChunkFileName(path.filename().string(), cx, cz);
			std::vector<worldsave::ChunkEdit> edits;
			const worldsave::SaveStatus st = worldsave::readChunkFile(path, cx, cz, edits);
			if (st == worldsave::SaveStatus::Ok)
				m_overrides[coordKey(cx, cz)] = std::move(edits);
			else
			{
				const std::string message = "chunk file " + path.filename().string() +
				                            " failed to load (" + statusName(st) +
				                            "); using procedural fallback for that chunk";
				if (!m_errorLog.empty())
					m_errorLog += "\n";
				m_errorLog += message;
			}
		}
	}

	m_worldDir = worldDir;
	m_chunksDir = chunksDir;
	m_name = name;
	m_seed = currentSeed;
	m_service = std::make_unique<SaveService>(chunksDir, currentSeed);
	// Completion refinement: after a successful write/delete the index
	// converges to the true minimal override set (the captured entry is a
	// superset until the worker diffs it). Called from the worker thread.
	m_service->setCompletionCallback(
		[this](int32_t cx, int32_t cz, const std::vector<worldsave::ChunkEdit> &finalOverrides) {
			std::lock_guard<std::mutex> lock(m_indexMutex);
			if (finalOverrides.empty())
				m_overrides.erase(coordKey(cx, cz));
			else
				m_overrides[coordKey(cx, cz)] = finalOverrides;
		});
	m_enabled = true;

	info.ok = true;
	return info;
}

bool WorldPersistence::hasOverrides(int32_t chunkX, int32_t chunkZ) const
{
	std::lock_guard<std::mutex> lock(m_indexMutex);
	return m_overrides.find(coordKey(chunkX, chunkZ)) != m_overrides.end();
}

const std::vector<worldsave::ChunkEdit> *WorldPersistence::overridesFor(int32_t chunkX,
                                                                        int32_t chunkZ) const
{
	std::lock_guard<std::mutex> lock(m_indexMutex);
	const auto it = m_overrides.find(coordKey(chunkX, chunkZ));
	return it != m_overrides.end() ? &it->second : nullptr;
}

std::vector<worldsave::ChunkEdit> WorldPersistence::overridesSnapshot(int32_t chunkX,
                                                                      int32_t chunkZ) const
{
	std::lock_guard<std::mutex> lock(m_indexMutex);
	const auto it = m_overrides.find(coordKey(chunkX, chunkZ));
	return it != m_overrides.end() ? it->second : std::vector<worldsave::ChunkEdit>{};
}

void WorldPersistence::captureChunkEdits(int32_t chunkX, int32_t chunkZ,
                                         std::vector<worldsave::ChunkEdit> currentValues)
{
	// Canonical order: ascending localIndex; repeated writes to one voxel
	// collapse to the last value (defensive - captures from Chunk edit maps
	// already hold unique indices).
	std::stable_sort(currentValues.begin(), currentValues.end(),
	                 [](const worldsave::ChunkEdit &a, const worldsave::ChunkEdit &b) {
		                 return a.localIndex < b.localIndex;
	                 });
	for (size_t i = 0; i + 1 < currentValues.size();)
	{
		if (currentValues[i].localIndex == currentValues[i + 1].localIndex)
			currentValues.erase(currentValues.begin() + static_cast<std::ptrdiff_t>(i) + 1);
		else
			++i;
	}

	bool enqueue = true;
	{
		std::lock_guard<std::mutex> lock(m_indexMutex);
		const auto key = coordKey(chunkX, chunkZ);
		const auto it = m_overrides.find(key);
		const bool hadOverrides = it != m_overrides.end() && !it->second.empty();
		if (currentValues.empty() && !hadOverrides)
		{
			// Nothing captured and nothing recorded: no override state
			// exists anywhere, so no file can be stale. Skip entirely.
			enqueue = false;
		}
		else if (hadOverrides && sameEditList(it->second, currentValues))
		{
			// Identical to the recorded entry: the index already carries
			// these values and the last persisted write produced exactly
			// this diff (or is still queued / superseded-safe). Skip.
			enqueue = false;
		}
		else if (currentValues.empty())
		{
			m_overrides.erase(key);
		}
		else
		{
			m_overrides[key] = currentValues;
		}
	}

	if (!enqueue)
		return;

	if (!m_enabled || !m_service)
	{
		recordError("save service not running: capture for chunk (" + std::to_string(chunkX) +
		            ", " + std::to_string(chunkZ) + ") was dropped");
		return;
	}

	ChunkSaveRequest request;
	request.chunkX = chunkX;
	request.chunkZ = chunkZ;
	request.currentValues = currentValues; // index keeps the superset copy
	if (!m_service->enqueue(std::move(request)))
		recordError("save service rejected capture for chunk (" + std::to_string(chunkX) + ", " +
		            std::to_string(chunkZ) + ") because it is shutting down");
}

bool WorldPersistence::flush()
{
	return m_service ? m_service->flush() : true;
}

// ---------------------------------------------------------------------------
// Player state (issue #180, Phase 5) — <world>/player.state, format in
// WorldSave.hpp (magic "FTVP", version 1, 45 bytes, explicit little-endian).
// ---------------------------------------------------------------------------

bool WorldPersistence::writePlayerState(const PlayerPersistState &state) const
{
	if (!m_enabled || m_worldDir.empty())
	{
		recordError("player state write skipped: no world open");
		return false;
	}
	worldsave::ByteWriter w;
	w.raw(worldsave::kPlayerStateMagic, sizeof(worldsave::kPlayerStateMagic));
	w.u32(worldsave::kPlayerStateFormatVersion);
	w.f64(state.x);
	w.f64(state.y);
	w.f64(state.z);
	w.f32(state.yaw);
	w.f32(state.pitch);
	w.u8(state.flight ? 1u : 0u);
	w.i32(state.selectedBlock);
	if (!worldsave::writeFileAtomic(m_worldDir / "player.state", w.take()))
	{
		recordError("failed to write player.state (I/O error)");
		return false;
	}
	return true;
}

bool WorldPersistence::readPlayerState(PlayerPersistState &out) const
{
	out = PlayerPersistState{};
	if (!m_enabled || m_worldDir.empty())
		return false; // no world open: quiet (nothing was ever saved)

	std::vector<uint8_t> bytes;
	const worldsave::SaveStatus read = worldsave::readFileInto(m_worldDir / "player.state", bytes);
	if (read == worldsave::SaveStatus::NotFound)
		return false; // fresh world: normal, NOT an error
	if (read != worldsave::SaveStatus::Ok)
	{
		recordError("player.state unreadable (" + statusName(read) + ")");
		return false;
	}

	worldsave::ByteReader r(bytes.data(), bytes.size());
	char magic[sizeof(worldsave::kPlayerStateMagic)];
	r.raw(magic, sizeof(magic));
	const uint32_t version = r.u32();
	PlayerPersistState s;
	s.x = r.f64();
	s.y = r.f64();
	s.z = r.f64();
	s.yaw = r.f32();
	s.pitch = r.f32();
	s.flight = r.u8() != 0;
	s.selectedBlock = r.i32();
	if (r.failed())
	{
		recordError("player.state is truncated (expected 45 bytes, got " +
		            std::to_string(bytes.size()) + ")");
		return false;
	}
	if (std::memcmp(magic, worldsave::kPlayerStateMagic, sizeof(magic)) != 0)
	{
		recordError("player.state has a bad magic");
		return false;
	}
	if (version > worldsave::kPlayerStateFormatVersion)
	{
		recordError("player.state uses unsupported version " + std::to_string(version) +
		            " (this build supports up to " +
		            std::to_string(worldsave::kPlayerStateFormatVersion) + ")");
		return false;
	}

	// Payload sanity: a corrupt file must never teleport the player or hand
	// NaN into the physics solver. Y range covers the world plus margin for
	// creative flight above the build limit.
	const bool finite = std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.z) &&
	                    std::isfinite(s.yaw) && std::isfinite(s.pitch);
	constexpr double kMinY = -64.0;
	constexpr double kMaxY = static_cast<double>(CHUNK_HEIGHT) + 64.0;
	if (!finite || s.y < kMinY || s.y > kMaxY)
	{
		recordError("player.state rejected: position/orientation out of range");
		return false;
	}

	out = s;
	return true;
}

void WorldPersistence::shutdown()
{
	if (!m_service)
	{
		m_enabled = false;
		return;
	}
	if (!m_service->flush())
		recordError("world save flush during shutdown reported failures (see save stats)");
	m_service->shutdown();
	m_service.reset();
	m_enabled = false;
}

WorldPersistence::Status WorldPersistence::status() const
{
	Status s;
	s.name = m_name;
	s.seed = m_seed;
	if (m_service)
	{
		const SaveService::Stats st = m_service->stats();
		s.queueDepth = st.queueDepth;
		s.enqueued = st.enqueued;
		s.completed = st.completed;
		s.superseded = st.superseded;
		s.failed = st.failed;
		s.deleted = st.deleted;
		s.bytesWritten = st.bytesWritten;
		if (!st.lastError.empty())
			s.lastError = st.lastError;
	}
	std::lock_guard<std::mutex> lock(m_indexMutex);
	s.pendingCaptureEntries = m_overrides.size();
	if (!m_errorLog.empty())
		s.lastError = s.lastError.empty() ? m_errorLog : (s.lastError + "\n" + m_errorLog);
	return s;
}
