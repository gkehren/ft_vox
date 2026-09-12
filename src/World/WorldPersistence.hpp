#pragma once

// Persistent world-save service layer (issue #180, Phase 3). Builds on the
// pure format layer in WorldSave.hpp and adds the two runtime pieces the
// engine needs:
//
//   - SaveService: a dedicated async I/O worker that turns "capture the
//     current value of every edited voxel" requests into minimal chunk files
//     on disk. The worker never sees Chunk pointers: requests carry immutable
//     payloads, and the deterministic terrain base is REGENERATED inside the
//     worker (serialize time = base regen + diff), so a chunk can be recycled
//     the instant its payload was copied out.
//   - WorldPersistence: the facade owning the world directory + world.meta
//     identity and the per-chunk persistence state machine.
//
// Revisions and the desired/durable state machine (issue #180 review):
//
//   WorldPersistence (not SaveService) owns the monotonic save-revision
//   counter. Every capture stamps the coordinate's desiredRevision; the
//   enqueue carries it, the worker echoes it back through the completion
//   callback, and the state machine ignores any completion whose revision is
//   not the current desired one. A request captured while an older one is
//   in flight therefore can never be overwritten in memory by the older
//   completion, and a job that went stale during base regeneration performs
//   no I/O at all (checked before any file is touched).
//
//   Per coordinate the facade keeps desired (what the game last captured)
//   separate from durable (what is confirmed on disk):
//
//     desired == durable && !failed && !pending  ->  nothing to save
//     otherwise                                  ->  (re)enqueue / retry
//
//   A failed write leaves desiredRevision > durableRevision with failed=true
//   and is retried by the next capture, flush or close - never dropped.
//
// Threading contract:
//   - SaveService owns ONE dedicated std::jthread (disk I/O must never run on
//     the shared gen/mesh ThreadPool - issue #180 explicitly requires disk
//     stalls to be unable to starve worldgen).
//   - The completion callback is invoked from the worker thread.
//   - WorldPersistence's state map is mutex-guarded; captureChunkEdits(), the
//     completion callback and flush()'s retry sweep are the only writers.
//
// Destructor safety: ~SaveService / ~WorldPersistence discard pending work
// and join the worker (call flush() first when durability is required).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <World/WorldSave.hpp>

// One queued save unit. `currentValues` holds the CURRENT value of each
// edited voxel (an override superset, NOT a diff and NOT an operation log);
// the worker diffs it against the regenerated deterministic base. `revision`
// is assigned by WorldPersistence and must be > 0 and monotonically
// increasing across every enqueue.
struct ChunkSaveRequest
{
	int32_t chunkX{0};
	int32_t chunkZ{0};
	// Logical content revision (WorldPersistence-owned; used ONLY for
	// per-chunk stale detection).
	uint64_t revision{0};
	// Barrier ticket: assigned by SaveService on ACCEPTED enqueue, in strict
	// acceptance order. flush() waits for all tickets <= its sampled target.
	// Independent from `revision` because coalescing can replace a queued
	// payload in place - a replaced revision is never "finished" by the
	// worker, so revision order must never be used as completion order
	// (issue #180 review round 2).
	uint64_t barrierTicket{0};
	std::vector<worldsave::ChunkEdit> currentValues;
};

/// Dedicated-thread async chunk save worker.
class SaveService
{
public:
	/// Invoked from the worker thread after a processed request: ok=true
	/// carries the minimal persisted override set (empty for a reverted
	/// chunk whose file was deleted); ok=false carries the error text. The
	/// revision lets WorldPersistence drop stale completions. Must not call
	/// back into the service. Installed before the first enqueue.
	using CompletionCallback = std::function<void(
		int32_t chunkX, int32_t chunkZ, uint64_t revision, bool ok,
		const std::vector<worldsave::ChunkEdit> &finalOverrides, const std::string &error)>;

	/// Test seam (issue #180 review): replaces the real "write the .tmp
	/// sidecar" step so write failures and mid-write supersede windows are
	/// deterministic. Null = real writer. Must be installed before the first
	/// enqueue; invoked from the worker thread. The commit (rename over the
	/// authoritative file) always stays with the service.
	using WriteTmpFn = std::function<worldsave::SaveStatus(
		const std::filesystem::path &chunksDir, int32_t chunkX, int32_t chunkZ,
		const std::vector<worldsave::ChunkEdit> &overrides, std::filesystem::path &outTmpPath)>;

	/// Test seam: replaces the "regenerate deterministic base + diff"
	/// serialize step (a full chunk generation per request - far too slow
	/// for queue-behavior tests). Null = real serialize. Invoked from the
	/// worker thread before the write step.
	using SerializeFn = std::function<std::vector<worldsave::ChunkEdit>(
		int32_t chunkX, int32_t chunkZ, const std::vector<worldsave::ChunkEdit> &currentValues)>;

	/// Test seam: invoked from the worker IMMEDIATELY BEFORE the
	/// authoritative-commit critical section (rename over the final file, or
	/// authoritative delete) and WITHOUT holding the service mutex, so a
	/// test can block the worker there while another thread enqueues a newer
	/// revision for the same coordinate. Covers the exact
	/// stale-check/commit window (issue #180 review round 2).
	using BeforeCommitFn = std::function<void(int32_t chunkX, int32_t chunkZ, uint64_t revision)>;

	struct Stats
	{
		uint64_t enqueued{0};
		uint64_t completed{0};  // written chunk files
		uint64_t superseded{0}; // dropped without I/O (a newer revision owns the coordinate)
		uint64_t failed{0};
		uint64_t deleted{0}; // chunk files removed (all overrides reverted)
		uint64_t bytesWritten{0};
		size_t queueDepth{0}; // DISTINCT coordinates pending (coalesced)
		// Serialize = deterministic base regeneration + diff; write = the
		// chunk file I/O. Averages are over processed (completed+deleted).
		double avgSerializeMs{0.0};
		double maxSerializeMs{0.0};
		double avgWriteMs{0.0};
		double maxWriteMs{0.0};
		std::string lastError;
	};

	// `seed` is the world seed: the worker regenerates each chunk's
	// deterministic base through TerrainGenerator::getThreadLocal(seed).
	SaveService(std::filesystem::path chunksDir, int seed);
	~SaveService();
	SaveService(const SaveService &) = delete;
	SaveService &operator=(const SaveService &) = delete;

	void setCompletionCallback(CompletionCallback callback);
	void setWriteTmpFnForTests(WriteTmpFn fn);
	void setSerializeFnForTests(SerializeFn fn);
	void setBeforeCommitFnForTests(BeforeCommitFn fn);

	/// Takes ownership of the request. Coalescing is per COORDINATE: if a
	/// request for the same chunk is already queued, its payload and revision
	/// are replaced in place (queue position preserved), so at most one
	/// pending job exists per chunk and the bound counts distinct chunks.
	/// Never blocks: when the distinct-coordinate bound is reached the
	/// request is rejected (returns false) and the caller keeps the state
	/// dirty for retry on a later capture/flush. Also returns false when the
	/// service is shutting down or revision is 0.
	bool enqueue(ChunkSaveRequest &&request);

	/// Waits until every request enqueued before this call has been processed
	/// (written, deleted or superseded). Returns false when any of those
	/// requests failed since the previous flush observation - failures are a
	/// monotone counter and the checkpoint only advances when a flush has
	/// observed it, so this return value describes exactly the range covered
	/// by THIS barrier. Returns false immediately when the service is already
	/// shut down.
	bool flush();

	/// Discards pending work, stops and joins the worker. Idempotent.
	/// enqueue() returns false afterwards. Unlike the destructor this does
	/// NOT wait for pending I/O - call flush() first for durability.
	void shutdown();

	Stats stats() const;

private:
	struct Job
	{
		ChunkSaveRequest request;
	};

	void workerLoop(std::stop_token stop);
	void processJob(Job &job);
	static uint64_t coordKey(int32_t chunkX, int32_t chunkZ);
	/// Marks an accepted ticket as no longer pending (completed, failed,
	/// superseded, or replaced in the queue) and wakes flush() waiters.
	/// Caller holds m_mutex.
	void resolveTicketLocked(uint64_t ticket);

	std::filesystem::path m_chunksDir;
	int m_seed{0};
	CompletionCallback m_completion; // guarded by m_mutex
	WriteTmpFn m_writeTmp;           // guarded by m_mutex (test seam)
	SerializeFn m_serialize;         // guarded by m_mutex (test seam)
	BeforeCommitFn m_beforeCommit;   // guarded by m_mutex (test seam)

	mutable std::mutex m_mutex;
	std::condition_variable m_queueCv; // worker wakes on enqueue / shutdown
	std::condition_variable m_doneCv;  // flush() waits for revision progress
	// coord -> latest pending payload (coalescing map) + FIFO of coords.
	// A coordinate occupies at most ONE queue slot: a newer capture replaces
	// the payload in place instead of enqueueing a second entry.
	std::unordered_map<uint64_t, ChunkSaveRequest> m_pendingByCoord;
	std::deque<uint64_t> m_order;
	// coord -> newest revision ever ENQUEUED (never erased while running).
	// A job whose revision no longer matches at I/O time has been superseded
	// in flight and must not touch the authoritative file.
	std::unordered_map<uint64_t, uint64_t> m_latestRevision;
	static constexpr size_t kMaxPendingCoords = 1024;

	// Barrier tickets (acceptance order, independent of content revisions).
	uint64_t m_nextBarrierTicket{1};
	uint64_t m_lastAcceptedTicket{0};
	// Ordered set of accepted-but-unresolved tickets. flush() waits until no
	// ticket <= its sampled target remains; the ordered begin() makes that
	// check O(1) instead of a scan.
	std::set<uint64_t> m_unfinishedTickets;

	// Failures are a monotone counter; flush() compares against the checkpoint
	// of the last OBSERVED value so a failure that completed after the
	// previous flush but before this one is reported by THIS flush
	// (issue #180 review round 2).
	uint64_t m_failureCount{0};
	uint64_t m_failureCheckpoint{0};
	bool m_shuttingDown{false};

	// Counters / timings, all guarded by m_mutex.
	uint64_t m_enqueued{0};
	uint64_t m_completed{0};
	uint64_t m_superseded{0};
	uint64_t m_failed{0};
	uint64_t m_deleted{0};
	uint64_t m_bytesWritten{0};
	double m_serializeTotalMs{0.0};
	double m_serializeMaxMs{0.0};
	double m_writeTotalMs{0.0};
	double m_writeMaxMs{0.0};
	std::string m_lastError;

	std::jthread m_worker;
};

/// Feet position + view + movement mode + selection captured at shutdown and
/// restored on open (issue #180, Phase 5). Plain value: no engine types, so
/// the save layer stays Chunk/Engine-free.
struct PlayerPersistState
{
	double x{0.0}, y{0.0}, z{0.0}; // feet position, world coords
	float yaw{0.f}, pitch{0.f};    // degrees, camera convention
	bool flight{false};            // movement mode
	int32_t selectedBlock{0};      // selected TextureType ordinal
};

/// World-save facade: world directory + identity (world.meta) + per-chunk
/// desired/durable persistence state + the async SaveService. Chunk-free: no
/// Chunk/ChunkManager dependency, so it is unit-testable headlessly.
class WorldPersistence
{
public:
	struct OpenInfo
	{
		bool ok{false};
		bool created{false};
		int seed{0};
		uint32_t storedFormatVersion{0};
		std::string error;
	};

	struct Status
	{
		std::string name;
		int seed{0};
		size_t queueDepth{0};
		// Number of coordinates tracked by the desired/durable state machine.
		uint64_t pendingCaptureEntries{0};
		// Coordinates whose desired content is not yet durable (queued,
		// in flight or failed) - the "dirty save" set the UI can show.
		uint64_t dirtyCoordinates{0};
		uint64_t enqueued{0};
		uint64_t completed{0};
		uint64_t superseded{0};
		uint64_t failed{0};
		uint64_t deleted{0};
		uint64_t bytesWritten{0};
		std::string lastError;
	};

	/// Filesystem-safe world name: non-empty, no path separators, no
	/// reserved Windows characters, no ".." traversal. The persistence layer
	/// enforces this on every entry point so callers cannot be tricked into
	/// escaping the saves root (issue #180 review).
	static bool isValidWorldName(std::string_view name);

	WorldPersistence() = default;
	~WorldPersistence();
	WorldPersistence(const WorldPersistence &) = delete;
	WorldPersistence &operator=(const WorldPersistence &) = delete;

	/// True when `<savesRoot>/<name>/world.meta` exists.
	static bool worldExists(const std::filesystem::path &savesRoot, const std::string &name);

	/// Read-only probe of the stored seed (UI confirm dialogs). On failure
	/// returns false and fills `error`.
	static bool peekStoredSeed(const std::filesystem::path &savesRoot, const std::string &name,
	                           int &outSeed, std::string &error); // read-only meta probe

	/// Opens (or creates) `<savesRoot>/<name>/`. Layout:
	/// `<root>/<name>/world.meta` + `<root>/<name>/chunks/<cx>_<cz>.chunk`
	/// + `<root>/<name>/player.state` (written on shutdown, Phase 5).
	///
	/// Identity rules: an existing world.meta is validated - bad magic /
	/// unsupported format version are rejected, a stored generatorVersion
	/// different from `currentGeneratorVersion` is rejected (no migration in
	/// v1), and a stored seed different from `currentSeed` is rejected with a
	/// "seed mismatch" error (opening NEVER overwrites identity). A missing
	/// meta creates the directories and writes a fresh meta. Afterwards,
	/// leftover ".tmp" writes are cleaned (never authoritative) and every
	/// chunk file is loaded as the DURABLE baseline of the per-coordinate
	/// state machine. A file that fails to read (Corrupt / CoordinateMismatch
	/// / ...) is recorded in the status error log and SKIPPED - documented
	/// policy: that chunk falls back to procedural generation, loudly
	/// reported. A chunk-directory scan error (permission denied, dir is a
	/// file, ...) REFUSES the open instead: an I/O error must never be
	/// misread as "no overrides saved".
	OpenInfo openOrCreate(const std::filesystem::path &savesRoot, const std::string &name,
	                      int currentSeed, uint32_t currentGeneratorVersion);

	bool enabled() const { return m_enabled; }
	int seed() const { return m_seed; }
	const std::string &name() const { return m_name; }

	/// Mutex-guarded state queries. A coordinate "has overrides" when the
	/// content a reload would apply (desired if captured this session, else
	/// the durable baseline) is non-empty.
	bool hasOverrides(int32_t chunkX, int32_t chunkZ) const;
	/// Race-free COPY of the content a fresh generation must re-apply:
	/// the session's desired payload when one exists, else the durable
	/// baseline loaded from disk. Safe to consume from a worker while the
	/// state machine mutates.
	std::vector<worldsave::ChunkEdit> overridesSnapshot(int32_t chunkX, int32_t chunkZ) const;

	/// Capture the current edited-voxel values for one chunk (main thread;
	/// values are final/authoritative and carry one entry per edited voxel,
	/// duplicates collapsing last-write-wins). Stamps a new desiredRevision
	/// and (re)enqueues unless `desired == durable && !failed && !pending`.
	/// Never blocks: when the service queue is full the state stays dirty
	/// (desiredRevision > durableRevision) and is retried by the next
	/// capture or flush. If the service is not running the capture is NOT
	/// silently dropped: the error log records it.
	void captureChunkEdits(int32_t chunkX, int32_t chunkZ,
	                       std::vector<worldsave::ChunkEdit> currentValues);

	/// Retry every dirty coordinate (desired not durable, not pending), then
	/// barrier-wait for the service. Returns false when any request covered
	/// by the barrier failed. This is THE retry point for failed/busy writes.
	bool flush();

	/// Flush, then shut the service down. Idempotent; afterwards
	/// captureChunkEdits() is a no-op that reports through the error log.
	void shutdown();

	/// Atomically write `<world>/player.state` (tmp + rename, magic "FTVP",
	/// format in WorldSave.hpp). False when no world is open or the write
	/// fails; failures are appended to status().lastError.
	bool writePlayerState(const PlayerPersistState &state) const;

	/// Read `<world>/player.state`. A missing file is the normal fresh-world
	/// case: returns false WITHOUT recording an error. A truncated/corrupt
	/// file, unsupported version, or an out-of-range payload (non-finite
	/// position/orientation, y outside [-64, CHUNK_HEIGHT + 64]) returns
	/// false and appends the reason to status().lastError.
	bool readPlayerState(PlayerPersistState &out) const;

	Status status() const;

	/// Test seam passthrough to the owned SaveService (write failures and
	/// supersede windows must be deterministic in tests). No-op when no
	/// world is open.
	void setWriteTmpFnForTests(SaveService::WriteTmpFn fn);
	void setSerializeFnForTests(SaveService::SerializeFn fn);
	void setBeforeCommitFnForTests(SaveService::BeforeCommitFn fn);

private:
	/// Per-coordinate desired/durable state (issue #180 review): `desired`
	/// is what the game last captured; `durable` is what is confirmed on
	/// disk. Revisions come from WorldPersistence's monotonic capture
	/// counter; durableRevision == 0 means "loaded from disk at open, never
	/// rewritten this session". desiredRevision == 0 means "no capture this
	/// session, the durable baseline is what a reload applies".
	struct ChunkPersistenceState
	{
		int32_t chunkX{0};
		int32_t chunkZ{0};
		std::vector<worldsave::ChunkEdit> desired;
		uint64_t desiredRevision{0};

		std::vector<worldsave::ChunkEdit> durable;
		uint64_t durableRevision{0};

		bool pending{false}; // a request for the current desired is queued/in flight
		bool failed{false};  // the last attempt for the current desired failed

		/// True when the durable copy does not yet reflect desired (pending
		/// work, failed attempt, or revisions ahead with differing content).
		/// Content-equal-but-revision-behind is NOT dirty: the file on disk
		/// already carries exactly these overrides.
		bool dirty() const;
	};

	using StateMap = std::unordered_map<uint64_t, ChunkPersistenceState>;

	static uint64_t coordKey(int32_t chunkX, int32_t chunkZ);
	void recordError(const std::string &error) const;
	void recordErrorLocked(const std::string &error) const; // m_indexMutex held
	/// The content a fresh generation must re-apply. Caller holds m_indexMutex.
	const std::vector<worldsave::ChunkEdit> &applicableLocked(const ChunkPersistenceState &state) const;

	mutable std::mutex m_indexMutex;
	StateMap m_states;              // guarded by m_indexMutex
	mutable std::string m_errorLog; // guarded by m_indexMutex, newline-appended
	uint64_t m_nextCaptureRevision{1}; // guarded by m_indexMutex
	std::filesystem::path m_worldDir; // <savesRoot>/<name>, empty until open
	std::filesystem::path m_chunksDir;
	std::string m_name;
	int m_seed{0};
	bool m_enabled{false};
	std::unique_ptr<SaveService> m_service;
};
