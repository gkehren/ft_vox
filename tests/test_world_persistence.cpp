// World persistence (issue #180, Phase 3+4): SaveService ordering and
// superseding, WorldPersistence open/create identity semantics, sparse-save
// diffing against regenerated terrain, recycled-chunk payload isolation, and
// the ChunkManager unload/reload integration. Headless setup copied from
// test_chunk_lifecycle.cpp (real Chunk/ChunkManager/TerrainGenerator + a real
// ThreadPool for the async paths, but never a Vulkan device).
#include <Chunk/Chunk.hpp>
#include <Chunk/ChunkManager.hpp>
#include <Chunk/ChunkCollisionView.hpp>
#include <Physics/PlayerController.hpp>
#include <Chunk/ChunkPool.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Camera/Camera.hpp>
#include <Engine/ThreadPool.hpp>
#include <World/WorldPersistence.hpp>
#include <Engine/GameUI.hpp>
#include <World/WorldSave.hpp>

#include <algorithm>
#include <atomic>
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
	uint64_t lastCompletionRevision = 0;
	std::vector<worldsave::ChunkEdit> completionEdits;
	svc.setCompletionCallback(
		[&](int32_t, int32_t, uint64_t revision, bool ok,
		    const std::vector<worldsave::ChunkEdit> &finalOverrides, const std::string &error)
		{
			(void)error;
			CHECK(ok, "service-level completion succeeds");
			++completions;
			lastCompletionRevision = revision;
			completionEdits = finalOverrides;
		});

	// A then B for the same coordinate: B REPLACES A's queued payload in
	// place (per-coordinate coalescing), so A performs no I/O at all and B
	// is the only completion.
	{
		const std::vector<worldsave::ChunkEdit> valuesA = {{ia, va}, {ib, static_cast<uint8_t>(BRICKS)}};
		const std::vector<worldsave::ChunkEdit> valuesB = {{ia, va}, {ib, vb}};

		ChunkSaveRequest a;
		a.chunkX = 0;
		a.chunkZ = 0;
		a.revision = 1;
		a.currentValues = valuesA;
		ChunkSaveRequest b;
		b.chunkX = 0;
		b.chunkZ = 0;
		b.revision = 2;
		b.currentValues = valuesB;
		CHECK(svc.enqueue(std::move(a)), "enqueue A");
		CHECK(svc.enqueue(std::move(b)), "enqueue B");
		CHECK(svc.flush(), "flush A/B");

		const SaveService::Stats st = svc.stats();
		CHECK(st.superseded >= 1, "in-queue replacement resolves the replaced ticket as superseded");
		CHECK(st.completed == 1, "exactly one request written");
		CHECK(st.queueDepth == 0, "queue drained (one slot per coordinate)");

		std::vector<worldsave::ChunkEdit> disk;
		CHECK(readChunk(chunks, 0, 0, disk), "chunk file exists after flush");
		CHECK(sameEdits(disk, diffAgainstBase(base, valuesB)),
		      "disk reflects the newest payload only");
		CHECK(completions == 1, "completion callback fired once");
		CHECK(lastCompletionRevision == 2, "completion carries B's revision");
		CHECK(sameEdits(completionEdits, diffAgainstBase(base, valuesB)),
		      "completion carries the minimal persisted diff");
	}

	// Older payload enqueued after a newer one for the same coordinate: the
	// payload is replaced in place regardless of capture order, so the
	// newest content always owns the single queue slot.
	{
		const std::vector<worldsave::ChunkEdit> valuesC = {{ia, vb}};
		const std::vector<worldsave::ChunkEdit> valuesD = {{ia, va}, {ib, vb}};

		ChunkSaveRequest c;
		c.chunkX = 0;
		c.chunkZ = 0;
		c.revision = 3;
		c.currentValues = valuesC;
		ChunkSaveRequest d;
		d.chunkX = 0;
		d.chunkZ = 0;
		d.revision = 4;
		d.currentValues = valuesD;
		CHECK(svc.enqueue(std::move(c)), "enqueue C");
		CHECK(svc.enqueue(std::move(d)), "enqueue D");
		CHECK(svc.flush(), "flush C/D");

		std::vector<worldsave::ChunkEdit> disk;
		CHECK(readChunk(chunks, 0, 0, disk), "file readable after second flush");
		CHECK(sameEdits(disk, diffAgainstBase(base, valuesD)),
		      "disk reflects the last-enqueued payload");
		CHECK(completions == 2 && lastCompletionRevision == 4, "only D completed");
	}

	// In-flight supersede (issue #180 review): a job that already POPPED and
	// wrote its .tmp while a newer capture arrives must never replace the
	// authoritative file. The seam blocks A between tmp-write and commit,
	// the test enqueues B in that window, and A must be discarded with its
	// tmp removed.
	{
		std::mutex m;
		std::condition_variable cv;
		bool aReachedCommit = false;
		bool releaseA = false;
		int aTmpWrites = 0;
		svc.setWriteTmpFnForTests(
			[&](const std::filesystem::path &dir, int32_t cx, int32_t cz,
			    const std::vector<worldsave::ChunkEdit> &edits, std::filesystem::path &tmpPath)
			{
				const worldsave::SaveStatus st =
					worldsave::writeChunkFileTmp(dir, cx, cz, edits, tmpPath);
				if (st == worldsave::SaveStatus::Ok && cx == 0 && cz == 0)
				{
					// First call is A: park between tmp and commit until B
					// has been enqueued.
					std::unique_lock<std::mutex> lock(m);
					if (++aTmpWrites == 1)
					{
						aReachedCommit = true;
						cv.notify_all();
						cv.wait(lock, [&] { return releaseA; });
					}
				}
				return st;
			});

		std::thread enqueuer([&]
		                     {
			                     std::unique_lock<std::mutex> lock(m);
			                     cv.wait(lock, [&] { return aReachedCommit; });
			                     ChunkSaveRequest b2;
			                     b2.chunkX = 0;
			                     b2.chunkZ = 0;
			                     b2.revision = 6;
			                     b2.currentValues = {{ia, vb}};
			                     CHECK(svc.enqueue(std::move(b2)), "enqueue B while A is mid-write");
			                     releaseA = true;
			                     cv.notify_all();
		                     });

		ChunkSaveRequest a2;
		a2.chunkX = 0;
		a2.chunkZ = 0;
		a2.revision = 5;
		a2.currentValues = {{ia, va}, {ib, vb}};
		CHECK(svc.enqueue(std::move(a2)), "enqueue A (in-flight supersede scenario)");
		enqueuer.join();
		CHECK(svc.flush(), "flush after in-flight supersede");
		svc.setWriteTmpFnForTests(nullptr);

		const SaveService::Stats st = svc.stats();
		CHECK(st.superseded >= 1, "in-flight stale job counted as superseded");
		CHECK(!fileExists(chunks / "0_0.chunk.tmp"), "stale job's tmp file was removed");
		std::vector<worldsave::ChunkEdit> disk;
		CHECK(readChunk(chunks, 0, 0, disk), "authoritative file readable");
		CHECK(sameEdits(disk, diffAgainstBase(base, {{ia, vb}})),
		      "final disk state is the newer revision's payload");
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
		e.revision = 7;
		e.currentValues = valuesE;
		ChunkSaveRequest f;
		f.chunkX = -3;
		f.chunkZ = 4;
		f.revision = 8;
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
// 1b. Write failure -> flush() false -> retry succeeds (issue #180 review)
// ---------------------------------------------------------------------------

static void testWriteFailureRetry()
{
	TerrainGenerator gen(42);
	auto root = makeTempDir("failretry");
	TerrainGenerator gen2(42);
	const std::vector<Voxel> base = gen.generateChunk(0, 0).voxels;

	WorldPersistence wp;
	const WorldPersistence::OpenInfo info =
		wp.openOrCreate(root, "w", 42, TerrainGenerator::kGeneratorVersion);
	CHECK(info.ok, "world opened");

	const uint32_t ia = idxOf(5, 50, 5);
	const uint8_t va = pickNonBase(base[ia].type);
	const std::vector<worldsave::ChunkEdit> values = {{ia, va}};

	// First attempt fails deterministically at the tmp-write step.
	std::atomic<int> writeCalls{0};
	wp.setWriteTmpFnForTests(
		[&](const std::filesystem::path &, int32_t, int32_t,
		    const std::vector<worldsave::ChunkEdit> &, std::filesystem::path &)
		{
			++writeCalls;
			return worldsave::SaveStatus::IoError;
		});
	wp.captureChunkEdits(0, 0, values);
	CHECK(!wp.flush(), "flush reports the failed write");
	{
		const WorldPersistence::Status st = wp.status();
		CHECK(st.failed >= 1, "failure counted");
		CHECK(st.dirtyCoordinates >= 1, "failed coordinate stays dirty");
	}
	CHECK(!fileExists(root / "w" / "chunks" / "0_0.chunk"), "no file created by the failed write");
	// The flush's bounded retry rounds re-attempted the write internally
	// (kMaxFlushRounds rounds => the seam saw several failing attempts).
	CHECK(writeCalls.load() >= 2, "flush retried the failure within its rounds");

	// Retry: clear the seam and flush again - the dirty coordinate is
	// re-enqueued by flush itself and the real writer now succeeds (the seam
	// counter stays at the one failed attempt).
	wp.setWriteTmpFnForTests(nullptr);
	CHECK(wp.flush(), "retry flush succeeds");
	CHECK(wp.status().completed >= 1, "retry completed a real write");

	std::vector<worldsave::ChunkEdit> disk;
	const auto chunks = root / "w" / "chunks";
	CHECK(readChunk(chunks, 0, 0, disk), "retry wrote the chunk file");
	CHECK(sameEdits(disk, diffAgainstBase(base, values)), "retry payload correct");

	// A follow-up flush with nothing new stays true (checkpoint semantics).
	CHECK(wp.flush(), "idempotent flush stays true");

	wp.shutdown();
	removeDir(root);
}

// ---------------------------------------------------------------------------
// 1c. Barrier ordering is independent from content revisions (issue #180
// review round 2): a replaced revision is never "finished", so flush must
// wait for TICKETS, not revisions.
// ---------------------------------------------------------------------------

static void testBarrierOrderingIndependentOfRevisions()
{
	std::cout << "== Barrier ordering (tickets vs revisions) ==" << std::endl;
	TerrainGenerator gen(42);
	auto root = makeTempDir("barrier");
	const auto chunks = root / "w" / "chunks";
	std::filesystem::create_directories(chunks);
	const std::vector<Voxel> baseX = gen.generateChunk(0, 0).voxels;
	const std::vector<Voxel> baseB = gen.generateChunk(1, 0).voxels;
	const std::vector<Voxel> baseC = gen.generateChunk(2, 0).voxels;

	const uint32_t ix = idxOf(3, 50, 3);
	const uint32_t ib = idxOf(2, 60, 4);
	const uint32_t ic = idxOf(7, 80, 9);
	const uint8_t vx = pickNonBase(baseX[ix].type);
	const uint8_t vb = pickNonBase(baseB[ib].type);
	const uint8_t vb2 = (vb == static_cast<uint8_t>(BRICKS)) ? static_cast<uint8_t>(GLASS)
	                                                         : static_cast<uint8_t>(BRICKS);
	const uint8_t vc = pickNonBase(baseC[ic].type);

	SaveService svc(chunks, 42);

	// Block the FIRST serialize call of X and of C; B runs through.
	std::mutex m;
	std::condition_variable cv;
	bool xBlocked = false;
	bool cBlocked = false;
	bool releaseX = false;
	bool releaseC = false;
	svc.setSerializeFnForTests(
		[&](int32_t cx, int32_t cz, const std::vector<worldsave::ChunkEdit> &currentValues)
		{
			if (cx == 0 && cz == 0)
			{
				std::unique_lock<std::mutex> lock(m);
				if (!xBlocked)
				{
					xBlocked = true;
					cv.notify_all();
					cv.wait(lock, [&] { return releaseX; });
				}
			}
			else if (cx == 2 && cz == 0)
			{
				std::unique_lock<std::mutex> lock(m);
				if (!cBlocked)
				{
					cBlocked = true;
					cv.notify_all();
					cv.wait(lock, [&] { return releaseC; });
				}
			}
			return currentValues;
		});

	// ticket1: X/rev1 - the worker pops it and blocks in serialize.
	ChunkSaveRequest rx;
	rx.chunkX = 0;
	rx.chunkZ = 0;
	rx.revision = 1;
	rx.currentValues = {{ix, vx}};
	CHECK(svc.enqueue(std::move(rx)), "enqueue X (ticket1)");
	{
		std::unique_lock<std::mutex> lock(m);
		cv.wait(lock, [&] { return xBlocked; });
	}

	// ticket2: B/rev2, ticket3: C/rev3, ticket4: B/rev4 REPLACES ticket2.
	ChunkSaveRequest rb2;
	rb2.chunkX = 1;
	rb2.chunkZ = 0;
	rb2.revision = 2;
	rb2.currentValues = {{ib, vb}};
	CHECK(svc.enqueue(std::move(rb2)), "enqueue B rev2 (ticket2)");

	ChunkSaveRequest rc;
	rc.chunkX = 2;
	rc.chunkZ = 0;
	rc.revision = 3;
	rc.currentValues = {{ic, vc}};
	CHECK(svc.enqueue(std::move(rc)), "enqueue C rev3 (ticket3)");

	ChunkSaveRequest rb4;
	rb4.chunkX = 1;
	rb4.chunkZ = 0;
	rb4.revision = 4;
	rb4.currentValues = {{ib, vb2}};
	CHECK(svc.enqueue(std::move(rb4)), "enqueue B rev4 (ticket4 replaces ticket2)");

	// Release X. The single worker then: finishes X, finishes B/rev4 (a
	// HIGHER revision than C/rev3, which is still queued), then pops C/rev3
	// and blocks again. Exactly the reordering that broke the old
	// revision-ordered barrier.
	{
		std::lock_guard<std::mutex> lock(m);
		releaseX = true;
		cv.notify_all();
	}
	while (svc.stats().completed < 2)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	{
		std::unique_lock<std::mutex> lock(m);
		cv.wait(lock, [&] { return cBlocked; });
	}

	// Old barrier: lastFinishedRevision(rev4) >= highestEnqueued(rev4) =>
	// flush returned HERE while rev3 was still pending. New ticket barrier:
	// unfinished ticket3 <= target ticket4 => flush must stay blocked.
	std::atomic<bool> flushReturned{false};
	std::thread flusher(
		[&]
		{
			svc.flush();
			flushReturned.store(true, std::memory_order_release);
		});
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	CHECK(!flushReturned.load(std::memory_order_acquire),
	      "flush waits for the older ticket although a newer revision already finished");
	{
		std::lock_guard<std::mutex> lock(m);
		releaseC = true;
		cv.notify_all();
	}
	flusher.join();
	CHECK(flushReturned.load(std::memory_order_acquire), "flush returns once ticket3 resolves");

	std::vector<worldsave::ChunkEdit> diskX, diskB, diskC;
	CHECK(readChunk(chunks, 0, 0, diskX), "X file written");
	CHECK(readChunk(chunks, 1, 0, diskB), "B file written");
	CHECK(readChunk(chunks, 2, 0, diskC), "C file written");
	CHECK(sameEdits(diskX, {{ix, vx}}), "X reflects its revision");
	CHECK(sameEdits(diskB, {{ib, vb2}}), "B reflects the replacing revision");
	CHECK(sameEdits(diskC, {{ic, vc}}), "C reflects the revision the barrier waited for");

	svc.shutdown();
	removeDir(root);
}

// ---------------------------------------------------------------------------
// 1d. Failure checkpoint (issue #180 review round 2): a failure that
// COMPLETED before flush() is called must be reported by that flush.
// ---------------------------------------------------------------------------

static void testFlushFailureCheckpoint()
{
	std::cout << "== Flush failure checkpoint ==" << std::endl;
	TerrainGenerator gen(42);
	auto root = makeTempDir("ckpt");
	const auto chunks = root / "w" / "chunks";
	std::filesystem::create_directories(chunks);
	const std::vector<Voxel> base = gen.generateChunk(0, 0).voxels;
	const uint32_t ia = idxOf(5, 55, 5);
	const uint8_t va = pickNonBase(base[ia].type);

	SaveService svc(chunks, 42);
	svc.setWriteTmpFnForTests(
		[](const std::filesystem::path &, int32_t, int32_t,
		   const std::vector<worldsave::ChunkEdit> &, std::filesystem::path &)
		{ return worldsave::SaveStatus::IoError; });

	ChunkSaveRequest bad;
	bad.chunkX = 0;
	bad.chunkZ = 0;
	bad.revision = 1;
	bad.currentValues = {{ia, va}};
	CHECK(svc.enqueue(std::move(bad)), "enqueue failing request");

	// Wait for the completion WITHOUT calling flush first.
	while (svc.stats().failed < 1)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	CHECK(svc.flush() == false, "flush reports a failure that completed before the call");

	// A retry that succeeds without a NEW failure reports true.
	svc.setWriteTmpFnForTests(nullptr);
	ChunkSaveRequest good;
	good.chunkX = 0;
	good.chunkZ = 0;
	good.revision = 2;
	good.currentValues = {{ia, va}};
	CHECK(svc.enqueue(std::move(good)), "enqueue succeeding request");
	CHECK(svc.flush(), "flush after a successful retry is true");

	std::vector<worldsave::ChunkEdit> disk;
	CHECK(readChunk(chunks, 0, 0, disk), "retry wrote the file");
	CHECK(sameEdits(disk, diffAgainstBase(base, {{ia, va}})), "retry payload correct");

	svc.shutdown();
	removeDir(root);
}

// ---------------------------------------------------------------------------
// 1e. Stale DELETE commit (issue #180 review round 2): a delete is as
// authoritative as a rename - a job superseded before its commit must never
// remove the authoritative file.
// ---------------------------------------------------------------------------

static void testStaleDeleteNeverRemovesAuthoritativeFile()
{
	std::cout << "== Stale delete commit ==" << std::endl;
	TerrainGenerator gen(42);
	auto root = makeTempDir("staledel");
	WorldPersistence wp;
	const WorldPersistence::OpenInfo info =
		wp.openOrCreate(root, "w", 42, TerrainGenerator::kGeneratorVersion);
	CHECK(info.ok, "world opened");
	const auto chunks = root / "w" / "chunks";

	const std::vector<Voxel> base = gen.generateChunk(0, 0).voxels;
	const uint32_t ia = idxOf(4, 70, 6);
	const uint32_t ib = idxOf(9, 90, 2);
	const uint8_t va = pickNonBase(base[ia].type);

	// X on disk.
	wp.captureChunkEdits(0, 0, {{ia, va}});
	CHECK(wp.flush(), "initial payload written");

	// A = revert/delete job; block it at the beforeCommit seam.
	std::mutex m;
	std::condition_variable cv;
	bool deleteBlocked = false;
	bool releaseDelete = false;
	wp.setBeforeCommitFnForTests(
		[&](int32_t cx, int32_t cz, uint64_t)
		{
			if (cx != 0 || cz != 0)
				return;
			std::unique_lock<std::mutex> lock(m);
			if (!deleteBlocked)
			{
				deleteBlocked = true;
				cv.notify_all();
				cv.wait(lock, [&] { return releaseDelete; });
			}
		});
	// Revert is expressed by capturing the PROCEDURAL value (delta model,
	// issue #180 review round 5): the worker regenerates the base and drops
	// the entry, producing an empty diff -> authoritative delete.
	const uint8_t baseA = base[ia].type;
	wp.captureChunkEdits(0, 0, {{ia, baseA}});
	{
		std::unique_lock<std::mutex> lock(m);
		cv.wait(lock, [&] { return deleteBlocked; });
	}

	// B = a newer capture for the same coordinate, enqueued while the delete
	// is blocked right before its authoritative commit.
	const std::vector<worldsave::ChunkEdit> bValues = {{ia, baseA}, {ib, static_cast<uint8_t>(BRICKS)}};
	wp.captureChunkEdits(0, 0, bValues);
	wp.setBeforeCommitFnForTests(nullptr); // future jobs must not block
	{
		std::lock_guard<std::mutex> lock(m);
		releaseDelete = true;
		cv.notify_all();
	}

	CHECK(wp.flush(), "flush after the stale delete");
	std::vector<worldsave::ChunkEdit> disk;
	CHECK(readChunk(chunks, 0, 0, disk), "authoritative file still present");
	CHECK(sameEdits(disk, diffAgainstBase(base, bValues)),
	      "final disk state is the newer capture, the delete never won");

	wp.shutdown();
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
		CHECK(wp.overridesSnapshot(0, 0).empty(), "corrupt chunk falls back to procedural");
		const std::vector<worldsave::ChunkEdit> healthy = wp.overridesSnapshot(1, 0);
		CHECK(healthy.size() == 1 && healthy[0].localIndex == idxOf(2, 205, 2),
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

	// Capture of all-edits-reverted for the saved chunk: the revert is
	// expressed by capturing the PROCEDURAL values (delta model); the
	// worker's diff produces an empty override set and the file is deleted.
	const uint64_t deletedBefore = wp.status().deleted;
	// Revert BOTH persisted edits to their procedural values; i3 already
	// holds its base value in desired.
	wp.captureChunkEdits(0, 0, {{i1, base[i1].type}, {i2, base[i2].type}});
	CHECK(wp.flush(), "flush revert");
	CHECK(!fileExists(chunkPath(chunks, 0, 0)), "fully reverted chunk file deleted");
	CHECK(wp.status().deleted >= deletedBefore + 1, "deletion counted");
	CHECK(wp.overridesSnapshot(0, 0).empty(), "index entry refined away after delete");

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
		const std::vector<worldsave::ChunkEdit> o =
			fresh.overridesSnapshot(e.coord.x, e.coord.z);
		const std::string label = "chunk " + std::to_string(e.coord.x) + "_" +
		                          std::to_string(e.coord.z);
		CHECK(!o.empty(), ("overrides exist for edited " + label).c_str());
		if (o.empty())
			continue;
		CHECK(o.size() == 2, ("exactly the two edits persisted for " + label).c_str());
		CHECK(containsEdit(o, idxOf(3, 200, 3), e.firstValue),
		      ("right first edit value for " + label).c_str());
		CHECK(containsEdit(o, idxOf(9, 210, 9), e.secondValue),
		      ("right second edit value for " + label).c_str());
	}

	CHECK(fresh.overridesSnapshot(untouched.x, untouched.z).empty(),
	      "never-edited chunk has no overrides");
	CHECK(!fileExists(chunkPath(chunksDir, untouched.x, untouched.z)),
	      "never-edited chunk has no file");
	CHECK(worldsave::scanChunkFiles(chunksDir).files.size() == edited.size(),
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
	CHECK(worldsave::scanChunkFiles(root).files.empty(), "no chunk files anywhere");
	removeDir(root);
}

// ---------------------------------------------------------------------------
// 8b. closeWorld quiesce: an edit deferred while its chunk is mid-job must
// land in the save (issue #180 review - the old capture-then-wait order
// lost it)
// ---------------------------------------------------------------------------

// Friend probe (same name-based access as test_chunk_lifecycle): lets the
// drain test observe whether the edit actually took the PendingVoxelEdit
// deferred path.
struct ChunkManagerStreamProbe
{
	static const std::vector<PendingVoxelEdit> &pendingEdits(const ChunkManager &m)
	{
		return m.m_pendingEdits;
	}
};

static void testCloseWorldDrainsPendingEdits()
{
	auto root = makeTempDir("drain");
	const std::string savesRoot = (root / "saves").string();
	const auto chunksDir = root / "saves" / "drain" / "chunks";
	const int seed = 42;

	TerrainGenerator gen(seed);
	ThreadPool threads(2);
	ChunkPool pool(64);
	ChunkManager mgr(&gen, &threads, &pool);
	std::string err;
	CHECK(mgr.openWorld(savesRoot, "drain", err), ("openWorld failed: " + err).c_str());

	const glm::ivec3 target(1, 0, 1);
	const glm::vec3 homePos(1.0f * CHUNK_SIZE + 8.0f, 100.0f, 1.0f * CHUNK_SIZE + 8.0f);
	Camera camera(homePos);
	RenderSettings settings;
	settings.minRenderDistance = 32;
	settings.maxRenderDistance = 48;
	mgr.updateStreaming(camera, settings);
	mgr.processChunkLoading(64);
	// Dispatch generation but do NOT drain: the target chunk is in transit.
	mgr.generatePendingVoxels(camera, settings, 16);

	// The edit races the in-flight job: it must be deferred as a
	// PendingVoxelEdit (or applied directly if the job already finished -
	// both paths must survive the close).
	const glm::vec3 editPos(1.0f * CHUNK_SIZE + 5.0f, 140.0f, 1.0f * CHUNK_SIZE + 5.0f);
	CHECK(mgr.placeVoxel(editPos, BRICKS), "edit accepted while chunk may be in transit");
	// Coverage note: fast machines can finish generation before the edit, in
	// which case it applies directly (still durable). Whichever path ran, the
	// close below must persist the edit.
	const bool deferredEdit = !ChunkManagerStreamProbe::pendingEdits(mgr).empty();
	if (!deferredEdit)
		CHECK(mgr.pendingGenJobs() == 0, "direct apply only valid when no job is in flight");

	// closeWorld must quiesce (drain gen jobs, apply the deferred edit) and
	// THEN capture, so the deferred edit lands in the save.
	CHECK(mgr.closeWorld(), "closeWorld drained and flushed");

	// Reopen the world the way the next process would: a fresh facade on the
	// same directory must carry the edit.
	WorldPersistence verify;
	const WorldPersistence::OpenInfo info =
		verify.openOrCreate(savesRoot, "drain", seed, TerrainGenerator::kGeneratorVersion);
	CHECK(info.ok && !info.created, "world reopens after close");

	const int cx = target.x, cz = target.z;
	const std::vector<Voxel> base = gen.generateChunk(cx, cz).voxels;
	const uint32_t editIdx = idxOf(5, 140, 5);
	std::vector<worldsave::ChunkEdit> saved = verify.overridesSnapshot(cx, cz);
	CHECK(!saved.empty(), "deferred edit was captured by closeWorld");
	bool found = false;
	for (const worldsave::ChunkEdit &e : saved)
		if (e.localIndex == editIdx)
		{
			found = true;
			const uint8_t expected = diffAgainstBase(base, {{editIdx, static_cast<uint8_t>(BRICKS)}})[0].blockType;
			CHECK(e.blockType == expected, "deferred edit value correct");
		}
	CHECK(found, "deferred edit index present in the saved overrides");
	verify.shutdown();

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
	const std::vector<worldsave::ChunkEdit> o = verify.overridesSnapshot(0, 0);
	CHECK(o.size() == 1 && o[0].localIndex == idxOf(8, 130, 8) &&
	          o[0].blockType == static_cast<uint8_t>(BRICKS),
	      "destructor flushed the recorded edit");
	verify.shutdown();

	removeDir(root);
}

// ---------------------------------------------------------------------------
// 8c. Queue bounds: heavy same-coordinate captures coalesce (never blocked),
// and more distinct coordinates than the bound stay dirty until flushed.
// ---------------------------------------------------------------------------

static void testQueueCoalescingAndBusy()
{
	TerrainGenerator gen(42);
	auto root = makeTempDir("queue");

	WorldPersistence wp;
	const WorldPersistence::OpenInfo info =
		wp.openOrCreate(root, "w", 42, TerrainGenerator::kGeneratorVersion);
	CHECK(info.ok, "world opened");

	// Stub the serialize step: a real base regeneration per request costs
	// seconds (full chunk gen), which is pointless for queue-behavior
	// coverage. The stub echoes the payload as the minimal diff.
	wp.setSerializeFnForTests(
		[](int32_t, int32_t, const std::vector<worldsave::ChunkEdit> &currentValues)
		{ return currentValues; });

	const std::vector<Voxel> base = gen.generateChunk(0, 0).voxels;
	const uint32_t ia = idxOf(3, 40, 5);
	const uint8_t va = pickNonBase(base[ia].type);

	// (a) Thousands of captures for ONE coordinate coalesce into at most one
	// pending job; the capture path never blocks on the queue.
	const auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < 5000; ++i)
		wp.captureChunkEdits(0, 0, {{ia, static_cast<uint8_t>(va + (i % 2))}});
	const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
	                           std::chrono::steady_clock::now() - t0)
	                           .count();
	CHECK(elapsedMs < 2000, "5000 same-coordinate captures never block the caller");
	CHECK(wp.flush(), "coalesced flush succeeds");
	{
		const WorldPersistence::Status st = wp.status();
		CHECK(st.queueDepth == 0, "queue drained after flush");
		CHECK(st.dirtyCoordinates == 0, "coordinate durable after flush");
		CHECK(st.completed >= 1, "the coalesced coordinate was written");
	}

	// (b) More DISTINCT coordinates than the queue bound: the overload is
	// rejected with Busy and stays dirty; flush retries until all durable.
	constexpr int kDistinct = 1100; // bound is 1024
	for (int i = 0; i < kDistinct; ++i)
	{
		const int32_t cx = static_cast<int32_t>(i % 50);
		const int32_t cz = static_cast<int32_t>(i / 50) - 20;
		wp.captureChunkEdits(cx, cz, {{static_cast<uint32_t>(ia + static_cast<uint32_t>(i)), va}});
	}
	{
		const WorldPersistence::Status st = wp.status();
		CHECK(st.queueDepth <= 1024, "queue bound holds (distinct coordinates)");
		CHECK(st.dirtyCoordinates > 0, "overflow coordinates stay dirty for retry");
	}
	CHECK(wp.flush(), "flush retries the Busy-rejected coordinates to completion");
	{
		const WorldPersistence::Status st = wp.status();
		CHECK(st.dirtyCoordinates == 0, "all coordinates durable after flush");
		CHECK(st.queueDepth == 0, "queue empty after flush");
	}

	wp.shutdown();
	removeDir(root);
}

// ---------------------------------------------------------------------------
// 8d. A chunk-directory scan error must REFUSE the open instead of being
// misread as "no overrides saved" (issue #180 review).
// ---------------------------------------------------------------------------

static void testScanErrorRefusesOpen()
{
	auto root = makeTempDir("scanerr");
	const uint32_t genVersion = TerrainGenerator::kGeneratorVersion;

	{
		WorldPersistence writer;
		const WorldPersistence::OpenInfo created =
			writer.openOrCreate(root, "w", 42, genVersion);
		CHECK(created.ok, "world created");
		writer.shutdown();
	}

	// Replace the chunks directory with a regular file: scanning it is a
	// real filesystem error, not an empty save set.
	const auto chunksDir = root / "w" / "chunks";
	std::error_code ec;
	std::filesystem::remove_all(chunksDir, ec);
	{
		std::ofstream file(chunksDir, std::ios::binary | std::ios::trunc);
		file.put(static_cast<char>(0x01));
		CHECK(!file.fail(), "chunks path replaced by a file");
	}

	WorldPersistence wp;
	const WorldPersistence::OpenInfo info = wp.openOrCreate(root, "w", 42, genVersion);
	CHECK(!info.ok, "open refused when the chunk directory cannot be scanned");
	// The refusal can come from the temp-clean pass or the scan pass; both
	// must name the I/O error rather than claim an empty save set.
	CHECK(info.error.find("I/O error") != std::string::npos, "scan error message");

	removeDir(root);
}

// ---------------------------------------------------------------------------
// 8c-bis. closeWorld refuses to destroy persistence while accepted edits are
// stranded outside authoritative voxel state (issue #180 review round 2).
// ---------------------------------------------------------------------------

static void testCloseWorldRefusesWhenEditsStranded()
{
	std::cout << "== Close refuses stranded edits ==" << std::endl;
	auto root = makeTempDir("stranded");
	const std::string savesRoot = (root / "saves").string();
	const int seed = 42;

	TerrainGenerator gen(seed);
	ThreadPool threads(2);
	ChunkPool pool(64);
	ChunkManager mgr(&gen, &threads, &pool);
	std::string err;
	CHECK(mgr.openWorld(savesRoot, "stranded", err), ("openWorld failed: " + err).c_str());

	Camera camera(glm::vec3(8.0f, 100.0f, 8.0f));
	RenderSettings settings;
	settings.minRenderDistance = 32;
	settings.maxRenderDistance = 48;
	mgr.updateStreaming(camera, settings);
	mgr.processChunkLoading(16);
	// Budget 0: chunks are acquired but NOTHING is dispatched - they stay
	// UNLOADED.
	mgr.generatePendingVoxels(camera, settings, 0);

	const glm::vec3 editPos(4.0f, 140.0f, 4.0f); // inside chunk (0,0)
	CHECK(mgr.placeVoxel(editPos, BRICKS), "edit accepted while chunk is unloaded");
	CHECK(mgr.hasPendingLogicalEdits(), "edit deferred as a PendingVoxelEdit");

	CHECK(!mgr.closeWorld(), "close refuses while accepted edits are stranded");
	CHECK(mgr.isWorldOpen(), "persistence stays open for a later retry");

	// Resolve: dispatch generation, let the deferred edit apply, then close.
	mgr.generatePendingVoxels(camera, settings, 16);
	while (mgr.pendingGenJobs() > 0)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		mgr.processFinishedJobs();
	}
	mgr.processFinishedJobs();
	CHECK(!mgr.hasPendingLogicalEdits(), "deferred edit applied after generation");
	CHECK(mgr.closeWorld(), "close succeeds once the stranded edit resolved");

	WorldPersistence verify;
	const WorldPersistence::OpenInfo info =
		verify.openOrCreate(savesRoot, "stranded", seed, TerrainGenerator::kGeneratorVersion);
	CHECK(info.ok, "world reopens");
	const uint32_t editIdx = idxOf(4, 140, 4);
	const std::vector<worldsave::ChunkEdit> saved = verify.overridesSnapshot(0, 0);
	bool found = false;
	for (const worldsave::ChunkEdit &e : saved)
		if (e.localIndex == editIdx)
			found = true;
	CHECK(found, "the stranded edit landed in the save after the retry");
	verify.shutdown();

	removeDir(root);
}

// ---------------------------------------------------------------------------
// 8c-ter. Destructor lifecycle (issue #180 review round 3, item 11): the
// destructor itself must publish completions, apply deferred edits and save
// - WITHOUT any manual processFinishedJobs() call.
// ---------------------------------------------------------------------------

static void testDestructorDrainsCompletionsBeforeTeardown()
{
	auto root = makeTempDir("dtordrain");
	const std::string savesRoot = (root / "saves").string();
	const int seed = 42;
	// Chunk (1,1); local (5,150,5).
	const glm::vec3 editPos(1.0f * CHUNK_SIZE + 5.0f, 150.0f, 1.0f * CHUNK_SIZE + 5.0f);
	const uint32_t editIdx = idxOf(5, 150, 5);

	{
		TerrainGenerator gen(seed);
		ThreadPool threads(2);
		ChunkPool pool(64);
		ChunkManager mgr(&gen, &threads, &pool);
		std::string err;
		CHECK(mgr.openWorld(savesRoot, "dtordrain", err), ("openWorld failed: " + err).c_str());

		Camera camera(glm::vec3(1.0f * CHUNK_SIZE + 8.0f, 100.0f, 1.0f * CHUNK_SIZE + 8.0f));
		RenderSettings settings;
		settings.minRenderDistance = 32;
		settings.maxRenderDistance = 48;
		mgr.updateStreaming(camera, settings);
		mgr.processChunkLoading(64);
		// Gen dispatched but NOT drained: chunk (1,1) is in transit when the
		// edit lands, so the edit is deferred as a PendingVoxelEdit.
		mgr.generatePendingVoxels(camera, settings, 16);
		CHECK(mgr.placeVoxel(editPos, BRICKS), "edit accepted while the gen job is in flight");

		// Deliberately NO processFinishedJobs, NO closeWorld, NO flush.
	} // ~ChunkManager: forceCloseWorldForShutdown must do all of it.

	WorldPersistence verify;
	const WorldPersistence::OpenInfo info =
		verify.openOrCreate(savesRoot, "dtordrain", seed, TerrainGenerator::kGeneratorVersion);
	CHECK(info.ok, "world reopens after the destructor");
	const std::vector<worldsave::ChunkEdit> saved = verify.overridesSnapshot(1, 1);
	bool found = false;
	for (const worldsave::ChunkEdit &e : saved)
		if (e.localIndex == editIdx && e.blockType == static_cast<uint8_t>(BRICKS))
			found = true;
	CHECK(found, "the edit accepted against an in-flight gen job was saved by the destructor");
	verify.shutdown();

	removeDir(root);
}

static void testDestructorPublishesUnpublishedMeshCompletions()
{
	auto root = makeTempDir("dtormesh");
	const std::string savesRoot = (root / "saves").string();
	const int seed = 42;
	// Chunk (0,0); local (6,160,3).
	const glm::vec3 editPos(6.0f, 160.0f, 3.0f);
	const uint32_t editIdx = idxOf(6, 160, 3);

	{
		TerrainGenerator gen(seed);
		ThreadPool threads(2);
		ChunkPool pool(64);
		ChunkManager mgr(&gen, &threads, &pool);
		std::string err;
		CHECK(mgr.openWorld(savesRoot, "dtormesh", err), ("openWorld failed: " + err).c_str());

		Camera camera(glm::vec3(8.0f, 100.0f, 8.0f));
		RenderSettings settings;
		settings.minRenderDistance = 32;
		settings.maxRenderDistance = 48;
		mgr.updateStreaming(camera, settings);
		mgr.processChunkLoading(64);
		mgr.generatePendingVoxels(camera, settings, 16);
		while (mgr.pendingGenJobs() > 0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		mgr.processFinishedJobs(); // publish generation (only this once)

		// Dispatch the mesh job and wait for the worker to finish WITHOUT
		// publishing: the completion sits in m_completedMeshJobs when the
		// destructor runs.
		mgr.meshPendingChunks(camera, settings, 16);
		while (mgr.pendingMeshJobs() > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		CHECK(mgr.placeVoxel(editPos, GLASS), "edit accepted against an unpublished mesh completion");

		// Deliberately NO processFinishedJobs: the destructor must publish
		// the mesh completion, apply the deferred edit and save.
	} // ~ChunkManager

	WorldPersistence verify;
	const WorldPersistence::OpenInfo info =
		verify.openOrCreate(savesRoot, "dtormesh", seed, TerrainGenerator::kGeneratorVersion);
	CHECK(info.ok, "world reopens after the destructor");
	const std::vector<worldsave::ChunkEdit> saved = verify.overridesSnapshot(0, 0);
	bool found = false;
	for (const worldsave::ChunkEdit &e : saved)
		if (e.localIndex == editIdx && e.blockType == static_cast<uint8_t>(GLASS))
			found = true;
	CHECK(found, "the edit accepted against an unpublished mesh completion was saved");
	verify.shutdown();

	removeDir(root);
}

// ---------------------------------------------------------------------------
// 8d-ter. Delta captures (issue #180 review round 5): takePersistentEdits()
// empties the chunk map at every capture, so payloads are DELTAS - the
// facade must merge them into desired, or the second flush loses the first
// edit.
// ---------------------------------------------------------------------------

static void testMultiFlushEditsAccumulate()
{
	std::cout << "== Multi-flush edits accumulate ==" << std::endl;
	TerrainGenerator gen(42);
	auto root = makeTempDir("multiflush");
	const auto chunks = root / "w" / "chunks";
	const std::vector<Voxel> base = gen.generateChunk(0, 0).voxels;

	const uint32_t ia = idxOf(3, 70, 3);
	const uint32_t ib = idxOf(9, 90, 6);
	const uint8_t va = static_cast<uint8_t>(BRICKS);
	const uint8_t vb = static_cast<uint8_t>(GLASS);
	// Sanity: both must be real changes (non-base) so the test is meaningful.
	CHECK(va != base[ia].type && vb != base[ib].type, "test edits differ from the procedural base");

	WorldPersistence wp;
	const WorldPersistence::OpenInfo info =
		wp.openOrCreate(root, "w", 42, TerrainGenerator::kGeneratorVersion);
	CHECK(info.ok, "world opened");

	// Save boundary 1: only A is known.
	wp.captureChunkEdits(0, 0, {{ia, va}});
	CHECK(wp.flush(), "first flush");
	{
		const WorldPersistence::Status st = wp.status();
		CHECK(st.dirtyCoordinates == 0, "coordinate durable after the first flush");
	}
	// Save boundary 2: only B is captured (A's map entry was taken at the
	// first capture - the payload is a DELTA).
	wp.captureChunkEdits(0, 0, {{ib, vb}});
	CHECK(wp.flush(), "second flush");
	{
		const WorldPersistence::Status st = wp.status();
		CHECK(st.dirtyCoordinates == 0, "coordinate durable after the second flush");
	}
	wp.shutdown();

	// Restart: a fresh facade must see A AND B.
	WorldPersistence verify;
	const WorldPersistence::OpenInfo reopen =
		verify.openOrCreate(root, "w", 42, TerrainGenerator::kGeneratorVersion);
	CHECK(reopen.ok && !reopen.created, "world reopens");
	const std::vector<worldsave::ChunkEdit> saved = verify.overridesSnapshot(0, 0);
	CHECK(saved.size() == 2, "BOTH edits survived the two flush boundaries");
	CHECK(containsEdit(saved, ia, va), "the first edit survived the second flush");
	CHECK(containsEdit(saved, ib, vb), "the second edit persisted");
	verify.shutdown();

	removeDir(root);
}

static void testRevertAcrossFlushes()
{
	std::cout << "== Revert across flushes ==" << std::endl;
	TerrainGenerator gen(42);
	auto root = makeTempDir("reverts");
	const auto chunks = root / "w" / "chunks";
	const std::vector<Voxel> base = gen.generateChunk(0, 0).voxels;
	const uint32_t ia = idxOf(2, 80, 2);
	const uint32_t ib = idxOf(11, 100, 7);
	const uint32_t ic = idxOf(6, 130, 13);
	const uint8_t baseA = base[ia].type;
	const uint8_t baseB = base[ib].type;
	const uint8_t va = static_cast<uint8_t>(BRICKS);
	const uint8_t vb = static_cast<uint8_t>(GLASS);
	const uint8_t vc = static_cast<uint8_t>(STONE);
	// Per-world chunk-file path: each case opens its own world directory, so
	// assertions must never look at a shared/foreign "chunks" root
	// (issue #180 review round 6, item 6).
	const auto chunkPathFor = [&](std::string_view world)
	{ return root / world / "chunks" / "0_0.chunk"; };

	// Case 1: edit A -> flush -> revert A to procedural -> flush => empty.
	{
		WorldPersistence wp;
		CHECK(wp.openOrCreate(root, "case1", 42, TerrainGenerator::kGeneratorVersion).ok,
		      "case1 opened");
		wp.captureChunkEdits(0, 0, {{ia, va}});
		CHECK(wp.flush(), "case1 first flush");
		wp.captureChunkEdits(0, 0, {{ia, baseA}});
		CHECK(wp.flush(), "case1 revert flush");
		CHECK(wp.status().dirtyCoordinates == 0, "case1 clean after revert");
		CHECK(wp.overridesSnapshot(0, 0).empty(), "case1 override refined away");
		CHECK(!fileExists(chunkPathFor("case1")), "case1 chunk file deleted");
		wp.shutdown();
	}

	// Case 2: edit A -> flush -> edit B -> flush -> revert A -> flush => B only.
	{
		WorldPersistence wp;
		CHECK(wp.openOrCreate(root, "case2", 42, TerrainGenerator::kGeneratorVersion).ok,
		      "case2 opened");
		wp.captureChunkEdits(0, 0, {{ia, va}});
		CHECK(wp.flush(), "case2 flush A");
		wp.captureChunkEdits(0, 0, {{ib, vb}});
		CHECK(wp.flush(), "case2 flush B");
		wp.captureChunkEdits(0, 0, {{ia, baseA}});
		CHECK(wp.flush(), "case2 revert flush");
		const std::vector<worldsave::ChunkEdit> saved = wp.overridesSnapshot(0, 0);
		CHECK(saved.size() == 1 && containsEdit(saved, ib, vb),
		      "case2 B present, A reverted away");
		CHECK(fileExists(chunkPathFor("case2")), "case2 file still exists");
		wp.shutdown();
	}

	// Case 3: A+B -> flush -> revert B -> flush -> edit C -> flush => A + C.
	{
		WorldPersistence wp;
		CHECK(wp.openOrCreate(root, "case3", 42, TerrainGenerator::kGeneratorVersion).ok,
		      "case3 opened");
		wp.captureChunkEdits(0, 0, {{ia, va}, {ib, vb}});
		CHECK(wp.flush(), "case3 flush A+B");
		wp.captureChunkEdits(0, 0, {{ib, baseB}});
		CHECK(wp.flush(), "case3 revert B flush");
		CHECK(wp.overridesSnapshot(0, 0).size() == 1, "case3 only A after the revert");
		wp.captureChunkEdits(0, 0, {{ic, vc}});
		CHECK(wp.flush(), "case3 flush C");
		const std::vector<worldsave::ChunkEdit> saved = wp.overridesSnapshot(0, 0);
		CHECK(saved.size() == 2 && containsEdit(saved, ia, va) && containsEdit(saved, ic, vc),
		      "case3 A + C after revert and re-edit");
		CHECK(wp.status().dirtyCoordinates == 0, "case3 fully durable at the end");
		wp.shutdown();
	}

	removeDir(root);
}

// ---------------------------------------------------------------------------
// 8d-quater. closeWorld retryability (issue #180 review round 5, item 3):
// a flush failure must keep the world open, tracking armed, and let a later
// close retry succeed.
// ---------------------------------------------------------------------------

static void testCloseWorldRetriesAfterFlushFailure()
{
	std::cout << "== CloseWorld retries after flush failure ==" << std::endl;
	auto root = makeTempDir("retryclose");
	const std::string savesRoot = (root / "saves").string();
	const int seed = 42;
	const glm::vec3 posA(8.0f, 140.0f, 8.0f);  // chunk (0,0), local (8,140,8)
	const glm::vec3 posB(10.0f, 145.0f, 10.0f);
	const uint32_t idxA = idxOf(8, 140, 8);
	const uint32_t idxB = idxOf(10, 145, 10);

	TerrainGenerator gen(seed);
	ThreadPool threads(2);
	ChunkPool pool(64);
	ChunkManager mgr(&gen, &threads, &pool);
	std::string err;
	CHECK(mgr.openWorld(savesRoot, "retry", err), ("openWorld failed: " + err).c_str());

	Camera camera(glm::vec3(8.0f, 100.0f, 8.0f));
	RenderSettings settings;
	settings.minRenderDistance = 32;
	settings.maxRenderDistance = 48;
	mgr.updateStreaming(camera, settings);
	mgr.processChunkLoading(64);
	mgr.generatePendingVoxels(camera, settings, 16);
	drainManagerJobs(mgr);
	CHECK(mgr.placeVoxel(posA, BRICKS), "edit A accepted");

	// Force every tmp write to fail, then close: the close must be REFUSED
	// and the world must stay fully usable.
	mgr.worldPersistence()->setWriteTmpFnForTests(
		[](const std::filesystem::path &, int32_t, int32_t,
		   const std::vector<worldsave::ChunkEdit> &, std::filesystem::path &)
		{ return worldsave::SaveStatus::IoError; });
	CHECK(!mgr.closeWorld(), "close refused on flush failure");
	CHECK(mgr.isWorldOpen(), "persistence stays open after the refused close");
	CHECK(mgr.worldPersistence() != nullptr, "save service still reachable");
	CHECK(mgr.worldPersistence()->status().dirtyCoordinates >= 1, "failed coordinate stays dirty");

	// Edit B AFTER the refused close: proves tracking was not disarmed.
	CHECK(mgr.placeVoxel(posB, GLASS), "tracking still armed after the refused close");

	// Clear the failure and close again: the retry must flush A (merged in
	// desired) plus the fresh B.
	mgr.worldPersistence()->setWriteTmpFnForTests(nullptr);
	CHECK(mgr.closeWorld(), "close succeeds after the failure is cleared");
	CHECK(!mgr.isWorldOpen(), "world closed on the retry");

	// Reopen: A AND B must be present.
	WorldPersistence verify;
	const WorldPersistence::OpenInfo info =
		verify.openOrCreate(savesRoot, "retry", seed, TerrainGenerator::kGeneratorVersion);
	CHECK(info.ok && !info.created, "world reopens after the retry close");
	const std::vector<worldsave::ChunkEdit> saved = verify.overridesSnapshot(0, 0);
	CHECK(saved.size() == 2, "both edits persisted across the refused close");
	CHECK(containsEdit(saved, idxA, static_cast<uint8_t>(BRICKS)), "edit A survived");
	CHECK(containsEdit(saved, idxB, static_cast<uint8_t>(GLASS)), "edit B (post-refusal) survived");
	verify.shutdown();

	removeDir(root);
}

static void testUnloadReloadAfterMultipleFlushes()
{
	std::cout << "== Unload/reload after multiple flushes ==" << std::endl;
	auto root = makeTempDir("multiflushmgr");
	const std::string savesRoot = (root / "saves").string();
	const int seed = 42;

	TerrainGenerator gen(seed);
	ThreadPool threads(2);
	ChunkPool pool(128);
	ChunkManager mgr(&gen, &threads, &pool);
	std::string err;
	CHECK(mgr.openWorld(savesRoot, "mfmgr", err), ("openWorld failed: " + err).c_str());

	const glm::vec3 homePos(8.0f, 100.0f, 8.0f);
	const glm::vec3 posA(4.0f, 140.0f, 4.0f);  // local (4,140,4)
	const glm::vec3 posB(6.0f, 145.0f, 6.0f);  // local (6,145,6)
	const uint32_t idxA = idxOf(4, 140, 4);
	const uint32_t idxB = idxOf(6, 145, 6);
	Camera camera(homePos);
	RenderSettings settings;
	settings.minRenderDistance = 32;
	settings.maxRenderDistance = 96;

	mgr.updateStreaming(camera, settings);
	mgr.processChunkLoading(64);
	mgr.generatePendingVoxels(camera, settings, 8);
	drainManagerJobs(mgr);

	// Save boundary 1: edit A, explicit flushWorld (the chunk edit map is
	// taken - afterwards the chunk map no longer knows A).
	CHECK(mgr.placeVoxel(posA, BRICKS), "edit A accepted");
	CHECK(mgr.flushWorld(), "first flushWorld");

	// Save boundary 2: edit B on the SAME live chunk, then the real UNLOAD
	// captures it as a delta.
	CHECK(mgr.placeVoxel(posB, GLASS), "edit B accepted after the first flush");

	camera.setPosition(glm::vec3(homePos.x + 4.0f * settings.maxRenderDistance, 100.0f,
	                             homePos.z + 4.0f * settings.maxRenderDistance));
	mgr.updateStreaming(camera, settings);
	drainDeferredReleases(mgr);
	CHECK(mgr.getChunk(glm::ivec3(0, 0, 0)) == nullptr, "chunk unloaded");
	{
		const WorldPersistence::Status st = mgr.worldPersistence()->status();
		CHECK(st.dirtyCoordinates == 0, "both edits durable after the unload capture");
	}

	// Reload the chunk: A AND B must come back from the save.
	camera.setPosition(homePos);
	mgr.updateStreaming(camera, settings);
	mgr.processChunkLoading(64);
	mgr.generatePendingVoxels(camera, settings, 8);
	drainManagerJobs(mgr);
	Chunk *chunk = mgr.getChunk(glm::ivec3(0, 0, 0));
	CHECK(chunk != nullptr && chunk->getState() >= ChunkState::GENERATED, "chunk reloaded");
	CHECK(chunk->getVoxel(4, 140, 4).getTextureType() == BRICKS, "A restored after unload/reload");
	CHECK(chunk->getVoxel(6, 145, 6).getTextureType() == GLASS, "B restored after unload/reload");

	CHECK(mgr.closeWorld(), "close");

	// Restart: a fresh facade sees both edits.
	WorldPersistence verify;
	const WorldPersistence::OpenInfo info =
		verify.openOrCreate(savesRoot, "mfmgr", seed, TerrainGenerator::kGeneratorVersion);
	CHECK(info.ok && !info.created, "world reopens after restart");
	const std::vector<worldsave::ChunkEdit> saved = verify.overridesSnapshot(0, 0);
	CHECK(saved.size() == 2, "A and B both persisted across flush + unload + restart");
	CHECK(containsEdit(saved, idxA, static_cast<uint8_t>(BRICKS)), "A in the save");
	CHECK(containsEdit(saved, idxB, static_cast<uint8_t>(GLASS)), "B in the save");
	verify.shutdown();

	removeDir(root);
}

static void testRestorePolicyWaitingForTerrain()
{
	std::cout << "== Restore policy: waitingForTerrain ==" << std::endl;
	auto root = makeTempDir("restorewait");
	const int seed = 42;

	TerrainGenerator gen(seed);
	ThreadPool threads(2);
	ChunkPool pool(64);
	ChunkManager mgr(&gen, &threads, &pool);
	std::string err;
	CHECK(mgr.openWorld((root / "saves").string(), "rw", err),
	      ("openWorld failed: " + err).c_str());

	Camera camera(glm::vec3(8.0f, 100.0f, 8.0f));
	RenderSettings settings;
	settings.minRenderDistance = 32;
	settings.maxRenderDistance = 48;
	mgr.updateStreaming(camera, settings);
	mgr.processChunkLoading(16);
	// Deliberately NO generatePendingVoxels: the acquired chunks stay
	// UNLOADED, so collision queries around them report unknown cells - the
	// exact "terrain unavailable at restore" situation.

	// The restore policy (Engine::restorePlayerState) is: ChunkCollisionView
	// + physics::recover on a candidate body. Same calls here - the
	// semantics under test are the shared ones, not an Engine private.
	// NB: the view holds the manager's shared lock for its LIFETIME - scope
	// it tightly so closeWorld() (exclusive lock) below cannot deadlock.
	{
		ChunkCollisionView world(mgr);
		physics::QueryStats queries;
		physics::Body candidate;
		candidate.position = glm::dvec3(8.0, 120.0, 8.0); // inside an UNLOADED chunk
		const glm::dvec3 original = candidate.position;
		const bool cleared = physics::recover(world, candidate, queries);
		CHECK(!cleared, "restore over unavailable terrain does not clear");
		CHECK(candidate.waitingForTerrain, "restore flags waitingForTerrain");
		CHECK(glm::length(candidate.position - original) < 1e-9,
		      "no speculative teleport: position kept verbatim");

		// A distant generated-free position behaves the same (no chunk at all).
		// NB: "far" is a legacy macro on Windows - use distantBody.
		physics::Body distantBody;
		distantBody.position = glm::dvec3(5000.0, 120.0, 5000.0);
		const bool distantCleared = physics::recover(world, distantBody, queries);
		CHECK(!distantCleared && distantBody.waitingForTerrain,
		      "absent chunk also waits, never teleports");
	}

	CHECK(mgr.closeWorld(), "close");
	removeDir(root);
}

static void testSaveStatusAfterRetry()
{
	std::cout << "== Save status after retry ==" << std::endl;
	TerrainGenerator gen(42);
	auto root = makeTempDir("uistatus");
	const auto chunks = root / "w" / "chunks";
	std::filesystem::create_directories(chunks);
	const std::vector<Voxel> base = gen.generateChunk(0, 0).voxels;
	const uint32_t ia = idxOf(6, 75, 6);
	const uint8_t va = pickNonBase(base[ia].type);

	WorldPersistence wp;
	const WorldPersistence::OpenInfo info =
		wp.openOrCreate(root, "w", 42, TerrainGenerator::kGeneratorVersion);
	CHECK(info.ok, "world opened");

	// Save fails deterministically: only the FIRST attempt (the flush's
	// bounded retry rounds would otherwise each fail and inflate the
	// historical counter).
	// Failing write, observed WITHOUT flush: flush() is the retry point, so
	// the active-failure window must be sampled from status() directly.
	std::atomic<int> writeAttempts{0};
	wp.setWriteTmpFnForTests(
		[&writeAttempts](const std::filesystem::path &, int32_t, int32_t,
		                 const std::vector<worldsave::ChunkEdit> &,
		                 std::filesystem::path &)
		{ return worldsave::SaveStatus::IoError; });
	wp.captureChunkEdits(0, 0, {{ia, va}});
	{
		const WorldPersistence::Status *observed = nullptr;
		WorldPersistence::Status st;
		for (int i = 0; i < 5000; ++i)
		{
			st = wp.status();
			if (st.failedCoordinates == 1)
			{
				observed = &st;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		CHECK(observed != nullptr, "active failure observed via status()");
		CHECK(st.failed == 1, "historical failed counter == 1");
		CHECK(st.dirtyCoordinates == 1, "the coordinate stays dirty");
	}

	// Clear the failure: flush() is THE retry point. Its FIRST barrier
	// accounts the never-flushed failure (returns false by contract) while
	// the bounded retry rounds inside it already re-write the coordinate.
	wp.setWriteTmpFnForTests(nullptr);
	CHECK(!wp.flush(), "first flush accounts the failure range");
	{
		const WorldPersistence::Status st = wp.status();
		CHECK(st.failedCoordinates == 0, "failedCoordinates cleared by the retry");
		CHECK(st.dirtyCoordinates == 0, "coordinate durable after the retry");
		CHECK(st.failed == 1, "historical failed counter kept");
	}
	CHECK(wp.flush(), "subsequent flush is true");

	// Pure UI-health matrix (issue #180 review round 7, item 1).
	CHECK(computeSaveUiHealth(0, 0, 0) == SaveUiHealth::Saved, "0/0/0 -> Saved");
	CHECK(computeSaveUiHealth(1, 0, 0) == SaveUiHealth::Saving, "dirty -> Saving");
	CHECK(computeSaveUiHealth(0, 0, 3) == SaveUiHealth::Saving, "queue -> Saving");
	CHECK(computeSaveUiHealth(1, 1, 0) == SaveUiHealth::Failed, "active failure -> Failed");
	// Historical failure alone must NOT keep the label Failed.
	CHECK(computeSaveUiHealth(0, 0, 0) == SaveUiHealth::Saved,
	      "historical failure only -> Saved");

	wp.shutdown();
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
	testWriteFailureRetry();
	testBarrierOrderingIndependentOfRevisions();
	testFlushFailureCheckpoint();
	testStaleDeleteNeverRemovesAuthoritativeFile();
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
	testCloseWorldDrainsPendingEdits();
	testCloseWorldRefusesWhenEditsStranded();
	testCloseWorldRetriesAfterFlushFailure();
	testMultiFlushEditsAccumulate();
	testRevertAcrossFlushes();
	testUnloadReloadAfterMultipleFlushes();
	testRestorePolicyWaitingForTerrain();
	testSaveStatusAfterRetry();
	testDestructorDrainsCompletionsBeforeTeardown();
	testDestructorPublishesUnpublishedMeshCompletions();
	testQueueCoalescingAndBusy();
	testScanErrorRefusesOpen();
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
