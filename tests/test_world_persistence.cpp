// World persistence (issue #180, Phase 3+4): SaveService ordering and
// superseding, WorldPersistence open/create identity semantics, sparse-save
// diffing against regenerated terrain, recycled-chunk payload isolation, and
// the ChunkManager unload/reload integration. Headless setup copied from
// test_chunk_lifecycle.cpp (real Chunk/ChunkManager/TerrainGenerator + a real
// ThreadPool for the async paths, but never a Vulkan device).
#include <Chunk/Chunk.hpp>
#include <Chunk/ChunkManager.hpp>
#include <Chunk/ChunkPool.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Camera/Camera.hpp>
#include <Engine/ThreadPool.hpp>
#include <World/WorldPersistence.hpp>
#include <World/WorldSave.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static int g_fails = 0;

#define CHECK(cond, msg)                                                       \
	do                                                                         \
	{                                                                          \
		if (!(cond))                                                           \
		{                                                                      \
			std::cerr << "FAIL: " << msg << " (" << __LINE__ << ")\n";         \
			++g_fails;                                                         \
		}                                                                      \
	} while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int g_dirCounter = 0;

static std::filesystem::path makeTempDir(const char *tag)
{
	++g_dirCounter;
	std::filesystem::path dir = std::filesystem::temp_directory_path() /
	                            ("ft-vox-persist-" + std::string(tag) + "-" +
	                             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
	                             "-" + std::to_string(g_dirCounter));
	std::filesystem::create_directories(dir);
	return dir;
}

static void removeDir(const std::filesystem::path &dir)
{
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	if (ec)
		std::cerr << "WARN: could not remove temp dir " << dir.string() << ": " << ec.message()
		          << "\n";
}

static bool fileExists(const std::filesystem::path &path)
{
	std::error_code ec;
	return std::filesystem::exists(path, ec) && !ec;
}

static std::filesystem::path chunkPath(const std::filesystem::path &chunksDir, int32_t cx, int32_t cz)
{
	return chunksDir / worldsave::chunkFileName(cx, cz);
}

static bool readChunk(const std::filesystem::path &chunksDir, int32_t cx, int32_t cz,
                      std::vector<worldsave::ChunkEdit> &out)
{
	return worldsave::readChunkFile(chunkPath(chunksDir, cx, cz), cx, cz, out) ==
	       worldsave::SaveStatus::Ok;
}

// Canonical y-major local index: y*256 + z*16 + x.
static constexpr uint32_t idxOf(int x, int y, int z)
{
	return static_cast<uint32_t>(y) * 256u + static_cast<uint32_t>(z) * 16u +
	       static_cast<uint32_t>(x);
}

static void setByIndex(Chunk *chunk, uint32_t localIndex, TextureType type)
{
	chunk->setVoxel(static_cast<int>(localIndex & 15u), static_cast<int>(localIndex >> 8),
	                static_cast<int>((localIndex >> 4) & 15u), type);
}

// A block type guaranteed != `reference` (GLASS/BRICKS never generate, but be
// exact anyway).
static uint8_t pickNonBase(uint8_t reference)
{
	return reference == static_cast<uint8_t>(GLASS) ? static_cast<uint8_t>(BRICKS)
	                                                : static_cast<uint8_t>(GLASS);
}

static std::vector<worldsave::ChunkEdit> editsFromMap(const Chunk::ChunkEditMap &map)
{
	std::vector<worldsave::ChunkEdit> out;
	out.reserve(map.size());
	for (const auto &[localIndex, type] : map)
		out.push_back({localIndex, type});
	std::sort(out.begin(), out.end(),
	          [](const worldsave::ChunkEdit &a, const worldsave::ChunkEdit &b)
	          { return a.localIndex < b.localIndex; });
	return out;
}

static bool sameEdits(const std::vector<worldsave::ChunkEdit> &a,
                      const std::vector<worldsave::ChunkEdit> &b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (a[i].localIndex != b[i].localIndex || a[i].blockType != b[i].blockType)
			return false;
	return true;
}

static std::vector<worldsave::ChunkEdit> diffAgainstBase(const std::vector<Voxel> &base,
                                                         const std::vector<worldsave::ChunkEdit> &values)
{
	static_assert(sizeof(Voxel) == 1, "diff walks the voxel bytes as u8");
	return worldsave::diffEditsAgainstBase(reinterpret_cast<const uint8_t *>(base.data()), values);
}

// Wait for all in-flight gen/mesh jobs, mirroring the engine's per-frame
// processFinishedJobs() publish pass.
static void drainManagerJobs(ChunkManager &mgr)
{
	int spins = 0;
	while ((mgr.pendingGenJobs() > 0 || mgr.pendingMeshJobs() > 0) && spins++ < 30000)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		mgr.processFinishedJobs();
	}
	mgr.processFinishedJobs();
}

// Drives the real unload path to completion (queueUnloadOutOfRange must have
// run; the deferred release needs kDeferredReleaseFrames calls to age).
static void drainDeferredReleases(ChunkManager &mgr)
{
	int spins = 0;
	while (mgr.deferredReleaseCount() > 0 && spins++ < 1000)
	{
		mgr.processDeferredReleases();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	mgr.processDeferredReleases();
}

static glm::ivec3 chunkCoordOf(const glm::vec3 &chunkOrigin)
{
	return {static_cast<int>(std::round(chunkOrigin.x)) / CHUNK_SIZE, 0,
	        static_cast<int>(std::round(chunkOrigin.z)) / CHUNK_SIZE};
}

static bool containsEdit(const std::vector<worldsave::ChunkEdit> &edits, uint32_t localIndex,
                         uint8_t blockType)
{
	return std::any_of(edits.begin(), edits.end(), [&](const worldsave::ChunkEdit &e)
	                   { return e.localIndex == localIndex && e.blockType == blockType; });
}

// Hand-writes a 16-byte world.meta for corruption tests.
static void writeMetaBytes(const std::filesystem::path &metaPath, const char magic[4],
                           uint32_t formatVersion, int32_t seed, uint32_t generatorVersion)
{
	std::vector<uint8_t> bytes;
	bytes.insert(bytes.end(), magic, magic + 4);
	for (int i = 0; i < 4; ++i)
		bytes.push_back(static_cast<uint8_t>((formatVersion >> (8 * i)) & 0xFFu));
	const uint32_t seedU = static_cast<uint32_t>(seed);
	for (int i = 0; i < 4; ++i)
		bytes.push_back(static_cast<uint8_t>((seedU >> (8 * i)) & 0xFFu));
	for (int i = 0; i < 4; ++i)
		bytes.push_back(static_cast<uint8_t>((generatorVersion >> (8 * i)) & 0xFFu));
	std::ofstream file(metaPath, std::ios::binary | std::ios::trunc);
	file.write(reinterpret_cast<const char *>(bytes.data()),
	           static_cast<std::streamsize>(bytes.size()));
}

// Flips a byte inside the record region -> checksum mismatch -> Corrupt.
static void corruptChunkFile(const std::filesystem::path &path)
{
	std::ifstream in(path, std::ios::binary);
	std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	in.close();
	CHECK(bytes.size() > 32, "corrupt target has a record region");
	if (bytes.size() <= 32)
		return;
	bytes[bytes.size() - 1] = static_cast<char>(bytes[bytes.size() - 1] ^ 0xFF);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// ---------------------------------------------------------------------------
// 1. SaveService ordering / superseding
// ---------------------------------------------------------------------------

static void testSaveServiceOrdering()
{
	TerrainGenerator gen(42);
	auto root = makeTempDir("svc");
	const auto chunks = root / "w" / "chunks";
	std::filesystem::create_directories(chunks);
	const std::vector<Voxel> base = gen.generateChunk(0, 0).voxels;

	const uint32_t ia = idxOf(3, 40, 5);
	const uint32_t ib = idxOf(8, 60, 9);
	const uint8_t va = pickNonBase(base[ia].type);
	const uint8_t vb = pickNonBase(base[ib].type);

	SaveService svc(chunks, 42);
	int completions = 0;
	std::vector<worldsave::ChunkEdit> completionEdits;
	svc.setCompletionCallback(
		[&](int32_t, int32_t, const std::vector<worldsave::ChunkEdit> &finalOverrides)
		{
			++completions;
			completionEdits = finalOverrides;
		});

	// A then B for the same coordinate: B (last enqueued) must win, A must be
	// dropped without I/O.
	{
		const std::vector<worldsave::ChunkEdit> valuesA = {{ia, va}, {ib, static_cast<uint8_t>(BRICKS)}};
		const std::vector<worldsave::ChunkEdit> valuesB = {{ia, va}, {ib, vb}};

		ChunkSaveRequest a;
		a.chunkX = 0;
		a.chunkZ = 0;
		a.currentValues = valuesA;
		ChunkSaveRequest b;
		b.chunkX = 0;
		b.chunkZ = 0;
		b.currentValues = valuesB;
		CHECK(svc.enqueue(std::move(a)), "enqueue A");
		CHECK(svc.enqueue(std::move(b)), "enqueue B");
		CHECK(svc.flush(), "flush A/B");

		const SaveService::Stats st = svc.stats();
		CHECK(st.superseded >= 1, "older pending request superseded");
		CHECK(st.completed == 1, "exactly one request written");
		CHECK(st.queueDepth == 0, "queue drained");

		std::vector<worldsave::ChunkEdit> disk;
		CHECK(readChunk(chunks, 0, 0, disk), "chunk file exists after flush");
		CHECK(sameEdits(disk, diffAgainstBase(base, valuesB)),
		      "disk reflects the newest payload only");
		CHECK(completions == 1, "completion callback fired once");
		CHECK(sameEdits(completionEdits, diffAgainstBase(base, valuesB)),
		      "completion carries the minimal persisted diff");
	}

	// An older payload enqueued after a newer one for the same coordinate: the
	// LAST-ENQUEUED revision still wins, the earlier request is dropped
	// without I/O (ordering is by enqueue revision, never by disk timing).
	{
		const uint64_t supersededBefore = svc.stats().superseded;
		const std::vector<worldsave::ChunkEdit> valuesC = {{ia, vb}};
		const std::vector<worldsave::ChunkEdit> valuesD = {{ia, va}, {ib, vb}};

		ChunkSaveRequest c;
		c.chunkX = 0;
		c.chunkZ = 0;
		c.currentValues = valuesC;
		ChunkSaveRequest d;
		d.chunkX = 0;
		d.chunkZ = 0;
		d.currentValues = valuesD;
		CHECK(svc.enqueue(std::move(c)), "enqueue C");
		CHECK(svc.enqueue(std::move(d)), "enqueue D");
		CHECK(svc.flush(), "flush C/D");

		CHECK(svc.stats().superseded >= supersededBefore + 1, "earlier request dropped");
		std::vector<worldsave::ChunkEdit> disk;
		CHECK(readChunk(chunks, 0, 0, disk), "file readable after second flush");
		CHECK(sameEdits(disk, diffAgainstBase(base, valuesD)),
		      "disk reflects the last-enqueued payload");
	}

	// Two different chunks: both files exist with correct coordinates and no
	// cross-contamination.
	{
		const std::vector<Voxel> baseE = gen.generateChunk(2, -1).voxels;
		const std::vector<Voxel> baseF = gen.generateChunk(-3, 4).voxels;
		const std::vector<worldsave::ChunkEdit> valuesE = {{idxOf(0, 30, 0), va}};
		const std::vector<worldsave::ChunkEdit> valuesF = {{idxOf(15, 45, 15), vb}};

		ChunkSaveRequest e;
		e.chunkX = 2;
		e.chunkZ = -1;
		e.currentValues = valuesE;
		ChunkSaveRequest f;
		f.chunkX = -3;
		f.chunkZ = 4;
		f.currentValues = valuesF;
		CHECK(svc.enqueue(std::move(e)), "enqueue chunk (2,-1)");
		CHECK(svc.enqueue(std::move(f)), "enqueue chunk (-3,4)");
		CHECK(svc.flush(), "flush two chunks");

		std::vector<worldsave::ChunkEdit> diskE, diskF;
		CHECK(readChunk(chunks, 2, -1, diskE), "chunk (2,-1) file exists");
		CHECK(readChunk(chunks, -3, 4, diskF), "chunk (-3,4) file exists");
		CHECK(sameEdits(diskE, diffAgainstBase(baseE, valuesE)), "chunk (2,-1) payload correct");
		CHECK(sameEdits(diskF, diffAgainstBase(baseF, valuesF)), "chunk (-3,4) payload correct");
	}

	svc.shutdown();
	ChunkSaveRequest rejected;
	CHECK(svc.enqueue(std::move(rejected)) == false, "enqueue rejected after shutdown");
	removeDir(root);
}

// ---------------------------------------------------------------------------
// 2. Recycled-chunk race: the worker must use the immutable captured payload
// ---------------------------------------------------------------------------

static void testRecycledChunkIsolation()
{
	auto root = makeTempDir("recycle");
	const auto chunks = root / "w" / "chunks";
	TerrainGenerator gen(7);
	const std::vector<Voxel> base = gen.generateChunk(0, 0).voxels;

	WorldPersistence wp;
	const WorldPersistence::OpenInfo info =
		wp.openOrCreate(root, "w", 7, TerrainGenerator::kGeneratorVersion);
	CHECK(info.ok, "world opened");

	ChunkPool pool(2);
	Chunk *chunk = pool.acquire(glm::vec3(0.0f, 0.0f, 0.0f));
	CHECK(chunk != nullptr, "chunk acquired");
	chunk->setTrackPersistentEdits(true);

	const uint32_t i1 = idxOf(1, 210, 3);
	const uint32_t i2 = idxOf(4, 215, 6);
	const uint8_t v1 = pickNonBase(base[i1].type);
	const uint8_t v2 = pickNonBase(base[i2].type);
	chunk->setVoxel(1, 210, 3, static_cast<TextureType>(v1));
	chunk->setVoxel(4, 215, 6, static_cast<TextureType>(v2));
	const std::vector<worldsave::ChunkEdit> payload1 = editsFromMap(chunk->takePersistentEdits());
	CHECK(payload1.size() == 2, "two edits captured from the first incarnation");

	// Hand the payload to the save pipeline, then recycle the SAME chunk
	// object to a different coordinate and record different values there.
	wp.captureChunkEdits(0, 0, payload1);

	pool.release(chunk);
	Chunk *recycled = pool.acquire(glm::vec3(3.0f * CHUNK_SIZE, 0.0f, 2.0f * CHUNK_SIZE));
	CHECK(recycled == chunk, "pool actually recycled the same chunk object");
	recycled->setTrackPersistentEdits(true);
	const uint32_t j1 = idxOf(0, 220, 1);
	const uint8_t w1 = pickNonBase(gen.generateChunk(3, 2).voxels[j1].type);
	recycled->setVoxel(0, 220, 1, static_cast<TextureType>(w1));

	CHECK(wp.flush(), "flush while the source object is already recycled");

	// The saved file must contain exactly the captured payload's diff - the
	// worker can never have looked at the (mutated) live chunk.
	std::vector<worldsave::ChunkEdit> disk;
	CHECK(readChunk(chunks, 0, 0, disk), "first payload written");
	CHECK(sameEdits(disk, diffAgainstBase(base, payload1)),
	      "disk equals the immutable captured payload");
	CHECK(!fileExists(chunkPath(chunks, 3, 2)), "recycled incarnation was never saved");

	// Capture the second incarnation explicitly: isolation holds both ways.
	const std::vector<worldsave::ChunkEdit> payload2 = editsFromMap(recycled->takePersistentEdits());
	wp.captureChunkEdits(3, 2, payload2);
	CHECK(wp.flush(), "second flush");

	std::vector<worldsave::ChunkEdit> disk1, disk2;
	CHECK(readChunk(chunks, 0, 0, disk1), "first file still readable");
	CHECK(readChunk(chunks, 3, 2, disk2), "second file written");
	CHECK(sameEdits(disk1, diffAgainstBase(base, payload1)), "first file untouched");
	const std::vector<Voxel> base32 = gen.generateChunk(3, 2).voxels;
	CHECK(sameEdits(disk2, diffAgainstBase(base32, payload2)), "second file has its own payload");

	wp.shutdown();
	removeDir(root);
}

// ---------------------------------------------------------------------------
// 3. WorldPersistence open/create semantics
// ---------------------------------------------------------------------------

static void testOpenCreateSemantics()
{
	auto root = makeTempDir("open");
	const uint32_t genVersion = TerrainGenerator::kGeneratorVersion;

	{
		WorldPersistence creator;
		const WorldPersistence::OpenInfo created =
			creator.openOrCreate(root, "alpha", 42, genVersion);
		CHECK(created.ok && created.created, "fresh world created");
		CHECK(created.seed == 42, "created seed");
		CHECK(creator.enabled() && creator.seed() == 42 && creator.name() == "alpha",
		      "facade state after create");
	}
	CHECK(WorldPersistence::worldExists(root, "alpha"), "worldExists true after create");
	CHECK(!WorldPersistence::worldExists(root, "missing"), "worldExists false for unknown");

	{
		WorldPersistence wp;
		const WorldPersistence::OpenInfo reopened =
			wp.openOrCreate(root, "alpha", 42, genVersion);
		CHECK(reopened.ok && !reopened.created, "existing world reopened");
		CHECK(reopened.seed == 42, "stored seed reported");

		WorldPersistence wrongSeed;
		const WorldPersistence::OpenInfo seedMismatch =
			wrongSeed.openOrCreate(root, "alpha", 43, genVersion);
		CHECK(!seedMismatch.ok, "seed mismatch rejected");
		CHECK(seedMismatch.error.find("seed mismatch") != std::string::npos,
		      "seed mismatch error message");
		// Identity must be untouched by the rejected open: meta still says 42.
		int stored = 0;
		std::string err;
		CHECK(WorldPersistence::peekStoredSeed(root, "alpha", stored, err) && stored == 42,
		      "identity not overwritten by a rejected open");

		WorldPersistence wrongGen;
		const WorldPersistence::OpenInfo genMismatch =
			wrongGen.openOrCreate(root, "alpha", 42, genVersion + 1);
		CHECK(!genMismatch.ok, "generator version mismatch rejected");
		CHECK(genMismatch.error.find("generator version mismatch") != std::string::npos,
		      "generator mismatch error message");
	}

	// Unknown NEWER format version (hand-written meta) -> UnsupportedVersion.
	{
		const auto metaPath = root / "alpha" / "world.meta";
		const auto backup = root / "alpha.meta.bak";
		std::filesystem::copy_file(metaPath, backup,
		                           std::filesystem::copy_options::overwrite_existing);
		writeMetaBytes(metaPath, "FTVW", 999u, 42, genVersion);
		WorldPersistence wp;
		const WorldPersistence::OpenInfo info = wp.openOrCreate(root, "alpha", 42, genVersion);
		CHECK(!info.ok, "newer format version rejected");
		CHECK(info.storedFormatVersion == 999, "stored format version reported");
		CHECK(info.error.find("format version") != std::string::npos,
		      "unsupported version error message");
		std::filesystem::copy_file(backup, metaPath,
		                           std::filesystem::copy_options::overwrite_existing);
		std::error_code ec;
		std::filesystem::remove(backup, ec);
	}

	// Wrong magic -> BadMagic.
	{
		const auto metaPath = root / "alpha" / "world.meta";
		writeMetaBytes(metaPath, "XXXX", 1u, 42, genVersion);
		WorldPersistence wp;
		const WorldPersistence::OpenInfo info = wp.openOrCreate(root, "alpha", 42, genVersion);
		CHECK(!info.ok, "bad magic rejected");
		CHECK(info.error.find("magic") != std::string::npos, "bad magic error message");
	}

	// peekStoredSeed failure path.
	{
		int seed = 0;
		std::string err;
		CHECK(!WorldPersistence::peekStoredSeed(root, "missing", seed, err), "peek missing fails");
		CHECK(!err.empty(), "peek error text");
	}

	// A corrupt chunk file on disk: open still succeeds, the file is loudly
	// reported, its overrides are skipped, and other chunks load normally.
	{
		const auto gammaRoot = root / "gamma-world";
		{
			WorldPersistence writer;
			const WorldPersistence::OpenInfo created =
				writer.openOrCreate(gammaRoot, "gamma", 5, genVersion);
			CHECK(created.ok, "gamma world created");
			writer.captureChunkEdits(0, 0, {{idxOf(1, 200, 1), static_cast<uint8_t>(BRICKS)}});
			writer.captureChunkEdits(1, 0, {{idxOf(2, 205, 2), static_cast<uint8_t>(GLASS)}});
			CHECK(writer.flush(), "gamma chunks written");
			writer.shutdown();
		}
		const auto chunks = gammaRoot / "gamma" / "chunks";
		corruptChunkFile(chunkPath(chunks, 0, 0));

		WorldPersistence wp;
		const WorldPersistence::OpenInfo info = wp.openOrCreate(gammaRoot, "gamma", 5, genVersion);
		CHECK(info.ok, "open survives a corrupt chunk file");
		const WorldPersistence::Status st = wp.status();
		CHECK(st.lastError.find("0_0.chunk") != std::string::npos,
		      "corrupt file named in status().lastError");
		CHECK(wp.overridesFor(0, 0) == nullptr, "corrupt chunk falls back to procedural");
		const std::vector<worldsave::ChunkEdit> *healthy = wp.overridesFor(1, 0);
		CHECK(healthy != nullptr && healthy->size() == 1 &&
		          (*healthy)[0].localIndex == idxOf(2, 205, 2),
		      "healthy chunk overrides loaded");
	}

	removeDir(root);
}

// ---------------------------------------------------------------------------
// 4. Sparse-save behavior end-to-end via Chunk + regenerated base
// ---------------------------------------------------------------------------

static void testSparseSaveBehavior()
{
	const int seed = 1337;
	TerrainGenerator gen(seed);
	const std::vector<Voxel> base = gen.generateChunk(0, 0).voxels;

	// Three high-air positions; replacements are guaranteed != base, and the
	// third edit is restored to its generated base value before capture.
	const uint32_t i1 = idxOf(4, 200, 4);
	const uint32_t i2 = idxOf(8, 210, 8);
	const uint32_t i3 = idxOf(12, 220, 12);
	const uint8_t v1 = pickNonBase(base[i1].type);
	const uint8_t v2 = pickNonBase(base[i2].type);
	const uint8_t v3 = pickNonBase(base[i3].type);

	auto root = makeTempDir("sparse");
	const auto chunks = root / "w" / "chunks";
	WorldPersistence wp;
	const WorldPersistence::OpenInfo info =
		wp.openOrCreate(root, "w", seed, TerrainGenerator::kGeneratorVersion);
	CHECK(info.ok, "world opened");

	ChunkPool pool(2);
	Chunk *chunk = pool.acquire(glm::vec3(0.0f, 0.0f, 0.0f));
	CHECK(chunk != nullptr, "chunk acquired");
	chunk->setTrackPersistentEdits(true);
	setByIndex(chunk, i1, static_cast<TextureType>(v1));
	setByIndex(chunk, i2, static_cast<TextureType>(v2));
	// Edited away, then restored to the generated base value: the recording
	// keeps the entry (the map holds CURRENT values, not ops).
	setByIndex(chunk, i3, static_cast<TextureType>(v3));
	setByIndex(chunk, i3, base[i3].getTextureType());

	std::vector<worldsave::ChunkEdit> captured = editsFromMap(chunk->takePersistentEdits());
	CHECK(captured.size() == 3, "all three value changes recorded (incl. the restored one)");
	wp.captureChunkEdits(0, 0, std::move(captured));
	CHECK(wp.flush(), "flush");

	std::vector<worldsave::ChunkEdit> disk;
	CHECK(readChunk(chunks, 0, 0, disk), "chunk file exists");
	const std::vector<worldsave::ChunkEdit> restoredValues = {
		{i1, v1}, {i2, v2}, {i3, base[i3].type}};
	const auto expected = diffAgainstBase(base, restoredValues);
	CHECK(disk.size() == 2, "value equal to the generated base was filtered by the worker");
	CHECK(sameEdits(disk, expected), "persisted diff is exactly the non-base edits");
	CHECK(containsEdit(disk, i1, v1) && containsEdit(disk, i2, v2), "both real edits present");

	// Capture of an EMPTY payload for a never-edited chunk: no file created,
	// no I/O wasted.
	wp.captureChunkEdits(9, 9, {});
	CHECK(wp.flush(), "flush empty capture");
	CHECK(!fileExists(chunkPath(chunks, 9, 9)), "empty capture creates no file");

	// Capture of all-edits-reverted for the saved chunk: the file is deleted.
	const uint64_t deletedBefore = wp.status().deleted;
	wp.captureChunkEdits(0, 0, {});
	CHECK(wp.flush(), "flush revert");
	CHECK(!fileExists(chunkPath(chunks, 0, 0)), "fully reverted chunk file deleted");
	CHECK(wp.status().deleted >= deletedBefore + 1, "deletion counted");
	CHECK(wp.overridesFor(0, 0) == nullptr, "index entry refined away after delete");

	wp.shutdown();
	removeDir(root);
}

// ---------------------------------------------------------------------------
// 5. ChunkManager unload/reload integration (async gen path + real unload)
// ---------------------------------------------------------------------------

static void testManagerUnloadReload()
{
	auto root = makeTempDir("mgr");
	const std::string savesRoot = (root / "saves").string();
	const auto chunksDir = root / "saves" / "it" / "chunks";
	const int seed = 42;

	TerrainGenerator gen(seed);
	ThreadPool threads(2);
	ChunkPool pool(128);
	ChunkManager mgr(&gen, &threads, &pool);

	std::string err;
	CHECK(mgr.openWorld(savesRoot, "it", err), ("openWorld failed: " + err).c_str());
	CHECK(mgr.isWorldOpen() && mgr.worldPersistence() != nullptr, "world open");

	const glm::ivec3 target(2, 0, 2);
	const glm::vec3 homePos(2.0f * CHUNK_SIZE + 8.0f, 100.0f, 2.0f * CHUNK_SIZE + 8.0f);
	Camera camera(homePos);
	RenderSettings settings;
	settings.minRenderDistance = 32;
	settings.maxRenderDistance = 96;

	// Load + async-generate the nearest chunks (the target first).
	mgr.updateStreaming(camera, settings);
	mgr.processChunkLoading(64);
	mgr.generatePendingVoxels(camera, settings, 4);
	drainManagerJobs(mgr);
	Chunk *chunk = mgr.getChunk(target);
	CHECK(chunk != nullptr && chunk->getState() >= ChunkState::GENERATED,
	      "target chunk loaded and generated");

	// Edits through the real API at chunk-boundary positions (local x/z 0 and
	// 15), plus the y=255 region, plus one surface deletion.
	const float bx = 2.0f * CHUNK_SIZE; // world X/Z of local 0 in chunk (2,2)
	const glm::vec3 glassPositions[4] = {
		{bx + 0.0f, 100.0f, 40.0f},  // local (0, 100, 8)
		{bx + 15.0f, 100.0f, 40.0f}, // local (15, 100, 8)
		{40.0f, 100.0f, bx + 0.0f},  // local (8, 100, 0)
		{40.0f, 100.0f, bx + 15.0f}, // local (8, 100, 15)
	};
	for (const glm::vec3 &p : glassPositions)
		CHECK(mgr.placeVoxel(p, GLASS), "boundary place accepted");
	CHECK(mgr.placeVoxel(glm::vec3(37.0f, 255.0f, 37.0f), STONE), "y=255 place accepted");

	// Delete a solid generated voxel in the interior column (10, y, 10).
	int solidY = -1;
	for (int y = 1; y < static_cast<int>(CHUNK_HEIGHT); ++y)
	{
		if (chunk->getVoxel(10, y, 10).type != static_cast<uint8_t>(AIR))
		{
			solidY = y;
			break;
		}
	}
	CHECK(solidY > 0, "found a solid voxel to delete");
	CHECK(mgr.deleteVoxel(glm::vec3(42.0f, static_cast<float>(solidY), 42.0f)),
	      "surface delete accepted");

	// Local verification before the unload.
	CHECK(chunk->getVoxel(0, 100, 8).getTextureType() == GLASS, "glass at local x=0");
	CHECK(chunk->getVoxel(15, 100, 8).getTextureType() == GLASS, "glass at local x=15");
	CHECK(chunk->getVoxel(8, 100, 0).getTextureType() == GLASS, "glass at local z=0");
	CHECK(chunk->getVoxel(8, 100, 15).getTextureType() == GLASS, "glass at local z=15");
	CHECK(chunk->getVoxel(5, 255, 5).getTextureType() == STONE, "stone at y=255");
	CHECK(chunk->getVoxel(10, solidY, 10).getTextureType() == AIR, "voxel deleted");
	CHECK(chunk->hasPersistentEdits(), "edits recorded");

	// Drive the REAL unload: move far outside the unload hysteresis radius.
	camera.setPosition(glm::vec3(homePos.x + 4.0f * settings.maxRenderDistance, 100.0f,
	                             homePos.z + 4.0f * settings.maxRenderDistance));
	mgr.updateStreaming(camera, settings);
	drainDeferredReleases(mgr);
	CHECK(mgr.getChunk(target) == nullptr, "chunk unloaded");

	const WorldPersistence::Status afterUnload = mgr.worldPersistence()->status();
	CHECK(afterUnload.enqueued >= 1, "unload captured the edits");

	CHECK(mgr.flushWorld(), "flushWorld");
	CHECK(mgr.worldPersistence()->status().completed >= 1, "save service completed the write");

	// Exactly the six surviving edits persisted for chunk (2,2).
	std::vector<worldsave::ChunkEdit> disk;
	CHECK(readChunk(chunksDir, 2, 2, disk), "chunk (2,2) file written");
	CHECK(disk.size() == 6, "exactly the six non-base edits persisted");
	CHECK(containsEdit(disk, idxOf(0, 100, 8), static_cast<uint8_t>(GLASS)), "file: glass x=0");
	CHECK(containsEdit(disk, idxOf(15, 100, 8), static_cast<uint8_t>(GLASS)), "file: glass x=15");
	CHECK(containsEdit(disk, idxOf(8, 100, 0), static_cast<uint8_t>(GLASS)), "file: glass z=0");
	CHECK(containsEdit(disk, idxOf(8, 100, 15), static_cast<uint8_t>(GLASS)), "file: glass z=15");
	CHECK(containsEdit(disk, idxOf(5, 255, 5), static_cast<uint8_t>(STONE)), "file: stone y=255");
	CHECK(containsEdit(disk, idxOf(10, solidY, 10), static_cast<uint8_t>(AIR)),
	      "file: deletion persisted as AIR");

	// Reload through the manager: the async gen job must re-apply overrides.
	camera.setPosition(homePos);
	mgr.updateStreaming(camera, settings);
	mgr.processChunkLoading(64);
	mgr.generatePendingVoxels(camera, settings, 4);
	drainManagerJobs(mgr);
	chunk = mgr.getChunk(target);
	CHECK(chunk != nullptr && chunk->getState() >= ChunkState::GENERATED, "chunk reloaded");
	CHECK(chunk->getVoxel(0, 100, 8).getTextureType() == GLASS, "reload: glass x=0");
	CHECK(chunk->getVoxel(15, 100, 8).getTextureType() == GLASS, "reload: glass x=15");
	CHECK(chunk->getVoxel(8, 100, 0).getTextureType() == GLASS, "reload: glass z=0");
	CHECK(chunk->getVoxel(8, 100, 15).getTextureType() == GLASS, "reload: glass z=15");
	CHECK(chunk->getVoxel(5, 255, 5).getTextureType() == STONE, "reload: stone y=255");
	CHECK(chunk->getVoxel(10, solidY, 10).getTextureType() == AIR, "reload: deletion restored");
	CHECK(chunk->hasPersistentEdits(), "re-applied overrides are the recapture baseline");

	// Occupancy/mesh sanity: the touched sections are alive and the chunk
	// goes through a full mesh pass. (No GPU in this harness, so the mesh
	// payload stays CPU-published: MESHED state is the strongest observable.)
	mgr.meshPendingChunks(camera, settings, 4);
	drainManagerJobs(mgr);
	CHECK(chunk->getState() == ChunkState::MESHED, "reloaded chunk meshes end-to-end");

	CHECK(mgr.closeWorld(), "closeWorld flush success");
	CHECK(!mgr.isWorldOpen() && mgr.worldPersistence() == nullptr, "world closed");
	removeDir(root);
}

// ---------------------------------------------------------------------------
// 6. Many concurrently modified chunks: zero cross-contamination
// ---------------------------------------------------------------------------

static void testManyChunksNoCrossContamination()
{
	auto root = makeTempDir("many");
	const std::string savesRoot = (root / "saves").string();
	const auto chunksDir = root / "saves" / "w" / "chunks";
	const int seed = 42;

	TerrainGenerator gen(seed);
	ThreadPool threads(2);
	ChunkPool pool(256);
	ChunkManager mgr(&gen, &threads, &pool);

	std::string err;
	CHECK(mgr.openWorld(savesRoot, "w", err), ("openWorld failed: " + err).c_str());

	Camera camera(glm::vec3(3.0f * CHUNK_SIZE, 100.0f, 3.0f * CHUNK_SIZE));
	RenderSettings settings;
	settings.minRenderDistance = 32;
	settings.maxRenderDistance = 80;
	mgr.updateStreaming(camera, settings);
	mgr.processChunkLoading(256);
	mgr.generatePendingVoxels(camera, settings, 256);
	drainManagerJobs(mgr);

	// Edit ~20 distinct chunks, two interior edits each (interior => no
	// neighbor mirrors). Replacement blocks are picked != the current voxel,
	// and the exact placed values are recorded for the on-disk assertions.
	constexpr size_t kTargetEdited = 20;
	struct EditedChunk
	{
		glm::ivec3 coord;
		uint8_t firstValue;
		uint8_t secondValue;
	};
	std::vector<EditedChunk> edited;
	glm::ivec3 untouched(-1000, 0, -1000);
	for (Chunk *c : mgr.getActiveChunks())
	{
		if (!c || c->getState() < ChunkState::GENERATED)
			continue;
		const glm::ivec3 cc = chunkCoordOf(c->getPosition());
		if (std::any_of(edited.begin(), edited.end(),
		                [&](const EditedChunk &e) { return e.coord == cc; }))
			continue;
		if (edited.size() >= kTargetEdited)
		{
			untouched = cc; // remember a loaded-but-never-edited chunk
			continue;
		}
		const float ox = static_cast<float>(cc.x * CHUNK_SIZE);
		const float oz = static_cast<float>(cc.z * CHUNK_SIZE);
		const uint8_t cur1 = c->getVoxel(3, 200, 3).type;
		const uint8_t cur2 = c->getVoxel(9, 210, 9).type;
		const uint8_t t1 = pickNonBase(cur1);
		const uint8_t t2 = pickNonBase(cur2);
		const bool ok1 = mgr.placeVoxel(glm::vec3(ox + 3.0f, 200.0f, oz + 3.0f),
		                                static_cast<TextureType>(t1));
		const bool ok2 = mgr.placeVoxel(glm::vec3(ox + 9.0f, 210.0f, oz + 9.0f),
		                                static_cast<TextureType>(t2));
		if (ok1 && ok2)
			edited.push_back({cc, t1, t2});
	}
	CHECK(edited.size() == kTargetEdited, "edited the target number of chunks");
	CHECK(!(untouched.x == -1000), "found a never-edited chunk");

	CHECK(mgr.flushWorld(), "flushWorld captures all dirty chunks and flushes");
	CHECK(mgr.closeWorld(), "closeWorld");

	// Fresh WorldPersistence on the same directory: every chunk's overrides
	// must match exactly what was edited.
	WorldPersistence fresh;
	const WorldPersistence::OpenInfo info =
		fresh.openOrCreate(savesRoot, "w", seed, TerrainGenerator::kGeneratorVersion);
	CHECK(info.ok && !info.created, "fresh open of the same world");

	for (const EditedChunk &e : edited)
	{
		const std::vector<worldsave::ChunkEdit> *o = fresh.overridesFor(e.coord.x, e.coord.z);
		const std::string label = "chunk " + std::to_string(e.coord.x) + "_" +
		                          std::to_string(e.coord.z);
		CHECK(o != nullptr, ("overrides exist for edited " + label).c_str());
		if (!o)
			continue;
		CHECK(o->size() == 2, ("exactly the two edits persisted for " + label).c_str());
		CHECK(containsEdit(*o, idxOf(3, 200, 3), e.firstValue),
		      ("right first edit value for " + label).c_str());
		CHECK(containsEdit(*o, idxOf(9, 210, 9), e.secondValue),
		      ("right second edit value for " + label).c_str());
	}

	CHECK(fresh.overridesFor(untouched.x, untouched.z) == nullptr,
	      "never-edited chunk has no overrides");
	CHECK(!fileExists(chunkPath(chunksDir, untouched.x, untouched.z)),
	      "never-edited chunk has no file");
	CHECK(worldsave::scanChunkFiles(chunksDir).size() == edited.size(),
	      "one file per edited chunk, nothing else");

	fresh.shutdown();
	removeDir(root);
}

// ---------------------------------------------------------------------------
// 7. Disabled persistence: the benchmark/transient guarantee
// ---------------------------------------------------------------------------

static void testDisabledPersistence()
{
	auto root = makeTempDir("disabled");
	const auto savesRoot = root / "saves"; // must NEVER be created

	TerrainGenerator gen(42);
	ThreadPool threads(2);
	ChunkPool pool(64);
	ChunkManager mgr(&gen, &threads, &pool);
	CHECK(!mgr.isWorldOpen() && mgr.worldPersistence() == nullptr, "no world open");

	Camera camera(glm::vec3(8.0f, 100.0f, 8.0f));
	RenderSettings settings;
	settings.minRenderDistance = 32;
	settings.maxRenderDistance = 48;
	mgr.updateStreaming(camera, settings);
	mgr.processChunkLoading(64);
	mgr.generatePendingVoxels(camera, settings, 16);
	drainManagerJobs(mgr);

	CHECK(mgr.placeVoxel(glm::vec3(8.0f, 120.0f, 8.0f), GLASS), "edit accepted without a world");
	CHECK(mgr.placeVoxel(glm::vec3(9.0f, 120.0f, 8.0f), BRICKS), "second edit accepted");

	// Real unload with edits present: still nothing persisted.
	camera.setPosition(glm::vec3(8.0f + 4.0f * settings.maxRenderDistance, 100.0f, 8.0f));
	mgr.updateStreaming(camera, settings);
	drainDeferredReleases(mgr);

	CHECK(mgr.flushWorld(), "flushWorld is a no-op success without a world");
	CHECK(!std::filesystem::exists(savesRoot), "no saves root was created");
	CHECK(worldsave::scanChunkFiles(root).empty(), "no chunk files anywhere");
	removeDir(root);
}

// ---------------------------------------------------------------------------
// 8. Destructor safety net: an unclosed world is flushed by ~ChunkManager
// ---------------------------------------------------------------------------

static void testDestructorSafetyNet()
{
	auto root = makeTempDir("dtor");
	const std::string savesRoot = (root / "saves").string();
	const auto chunksDir = root / "saves" / "w" / "chunks";

	{
		TerrainGenerator gen(42);
		ThreadPool threads(2);
		ChunkPool pool(64);
		ChunkManager mgr(&gen, &threads, &pool);
		std::string err;
		CHECK(mgr.openWorld(savesRoot, "w", err), ("openWorld failed: " + err).c_str());

		Camera camera(glm::vec3(8.0f, 100.0f, 8.0f));
		RenderSettings settings;
		settings.minRenderDistance = 32;
		settings.maxRenderDistance = 48;
		mgr.updateStreaming(camera, settings);
		mgr.processChunkLoading(64);
		mgr.generatePendingVoxels(camera, settings, 8);
		drainManagerJobs(mgr);
		CHECK(mgr.placeVoxel(glm::vec3(8.0f, 130.0f, 8.0f), BRICKS), "edit accepted");
		// Deliberately NO closeWorld/flushWorld: the destructor must flush.
	}

	// The destructor path persisted the edit.
	WorldPersistence verify;
	const WorldPersistence::OpenInfo info =
		verify.openOrCreate(savesRoot, "w", 42, TerrainGenerator::kGeneratorVersion);
	CHECK(info.ok, "world readable after unclosed manager destruction");
	const std::vector<worldsave::ChunkEdit> *o = verify.overridesFor(0, 0);
	CHECK(o != nullptr && o->size() == 1 && (*o)[0].localIndex == idxOf(8, 130, 8) &&
	          (*o)[0].blockType == static_cast<uint8_t>(BRICKS),
	      "destructor flushed the recorded edit");
	verify.shutdown();

	removeDir(root);
}

// ---------------------------------------------------------------------------
// 8. Player state (issue #180, Phase 5): <world>/player.state
// ---------------------------------------------------------------------------

static PlayerPersistState samplePlayerState()
{
	PlayerPersistState s;
	s.x = 1234.5678901234;
	s.y = 71.25;
	s.z = -957.125;
	s.yaw = 269.5f;
	s.pitch = -42.25f;
	s.flight = true;
	s.selectedBlock = static_cast<int32_t>(BRICKS);
	return s;
}

static bool samePlayerState(const PlayerPersistState &a, const PlayerPersistState &b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z && a.yaw == b.yaw && a.pitch == b.pitch &&
	       a.flight == b.flight && a.selectedBlock == b.selectedBlock;
}

static void writePlayerBytes(const std::filesystem::path &path,
                             const std::vector<uint8_t> &bytes)
{
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file.write(reinterpret_cast<const char *>(bytes.data()),
	           static_cast<std::streamsize>(bytes.size()));
}

static void testPlayerStatePersistence()
{
	auto root = makeTempDir("player");
	const uint32_t genVersion = TerrainGenerator::kGeneratorVersion;
	const auto worldDir = root / "walker";
	const auto statePath = worldDir / "player.state";

	// Round-trip with exact values + atomic replace (A then B -> B).
	{
		WorldPersistence wp;
		CHECK(wp.openOrCreate(root, "walker", 321, genVersion).ok, "player world open");

		const PlayerPersistState a = samplePlayerState();
		CHECK(wp.writePlayerState(a), "write player.state A");
		CHECK(fileExists(statePath), "player.state exists");
		PlayerPersistState back;
		CHECK(wp.readPlayerState(back), "read player.state A");
		CHECK(samePlayerState(a, back), "round-trip values exact");
		CHECK(wp.status().lastError.empty(), "no errors on the happy path");

		PlayerPersistState b;
		b.x = -8.5;
		b.y = 129.75;
		b.z = 77.0;
		b.yaw = 1.5f;
		b.pitch = 88.0f;
		b.flight = false;
		b.selectedBlock = static_cast<int32_t>(GLASS);
		CHECK(wp.writePlayerState(b), "write player.state B");
		PlayerPersistState backB;
		CHECK(wp.readPlayerState(backB), "read player.state B");
		CHECK(samePlayerState(b, backB), "atomic replace: B wins");
		CHECK(!samePlayerState(a, backB), "atomic replace: A is gone");

		// Truncated payload -> false + loud error.
		std::ifstream in(statePath, std::ios::binary);
		std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
		                        std::istreambuf_iterator<char>());
		in.close();
		CHECK(bytes.size() == 45, "player.state payload is 45 bytes");
		bytes.resize(20);
		writePlayerBytes(statePath, std::vector<uint8_t>(bytes.begin(), bytes.end()));
		PlayerPersistState out;
		CHECK(!wp.readPlayerState(out), "truncated player.state rejected");
		CHECK(!wp.status().lastError.empty(), "truncation reported in lastError");
	}

	// Missing file: the fresh-world case — false, NOT an error.
	{
		WorldPersistence fresh;
		CHECK(fresh.openOrCreate(root, "fresh", 7, genVersion).ok, "fresh world open");
		PlayerPersistState out;
		CHECK(!fresh.readPlayerState(out), "missing player.state -> false");
		CHECK(fresh.status().lastError.empty(), "missing file is not an error");
	}

	// Crafted payloads (built with the exposed ByteWriter so byte order and
	// encoding match the format exactly): NaN, out-of-range y, bad magic.
	{
		WorldPersistence wp;
		CHECK(wp.openOrCreate(root, "walker", 321, genVersion).ok, "reopen walker");

		auto buildState = [](double y, const char magic[4]) {
			worldsave::ByteWriter w;
			w.raw(magic, 4);
			w.u32(worldsave::kPlayerStateFormatVersion);
			w.f64(1.0);
			w.f64(y);
			w.f64(3.0);
			w.f32(359.0f);
			w.f32(-10.0f);
			w.u8(1);
			w.i32(static_cast<int32_t>(STONE));
			return w.take();
		};

		PlayerPersistState out;
		writePlayerBytes(statePath, buildState(std::nan(""), worldsave::kPlayerStateMagic));
		CHECK(!wp.readPlayerState(out), "NaN y rejected");
		CHECK(!wp.status().lastError.empty(), "NaN reported in lastError");

		writePlayerBytes(statePath,
		                 buildState(static_cast<double>(CHUNK_HEIGHT) + 100.0,
		                            worldsave::kPlayerStateMagic));
		CHECK(!wp.readPlayerState(out), "y above CHUNK_HEIGHT + 64 rejected");

		writePlayerBytes(statePath, buildState(-65.0, worldsave::kPlayerStateMagic));
		CHECK(!wp.readPlayerState(out), "y below -64 rejected");

		writePlayerBytes(statePath, buildState(70.0, "XXXX"));
		CHECK(!wp.readPlayerState(out), "bad magic rejected");
		CHECK(!wp.status().lastError.empty(), "bad magic reported in lastError");

		// Boundaries themselves must be accepted.
		writePlayerBytes(statePath,
		                 buildState(static_cast<double>(CHUNK_HEIGHT) + 64.0,
		                            worldsave::kPlayerStateMagic));
		CHECK(wp.readPlayerState(out) && out.y == static_cast<double>(CHUNK_HEIGHT) + 64.0,
		      "y at upper bound accepted");
	}

	removeDir(root);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
	std::cout << "== SaveService ordering ==\n";
	testSaveServiceOrdering();
	std::cout << "== Recycled-chunk isolation ==\n";
	testRecycledChunkIsolation();
	std::cout << "== Open/create semantics ==\n";
	testOpenCreateSemantics();
	std::cout << "== Sparse save ==\n";
	testSparseSaveBehavior();
	std::cout << "== Manager unload/reload ==\n";
	testManagerUnloadReload();
	std::cout << "== Many chunks ==\n";
	testManyChunksNoCrossContamination();
	std::cout << "== Disabled persistence ==\n";
	testDisabledPersistence();
	std::cout << "== Destructor safety net ==\n";
	testDestructorSafetyNet();
	std::cout << "== Player state ==\n";
	testPlayerStatePersistence();

	if (g_fails == 0)
	{
		std::cout << "test_world_persistence: OK\n";
		return 0;
	}
	std::cout << "test_world_persistence: FAILED (" << g_fails << ")\n";
	return 1;
}
