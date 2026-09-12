// Persistence stress benchmark (issue #180 review round 6, items 8-13).
// NOT part of the default ctest suite: run manually from the build dir,
//
//     bench_persistence.exe [durationSec] [editsPerSec] [seed]
//
// It runs the SAME headless streaming loop twice over the same camera path -
// baseline (persistence off) and persistence active (real saves to a temp
// directory, deterministic edit workload, real flush on close) - and prints
// CPU frame-time percentiles plus the save-worker statistics, so frame-time
// regression and queue boundedness can be validated together.
//
// Filesystem note (item 13): every worldsave::writeChunkFileTmp /
// commitChunkFile / removeChunkFile call site lives in SaveService's worker
// thread; the main thread only ever samples immutable stats snapshots.
#include <Chunk/ChunkManager.hpp>
#include <World/WorldPersistence.hpp>
#include <Engine/Profiler.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

struct FrameStats
{
	size_t count = 0;
	double avg = 0.0, p50 = 0.0, p95 = 0.0, p99 = 0.0, max = 0.0;
};

// Split save statistics for one run (issue #180 review round 8, item 1):
// `steadyState` is sampled at the exact end of the measured window (async
// unload-capture work only) and must never be rewritten afterwards;
// `final` is sampled after the explicit flush; flush/close latencies are
// measured separately.
struct PersistenceRunStats
{
	WorldPersistence::Status steadyState;
	WorldPersistence::Status final;
	double flushMs{0.0};
	double closeMs{0.0};
};

FrameStats summarize(std::vector<double> &samples)
{
	FrameStats s;
	if (samples.empty())
		return s;
	std::sort(samples.begin(), samples.end());
	s.count = samples.size();
	s.avg = 0.0;
	for (double v : samples)
		s.avg += v;
	s.avg /= static_cast<double>(samples.size());
	auto pct = [&](double p) {
		const size_t idx = static_cast<size_t>(p * static_cast<double>(samples.size() - 1));
		return samples[std::min(idx, samples.size() - 1)];
	};
	s.p50 = pct(0.50);
	s.p95 = pct(0.95);
	s.p99 = pct(0.99);
	s.max = samples.back();
	return s;
}

int budgetFromRate(int perSec, double dt, double &accum)
{
	accum += static_cast<double>(perSec) * dt;
	const int budget = static_cast<int>(accum);
	accum -= static_cast<double>(budget);
	return budget;
}

int runSession(bool persistence, double durationSec, double editsPerSec, int seed,
               const std::filesystem::path &savesRoot, PersistenceRunStats *outStats)
{
	TerrainGenerator gen(seed);
	const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
	ThreadPool threads(hw);
	ChunkPool pool(256);
	ChunkManager mgr(&gen, &threads, &pool);

	std::string err;
	if (persistence)
	{
		if (!mgr.openWorld(savesRoot.string(), "stress", err))
		{
			std::cerr << "openWorld failed: " << err << "\n";
			return 1;
		}
	}

	Camera camera(glm::vec3(8.0f, 100.0f, 8.0f));
	RenderSettings settings;
	// Small distances so edited chunks regularly FALL BEHIND the camera and
	// unload -> capture -> async-save (the steady-state path under test,
	// issue #180 review round 7, item 2).
	settings.minRenderDistance = 48;
	settings.maxRenderDistance = 96;

	// Forward streaming path (issue #180 review round 7, item 2): constant
	// 24 blocks/s advance with a gentle lateral sway - edited chunks end up
	// behind the player and unload naturally.
	const float speed = 24.0f;

	std::vector<double> frameMs;
	std::vector<double> streamingMs;
	frameMs.reserve(static_cast<size_t>(durationSec * 120.0) + 16);

	Clock::time_point t0 = Clock::now();
	double loadAccum = 0.0, genAccum = 0.0, meshAccum = 0.0;
	double editAccum = 0.0;
	int editColumn = 0; // rotating column index within a chunk (48 columns)
	int attemptedTotal = 0, acceptedTotal = 0;

	while (true)
	{
		const Clock::time_point now = Clock::now();
		const double elapsed = std::chrono::duration<double>(now - t0).count();
		if (elapsed >= durationSec)
			break;
		// Nominal 60 Hz pacing: budgets need a realistic dt (an unfixed dt at
		// multi-MHz loop speeds never accumulates to one unit), and the wall
		// sleep below keeps the loop at a representative cadence. frameMs
		// measures ONLY the tick work, never the sleep.
		const double dt = 1.0 / 60.0;
		const Clock::time_point frameStart = Clock::now();

		const glm::vec3 pos(static_cast<float>(elapsed * speed), 100.0f,
		                    std::sin(static_cast<float>(elapsed * 0.3)) * 48.0f);
		camera.setPosition(pos);

		// Same per-frame streaming order as Engine::tickStreaming; the whole
		// streaming block is timed separately (issue #180 review round 7,
		// item 3).
		const Clock::time_point streamingStart = Clock::now();
		mgr.processFinishedJobs();
		mgr.processDeferredReleases();
		mgr.updateStreaming(camera, settings);
		mgr.processChunkLoading(budgetFromRate(settings.loadPerSec, dt, loadAccum));
		mgr.generatePendingVoxels(camera, settings, budgetFromRate(settings.genPerSec, dt, genAccum));
		mgr.meshPendingChunks(camera, settings, budgetFromRate(settings.meshPerSec, dt, meshAccum));
		const double streamingNow =
			std::chrono::duration<double, std::milli>(Clock::now() - streamingStart).count();
		streamingMs.push_back(streamingNow);

		// Deterministic edit workload: rotate over columns of chunks near the
		// camera using the CANONICAL world->chunk mapping (issue #180 review
		// round 7, item 4 - plain integer division is wrong for negative
		// coordinates); each accepted edit either deletes the topmost solid
		// voxel of the column or places one above it.
		editAccum += editsPerSec * dt;
		int attempted = 0, accepted = 0;
		while (editAccum >= 1.0)
		{
			editAccum -= 1.0;
			++attempted;
			const glm::ivec3 cameraChunk = ChunkManager::worldToChunkCoord(pos);
			const int ecx = cameraChunk.x + (editColumn % 3) - 1;
			const int ecz = cameraChunk.z + (editColumn / 3 % 3) - 1;
			if (Chunk *chunk = mgr.getChunk(glm::ivec3(ecx, 0, ecz));
			    chunk && chunk->getState() >= ChunkState::GENERATED)
			{
				const int lx = (editColumn * 7) % 16;
				const int lz = (editColumn * 13) % 16;
				int solidY = -1;
				for (int y = static_cast<int>(CHUNK_HEIGHT) - 2; y > 0; --y)
					if (chunk->getVoxel(lx, y, lz).type != static_cast<uint8_t>(AIR))
					{
						solidY = y;
						break;
					}
				if (solidY > 0)
				{
					const glm::vec3 worldPos(static_cast<float>(ecx * 16 + lx),
					                         static_cast<float>(solidY),
					                         static_cast<float>(ecz * 16 + lz));
					accepted += mgr.deleteVoxel(worldPos) ? 1 : 0;
				}
			}
			editColumn = (editColumn + 1) % 48;
		}
		attemptedTotal += attempted;
		acceptedTotal += accepted;

		const double ms = std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count();
		frameMs.push_back(ms);

		// Pace the loop at ~60 Hz; the sleep is excluded from frameMs.
		constexpr auto kFrameBudget = std::chrono::milliseconds(16);
		const auto spent = Clock::now() - frameStart;
		if (spent < kFrameBudget)
			std::this_thread::sleep_for(kFrameBudget - spent);
	}

	std::printf("  edits: attempted=%d accepted=%d (deltas captured on unload)\n",
	            attemptedTotal, acceptedTotal);
	{
		int generated = 0;
		for (const Chunk *c : mgr.activeChunksSnapshot())
			if (c && c->getState() >= ChunkState::GENERATED)
				++generated;
		std::printf("  session: chunks=%d generated=%d\n", mgr.chunkCount(), generated);
	}

	// Steady-state snapshot FIRST, at the exact end of the measured window
	// (issue #180 review round 8, item 1): it covers only async
	// unload-capture work and is never rewritten afterwards.
	if (persistence && outStats)
		outStats->steadyState = mgr.worldPersistence()->status();

	// Explicit flush + close OUTSIDE the measured window, timed separately
	// (issue #180 review round 8, item 3): flushWorld drains the remaining
	// dirty coordinates; the closeWorld after it is NOT a cold Save & Quit
	// (everything is already durable) - the pair is reported as an
	// "explicit flush + close sequence".
	if (persistence)
	{
		const Clock::time_point flushStart = Clock::now();
		const bool flushed = mgr.flushWorld();
		const double flushMs =
			std::chrono::duration<double, std::milli>(Clock::now() - flushStart).count();
		if (outStats)
		{
			outStats->flushMs = flushMs;
			outStats->final = mgr.worldPersistence()->status();
		}

		const Clock::time_point closeStart = Clock::now();
		const bool closed = mgr.closeWorld();
		const double closeMs =
			std::chrono::duration<double, std::milli>(Clock::now() - closeStart).count();
		if (outStats)
			outStats->closeMs = closeMs;
		if (!flushed)
		{
			std::cerr << "benchmark invalid: final persistence flush failed" << std::endl;
			return 1;
		}
		if (!closed)
		{
			std::cerr << "benchmark invalid: persistent world close failed" << std::endl;
			return 1;
		}
	}

	const FrameStats fs = summarize(frameMs);
	std::printf("  frame:    n=%zu avg=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f (ms)\n",
	            fs.count, fs.avg, fs.p50, fs.p95, fs.p99, fs.max);
	const FrameStats ss = summarize(streamingMs);
	std::printf("  streaming: avg=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f (ms)\n",
	            ss.avg, ss.p50, ss.p95, ss.p99, ss.max);
	return 0;
}

void printSaveStats(const WorldPersistence::Status &st)
{
	std::printf("  save-worker: enqueued=%llu completed=%llu superseded=%llu failed=%llu deleted=%llu\n",
	            (unsigned long long)st.enqueued, (unsigned long long)st.completed,
	            (unsigned long long)st.superseded, (unsigned long long)st.failed,
	            (unsigned long long)st.deleted);
	std::printf("  queue: depth=%zu peak=%zu busyQueueFull=%llu busyCommitGate=%llu\n",
	            st.queueDepth, st.queueDepthPeak, (unsigned long long)st.rejectedBusyQueueFull,
	            (unsigned long long)st.rejectedBusyCommitGate);
	std::printf("  bytes=%llu serialize avg=%.3f max=%.3f ms | write avg=%.3f max=%.3f ms\n",
	            (unsigned long long)st.bytesWritten, st.avgSerializeMs, st.maxSerializeMs,
	            st.avgWriteMs, st.maxWriteMs);
	if (!st.lastError.empty())
		std::printf("  lastError: %s\n", st.lastError.c_str());
}
} // namespace

int main(int argc, char **argv)
{
	double durationSec = 20.0;
	double editsPerSec = 200.0;
	int seed = 42;
	if (argc > 1)
		durationSec = std::max(5.0, std::atof(argv[1]));
	if (argc > 2)
		editsPerSec = std::max(0.0, std::atof(argv[2]));
	if (argc > 3)
		seed = std::atoi(argv[3]);

	std::filesystem::path root = std::filesystem::temp_directory_path() /
	                             ("ft-vox-bench-persistence-" +
	                              std::to_string(std::chrono::steady_clock::now()
	                                                 .time_since_epoch()
	                                                 .count()));

	std::printf("Persistence stress benchmark (seed %d, %.0fs per run, %.0f edits/s target)\n",
	            seed, durationSec, editsPerSec);

	std::printf("A. baseline steady-state (persistence off, same edits/streaming):\n");
	if (runSession(false, durationSec, editsPerSec, seed, root, nullptr) != 0)
		return 1;

	PersistenceRunStats stats;
	std::printf("B. persistence async steady-state (same edits/streaming, no forced flush):\n");
	if (runSession(true, durationSec, editsPerSec, seed, root, &stats) != 0)
		return 1;

	// Validity gate on the STEADY-STATE snapshot only (issue #180 review
	// round 8, item 2): it proves dirty chunk -> unload -> capture ->
	// enqueue -> SaveService -> disk happened DURING the measured window,
	// not during the final flush. A soft warning on the queue peak stays a
	// warning - a fast worker can legitimately never be observed non-empty.
	const WorldPersistence::Status &steady = stats.steadyState;
	if (steady.enqueued == 0)
	{
		std::cerr << "benchmark invalid: no async saves were enqueued\n";
		return 1;
	}
	if (steady.completed + steady.deleted == 0)
	{
		std::cerr << "benchmark invalid: no async saves completed during the measured window\n";
		return 1;
	}
	if (steady.queueDepthPeak == 0)
		std::printf("  WARNING: queue peak == 0 (saves never overlapped)\n");

	std::printf("\nSteady-state save worker (before the final flush):\n");
	printSaveStats(steady);
	std::printf("\nAfter explicit final flush:\n");
	std::printf("  completed=%llu deleted=%llu failed=%llu remainingDirty=%llu\n",
	            (unsigned long long)stats.final.completed,
	            (unsigned long long)stats.final.deleted,
	            (unsigned long long)stats.final.failed,
	            (unsigned long long)stats.final.dirtyCoordinates);
	std::printf("\nExplicit flush + close sequence:\n");
	std::printf("  explicit flush latency: %.1f ms\n", stats.flushMs);
	std::printf("  close-after-flush latency: %.1f ms\n", stats.closeMs);
	std::printf("  total shutdown latency: %.1f ms\n", stats.flushMs + stats.closeMs);

	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	return 0;
}
