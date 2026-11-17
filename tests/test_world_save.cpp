// Persistent world-save format layer (issue #180, Phases 1+2): world.meta /
// chunk-file round trips and error taxonomy, boundary + negative-coordinate
// coverage, sparse-diff semantics, empty-payload removal, corruption
// detection, atomic replace + ".tmp" hygiene, and on-disk format exactness.
// CPU-only: no Vulkan and no Chunk/ChunkManager dependency (the format layer
// is pure std code).
#include <World/WorldSave.hpp>
#include <Items/ItemEntitySystem.hpp>
#include <Items/ItemRegistry.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using worldsave::ChunkEdit;
using worldsave::SaveStatus;


#include "TestMain.hpp"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Canonical chunk-local voxel index (y-major): y*256 + z*16 + x.
static uint32_t voxelIndex(int x, int y, int z)
{
	return static_cast<uint32_t>(y * CHUNK_SIZE * CHUNK_SIZE + z * CHUNK_SIZE + x);
}

static bool writeRawBytes(const fs::path &path, const std::vector<uint8_t> &bytes)
{
	std::error_code ec;
	if (!path.parent_path().empty())
		fs::create_directories(path.parent_path(), ec);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out)
		return false;
	if (!bytes.empty())
		out.write(reinterpret_cast<const char *>(bytes.data()),
		          static_cast<std::streamsize>(bytes.size()));
	return out.good();
}

static std::vector<uint8_t> readRawBytes(const fs::path &path)
{
	std::ifstream in(path, std::ios::binary);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static void pushU32(std::vector<uint8_t> &b, uint32_t v)
{
	b.push_back(static_cast<uint8_t>(v));
	b.push_back(static_cast<uint8_t>(v >> 8));
	b.push_back(static_cast<uint8_t>(v >> 16));
	b.push_back(static_cast<uint8_t>(v >> 24));
}

static void patchU32(std::vector<uint8_t> &b, size_t off, uint32_t v)
{
	b[off] = static_cast<uint8_t>(v);
	b[off + 1] = static_cast<uint8_t>(v >> 8);
	b[off + 2] = static_cast<uint8_t>(v >> 16);
	b[off + 3] = static_cast<uint8_t>(v >> 24);
}

static uint32_t getU32(const std::vector<uint8_t> &b, size_t off)
{
	return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
	       (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

static uint64_t getU64(const std::vector<uint8_t> &b, size_t off)
{
	return static_cast<uint64_t>(getU32(b, off)) | (static_cast<uint64_t>(getU32(b, off + 4)) << 32);
}

static std::vector<ChunkEdit> sortedByIndex(std::vector<ChunkEdit> edits)
{
	std::sort(edits.begin(), edits.end(),
	          [](const ChunkEdit &a, const ChunkEdit &b) { return a.localIndex < b.localIndex; });
	return edits;
}

static bool editsEqual(const std::vector<ChunkEdit> &a, const std::vector<ChunkEdit> &b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (a[i].localIndex != b[i].localIndex || a[i].blockType != b[i].blockType)
			return false;
	return true;
}

// ---------------------------------------------------------------------------
// 0) Checksum primitive (FNV-1a 64 known-answer vectors).
// ---------------------------------------------------------------------------

static void testFnv1a64()
{
	CHECK(worldsave::fnv1a64("", 0) == 0xcbf29ce484222325ull, "FNV-1a of empty input == offset basis");
	CHECK(worldsave::fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull, "FNV-1a of \"a\" matches reference vector");
	CHECK(worldsave::fnv1a64("foobar", 6) == 0x85944171f73967e8ull, "FNV-1a of \"foobar\" matches reference vector");
}

// ---------------------------------------------------------------------------
// 1) world.meta round trip (multi seed / generator version) + error taxonomy.
// ---------------------------------------------------------------------------

static void testWorldMetaRoundTrip(const fs::path &dir)
{
	const fs::path meta = dir / "world.meta";
	const int seeds[] = {0, 1337, -42, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()};
	const uint32_t genVersions[] = {0u, 1u, 0xDEADBEEFu};
	for (int seed : seeds)
	{
		for (uint32_t genVersion : genVersions)
		{
			const worldsave::WorldMeta in{worldsave::kWorldSaveFormatVersion, seed, genVersion};
			CHECK(worldsave::writeWorldMeta(meta, in) == SaveStatus::Ok, "writeWorldMeta returns Ok");
			worldsave::WorldMeta out{};
			CHECK(worldsave::readWorldMeta(meta, out) == SaveStatus::Ok, "readWorldMeta returns Ok");
			CHECK(out.formatVersion == worldsave::kWorldSaveFormatVersion && out.seed == seed &&
			          out.generatorVersion == genVersion,
			      "world.meta round-trips formatVersion/seed/generatorVersion");
		}
	}
	std::error_code ec;
	CHECK(fs::file_size(meta, ec) == 16 && !ec, "world.meta is exactly 16 bytes");
}

static void testWorldMetaErrors(const fs::path &dir)
{
	worldsave::WorldMeta out{};

	CHECK(worldsave::readWorldMeta(dir / "does_not_exist.meta", out) == SaveStatus::NotFound,
	      "missing world.meta -> NotFound");

	// Wrong magic with valid length.
	const fs::path badMagic = dir / "bad_magic.meta";
	std::vector<uint8_t> bytes = {'X', 'Y', 'Z', 'W', 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
	CHECK(writeRawBytes(badMagic, bytes), "bad-magic fixture written");
	CHECK(worldsave::readWorldMeta(badMagic, out) == SaveStatus::BadMagic, "wrong magic -> BadMagic");

	// Unknown NEWER format version (magic valid); the encountered version
	// number must still be reported through outMeta.
	const fs::path newer = dir / "newer_version.meta";
	const uint32_t futureVersion = worldsave::kWorldSaveFormatVersion + 7;
	bytes = {'F', 'T', 'V', 'W'};
	pushU32(bytes, futureVersion);
	pushU32(bytes, static_cast<uint32_t>(1337));
	pushU32(bytes, 1);
	CHECK(writeRawBytes(newer, bytes), "newer-version fixture written");
	out = {};
	CHECK(worldsave::readWorldMeta(newer, out) == SaveStatus::UnsupportedVersion,
	      "newer formatVersion -> UnsupportedVersion");
	CHECK(out.formatVersion == futureVersion, "UnsupportedVersion still reports the encountered version");

	// OLDER format version: v1 has no migrations, so version 0 is rejected
	// exactly like a future one (issue #180 review).
	const fs::path older = dir / "older_version.meta";
	bytes = {'F', 'T', 'V', 'W'};
	pushU32(bytes, 0);
	pushU32(bytes, static_cast<uint32_t>(1337));
	pushU32(bytes, 1);
	CHECK(writeRawBytes(older, bytes), "older-version fixture written");
	out = {};
	CHECK(worldsave::readWorldMeta(older, out) == SaveStatus::UnsupportedVersion,
	      "version 0 -> UnsupportedVersion");

	// Truncated file (first 10 bytes of a valid one) and empty file.
	const fs::path truncated = dir / "truncated.meta";
	CHECK(writeRawBytes(truncated, std::vector<uint8_t>(bytes.begin(), bytes.begin() + 10)),
	      "truncated fixture written");
	CHECK(worldsave::readWorldMeta(truncated, out) == SaveStatus::Corrupt, "truncated world.meta -> Corrupt");
	const fs::path empty = dir / "empty.meta";
	CHECK(writeRawBytes(empty, {}), "empty fixture written");
	CHECK(worldsave::readWorldMeta(empty, out) == SaveStatus::Corrupt, "empty world.meta -> Corrupt");
}

// ---------------------------------------------------------------------------
// 2) File-name mapping and strict parsing (scanning depends on both).
// ---------------------------------------------------------------------------

static void testChunkFileNames()
{
	CHECK(worldsave::chunkFileName(-17, 4) == "-17_4.chunk", "chunkFileName formats negatives");
	CHECK(worldsave::chunkFileName(0, -1) == "0_-1.chunk", "chunkFileName formats (0,-1)");
	CHECK(worldsave::chunkFileName(12345, -12345) == "12345_-12345.chunk", "chunkFileName formats large magnitudes");

	int32_t cx = 99;
	int32_t cz = 99;
	CHECK(worldsave::parseChunkFileName("1_2.chunk", cx, cz) && cx == 1 && cz == 2, "parse accepts plain coords");
	CHECK(worldsave::parseChunkFileName("-17_4.chunk", cx, cz) && cx == -17 && cz == 4,
	      "parse accepts negative coords");
	CHECK(worldsave::parseChunkFileName("0_-1.chunk", cx, cz) && cx == 0 && cz == -1, "parse accepts (0,-1)");
	CHECK(!worldsave::parseChunkFileName("17.chunk", cx, cz), "missing separator rejected");
	CHECK(!worldsave::parseChunkFileName("1_2_3.chunk", cx, cz), "second separator rejected");
	CHECK(!worldsave::parseChunkFileName("1_2.chunk.tmp", cx, cz), ".tmp sidecar rejected");
	CHECK(!worldsave::parseChunkFileName("1_2.txt", cx, cz), "wrong extension rejected");
	CHECK(!worldsave::parseChunkFileName("_2.chunk", cx, cz), "empty x rejected");
	CHECK(!worldsave::parseChunkFileName("1_.chunk", cx, cz), "empty z rejected");
	CHECK(!worldsave::parseChunkFileName("x_y.chunk", cx, cz), "non-numeric rejected");
	CHECK(!worldsave::parseChunkFileName("1_+2.chunk", cx, cz), "explicit plus rejected");
	CHECK(!worldsave::parseChunkFileName(" 1_2.chunk", cx, cz), "leading space rejected");
	CHECK(!worldsave::parseChunkFileName("99999999999999_0.chunk", cx, cz), "x overflow rejected");
	CHECK(!worldsave::parseChunkFileName("0_99999999999999.chunk", cx, cz), "z overflow rejected");
	CHECK(!worldsave::parseChunkFileName(".chunk", cx, cz), "extension-only name rejected");
	CHECK(!worldsave::parseChunkFileName("", cx, cz), "empty name rejected");
}

// ---------------------------------------------------------------------------
// 3) Chunk-file round trips: boundary indices, negative coordinates, record
//    order independence, coordinate validation, directory scan.
// ---------------------------------------------------------------------------

static void testChunkRoundTripBoundaries(const fs::path &dir)
{
	const fs::path chunks = dir / "chunks";

	// Boundary coverage: chunk edges (x/z = 0/15), extreme Y (0/255),
	// 16-voxel section boundaries (y=16/240), and the very last index.
	const std::vector<ChunkEdit> boundaryEdits = {
		{voxelIndex(0, 0, 0), 1},     // min corner (x=0, z=0, y=0)
		{voxelIndex(15, 255, 15), 2}, // max corner == CHUNK_VOLUME-1
		{voxelIndex(0, 16, 15), 3},   // section boundary y=16, edges x=0/z=15
		{voxelIndex(15, 240, 0), 4},  // section boundary y=240, edges x=15/z=0
		{voxelIndex(8, 255, 8), 5},   // top section interior
		{voxelIndex(15, 0, 15), 6},   // x=15, z=15, y=0
		{voxelIndex(0, 255, 0), 7},   // x=0, z=0, y=255
	};
	CHECK(voxelIndex(15, 255, 15) == CHUNK_VOLUME - 1, "y-major index convention sanity");

	// Shuffled records must read back ascending with exact values.
	std::vector<ChunkEdit> shuffled = boundaryEdits;
	std::mt19937 rng(1337);
	std::shuffle(shuffled.begin(), shuffled.end(), rng);
	CHECK(worldsave::writeChunkFile(chunks, 0, 0, shuffled) == SaveStatus::Ok, "write chunk (0,0) shuffled");
	std::vector<ChunkEdit> read;
	CHECK(worldsave::readChunkFile(chunks / "0_0.chunk", 0, 0, read) == SaveStatus::Ok, "read chunk (0,0)");
	CHECK(editsEqual(read, sortedByIndex(boundaryEdits)), "record order independent, ascending + exact");

	// Multiple chunks, including negative coordinates.
	const std::pair<int32_t, int32_t> coords[] = {{-17, 4}, {0, -1}, {12345, -12345}, {-1, -1}, {7, 0}};
	for (const auto &c : coords)
	{
		const std::vector<ChunkEdit> edits = {
			{voxelIndex(3, 4, 5), 10},
			{voxelIndex(0, 0, 0), 11},
			{voxelIndex(15, 255, 15), 12},
			{voxelIndex(8, 128, 9), 13},
		};
		CHECK(worldsave::writeChunkFile(chunks, c.first, c.second, edits) == SaveStatus::Ok, "write chunk");
		std::vector<ChunkEdit> out;
		CHECK(worldsave::readChunkFile(chunks / worldsave::chunkFileName(c.first, c.second),
		                               c.first, c.second, out) == SaveStatus::Ok,
		      "read chunk with negative/large coords");
		CHECK(editsEqual(out, sortedByIndex(edits)), "chunk values round-trip");
	}

	// Header coordinates must match the expectation.
	CHECK(worldsave::readChunkFile(chunks / "-17_4.chunk", -16, 4, read) == SaveStatus::CoordinateMismatch,
	      "wrong expected chunkZ -> CoordinateMismatch");
	CHECK(worldsave::readChunkFile(chunks / "-17_4.chunk", -17, 5, read) == SaveStatus::CoordinateMismatch,
	      "wrong expected chunkX -> CoordinateMismatch");

	// scanChunkFiles lists exactly the written files (5 loop chunks + (0,0)).
	const worldsave::ScanResult scanned = worldsave::scanChunkFiles(chunks);
	CHECK(scanned.status == SaveStatus::Ok, "scan status Ok");
	CHECK(scanned.files.size() == 1 + sizeof(coords) / sizeof(coords[0]), "scan lists every chunk file once");
	for (const fs::path &p : scanned.files)
	{
		int32_t sx = 0;
		int32_t sz = 0;
		CHECK(worldsave::parseChunkFileName(p.filename().string(), sx, sz), "scanned names all parse strictly");
	}
}

// ---------------------------------------------------------------------------
// 4) Sparse semantics: diffEditsAgainstBase is a pure filter.
// ---------------------------------------------------------------------------

static void testDiffEditsAgainstBase()
{
	std::vector<uint8_t> base(CHUNK_VOLUME);
	for (size_t i = 0; i < base.size(); ++i)
		base[i] = static_cast<uint8_t>(i * 31 + 7);

	const uint32_t idxA = voxelIndex(0, 0, 0);
	const uint32_t idxB = voxelIndex(15, 255, 15);
	const uint32_t idxC = voxelIndex(0, 16, 8);

	// One differing edit -> exactly one override.
	std::vector<ChunkEdit> current = {{idxA, static_cast<uint8_t>(base[idxA] + 1)}};
	std::vector<ChunkEdit> diff = worldsave::diffEditsAgainstBase(base.data(), current);
	CHECK(diff.size() == 1 && diff[0].localIndex == idxA && diff[0].blockType == base[idxA] + 1,
	      "one edit -> one override");

	// Current value equal to base -> filtered out (override removed).
	current = {{idxA, base[idxA]}};
	diff = worldsave::diffEditsAgainstBase(base.data(), current);
	CHECK(diff.empty(), "restoring the base value removes the override");

	// Pure filter over an unsorted mixed input: only overrides survive,
	// output ascending, current values preserved.
	current = {
		{idxB, static_cast<uint8_t>(base[idxB] ^ 0xFF)}, // differs
		{idxA, base[idxA]},                              // same -> dropped
		{idxC, static_cast<uint8_t>(base[idxC] + 3)},    // differs
	};
	diff = worldsave::diffEditsAgainstBase(base.data(), current);
	CHECK(diff.size() == 2 && diff[0].localIndex == idxC && diff[1].localIndex == idxB,
	      "mixed diff keeps only overrides, ascending by index");
	CHECK(diff[0].blockType == static_cast<uint8_t>(base[idxC] + 3) &&
	          diff[1].blockType == static_cast<uint8_t>(base[idxB] ^ 0xFF),
	      "diff preserves the current values");

	// Everything restored -> empty vector (no payload).
	current = {{idxA, base[idxA]}, {idxB, base[idxB]}, {idxC, base[idxC]}};
	diff = worldsave::diffEditsAgainstBase(base.data(), current);
	CHECK(diff.empty(), "all overrides reverted -> empty diff");

	// A collapsed currentValues table diffs down to its changed entries only.
	std::vector<ChunkEdit> table = {{idxA, base[idxA]}, {idxB, static_cast<uint8_t>(base[idxB] + 9)}};
	diff = worldsave::diffEditsAgainstBase(base.data(), table);
	CHECK(diff.size() == 1 && diff[0].localIndex == idxB && diff[0].blockType == base[idxB] + 9,
	      "collapsed table -> only the surviving override");
}

// ---------------------------------------------------------------------------
// 5) Empty edit list == "all overrides reverted": file removed / no-op Ok.
// ---------------------------------------------------------------------------

static void testEmptyEditsRemovesFile(const fs::path &dir)
{
	const fs::path chunks = dir / "chunks_empty";
	const std::vector<ChunkEdit> edits = {{voxelIndex(1, 2, 3), 42}};
	CHECK(worldsave::writeChunkFile(chunks, 7, -3, edits) == SaveStatus::Ok, "seed payload written");
	CHECK(fs::exists(chunks / "7_-3.chunk"), "chunk file exists");

	CHECK(worldsave::writeChunkFile(chunks, 7, -3, {}) == SaveStatus::Ok, "empty edits -> Ok");
	CHECK(!fs::exists(chunks / "7_-3.chunk"), "empty edits removed the file");
	std::vector<ChunkEdit> out;
	CHECK(worldsave::readChunkFile(chunks / "7_-3.chunk", 7, -3, out) == SaveStatus::NotFound,
	      "removed file reads NotFound");

	// Empty edits with no file present stays a no-op Ok.
	CHECK(worldsave::writeChunkFile(chunks, 7, -3, {}) == SaveStatus::Ok, "empty edits without file -> Ok");
}

// ---------------------------------------------------------------------------
// 6) Corruption taxonomy: payload flip, mid-record truncation, tampered
//    count field (checksum), bad magic, newer version.
// ---------------------------------------------------------------------------

static void testCorruptionDetection(const fs::path &dir)
{
	const fs::path chunks = dir / "chunks_corrupt";
	const std::vector<ChunkEdit> edits = {
		{voxelIndex(0, 0, 0), 1},
		{voxelIndex(15, 255, 15), 2},
		{voxelIndex(5, 100, 6), 3},
		{voxelIndex(9, 33, 1), 4},
	};
	const fs::path file = chunks / "10_20.chunk";
	CHECK(worldsave::writeChunkFile(chunks, 10, 20, edits) == SaveStatus::Ok, "corruption fixture written");
	std::vector<uint8_t> bytes = readRawBytes(file);
	CHECK(bytes.size() == 28 + edits.size() * 5, "fixture size sanity");

	std::vector<ChunkEdit> out;

	// Flip one payload byte -> checksum catches it.
	std::vector<uint8_t> flipped = bytes;
	flipped.back() ^= 0xFF;
	CHECK(writeRawBytes(file, flipped), "flipped fixture written");
	CHECK(worldsave::readChunkFile(file, 10, 20, out) == SaveStatus::Corrupt, "flipped payload byte -> Corrupt");

	// Truncate mid-record (3 full records + 2 stray bytes).
	std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + 28 + 3 * 5 + 2);
	CHECK(writeRawBytes(file, truncated), "truncated fixture written");
	CHECK(worldsave::readChunkFile(file, 10, 20, out) == SaveStatus::Corrupt, "truncation mid-record -> Corrupt");

	// Editing the count field shifts the record region -> bad checksum.
	std::vector<uint8_t> counted = bytes;
	counted[16] = static_cast<uint8_t>(counted[16] - 1);
	CHECK(writeRawBytes(file, counted), "count-tampered fixture written");
	CHECK(worldsave::readChunkFile(file, 10, 20, out) == SaveStatus::Corrupt,
	      "edited count field -> bad checksum -> Corrupt");

	// Bad magic on a chunk file.
	std::vector<uint8_t> badMagic = bytes;
	badMagic[0] = 'X';
	CHECK(writeRawBytes(file, badMagic), "bad-magic chunk fixture written");
	CHECK(worldsave::readChunkFile(file, 10, 20, out) == SaveStatus::BadMagic, "chunk wrong magic -> BadMagic");

	// Newer chunk-file version.
	std::vector<uint8_t> newer = bytes;
	patchU32(newer, 4, worldsave::kWorldSaveFormatVersion + 3);
	CHECK(writeRawBytes(file, newer), "newer-version chunk fixture written");
	CHECK(worldsave::readChunkFile(file, 10, 20, out) == SaveStatus::UnsupportedVersion,
	      "chunk newer version -> UnsupportedVersion");

	// OLDER chunk-file version: v1 has no migrations, version 0 is rejected
	// exactly like a future one (issue #180 review).
	std::vector<uint8_t> older = bytes;
	patchU32(older, 4, 0);
	CHECK(writeRawBytes(file, older), "older-version chunk fixture written");
	CHECK(worldsave::readChunkFile(file, 10, 20, out) == SaveStatus::UnsupportedVersion,
	      "chunk version 0 -> UnsupportedVersion");

	// blockType above AIR (COUNT + 1 is the last encodable value) is
	// corruption: it can never be produced by an authoritative edit.
	{
		const fs::path file3040 = chunks / "30_40.chunk";
		const std::vector<ChunkEdit> badType = {{voxelIndex(1, 1, 1), 255}};
		CHECK(worldsave::writeChunkFile(chunks, 30, 40, badType) == SaveStatus::Corrupt,
		      "write refuses blockType > AIR");
		// Hand-patch a valid payload's blockType to an invalid value.
		const std::vector<ChunkEdit> one = {{voxelIndex(1, 1, 1), 1}};
		CHECK(worldsave::writeChunkFile(chunks, 30, 40, one) == SaveStatus::Ok, "valid single written");
		std::vector<uint8_t> tampered = readRawBytes(file3040);
		tampered[28 + 4] = 255; // blockType byte of the first record
		CHECK(writeRawBytes(file3040, tampered), "tampered blockType fixture written");
		CHECK(worldsave::readChunkFile(file3040, 30, 40, out) == SaveStatus::Corrupt,
		      "blockType > AIR -> Corrupt");
	}

	// AIR is a legitimate stored override (deletion); COUNT is the enum's
	// size sentinel sitting between the last texture and AIR and is never
	// storable (issue #180 review round 2).
	{
		const fs::path fileBT = chunks / "50_60.chunk";
		const std::vector<ChunkEdit> airPayload = {{voxelIndex(1, 1, 1), static_cast<uint8_t>(AIR)}};
		CHECK(worldsave::writeChunkFile(chunks, 50, 60, airPayload) == SaveStatus::Ok,
		      "AIR is a persistable block type");
		CHECK(worldsave::readChunkFile(fileBT, 50, 60, out) == SaveStatus::Ok, "AIR payload reads back");
		const std::vector<ChunkEdit> countPayload = {{voxelIndex(1, 1, 1), static_cast<uint8_t>(COUNT)}};
		CHECK(worldsave::writeChunkFile(chunks, 50, 60, countPayload) == SaveStatus::Corrupt,
		      "COUNT is rejected");
		const std::vector<ChunkEdit> lastTexture = {{voxelIndex(1, 1, 1), static_cast<uint8_t>(COUNT - 1)}};
		CHECK(worldsave::writeChunkFile(chunks, 50, 60, lastTexture) == SaveStatus::Ok,
		      "COUNT-1 (last real texture) is persistable");
		const std::vector<ChunkEdit> beyond = {{voxelIndex(1, 1, 1), 255}};
		CHECK(worldsave::writeChunkFile(chunks, 50, 60, beyond) == SaveStatus::Corrupt,
		      "values above AIR are rejected");
	}

	// Duplicate localIndex records cannot be produced by the writer.
	{
		// Encode two records with the same index via the tmp writer is not
		// possible (it collapses nothing at this layer but the writer sorts
		// real edits) - hand-assemble the record region instead.
		worldsave::ByteWriter records;
		const uint32_t dup = voxelIndex(2, 2, 2);
		records.u32(dup);
		records.u8(1);
		records.u32(dup);
		records.u8(2);
		const std::vector<uint8_t> recordBytes = records.take();
		worldsave::ByteWriter w;
		for (const char c : worldsave::kChunkFileMagic)
			w.u8(static_cast<uint8_t>(c));
		w.u32(worldsave::kWorldSaveFormatVersion);
		w.i32(30);
		w.i32(40);
		w.u32(2);
		w.u64(worldsave::fnv1a64(recordBytes.data(), recordBytes.size()));
		w.raw(recordBytes.data(), recordBytes.size());
		const fs::path file3040b = chunks / "30_40.chunk";
		CHECK(writeRawBytes(file3040b, w.take()), "duplicate-index fixture written");
		CHECK(worldsave::readChunkFile(file3040b, 30, 40, out) == SaveStatus::Corrupt,
		      "duplicate localIndex -> Corrupt");
	}

	// Zero records and an over-volume count are both corruption.
	{
		worldsave::ByteWriter w;
		for (const char c : worldsave::kChunkFileMagic)
			w.u8(static_cast<uint8_t>(c));
		w.u32(worldsave::kWorldSaveFormatVersion);
		w.i32(30);
		w.i32(40);
		w.u32(0);
		w.u64(worldsave::fnv1a64(nullptr, 0));
		const fs::path file3040c = chunks / "30_40.chunk";
		const std::vector<uint8_t> zeroBytes = w.take();
		CHECK(writeRawBytes(file3040c, zeroBytes), "zero-record fixture written");
				CHECK(worldsave::readChunkFile(file3040c, 30, 40, out) == SaveStatus::Corrupt,
		      "editCount 0 -> Corrupt");

		std::vector<uint8_t> huge = zeroBytes;
		patchU32(huge, 16, CHUNK_VOLUME + 1);
		CHECK(writeRawBytes(file3040c, huge), "over-volume count fixture written");
				CHECK(worldsave::readChunkFile(file3040c, 30, 40, out) == SaveStatus::Corrupt,
		      "editCount > CHUNK_VOLUME -> Corrupt");
	}

	// Wrong stored coordinates vs expected (file itself fully intact).
	CHECK(worldsave::writeChunkFile(chunks, 10, 20, edits) == SaveStatus::Ok, "valid file restored");
	CHECK(worldsave::readChunkFile(file, 10, 21, out) == SaveStatus::CoordinateMismatch,
	      "wrong expected chunkZ -> CoordinateMismatch");
	CHECK(worldsave::readChunkFile(file, 11, 20, out) == SaveStatus::CoordinateMismatch,
	      "wrong expected chunkX -> CoordinateMismatch");
}

// ---------------------------------------------------------------------------
// 7) Atomic replace + ".tmp" sidecar hygiene.
// ---------------------------------------------------------------------------

static void testAtomicReplaceAndTmpFiles(const fs::path &dir)
{
	const fs::path chunks = dir / "chunks_atomic";
	const std::vector<ChunkEdit> payloadA = {
		{voxelIndex(0, 0, 0), 100},
		{voxelIndex(1, 0, 0), 101},
		{voxelIndex(2, 0, 0), 102},
		{voxelIndex(3, 0, 0), 103},
	};
	const std::vector<ChunkEdit> payloadB = {{voxelIndex(15, 255, 15), 104}, {voxelIndex(0, 16, 3), 103}};

	CHECK(worldsave::writeChunkFile(chunks, 5, 9, payloadA) == SaveStatus::Ok, "payload A written");
	std::vector<ChunkEdit> out;
	CHECK(worldsave::readChunkFile(chunks / "5_9.chunk", 5, 9, out) == SaveStatus::Ok && editsEqual(out, payloadA),
	      "payload A reads back");

	// Atomic replace: rewriting the same chunk with B exposes exactly B.
	CHECK(worldsave::writeChunkFile(chunks, 5, 9, payloadB) == SaveStatus::Ok, "payload B written");
	out.clear();
	CHECK(worldsave::readChunkFile(chunks / "5_9.chunk", 5, 9, out) == SaveStatus::Ok &&
	          editsEqual(out, sortedByIndex(payloadB)),
	      "replaced file exposes exactly B");
	std::error_code ec;
	CHECK(fs::file_size(chunks / "5_9.chunk", ec) == 28 + payloadB.size() * 5 && !ec, "file size matches B");

	// A leftover ".tmp" sidecar is invisible to scans; cleanup removes only
	// the sidecar and the valid file keeps reading.
	const fs::path tmp = worldsave::chunkTmpPath(chunks, 5, 9);
	CHECK(tmp.filename() == "5_9.chunk.tmp", "tmp-path helper name");
	CHECK(writeRawBytes(tmp, {0xDE, 0xAD, 0xBE, 0xEF}), "junk .tmp sidecar written");
	const worldsave::ScanResult scanned = worldsave::scanChunkFiles(chunks);
	CHECK(scanned.status == SaveStatus::Ok, "scan status Ok");
	CHECK(scanned.files.size() == 1 && scanned.files[0].filename() == "5_9.chunk",
	      "scan ignores the .tmp sidecar");
	CHECK(worldsave::cleanTempFiles(chunks) == SaveStatus::Ok, "cleanTempFiles returns Ok");
	CHECK(!fs::exists(tmp), ".tmp sidecar removed by cleanup");
	out.clear();
	CHECK(worldsave::readChunkFile(chunks / "5_9.chunk", 5, 9, out) == SaveStatus::Ok &&
	          editsEqual(out, sortedByIndex(payloadB)),
	      "valid file still reads after cleanup");

	// Missing-directory behavior: Ok with no files (a fresh world simply has
	// no saves yet), nothing to clean.
	{
		const worldsave::ScanResult missing = worldsave::scanChunkFiles(dir / "no_such_dir");
		CHECK(missing.status == SaveStatus::Ok && missing.files.empty(),
		      "scan of missing dir -> Ok + empty");
		CHECK(worldsave::cleanTempFiles(dir / "no_such_dir") == SaveStatus::Ok,
		      "clean of missing dir -> Ok");
	}

	// A chunks path that exists but is NOT a directory is a real I/O error,
	// never "no overrides" (issue #180 review).
	{
		const fs::path asFile = dir / "chunks_as_file";
		CHECK(writeRawBytes(asFile, {0x01}), "file fixture written");
		const worldsave::ScanResult blocked = worldsave::scanChunkFiles(asFile);
		CHECK(blocked.status == SaveStatus::IoError && blocked.files.empty(),
		      "scan of a non-directory -> IoError");
	}
}

// ---------------------------------------------------------------------------
// 8) On-disk format exactness: packed sizes, field order, little-endian.
// ---------------------------------------------------------------------------

static void testFormatExactness(const fs::path &dir)
{
	const fs::path chunks = dir / "chunks_exact";
	const std::vector<ChunkEdit> edits = {
		{voxelIndex(0, 0, 1), 9},
		{voxelIndex(2, 3, 4), 8},
		{voxelIndex(15, 255, 15), 7},
	};
	CHECK(worldsave::writeChunkFile(chunks, -5, 12, edits) == SaveStatus::Ok, "exactness fixture written");
	const std::vector<uint8_t> bytes = readRawBytes(chunks / "-5_12.chunk");

	// Guards against accidental raw struct dumps: exact packed size
	// magic(4) version(4) x(4) z(4) count(4) checksum(8) + 5 per record.
	CHECK(bytes.size() == 4 + 4 + 4 + 4 + 4 + 8 + edits.size() * 5, "chunk file size is header + packed records");

	CHECK(bytes[0] == 'F' && bytes[1] == 'T' && bytes[2] == 'V' && bytes[3] == 'C', "chunk magic bytes spot-check");
	CHECK(getU32(bytes, 4) == worldsave::kWorldSaveFormatVersion, "version stored as little-endian u32");
	CHECK(getU32(bytes, 8) == static_cast<uint32_t>(-5), "chunkX stored as little-endian i32");
	CHECK(getU32(bytes, 12) == 12, "chunkZ stored as little-endian i32");
	CHECK(getU32(bytes, 16) == static_cast<uint32_t>(edits.size()), "editCount stored as little-endian u32");
	CHECK(getU64(bytes, 20) == worldsave::fnv1a64(bytes.data() + 28, edits.size() * 5),
	      "checksum field covers exactly the record bytes");

	// Records from offset 28, ascending: {u32 localIndex, u8 blockType}.
	const std::vector<ChunkEdit> expected = sortedByIndex(edits);
	for (size_t i = 0; i < expected.size(); ++i)
	{
		const size_t off = 28 + i * 5;
		CHECK(getU32(bytes, off) == expected[i].localIndex && bytes[off + 4] == expected[i].blockType,
		      "record bytes exact (ascending localIndex + type)");
	}
}

// ---------------------------------------------------------------------------
// Gameplay state formats (issue #228, Epic #223 Phase E): inventory.state
// (FTVI) + items.state (FTVJ) round trips, exact-size checks, the corruption
// taxonomy and id-ordering validation. Same discipline as the sibling
// formats: every rule is a loud SaveStatus, never a silent clamp.
// ---------------------------------------------------------------------------

static void patchU16(std::vector<uint8_t> &b, size_t off, uint16_t v)
{
	b[off] = static_cast<uint8_t>(v);
	b[off + 1] = static_cast<uint8_t>(v >> 8);
}
static void patchU64(std::vector<uint8_t> &b, size_t off, uint64_t v)
{
	for (int i = 0; i < 8; ++i)
		b[off + i] = static_cast<uint8_t>(v >> (8 * i));
}
static void patchF64(std::vector<uint8_t> &b, size_t off, double v)
{
	uint64_t bits = 0;
	std::memcpy(&bits, &v, sizeof(bits));
	patchU64(b, off, bits);
}

static worldsave::InventoryPersistState sampleInventory()
{
	worldsave::InventoryPersistState s;
	s.selectedSlot = 3;
	s.slots[0] = {static_cast<uint16_t>(items::ItemId::Stone), 64};
	s.slots[1] = {static_cast<uint16_t>(items::ItemId::Dirt), 7};
	s.slots[35] = {static_cast<uint16_t>(items::ItemId::LilyPad), 1};
	return s;
}

static worldsave::ItemsPersistState sampleItems()
{
	worldsave::ItemsPersistState s;
	s.nextEntityId = 100;
	worldsave::ItemEntityPersistState a;
	a.id = 5;
	a.itemId = static_cast<uint16_t>(items::ItemId::Cobblestone);
	a.count = 3;
	a.px = 8.5;
	a.py = 64.25;
	a.pz = -3.125;
	a.vx = 0.5;
	a.vy = -1.25;
	a.vz = 0.0;
	a.ageTicks = 42;
	a.pickupDelayTicks = 7;
	worldsave::ItemEntityPersistState b;
	b.id = 99;
	b.itemId = static_cast<uint16_t>(items::ItemId::Dirt);
	b.count = 64;
	b.px = -100.0;
	b.py = 200.0;
	b.pz = 0.0;
	b.vx = 0.0;
	b.vy = 0.0;
	b.vz = 0.0;
	b.ageTicks = 17999;
	b.pickupDelayTicks = 0;
	s.entities = {a, b};
	return s;
}

static bool sameInventory(const worldsave::InventoryPersistState &a, const worldsave::InventoryPersistState &b)
{
	if (a.selectedSlot != b.selectedSlot)
		return false;
	for (size_t i = 0; i < worldsave::kInventorySlotCount; ++i)
		if (a.slots[i].itemId != b.slots[i].itemId || a.slots[i].count != b.slots[i].count)
			return false;
	return true;
}

static bool sameItemRecord(const worldsave::ItemEntityPersistState &a, const worldsave::ItemEntityPersistState &b)
{
	// Doubles compare bit-exact: the codec memcpy's the IEEE bits.
	return a.id == b.id && a.itemId == b.itemId && a.count == b.count &&
	       a.px == b.px && a.py == b.py && a.pz == b.pz &&
	       a.vx == b.vx && a.vy == b.vy && a.vz == b.vz &&
	       a.ageTicks == b.ageTicks && a.pickupDelayTicks == b.pickupDelayTicks;
}

static void testGameplayStateRoundTrips(const fs::path &dir)
{
	fs::create_directories(dir);

	// inventory.state: round trip + exact size + valid empty payload.
	const auto inv = sampleInventory();
	const auto invBytes = worldsave::encodeInventoryState(inv);
	CHECK(invBytes.size() == worldsave::kInventoryStateBytes, "inventory.state encodes to exactly 153 bytes");
	worldsave::InventoryPersistState invOut;
	CHECK(worldsave::decodeInventoryState(invBytes.data(), invBytes.size(), invOut) == SaveStatus::Ok,
	      "inventory.state round trip Ok");
	CHECK(sameInventory(inv, invOut), "inventory.state round trip preserves selection + all 36 stacks");

	const worldsave::InventoryPersistState emptyInv;
	const auto emptyBytes = worldsave::encodeInventoryState(emptyInv);
	worldsave::InventoryPersistState emptyOut;
	CHECK(worldsave::decodeInventoryState(emptyBytes.data(), emptyBytes.size(), emptyOut) == SaveStatus::Ok &&
	          emptyOut.selectedSlot == 0,
	      "all-zero inventory is a VALID empty payload");

	// items.state: round trip (bit-exact transforms) + exact size + empty.
	const auto its = sampleItems();
	const auto itsBytes = worldsave::encodeItemsState(its);
	CHECK(itsBytes.size() == worldsave::kItemsStateHeaderBytes + 2 * worldsave::kItemEntityRecordBytes,
	      "items.state size = 20-byte header + 68 bytes per record");
	worldsave::ItemsPersistState itsOut;
	CHECK(worldsave::decodeItemsState(itsBytes.data(), itsBytes.size(), itsOut) == SaveStatus::Ok,
	      "items.state round trip Ok");
	CHECK(itsOut.nextEntityId == its.nextEntityId && itsOut.entities.size() == its.entities.size(),
	      "items.state round trip preserves allocator + population");
	CHECK(sameItemRecord(its.entities[0], itsOut.entities[0]) &&
	          sameItemRecord(its.entities[1], itsOut.entities[1]),
	      "items.state round trip is bit-exact (ids, stacks, transforms, tick counters)");

	const worldsave::ItemsPersistState emptyIts;
	const auto emptyItsBytes = worldsave::encodeItemsState(emptyIts);
	CHECK(emptyItsBytes.size() == worldsave::kItemsStateHeaderBytes, "empty items.state is exactly the 20-byte header");
	worldsave::ItemsPersistState emptyItsOut;
	CHECK(worldsave::decodeItemsState(emptyItsBytes.data(), emptyItsBytes.size(), emptyItsOut) == SaveStatus::Ok &&
	          emptyItsOut.entities.empty() && emptyItsOut.nextEntityId == 1,
	      "empty population with allocator at 1 is valid");

	// Atomic file writes: replace semantics + no .tmp leftover + file decode.
	const fs::path invPath = dir / "inventory.state";
	CHECK(worldsave::writeFileAtomic(invPath, invBytes), "inventory.state atomic write");
	CHECK(worldsave::writeFileAtomic(invPath, emptyBytes), "inventory.state atomic replace");
	const auto onDisk = readRawBytes(invPath);
	worldsave::InventoryPersistState diskOut;
	CHECK(worldsave::decodeInventoryState(onDisk.data(), onDisk.size(), diskOut) == SaveStatus::Ok &&
	          diskOut.selectedSlot == 0,
	      "replaced inventory.state decodes to the LATEST payload");
	CHECK(!fs::exists(invPath.string() + ".tmp"), "no .tmp leftover after atomic writes");

	const fs::path itsPath = dir / "items.state";
	CHECK(worldsave::writeFileAtomic(itsPath, itsBytes), "items.state atomic write");
	const auto itsDisk = readRawBytes(itsPath);
	worldsave::ItemsPersistState itsDiskOut;
	CHECK(worldsave::decodeItemsState(itsDisk.data(), itsDisk.size(), itsDiskOut) == SaveStatus::Ok &&
	          itsDiskOut.entities.size() == 2,
	      "items.state file round trip");
	CHECK(!fs::exists(itsPath.string() + ".tmp"), "no .tmp leftover after items.state write");
}

static void testGameplayStateCorruption()
{
	worldsave::InventoryPersistState invOut;
	const auto invBytes = worldsave::encodeInventoryState(sampleInventory());

	auto bad = invBytes;
	bad[0] = 'X';
	CHECK(worldsave::decodeInventoryState(bad.data(), bad.size(), invOut) == SaveStatus::BadMagic,
	      "inventory.state bad magic");
	bad = invBytes;
	patchU32(bad, 4, 2);
	CHECK(worldsave::decodeInventoryState(bad.data(), bad.size(), invOut) == SaveStatus::UnsupportedVersion,
	      "inventory.state unsupported version");
	bad = invBytes;
	bad.pop_back();
	CHECK(worldsave::decodeInventoryState(bad.data(), bad.size(), invOut) == SaveStatus::Corrupt,
	      "inventory.state truncated (exact-size)");
	bad = invBytes;
	bad.push_back(0);
	CHECK(worldsave::decodeInventoryState(bad.data(), bad.size(), invOut) == SaveStatus::Corrupt,
	      "inventory.state oversized (exact-size)");
	bad = invBytes;
	bad[8] = 9;
	CHECK(worldsave::decodeInventoryState(bad.data(), bad.size(), invOut) == SaveStatus::Corrupt,
	      "inventory.state selectedSlot out of range");
	bad = invBytes;
	patchU16(bad, 9, 0xFFFF);
	CHECK(worldsave::decodeInventoryState(bad.data(), bad.size(), invOut) == SaveStatus::Corrupt,
	      "inventory.state unknown ItemId");
	bad = invBytes;
	patchU16(bad, 11, 65);
	CHECK(worldsave::decodeInventoryState(bad.data(), bad.size(), invOut) == SaveStatus::Corrupt,
	      "inventory.state count > maxStack (never clamped)");
	bad = invBytes;
	patchU16(bad, 11, 0);
	CHECK(worldsave::decodeInventoryState(bad.data(), bad.size(), invOut) == SaveStatus::Corrupt,
	      "inventory.state count 0 with non-zero itemId (non-canonical empty)");
	bad = invBytes;
	patchU16(bad, 9, 0);
	CHECK(worldsave::decodeInventoryState(bad.data(), bad.size(), invOut) == SaveStatus::Corrupt,
	      "inventory.state None sentinel with a count");

	worldsave::ItemsPersistState itsOut;
	const auto itsBytes = worldsave::encodeItemsState(sampleItems());
	constexpr size_t rec1 = 20 + worldsave::kItemEntityRecordBytes; // second record

	bad = itsBytes;
	bad[0] = 'X';
	CHECK(worldsave::decodeItemsState(bad.data(), bad.size(), itsOut) == SaveStatus::BadMagic,
	      "items.state bad magic");
	bad = itsBytes;
	patchU32(bad, 4, 99);
	CHECK(worldsave::decodeItemsState(bad.data(), bad.size(), itsOut) == SaveStatus::UnsupportedVersion,
	      "items.state unsupported version");
	bad = itsBytes;
	bad.resize(19);
	CHECK(worldsave::decodeItemsState(bad.data(), bad.size(), itsOut) == SaveStatus::Corrupt,
	      "items.state truncated header");
	bad = itsBytes;
	bad.push_back(0);
	CHECK(worldsave::decodeItemsState(bad.data(), bad.size(), itsOut) == SaveStatus::Corrupt,
	      "items.state exact-size mismatch");
	bad = itsBytes;
	patchU64(bad, 8, 0);
	CHECK(worldsave::decodeItemsState(bad.data(), bad.size(), itsOut) == SaveStatus::Corrupt,
	      "items.state nextEntityId 0");
	bad = itsBytes;
	patchU32(bad, 16, 3);
	CHECK(worldsave::decodeItemsState(bad.data(), bad.size(), itsOut) == SaveStatus::Corrupt,
	      "items.state entityCount inconsistent with size");
	bad = itsBytes;
	patchU64(bad, rec1, 5);
	CHECK(worldsave::decodeItemsState(bad.data(), bad.size(), itsOut) == SaveStatus::Corrupt,
	      "items.state duplicate id");
	bad = itsBytes;
	patchU64(bad, rec1, 4);
	CHECK(worldsave::decodeItemsState(bad.data(), bad.size(), itsOut) == SaveStatus::Corrupt,
	      "items.state descending ids");
	bad = itsBytes;
	patchU64(bad, rec1, 100);
	CHECK(worldsave::decodeItemsState(bad.data(), bad.size(), itsOut) == SaveStatus::Corrupt,
	      "items.state id >= nextEntityId (allocator reuse)");
	bad = itsBytes;
	patchU64(bad, 20, 0);
	CHECK(worldsave::decodeItemsState(bad.data(), bad.size(), itsOut) == SaveStatus::Corrupt,
	      "items.state id 0");
	bad = itsBytes;
	patchU16(bad, 20 + 8, 0xFFFF);
	CHECK(worldsave::decodeItemsState(bad.data(), bad.size(), itsOut) == SaveStatus::Corrupt,
	      "items.state unknown ItemId");
	bad = itsBytes;
	patchU16(bad, 20 + 10, 0);
	CHECK(worldsave::decodeItemsState(bad.data(), bad.size(), itsOut) == SaveStatus::Corrupt,
	      "items.state count 0");
	bad = itsBytes;
	patchU16(bad, 20 + 10, 65);
	CHECK(worldsave::decodeItemsState(bad.data(), bad.size(), itsOut) == SaveStatus::Corrupt,
	      "items.state count > maxStack");
	bad = itsBytes;
	patchF64(bad, 20 + 12, std::numeric_limits<double>::quiet_NaN());
	CHECK(worldsave::decodeItemsState(bad.data(), bad.size(), itsOut) == SaveStatus::Corrupt,
	      "items.state NaN position");
	bad = itsBytes;
	patchF64(bad, 20 + 36, std::numeric_limits<double>::infinity());
	CHECK(worldsave::decodeItemsState(bad.data(), bad.size(), itsOut) == SaveStatus::Corrupt,
	      "items.state infinite velocity");

	// Population cap: one record beyond the bound is Corrupt.
	worldsave::ItemsPersistState overCap;
	overCap.nextEntityId = items::ItemEntitySettings::capacity + 2;
	for (size_t i = 0; i <= items::ItemEntitySettings::capacity; ++i)
	{
		worldsave::ItemEntityPersistState e;
		e.id = i + 1;
		e.itemId = static_cast<uint16_t>(items::ItemId::Stone);
		e.count = 1;
		overCap.entities.push_back(e);
	}
	const auto overBytes = worldsave::encodeItemsState(overCap);
	CHECK(worldsave::decodeItemsState(overBytes.data(), overBytes.size(), itsOut) == SaveStatus::Corrupt,
	      "items.state population beyond the cap");
}

int main()
{
	const fs::path root = fs::temp_directory_path() / "ft_vox_world_save_test";
	std::error_code ec;
	fs::remove_all(root, ec);
	fs::create_directories(root, ec);
	if (ec)
	{
		std::cerr << "FAIL: cannot create test directory " << root << "\n";
		return 2;
	}

	testFnv1a64();
	testWorldMetaRoundTrip(root / "meta");
	testWorldMetaErrors(root / "meta_errors");
	testChunkFileNames();
	testChunkRoundTripBoundaries(root / "chunks");
	testDiffEditsAgainstBase();
	testEmptyEditsRemovesFile(root / "empty");
	testCorruptionDetection(root / "corrupt");
	testAtomicReplaceAndTmpFiles(root / "atomic");
	testFormatExactness(root / "exact");
	testGameplayStateRoundTrips(root / "gameplay");
	testGameplayStateCorruption();

	if (testmain::fails != 0)
	{
		std::cerr << testmain::fails << " check(s) failed (" << testmain::checks << " run)\n";
		return 1;
	}
	std::cout << "PASS: world save format layer (" << testmain::checks << " checks)\n";
	return 0;
}
