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
//     identity, the in-memory override index (what a freshly generated chunk
//     must have re-applied), and the SaveService.
//
// Threading contract:
//   - SaveService owns ONE dedicated std::jthread (disk I/O must never run on
//     the shared gen/mesh ThreadPool - issue #180 explicitly requires disk
//     stalls to be unable to starve worldgen).
//   - Requests are superseded per coordinate by a monotonic internal
//     revision: the LAST request enqueued for a coordinate wins, every older
//     pending request is dropped without I/O.
//   - The completion callback (used by WorldPersistence to refine its index
//     to the true minimal diff) is invoked from the worker thread.
//   - WorldPersistence's index is mutex-guarded; captureChunkEdits() and the
//     completion callback are the only writers.
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
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <World/WorldSave.hpp>

// One queued save unit. `currentValues` holds the CURRENT value of each
// edited voxel (an override superset, NOT a diff and NOT an operation log);
// the worker diffs it against the regenerated deterministic base.
struct ChunkSaveRequest
{
	int32_t chunkX{0};
	int32_t chunkZ{0};
	std::vector<worldsave::ChunkEdit> currentValues;
};

/// Dedicated-thread async chunk save worker.
class SaveService
{
public:
	/// Invoked from the worker thread after a SUCCESSFUL write (with the
	/// minimal persisted override set) or delete (empty vector). Must not
	/// call back into the service. Installed before the first enqueue.
	using CompletionCallback =
		std::function<void(int32_t chunkX, int32_t chunkZ, const std::vector<worldsave::ChunkEdit> &finalOverrides)>;

	struct Stats
	{
		uint64_t enqueued{0};
		uint64_t completed{0};  // written chunk files
		uint64_t superseded{0}; // dropped at pop (a newer revision exists)
		uint64_t failed{0};
		uint64_t deleted{0}; // chunk files removed (all overrides reverted)
		uint64_t bytesWritten{0};
		size_t queueDepth{0};
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

	/// Takes ownership of the request and assigns the monotonic revision.
	/// Supersedes any pending request for the same coordinate. Blocks on the
	/// internal condition_variable while the queue holds kMaxPendingRequests
	/// entries - the bound is only hit with >1024 DISTINCT modified chunks
	/// pending, and blocking is the safe back-pressure (the caller is the
	/// engine's capture path, never the render loop's critical section).
	/// Returns false when the service is shutting down (the request is
	/// rejected, never silently dropped: callers report it).
	bool enqueue(ChunkSaveRequest &&request);

	/// Waits until every request enqueued before this call has been processed
	/// (written, deleted or superseded). Returns !anyFailed for the requests
	/// covered by THIS flush (failure state is sampled at flush start).
	/// Returns false immediately when the service is already shut down.
	bool flush();

	/// Discards pending work, stops and joins the worker. Idempotent.enqueue()
	/// returns false afterwards. Unlike the destructor this does NOT wait for
	/// pending I/O - call flush() first for durability.
	void shutdown();

	Stats stats() const;

private:
	struct Job
	{
		ChunkSaveRequest request;
		uint64_t revision{0};
	};

	void workerLoop(std::stop_token stop);
	void processJob(Job &job);
	static uint64_t coordKey(int32_t chunkX, int32_t chunkZ);

	std::filesystem::path m_chunksDir;
	int m_seed{0};
	CompletionCallback m_completion; // guarded by m_mutex

	mutable std::mutex m_mutex;
	std::condition_variable m_queueCv; // worker wakes on enqueue / shutdown
	std::condition_variable m_doneCv;  // flush() waits for revision progress
	std::deque<Job> m_queue;
	// Coordinate -> revision of the newest ENQUEUED request. A popped request
	// whose revision no longer matches is dropped (superseded); the entry is
	// erased once the newest request for the coordinate finishes.
	std::unordered_map<uint64_t, uint64_t> m_latestRevision;
	static constexpr size_t kMaxPendingRequests = 1024;

	uint64_t m_nextRevision{1};        // assigned under m_mutex
	uint64_t m_lastFinishedRevision{0}; // every popped job (incl. superseded) advances it
	bool m_shuttingDown{false};
	bool m_anyFailed{false};

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

/// World-save facade: world directory + identity (world.meta) + in-memory
/// override index + the async SaveService. Chunk-free: no Chunk/ChunkManager
/// dependency, so it is unit-testable headlessly.
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
		// Number of in-memory override entries held (the superset view a
		// reload would apply). Same quantity captured by captureChunkEdits.
		uint64_t pendingCaptureEntries{0};
		uint64_t enqueued{0};
		uint64_t completed{0};
		uint64_t superseded{0};
		uint64_t failed{0};
		uint64_t deleted{0};
		uint64_t bytesWritten{0};
		std::string lastError;
	};

	WorldPersistence() = default;
	~WorldPersistence();
	WorldPersistence(const WorldPersistence &) = delete;
	WorldPersistence &operator=(const WorldPersistence &) = delete;

	/// True when `<savesRoot>/<name>/world.meta` exists.
	static bool worldExists(const std::filesystem::path &savesRoot, const std::string &name);

	/// Read-only probe of the stored seed (UI confirm dialogs). On failure
	/// returns false and fills `error`.
	static bool peekStoredSeed(const std::filesystem::path &savesRoot, const std::string &name,
	                           int &outSeed, std::string &error);

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
	/// chunk file is read into the in-memory override index. A file that
	/// fails to load (Corrupt / CoordinateMismatch / ...) is recorded in the
	/// status error log and its overrides are SKIPPED - documented policy:
	/// that chunk falls back to procedural generation, loudly reported.
	OpenInfo openOrCreate(const std::filesystem::path &savesRoot, const std::string &name,
	                      int currentSeed, uint32_t currentGeneratorVersion);

	bool enabled() const { return m_enabled; }
	int seed() const { return m_seed; }
	const std::string &name() const { return m_name; }

	/// Mutex-guarded index queries.
	bool hasOverrides(int32_t chunkX, int32_t chunkZ) const;
	/// Pointer valid until the next index mutation (capture / completion /
	/// reopen). Intended for main-thread use (tests, UI). Code that runs on a
	/// worker while the index may be refined should use overridesSnapshot().
	const std::vector<worldsave::ChunkEdit> *overridesFor(int32_t chunkX, int32_t chunkZ) const;
	/// Race-free copy for worker-side consumption (gen jobs applying overrides).
	std::vector<worldsave::ChunkEdit> overridesSnapshot(int32_t chunkX, int32_t chunkZ) const;

	/// Capture the current edited-voxel values for one chunk (main thread;
	/// values are final/authoritative and carry one entry per edited voxel).
	/// Stores the values as the in-memory override entry (a superset of the
	/// true diff - safe to apply, idempotent for voxels that match the base)
	/// and hands the payload to the SaveService, which computes and persists
	/// the minimal diff and refines the index via the completion callback.
	/// Empty payloads for a coordinate with no recorded overrides are
	/// no-ops; a payload identical to the already-recorded override entry is
	/// not re-enqueued (disk and index already agree). If the service is not
	/// running the capture is NOT silently dropped: the error log records it.
	void captureChunkEdits(int32_t chunkX, int32_t chunkZ,
	                       std::vector<worldsave::ChunkEdit> currentValues);

	/// Flush the save service (durability barrier). False when any request
	/// failed since the last flush.
	bool flush();

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

	/// Flush, then shut the service down. Idempotent; afterwards
	/// captureChunkEdits() is a no-op that reports through the error log.
	void shutdown();

	Status status() const;

private:
	using OverrideIndex = std::unordered_map<uint64_t, std::vector<worldsave::ChunkEdit>>;
	static uint64_t coordKey(int32_t chunkX, int32_t chunkZ);
	void recordError(const std::string &error) const;

	mutable std::mutex m_indexMutex;
	OverrideIndex m_overrides; // guarded by m_indexMutex
	mutable std::string m_errorLog; // guarded by m_indexMutex, newline-appended
	std::filesystem::path m_worldDir; // <savesRoot>/<name>, empty until open
	std::filesystem::path m_chunksDir;
	std::string m_name;
	int m_seed{0};
	bool m_enabled{false};
	std::unique_ptr<SaveService> m_service;
};
