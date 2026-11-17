#pragma once

// =============================================================================
// WorldCheckpoint — generation-based gameplay persistence checkpoints
// (stabilization Phase 3; see docs/adr/gameplay-persistence-transactions.md).
//
// The issue #180 per-file atomic writes and the SaveService desired/durable
// state machine stay in force. This layer adds a MULTI-FILE transactional
// commit on top: an immutable generation directory (gameplay payloads +
// byte copies of the live chunk override files) published by a single atomic
// manifest rename. Recovery selects the most recent fully valid generation;
// a crash never yields a mixture of generations.
//
// Layout (under <world>/checkpoints/):
//   manifest            the commit point (atomic tmp+rename+sync)
//   manifest.prev       rotated previous manifest (fault tolerance)
//   generation-<N>/     immutable once committed
//     player.state      (optional — presence flagged in the manifest)
//     inventory.state
//     items.state
//     chunks/<cx>_<cz>.chunk
//
// Contract:
//   * commit() runs on the MAIN thread AFTER the caller's flush barrier
//     (the live working set is durable and the SaveService queue drained);
//     it never holds the world simulation lock and never mutates the live
//     working set or any committed generation.
//   * The previous committed checkpoint is untouched until the new manifest
//     rename succeeds. Any failure before that point is retryable and leaves
//     an orphaned staging directory that recovery ignores and cleanup removes.
//   * recover() validates a generation AS A WHOLE (payload codecs + every
//     referenced chunk checksum + no unreferenced chunk files). It never
//     combines generations. Unrecoverable corruption is reported explicitly
//     (the caller refuses the open) — never silently mixed with live data.
//   * Process-crash consistency is guaranteed by atomic renames; power-loss
//     durability is best-effort (file+directory sync on POSIX, file sync on
//     Windows — see the ADR §2.9).
//
// Dependency rule: CPU-only format/service layer (std + WorldSave + glm-free).
// No Chunk/Engine/Vulkan/SDL dependency — unit-testable headlessly.
// =============================================================================

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <World/WorldSave.hpp>

namespace worldsave
{
	inline constexpr char kCheckpointManifestMagic[4] = {'F', 'T', 'V', 'M'};
	inline constexpr uint32_t kCheckpointManifestVersion = 1;

	/// One referenced chunk override file: coordinate + FNV-1a 64 of the
	/// whole file bytes (the chunk codec additionally validates magic,
	/// version, its internal checksum, exact size and the coordinate).
	struct CheckpointChunkRef
	{
		int32_t chunkX{0};
		int32_t chunkZ{0};
		uint64_t checksum{0};
	};

	/// The commit manifest: a consistent collection of payload references.
	struct CheckpointManifest
	{
		uint64_t committedGeneration{0};
		uint64_t fallbackGeneration{0}; // retained previous generation (0 = none)
		uint64_t worldEditRevision{0};  // diagnostic: nextEditId at the boundary
		bool playerPresent{false};
		uint64_t playerChecksum{0}; // FNV-1a 64 of player.state bytes (0 when absent)
		uint64_t inventoryChecksum{0};
		uint64_t itemsChecksum{0};
		std::vector<CheckpointChunkRef> chunks; // sorted by (x, z)
	};

	std::vector<uint8_t> encodeCheckpointManifest(const CheckpointManifest &manifest);
	SaveStatus decodeCheckpointManifest(const uint8_t *data, size_t size, CheckpointManifest &out);

	/// Gameplay payloads for one commit, already ENCODED in the sibling
	/// formats (the caller captures + encodes at the snapshot boundary).
	/// playerPresent == false: the store carries the previous player payload
	/// forward from the current recovery source (active generation, else the
	/// legacy root file) — the shutdown/close seams have no host orientation.
	struct CheckpointPayloads
	{
		bool playerPresent{false};
		std::vector<uint8_t> player;    // encoded player.state (when present)
		std::vector<uint8_t> inventory; // encoded inventory.state (required)
		std::vector<uint8_t> items;     // encoded items.state (required)
		uint64_t worldEditRevision{0};
	};

	/// Deterministic fault-injection points (the SaveService test-seam
	/// pattern). A fault makes commit() fail at exactly that step; recovery
	/// must then yield the previous complete generation (points before
	/// publication) or the new one (points after publication).
	enum class CheckpointFaultPoint : uint8_t
	{
		None,
		BeforeStaging,            // 1
		AfterPlayerStaged,        // 2
		AfterInventoryStaged,     // 3
		AfterItemsStaged,         // 4
		DuringChunkCopy,          // 5
		AfterStagingBeforePublish,// 7 (6 is the SaveService write seam)
		DuringManifestPublish,    // 8 (tmp written, rename not done)
		AfterManifestPublish,     // 9 (commit succeeded)
		DuringCleanup             // 10 (commit succeeded)
	};

	class CheckpointStore
	{
	public:
		enum class RecoveryStatus
		{
			Legacy,        // no checkpoints: the legacy live working set rules
			Recovered,     // a fully valid committed generation was selected
			Unrecoverable  // checkpoints exist but none validate: refuse the open
		};

		explicit CheckpointStore(std::filesystem::path worldDir);

		/// Select + validate the best committed generation (manifest ->
		/// manifest.prev; never combines generations). On Recovered,
		/// activeGeneration()/activeDir() name the winning generation and
		/// activePlayerPresent() carries its player flag. Legacy leaves the
		/// store inactive. Unrecoverable fills outError.
		RecoveryStatus recover(std::string &outError);

		/// Commit a new generation from the LIVE chunks directory + payloads.
		/// Precondition: the caller flushed the live working set (barrier).
		/// Returns false on any failure BEFORE the manifest rename (retryable;
		/// the previous checkpoint stays authoritative). Points 9/10 faults
		/// fail AFTER publication: the commit IS durable and recovery uses it.
		bool commit(const std::filesystem::path &liveChunksDir, const CheckpointPayloads &payloads,
					std::string &outError);

		/// Best-effort removal of orphaned staging + generations older than
		/// the retained pair (active + fallback). Never invalidates committed
		/// state; failures are reported for the next retry.
		void cleanup(std::string &outError);

		uint64_t activeGeneration() const { return m_activeGeneration; }
		bool activePlayerPresent() const { return m_activePlayerPresent; }
		/// The winning generation directory (empty when legacy/inactive).
		std::filesystem::path activeDir() const;
		const std::filesystem::path &worldDir() const { return m_worldDir; }

		/// Test seam: install BEFORE the commit under test (main thread).
		void setFaultForTests(CheckpointFaultPoint point) { m_fault = point; }

	private:
		bool validateGeneration(uint64_t generation, const CheckpointManifest &manifest,
								std::string &outError) const;
		bool tryRecoverManifest(const std::filesystem::path &manifestPath, uint64_t &outGeneration,
								std::string &outError);
		bool stagePayload(const std::filesystem::path &finalPath, const std::vector<uint8_t> &bytes,
						  std::string &outError) const;
		bool publishManifest(const CheckpointManifest &manifest, std::string &outError);
		bool failAt(CheckpointFaultPoint point, std::string &outError) const;

		std::filesystem::path m_worldDir;
		uint64_t m_activeGeneration{0};
		uint64_t m_fallbackGeneration{0};
		bool m_activePlayerPresent{false};
		uint64_t m_nextGeneration{1};
		CheckpointFaultPoint m_fault{CheckpointFaultPoint::None};
	};

	/// Directory/path helpers (shared by the store, the persistence facade
	/// and tests).
	std::filesystem::path checkpointsDir(const std::filesystem::path &worldDir);
	std::filesystem::path checkpointGenerationDir(const std::filesystem::path &worldDir, uint64_t generation);
	std::filesystem::path checkpointManifestPath(const std::filesystem::path &worldDir);
	std::filesystem::path checkpointManifestPrevPath(const std::filesystem::path &worldDir);

	/// Platform sync helpers (ADR §2.9): flush a file's data to stable
	/// storage; flush a directory's metadata (POSIX only — Windows directory
	/// handles are not reliably flushable, documented best-effort). Returns
	/// false on I/O failure; a missing directory sync on Windows returns true
	/// (the file sync already ran).
	bool syncFileForCheckpoint(const std::filesystem::path &path);
	bool syncDirForCheckpoint(const std::filesystem::path &path);
} // namespace worldsave
