#pragma once

// Persistent world-save format layer (issue #180, Phase 1). Pure std-only
// format code: no Chunk/ChunkManager/Engine/Vulkan dependency, no exceptions
// across the API (every read/write returns a SaveStatus).
//
// ---------------------------------------------------------------------------
// Format specification
// ---------------------------------------------------------------------------
//
// All integers are LITTLE-ENDIAN, fields are tightly packed (no alignment
// padding), and every field goes through the explicit byte-width helpers in
// WorldSave.cpp - there are no raw struct dumps.
//
//   world.meta                                  (16 bytes)
//     offset  0: magic "FTVW"                   4 bytes
//     offset  4: formatVersion       u32        4 bytes
//     offset  8: seed                i32        4 bytes
//     offset 12: generatorVersion    u32        4 bytes
//
//   <cx>_<cz>.chunk                             (28 + 5*editCount bytes)
//     offset  0: magic "FTVC"                   4 bytes
//     offset  4: formatVersion       u32        4 bytes
//     offset  8: chunkX              i32        4 bytes
//     offset 12: chunkZ              i32        4 bytes
//     offset 16: editCount           u32        4 bytes
//     offset 20: checksum            u64        8 bytes
//     offset 28: records, editCount times 5 bytes:
//                  localIndex u32 | blockType u8
//
//   player.state                                (45 bytes)
//     offset  0: magic "FTVP"                   4 bytes
//     offset  4: formatVersion       u32        4 bytes
//     offset  8: x                   f64        8 bytes  (feet, world coords)
//     offset 16: y                   f64        8 bytes
//     offset 24: z                   f64        8 bytes
//     offset 32: yaw                 f32        4 bytes  (degrees)
//     offset 36: pitch               f32        4 bytes  (degrees)
//     offset 40: flight              u8         1 byte
//     offset 41: selectedBlock       i32        4 bytes  (TextureType ordinal)
//
//     Written/read by WorldPersistence::writePlayerState/readPlayerState via
//     the exposed ByteWriter/ByteReader helpers below; missing file means
//     "fresh world" (quiet false), anything malformed is a loud error.
//
// Index convention: `localIndex` is the canonical chunk-local voxel index
//   localIndex = y*256 + z*16 + x   (y-major; matches Chunk::getIndex)
// and `ChunkEdit::blockType` is the FINAL authoritative TextureType value
// for that voxel - an override table, not an operation log.
//
// Checksum: FNV-1a 64 (fnv1a64) over the raw record bytes only, i.e. the
// 5*editCount bytes starting at offset 28. The header itself is protected by
// the strict exact-size and coordinate validation on read.
//
// Chunk record order: callers may pass edits in ANY order; writeChunkFile()
// sorts a local copy into ascending localIndex before writing, and
// readChunkFile() tolerates any stored order and returns records sorted
// ascending.
//
// Atomic-write guarantee: every writer first writes "<name>.tmp" and then
// std::filesystem::rename()s it over the final name. POSIX rename() is
// atomic, and both MSVC and libstdc++ pass MOVEFILE_REPLACE_EXISTING on
// Windows, so an existing target file is replaced. A crash therefore leaves
// either the previous file or the complete new one - never a half-written
// mixture. ".tmp" files are NEVER authoritative: readChunkFile() /
// scanChunkFiles() ignore them, and cleanTempFiles() deletes leftovers from
// interrupted writes.
//
// writers create the target directory when missing; an empty edit list
// means "all overrides reverted" and removes the chunk file instead of
// writing an empty payload.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <utils.hpp>

namespace worldsave
{
	inline constexpr char kWorldMetaMagic[4] = {'F', 'T', 'V', 'W'};
	inline constexpr char kChunkFileMagic[4] = {'F', 'T', 'V', 'C'};
	inline constexpr char kPlayerStateMagic[4] = {'F', 'T', 'V', 'P'};
	inline constexpr uint32_t kWorldSaveFormatVersion = 1;
	inline constexpr uint32_t kPlayerStateFormatVersion = 1;

	struct WorldMeta
	{
		uint32_t formatVersion{0};
		int seed{0};
		uint32_t generatorVersion{0};
	};

	// localIndex uses the canonical y-major chunk-local voxel index
	// (y*256 + z*16 + x); blockType is the final authoritative TextureType
	// value for that voxel, not an operation log.
	struct ChunkEdit
	{
		uint32_t localIndex{0};
		uint8_t blockType{0};
	};

	enum class SaveStatus
	{
		Ok,
		NotFound,
		BadMagic,
		UnsupportedVersion,
		Corrupt,
		CoordinateMismatch,
		IoError
	};

	// FNV-1a 64-bit over `size` raw bytes (exposed for tests / diagnostics).
	uint64_t fnv1a64(const void *data, size_t size);

	// ------------------------------------------------------------------
	// Little-endian byte helpers, exposed so sibling formats (player.state)
	// share the exact same discipline as the chunk/meta writers - no raw
	// struct dumps anywhere (padding and host endianness would leak into
	// files). The reader is bounds-checked, latches the first error and
	// yields zeros for every read after it.
	// ------------------------------------------------------------------
	class ByteWriter
	{
	public:
		void u8(uint8_t v) { m_bytes.push_back(v); }
		void u16(uint16_t v)
		{
			u8(static_cast<uint8_t>(v));
			u8(static_cast<uint8_t>(v >> 8));
		}
		void u32(uint32_t v)
		{
			u16(static_cast<uint16_t>(v));
			u16(static_cast<uint16_t>(v >> 16));
		}
		void u64(uint64_t v)
		{
			u32(static_cast<uint32_t>(v));
			u32(static_cast<uint32_t>(v >> 32));
		}
		void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
		void f32(float v)
		{
			uint32_t bits = 0;
			static_assert(sizeof(bits) == sizeof(v), "unexpected float size");
			std::memcpy(&bits, &v, sizeof(bits));
			u32(bits);
		}
		void f64(double v)
		{
			uint64_t bits = 0;
			static_assert(sizeof(bits) == sizeof(v), "unexpected double size");
			std::memcpy(&bits, &v, sizeof(bits));
			u64(bits);
		}
		void raw(const void *data, size_t size)
		{
			const uint8_t *p = static_cast<const uint8_t *>(data);
			m_bytes.insert(m_bytes.end(), p, p + size);
		}
		std::vector<uint8_t> take() { return std::move(m_bytes); }

	private:
		std::vector<uint8_t> m_bytes;
	};

	class ByteReader
	{
	public:
		ByteReader(const uint8_t *data, size_t size) : m_data(data), m_size(size) {}

		bool failed() const { return m_failed; }

		uint8_t u8()
		{
			if (m_failed || m_pos + 1 > m_size)
			{
				m_failed = true;
				return 0;
			}
			return m_data[m_pos++];
		}
		uint16_t u16()
		{
			const uint16_t lo = u8();
			const uint16_t hi = u8();
			return static_cast<uint16_t>(lo | (hi << 8));
		}
		uint32_t u32()
		{
			const uint32_t lo = u16();
			const uint32_t hi = u16();
			return lo | (hi << 16);
		}
		uint64_t u64()
		{
			const uint64_t lo = u32();
			const uint64_t hi = u32();
			return lo | (hi << 32);
		}
		int32_t i32() { return static_cast<int32_t>(u32()); }
		float f32()
		{
			const uint32_t bits = u32();
			float v = 0.0f;
			if (!m_failed)
				std::memcpy(&v, &bits, sizeof(v));
			return v;
		}
		double f64()
		{
			const uint64_t bits = u64();
			double v = 0.0;
			if (!m_failed)
				std::memcpy(&v, &bits, sizeof(v));
			return v;
		}
		void raw(void *dst, size_t size)
		{
			if (m_failed || m_pos + size > m_size || m_pos + size < m_pos)
			{
				m_failed = true;
				std::memset(dst, 0, size);
				return;
			}
			std::memcpy(dst, m_data + m_pos, size);
			m_pos += size;
		}

	private:
		const uint8_t *m_data;
		size_t m_size;
		size_t m_pos{0};
		bool m_failed{false};
	};

	// Writes `bytes` to finalPath.tmp, then renames over finalPath (atomic
	// replace; see the atomic-write guarantee above). False on any I/O error.
	bool writeFileAtomic(const std::filesystem::path &finalPath, const std::vector<uint8_t> &bytes);

	// Reads a whole regular file. NotFound classifies the absent path (the
	// common "no save data yet" case); everything else is Ok or IoError.
	SaveStatus readFileInto(const std::filesystem::path &path, std::vector<uint8_t> &out);


	SaveStatus writeWorldMeta(const std::filesystem::path &metaPath, const WorldMeta &meta);
	// On UnsupportedVersion the parsed outMeta.formatVersion is still
	// recorded so callers can report the encountered number.
	SaveStatus readWorldMeta(const std::filesystem::path &metaPath, WorldMeta &outMeta);

	// "<x>_<z>.chunk" (decimal, negative coordinates fine) and its strict
	// inverse - used when scanning a chunks directory; rejects anything that
	// is not exactly two int32 decimals plus the .chunk extension.
	std::string chunkFileName(int32_t chunkX, int32_t chunkZ);
	bool parseChunkFileName(const std::string &fileName, int32_t &chunkX, int32_t &chunkZ);

	// Path of the transient "<x>_<z>.chunk.tmp" sidecar (never authoritative).
	std::filesystem::path chunkTmpPath(const std::filesystem::path &chunksDir, int32_t chunkX, int32_t chunkZ);

	// Writes atomically (tmp + rename). An empty `edits` list implements
	// "all overrides reverted": the chunk file is removed and Ok is returned.
	// Refuses out-of-range localIndex values (>= CHUNK_VOLUME) with Corrupt.
	SaveStatus writeChunkFile(const std::filesystem::path &chunksDir, int32_t chunkX, int32_t chunkZ,
	                          const std::vector<ChunkEdit> &edits);

	// Two-phase variant of writeChunkFile for save workers that must re-check
	// a revision between writing and replacing the authoritative file
	// (issue #180 review): writeChunkFileTmp encodes and writes ONLY the
	// transient .tmp sidecar (final file untouched, outTmpPath carries its
	// path on Ok); commitChunkFile then atomically renames it over the final
	// name. Same empty-edits/out-of-range rules as writeChunkFile, except an
	// empty list still resolves to "remove final file" at the COMMIT step.
	SaveStatus writeChunkFileTmp(const std::filesystem::path &chunksDir, int32_t chunkX,
	                             int32_t chunkZ, const std::vector<ChunkEdit> &edits,
	                             std::filesystem::path &outTmpPath);
	SaveStatus commitChunkFile(const std::filesystem::path &tmpPath);

	// Validates magic, version, checksum, exact size, localIndex < CHUNK_VOLUME
	// and the header coordinates (CoordinateMismatch otherwise). Tolerates any
	// record order; returns records sorted ascending by localIndex.
	SaveStatus readChunkFile(const std::filesystem::path &filePath, int32_t expectedChunkX, int32_t expectedChunkZ,
	                         std::vector<ChunkEdit> &outEdits);

	SaveStatus removeChunkFile(const std::filesystem::path &chunksDir, int32_t chunkX, int32_t chunkZ);

	// Regular files matching "<x>_<z>.chunk" (parseChunkFileName-strict);
	// everything else - including ".tmp" sidecars - is ignored. Empty when
	// the directory does not exist. Deterministic (lexicographic) order.
	std::vector<std::filesystem::path> scanChunkFiles(const std::filesystem::path &chunksDir);

	// Removes "*.chunk.tmp" leftovers from interrupted writes. Ok when the
	// directory is missing or nothing needed removing.
	SaveStatus cleanTempFiles(const std::filesystem::path &chunksDir);

	// Pure diff used by the future save worker and tests: from
	// `currentValues` (the CURRENT value per edited voxel, not operations -
	// callers must have collapsed repeated writes), keep exactly the entries
	// whose value differs from baseVoxels[localIndex]. Output is ascending by
	// localIndex. Out-of-range indices are skipped.
	std::vector<ChunkEdit> diffEditsAgainstBase(const uint8_t *baseVoxels, const std::vector<ChunkEdit> &currentValues);
} // namespace worldsave
