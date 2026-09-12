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

// Thrown by processJob when the revision check finds the job superseded
// mid-flight; unwinds before any file system mutation happened.
struct StaleJobException {};

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

// Merge a captured DELTA into the coordinate's desired state (issue #180
// review round 5): base entries win first, delta entries overwrite, the
// result is canonical (ascending localIndex, unique). Sizes are small (a
// chunk's edit count); an ordered merge of two sorted vectors would be a
// later optimization if profiling ever demands it.
void mergeEdits(std::vector<worldsave::ChunkEdit> &base,
                const std::vector<worldsave::ChunkEdit> &delta)
{
	if (delta.empty())
		return;
	std::unordered_map<uint32_t, uint8_t> merged;
	merged.reserve(base.size() + delta.size());
	for (const worldsave::ChunkEdit &edit : base)
		merged[edit.localIndex] = edit.blockType;
	for (const worldsave::ChunkEdit &edit : delta)
		merged[edit.localIndex] = edit.blockType;
	base.clear();
	base.reserve(merged.size());
	for (const auto &[localIndex, blockType] : merged)
		base.push_back({localIndex, blockType});
	std::sort(base.begin(), base.end(),
	          [](const worldsave::ChunkEdit &a, const worldsave::ChunkEdit &b)
	          { return a.localIndex < b.localIndex; });
}

// Canonical capture order: ascending localIndex with duplicates collapsing to
// the LAST value (true last-write-wins, issue #180 review - a stable_sort
// followed by keep-first would preserve a stale value).
std::vector<worldsave::ChunkEdit> collapseAndSort(std::vector<worldsave::ChunkEdit> edits)
{
	std::unordered_map<uint32_t, uint8_t> collapsed;
	collapsed.reserve(edits.size());
	for (const worldsave::ChunkEdit &edit : edits)
		collapsed[edit.localIndex] = edit.blockType;
	edits.clear();
	edits.reserve(collapsed.size());
	for (const auto &[localIndex, blockType] : collapsed)
		edits.push_back({localIndex, blockType});
	std::sort(edits.begin(), edits.end(),
	          [](const worldsave::ChunkEdit &a, const worldsave::ChunkEdit &b)
	          { return a.localIndex < b.localIndex; });
	return edits;
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

// Dirty means "the durable copy does not yet reflect desired": pending work,
// a failed attempt, or revisions that moved ahead with differing content.
// Content-equal-but-revision-behind is NOT dirty: the file on disk already
// carries exactly these overrides.
bool WorldPersistence::ChunkPersistenceState::dirty() const
{
	if (pending || failed)
		return true;
	return desiredRevision > durableRevision && !sameEditList(desired, durable);
}

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

void SaveService::setWriteTmpFnForTests(WriteTmpFn fn)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_writeTmp = std::move(fn);
}

void SaveService::setSerializeFnForTests(SerializeFn fn)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_serialize = std::move(fn);
}

void SaveService::setBeforeCommitFnForTests(BeforeCommitFn fn)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_beforeCommit = std::move(fn);
}

bool SaveService::enqueue(ChunkSaveRequest &&request)
{
	if (request.revision == 0)
		return false; // revisions are owned by WorldPersistence and start at 1

	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_shuttingDown)
		return false;

	const uint64_t key = coordKey(request.chunkX, request.chunkZ);

	// Commit gate (issue #180 review round 3): the coordinate's rename/
	// delete is in flight outside the mutex. Refuse with Busy and NO
	// bookkeeping (no ticket, no latestRevision, no queue entry) exactly
	// like the queue-bound case - a ticket created here could never be
	// resolved before the flush barrier sampled it, recreating the
	// revision-barrier deadlock in ticket form. The caller keeps the state
	// dirty and retries.
	if (m_committingCoords.count(key) != 0)
		return false;

	// Per-coordinate coalescing (issue #180 review): a newer capture REPLACES
	// the queued payload in place, so one coordinate is at most one pending
	// job and the bound below counts distinct chunks - the main thread is
	// never blocked waiting for disk I/O.
	const auto existing = m_pendingByCoord.find(key);
	if (existing == m_pendingByCoord.end() && m_pendingByCoord.size() >= kMaxPendingCoords)
		return false; // Busy: caller keeps the state dirty and retries later.
		              // Ticket/revision bookkeeping MUST NOT happen for a
		              // rejected request: the flush barrier waits for
		              // accepted tickets only, and a rejected ticket would
		              // never resolve -> deadlock.

	// In-place replacement resolves the replaced request's ticket as
	// SUPERSEDED (issue #180 review round 2): it will never be popped, and a
	// flush waiting on it must not block forever. The new ticket becomes the
	// only pending one for this coordinate.
	const bool replaced = existing != m_pendingByCoord.end();
	if (replaced)
	{
		resolveTicketLocked(existing->second.barrierTicket);
		++m_superseded;
	}

	request.barrierTicket = m_nextBarrierTicket++;
	m_lastAcceptedTicket = request.barrierTicket;
	m_unfinishedTickets.insert(request.barrierTicket);
	m_latestRevision[key] = request.revision;

	if (replaced)
	{
		existing->second = std::move(request);
		return true;
	}

	m_pendingByCoord.emplace(key, std::move(request));
	m_order.push_back(key);
	++m_enqueued;
	m_queueCv.notify_one();
	return true;
}

bool SaveService::flush()
{
	std::unique_lock<std::mutex> lock(m_mutex);
	if (m_shuttingDown)
		return false;
	// Ticket barrier (issue #180 review round 2): completion order is NOT
	// revision order under per-coordinate coalescing (a replaced revision is
	// never finished), so the barrier tracks ACCEPTANCE tickets: wait until
	// no ticket <= the sampled target remains. The ordered set makes the
	// predicate O(1).
	const uint64_t target = m_lastAcceptedTicket;
	const uint64_t failuresBeforeBarrier = m_failureCheckpoint;
	m_doneCv.wait(lock, [&] {
		return m_shuttingDown || m_unfinishedTickets.empty() ||
		       *m_unfinishedTickets.begin() > target;
	});
	if (m_shuttingDown)
		return false;
	// Failure accounting per barrier range: the baseline is the checkpoint of
	// the last OBSERVED flush (not the counter at call time), so a failure
	// that completed after the previous flush but before this call is
	// reported by THIS flush. The checkpoint advances only once observed.
	const bool ok = m_failureCount == failuresBeforeBarrier;
	m_failureCheckpoint = m_failureCount;
	return ok;
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
			m_pendingByCoord.clear();
			m_order.clear();
			m_latestRevision.clear();
			m_unfinishedTickets.clear();
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
	s.queueDepth = m_pendingByCoord.size();
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
			m_queueCv.wait(lock, [&] { return stop.stop_requested() || !m_order.empty(); });
			if (stop.stop_requested())
			{
				// Shutdown discards pending work by contract (flush() first
				// for durability).
				m_pendingByCoord.clear();
				m_order.clear();
				m_latestRevision.clear();
				m_unfinishedTickets.clear();
				m_doneCv.notify_all();
				return;
			}
			// The FIFO holds coordinates; the payload lives in the coalescing
			// map and is always the newest one captured for that coordinate.
			const uint64_t key = m_order.front();
			m_order.pop_front();
			const auto it = m_pendingByCoord.find(key);
			if (it == m_pendingByCoord.end())
				continue; // defensive: cannot happen (one slot per coordinate)
			job.request = std::move(it->second);
			m_pendingByCoord.erase(it);
		}

		processJob(job);

		// The ticket is resolved for EVERY outcome (written, deleted,
		// superseded, failed): it represents "this accepted enqueue is no
		// longer pending in the service".
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			resolveTicketLocked(job.request.barrierTicket);
		}
		m_doneCv.notify_all();
	}
}

void SaveService::resolveTicketLocked(uint64_t ticket)
{
	m_unfinishedTickets.erase(ticket);
	m_doneCv.notify_all();
}

void SaveService::processJob(Job &job)
{
	const int32_t chunkX = job.request.chunkX;
	const int32_t chunkZ = job.request.chunkZ;
	const uint64_t revision = job.request.revision;
	const uint64_t key = coordKey(chunkX, chunkZ);

	bool ok = false;
	bool stale = false;
	bool deleted = false;
	std::string error;
	std::vector<worldsave::ChunkEdit> overrides;
	double serializeMs = 0.0;
	double writeMs = 0.0;

	// Test seams are copied out and invoked WITHOUT the service mutex so a
	// test can block the worker while the main thread enqueues.
	SerializeFn serialize;
	WriteTmpFn writeTmp;
	BeforeCommitFn beforeCommit;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		serialize = m_serialize;
		writeTmp = m_writeTmp;
		beforeCommit = m_beforeCommit;
	}

	try
	{
		// (1) Regenerate the deterministic base (serialize time = base regen
		// + diff). The payload is immutable, so the source chunk was free to
		// recycle the moment it was captured. The serialize step is
		// injectable for tests (a full chunk generation per request is far
		// too slow for queue-behavior coverage).
		const Clock::time_point t0 = Clock::now();
		if (serialize)
			overrides = serialize(chunkX, chunkZ, job.request.currentValues);
		else
		{
			auto base = TerrainGenerator::getThreadLocal(m_seed).generateChunk(chunkX, chunkZ);
			if (base.voxels.size() < static_cast<size_t>(CHUNK_VOLUME))
				throw std::runtime_error("regenerated base has wrong voxel count");
			static_assert(sizeof(Voxel) == 1, "diff walks the voxel bytes as u8");
			overrides = worldsave::diffEditsAgainstBase(
				reinterpret_cast<const uint8_t *>(base.voxels.data()), job.request.currentValues);
		}
		serializeMs = elapsedMs(t0);

		// (2) Optimistic stale check before ANY file system work: a job
		// superseded while its base was regenerating is dropped without
		// spending a tmp write. This check is NOT the authority - step (4)
		// re-checks atomically with the commit.
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			const auto it = m_latestRevision.find(key);
			stale = it != m_latestRevision.end() && it->second != revision;
		}
		if (stale)
			throw StaleJobException{};

		// (3) Build the payload on disk but OUTSIDE the service mutex: the
		// tmp sidecar is transient and never authoritative.
		const Clock::time_point t1 = Clock::now();
		std::filesystem::path tmpPath;
		bool haveTmp = false;
		if (!overrides.empty())
		{
			const worldsave::SaveStatus st =
				writeTmp ? writeTmp(m_chunksDir, chunkX, chunkZ, overrides, tmpPath)
			             : worldsave::writeChunkFileTmp(m_chunksDir, chunkX, chunkZ, overrides,
			                                            tmpPath);
			if (st != worldsave::SaveStatus::Ok)
			{
				ok = false;
				error = "writeChunkFileTmp failed (" + statusName(st) + ")";
			}
			else
			{
				haveTmp = true;
			}
		}

		// (4) Authoritative commit, ATOMIC with the revision check
		// (issue #180 review round 2) WITHOUT holding the mutex during the
		// filesystem mutation (issue #180 review round 3): the check+mark
		// runs under the mutex, the rename/delete runs outside it, and
		// enqueue() refuses marked coordinates - so no newer revision can
		// slip between the check and the mutation, and enqueue() is never
		// blocked by disk I/O. Invariant: only the coordinate's
		// latestRevision may modify its authoritative file. Applies to BOTH
		// commits: a delete (empty diff) is as authoritative as a rename.
		if (haveTmp || overrides.empty())
		{
			if (beforeCommit)
				beforeCommit(chunkX, chunkZ, revision);
			bool marked = false;
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				const auto it = m_latestRevision.find(key);
				stale = it != m_latestRevision.end() && it->second != revision;
				if (!stale)
				{
					m_committingCoords.insert(key);
					marked = true;
				}
			}
			if (marked)
			{
				// Scope guard: the coordinate is un-marked on every path,
				// including exceptions from the filesystem layer.
				struct CommitGateGuard
				{
					SaveService *service;
					uint64_t key;
					~CommitGateGuard()
					{
						std::lock_guard<std::mutex> lock(service->m_mutex);
						service->m_committingCoords.erase(key);
					}
				} gate{this, key};

				if (overrides.empty())
				{
					deleted = true;
					const worldsave::SaveStatus st =
						worldsave::removeChunkFile(m_chunksDir, chunkX, chunkZ);
					// removeChunkFile is Ok for both "removed" and "was
					// already absent" (idempotent delete).
					ok = (st == worldsave::SaveStatus::Ok);
					if (!ok)
						error = "removeChunkFile failed (" + statusName(st) + ")";
				}
				else
				{
					const worldsave::SaveStatus st = worldsave::commitChunkFile(tmpPath);
					ok = (st == worldsave::SaveStatus::Ok);
					if (ok)
						haveTmp = false; // renamed over the final file
					else
						error = "commitChunkFile failed (" + statusName(st) + ")";
				}
			}
		}
		if (stale && haveTmp)
		{
			// A stale job's tmp sidecar is garbage: it must never linger.
			std::error_code removeEc;
			std::filesystem::remove(tmpPath, removeEc);
			haveTmp = false;
		}
		writeMs = elapsedMs(t1);
	}
	catch (const StaleJobException &)
	{
		stale = true;
	}
	catch (const std::exception &e)
	{
		ok = false;
		error = std::string("serialize exception: ") + e.what();
	}

	// (4) Commit accounting under the mutex (failure bookkeeping is atomic
	// with the revision progress flush() waits on), then run the completion
	// callback WITHOUT holding the service mutex (it locks the persistence
	// state; no lock-order cycle). Stale jobs are neither a completion nor a
	// failure: the newer revision owns the coordinate and reports for it.
	CompletionCallback completion;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (stale)
		{
			++m_superseded;
		}
		else if (ok)
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
			++m_failureCount;
			m_lastError = "chunk (" + std::to_string(chunkX) + ", " + std::to_string(chunkZ) +
			              "): " + error;
		}
		completion = m_completion;
	}
	if (!stale && completion)
	{
		if (deleted)
		{
			static const std::vector<worldsave::ChunkEdit> kEmpty;
			completion(chunkX, chunkZ, revision, ok, kEmpty, error);
		}
		else
		{
			completion(chunkX, chunkZ, revision, ok, overrides, error);
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

bool WorldPersistence::isValidWorldName(std::string_view name)
{
	if (name.empty() || name.find("..") != std::string_view::npos)
		return false;
	constexpr std::string_view kForbidden = "/\\:*?\"<>|";
	for (const char c : name)
		if (kForbidden.find(c) != std::string_view::npos)
			return false;
	return true;
}

void WorldPersistence::recordError(const std::string &error) const
{
	std::lock_guard<std::mutex> lock(m_indexMutex);
	recordErrorLocked(error);
}

void WorldPersistence::recordErrorLocked(const std::string &error) const
{
	if (!m_errorLog.empty())
		m_errorLog += "\n";
	m_errorLog += error;
	// Bounded log: keep the tail so a long session cannot grow it forever.
	constexpr size_t kMaxErrorLogBytes = 8192;
	if (m_errorLog.size() > kMaxErrorLogBytes)
		m_errorLog.erase(0, m_errorLog.size() - kMaxErrorLogBytes);
}

const std::vector<worldsave::ChunkEdit> &
WorldPersistence::applicableLocked(const ChunkPersistenceState &state) const
{
	// A capture this session makes `desired` authoritative (an override
	// superset is always safe to re-apply); before the first capture the
	// durable baseline loaded from disk is what a reload applies.
	return state.desiredRevision != 0 ? state.desired : state.durable;
}

bool WorldPersistence::worldExists(const std::filesystem::path &savesRoot, const std::string &name)
{
	if (!isValidWorldName(name))
		return false;
	std::error_code ec;
	return std::filesystem::exists(savesRoot / name / "world.meta", ec) && !ec;
}

bool WorldPersistence::peekStoredSeed(const std::filesystem::path &savesRoot,
                                      const std::string &name, int &outSeed, std::string &error)
{
	if (!isValidWorldName(name))
	{
		error = "invalid world name";
		return false;
	}
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
	if (!isValidWorldName(name))
	{
		info.error = "invalid world name (must be non-empty and free of path separators)";
		return info;
	}

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

	// Interrupted atomic writes are never authoritative. A failure to clean
	// them is a writable-directory problem: refuse rather than save into it.
	if (worldsave::cleanTempFiles(chunksDir) != worldsave::SaveStatus::Ok)
	{
		info.error = "world '" + name + "' chunk directory could not be cleaned (I/O error)";
		return info;
	}

	// Load every chunk file as the DURABLE baseline of the per-coordinate
	// state machine. A file that fails to read is loudly reported and
	// SKIPPED (procedural fallback for that chunk); a directory SCAN error
	// (permission denied, path is a file, ...) refuses the open instead - an
	// I/O error must never be misread as "no overrides saved".
	{
		const worldsave::ScanResult scan = worldsave::scanChunkFiles(chunksDir);
		if (scan.status != worldsave::SaveStatus::Ok)
		{
			info.error = "world '" + name + "' chunk directory is not readable (I/O error); "
			             "refusing to open rather than treating saves as absent";
			return info;
		}
		std::lock_guard<std::mutex> lock(m_indexMutex);
		m_states.clear();
		m_errorLog.clear();
		for (const std::filesystem::path &path : scan.files)
		{
			int32_t cx = 0;
			int32_t cz = 0;
			(void)worldsave::parseChunkFileName(path.filename().string(), cx, cz);
			std::vector<worldsave::ChunkEdit> edits;
			const worldsave::SaveStatus st = worldsave::readChunkFile(path, cx, cz, edits);
			if (st == worldsave::SaveStatus::Ok)
			{
				ChunkPersistenceState state;
				state.chunkX = cx;
				state.chunkZ = cz;
				state.durable = std::move(edits);
				m_states.emplace(coordKey(cx, cz), std::move(state));
			}
			else
			{
				const std::string message = "chunk file " + path.filename().string() +
				                            " failed to load (" + statusName(st) +
				                            "); using procedural fallback for that chunk";
				recordErrorLocked(message);
			}
		}
	}

	m_worldDir = worldDir;
	m_chunksDir = chunksDir;
	m_name = name;
	m_seed = currentSeed;
	m_service = std::make_unique<SaveService>(chunksDir, currentSeed);
	// Completion handling (issue #180 review): a completion only commits when
	// its revision is still the coordinate's desired one. A job superseded
	// in flight by a newer capture can never overwrite the newer state in
	// memory. Called from the worker thread.
	m_service->setCompletionCallback(
		[this](int32_t cx, int32_t cz, uint64_t revision, bool ok,
		       const std::vector<worldsave::ChunkEdit> &finalOverrides, const std::string &error) {
			std::lock_guard<std::mutex> lock(m_indexMutex);
			const auto key = coordKey(cx, cz);
			const auto it = m_states.find(key);
			if (it == m_states.end())
				return;
			ChunkPersistenceState &state = it->second;
			if (revision != state.desiredRevision)
				return; // stale completion: a newer capture owns this coordinate
			            // (and must NOT canonicalize desired - issue #180
			            // review round 5, item 8)
			state.pending = false;
			if (!ok)
			{
				// desiredRevision stays > durableRevision: the next capture,
				// flush or close retries the write (never dropped silently).
				state.failed = true;
				recordErrorLocked("chunk (" + std::to_string(cx) + ", " + std::to_string(cz) +
				                  ") save failed: " + error);
				return;
			}
			state.durable = finalOverrides;
			state.durableRevision = revision;
			// Canonicalize desired to the true minimal override set: entries
			// that reverted to procedural terrain are dropped here - the
			// worker is the only layer that knows the regenerated base.
			state.desired = finalOverrides;
			state.failed = false;
		});
	m_enabled = true;

	info.ok = true;
	return info;
}

bool WorldPersistence::hasOverrides(int32_t chunkX, int32_t chunkZ) const
{
	std::lock_guard<std::mutex> lock(m_indexMutex);
	const auto it = m_states.find(coordKey(chunkX, chunkZ));
	return it != m_states.end() && !applicableLocked(it->second).empty();
}

std::vector<worldsave::ChunkEdit> WorldPersistence::overridesSnapshot(int32_t chunkX,
                                                                      int32_t chunkZ) const
{
	std::lock_guard<std::mutex> lock(m_indexMutex);
	const auto it = m_states.find(coordKey(chunkX, chunkZ));
	return it != m_states.end() ? applicableLocked(it->second)
	                            : std::vector<worldsave::ChunkEdit>{};
}

void WorldPersistence::captureChunkEdits(int32_t chunkX, int32_t chunkZ,
                                         std::vector<worldsave::ChunkEdit> currentValues)
{
	// Canonical capture payload: ascending localIndex, duplicates collapsed
	// last-write-wins (defensive - captures from Chunk edit maps already
	// hold unique indices).
	currentValues = collapseAndSort(std::move(currentValues));

	if (!m_enabled || !m_service)
	{
		recordError("save service not running: capture for chunk (" + std::to_string(chunkX) +
		            ", " + std::to_string(chunkZ) + ") was dropped");
		return;
	}

	uint64_t revision = 0;
	std::vector<worldsave::ChunkEdit> payload;
	{
		std::lock_guard<std::mutex> lock(m_indexMutex);
		const uint64_t key = coordKey(chunkX, chunkZ);
		ChunkPersistenceState &state = m_states[key];
		state.chunkX = chunkX;
		state.chunkZ = chunkZ;

		// Delta merge (issue #180 review round 5): the payload carries only
		// the edits since the chunk's edit map was last taken, so it is
		// MERGED into desired (seeded from the durable baseline on the first
		// capture of the session) instead of replacing it. The procedural
		// base is unknown here - reverts capture the base VALUE and the save
		// worker's diff drops them from the persisted file.
		std::vector<worldsave::ChunkEdit> merged =
			state.desiredRevision != 0 ? state.desired : state.durable;
		mergeEdits(merged, currentValues);
		state.desired = std::move(merged);

		// Enqueue decision (issue #180 review): work is only needed when the
		// desired content is not already durable, not pending, and the last
		// attempt did not fail. A re-capture of identical content on a clean
		// coordinate skips; a FAILED coordinate retries even for identical
		// content (its durable copy is not confirmed).
		const bool clean =
			sameEditList(state.desired, state.durable) && !state.failed && !state.pending;
		if (clean)
			return;

		// (Re)enqueue with a FRESH revision: the service's flush barrier
		// tracks progress by revision, so a retry that reused the already
		// finished revision of the failed attempt would not be waited for.
		state.pending = true; // completion or rejection clears it
		state.failed = false;
		state.desiredRevision = m_nextCaptureRevision++;
		revision = state.desiredRevision;
		payload = state.desired;
	}

	ChunkSaveRequest request;
	request.chunkX = chunkX;
	request.chunkZ = chunkZ;
	request.revision = revision;
	request.currentValues = std::move(payload);
	if (!m_service->enqueue(std::move(request)))
	{
		// Busy (distinct-coordinate bound reached) or shutting down: the
		// state stays dirty (desiredRevision > durableRevision) and is
		// retried by the next capture or flush - never dropped silently.
		std::lock_guard<std::mutex> lock(m_indexMutex);
		const auto it = m_states.find(coordKey(chunkX, chunkZ));
		if (it != m_states.end())
			it->second.pending = false;
		recordErrorLocked("save service queue rejected capture for chunk (" +
		                  std::to_string(chunkX) + ", " + std::to_string(chunkZ) +
		                  "); retrying on the next capture/flush");
	}
}

void WorldPersistence::setWriteTmpFnForTests(SaveService::WriteTmpFn fn)
{
	if (m_service)
		m_service->setWriteTmpFnForTests(std::move(fn));
}

void WorldPersistence::setSerializeFnForTests(SaveService::SerializeFn fn)
{
	if (m_service)
		m_service->setSerializeFnForTests(std::move(fn));
}

void WorldPersistence::setBeforeCommitFnForTests(SaveService::BeforeCommitFn fn)
{
	if (m_service)
		m_service->setBeforeCommitFnForTests(std::move(fn));
}

bool WorldPersistence::flush()
{
	if (!m_service)
		return !m_enabled; // never opened: trivially consistent; open+shut: reported by shutdown

	// Bounded retry loop (issue #180 review): each round re-enqueues every
	// coordinate whose desired content is not durable and not pending
	// (failed writes, captures the queue rejected with Busy), then barrier-
	// waits for the service. Rounds repeat while progress is made, so a
	// queue-full overload drains across rounds; a genuine I/O failure stops
	// making progress and the loop exits with the failure reported.
	constexpr int kMaxFlushRounds = 8;
	bool ok = true;
	for (int round = 0; round < kMaxFlushRounds; ++round)
	{
		std::vector<ChunkSaveRequest> retries;
		{
			std::lock_guard<std::mutex> lock(m_indexMutex);
			for (auto &[key, state] : m_states)
			{
				(void)key;
				if (state.pending || state.desiredRevision == 0 || !state.dirty())
					continue;
				// Fresh revision for the retried attempt: the service's flush
				// barrier tracks progress by revision, so a retry must not
				// reuse a revision the barrier already saw finished.
				state.pending = true;
				state.failed = false;
				state.desiredRevision = m_nextCaptureRevision++;
				ChunkSaveRequest request;
				request.chunkX = state.chunkX;
				request.chunkZ = state.chunkZ;
				request.revision = state.desiredRevision;
				request.currentValues = state.desired;
				retries.push_back(std::move(request));
			}
		}
		for (ChunkSaveRequest &request : retries)
		{
			if (!m_service->enqueue(std::move(request)))
			{
				// Busy/shutdown: leave the state dirty for the next round.
				std::lock_guard<std::mutex> lock(m_indexMutex);
				const auto it = m_states.find(coordKey(request.chunkX, request.chunkZ));
				if (it != m_states.end())
				{
					it->second.pending = false;
					recordErrorLocked("save service queue rejected retry for chunk (" +
					                  std::to_string(request.chunkX) + ", " +
					                  std::to_string(request.chunkZ) + ")");
				}
			}
		}

		if (!m_service->flush())
			ok = false; // barrier range reported failures; keep sweeping/re-checking

		bool anyDirty = false;
		{
			std::lock_guard<std::mutex> lock(m_indexMutex);
			for (auto &[key, state] : m_states)
			{
				(void)key;
				if (state.dirty())
				{
					anyDirty = true;
					break;
				}
			}
		}
		if (!anyDirty)
			return ok;
	}
	// Still dirty after kMaxFlushRounds: report failure and record WHICH
	// coordinates stayed dirty so the error log/debug UI points at them.
	{
		std::lock_guard<std::mutex> lock(m_indexMutex);
		std::string coords;
		int listed = 0;
		for (const auto &[key, state] : m_states)
		{
			(void)key;
			if (!state.dirty())
				continue;
			coords += " (" + std::to_string(state.chunkX) + "," + std::to_string(state.chunkZ) + ")";
			if (++listed >= 8)
			{
				coords += " ...";
				break;
			}
		}
		if (!coords.empty())
			recordErrorLocked("flush exhausted retries, still dirty:" + coords);
	}
	return false;
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
	if (version != worldsave::kPlayerStateFormatVersion)
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
	if (!flush())
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
	uint64_t dirty = 0;
	{
		std::lock_guard<std::mutex> lock(m_indexMutex);
		s.pendingCaptureEntries = m_states.size();
		for (const auto &[key, state] : m_states)
		{
			(void)key;
			if (state.dirty())
				++dirty;
		}
		if (!m_errorLog.empty())
			s.lastError = s.lastError.empty() ? m_errorLog : (s.lastError + "\n" + m_errorLog);
	}
	s.dirtyCoordinates = dirty;
	return s;
}
