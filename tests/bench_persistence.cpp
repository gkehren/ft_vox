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
               const std::filesystem::path &savesRoot, WorldPersistence::Status *outStatus)
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
	settings.minRenderDistance = 96;
	settings.maxRenderDistance = 256; // streaming stays active throughout

	// Circular camera path: one revolution over the session, ~96 blocks
	// radius at walk height - constant chunk churn in and out.
	const glm::vec3 center(0.0f, 100.0f, 0.0f);
	const float pathRadius = 96.0f;

	std::vector<double> frameMs;
	frameMs.reserve(static_cast<size_t>(durationSec * 120.0) + 16);
	std::vector<std::pair<int, int>> editStats; // attempted vs accepted per frame

	Clock::time_point t0 = Clock::now();
	Clock::time_point prev = t0;
	Clock::time_point lastFlush = t0;
	double loadAccum = 0.0, genAccum = 0.0, meshAccum = 0.0;
	double editAccum = 0.0;
	int editColumn = 0; // rotating column index within a chunk (16 columns)

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
		prev = now;
		const Clock::time_point frameStart = Clock::now();

		// Camera on the circle, facing tangentially.
		const double angle = 2.0 * 3.14159265358979 * elapsed / std::max(1.0, durationSec);
		const glm::vec3 pos(center.x + pathRadius * std::cos(static_cast<float>(angle)),
		                    100.0f,
		                    center.z + pathRadius * std::sin(static_cast<float>(angle)));
		camera.setPosition(pos);
		camera.setYawPitch(static_cast<float>(angle * 180.0 / 3.14159265358979 + 90.0), 0.0f);

		// Same per-frame streaming order as Engine::tickStreaming.
		mgr.processFinishedJobs();
		mgr.processDeferredReleases();
		mgr.updateStreaming(camera, settings);
		mgr.processChunkLoading(budgetFromRate(settings.loadPerSec, dt, loadAccum));
		mgr.generatePendingVoxels(camera, settings, budgetFromRate(settings.genPerSec, dt, genAccum));
		mgr.meshPendingChunks(camera, settings, budgetFromRate(settings.meshPerSec, dt, meshAccum));

		// Deterministic edit workload: rotate over columns of chunks near the
		// camera; each accepted edit either deletes the topmost solid voxel
		// of the column or places one above it (guaranteed-accepted change).
		editAccum += editsPerSec * dt;
		int attempted = 0, accepted = 0;
		while (editAccum >= 1.0)
		{
			++attempted;
			editAccum -= 1.0;
			const int ecx = static_cast<int>(pos.x) / 16 + (editColumn % 3) - 1;
			const int ecz = static_cast<int>(pos.z) / 16 + (editColumn / 3 % 3) - 1;
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
		if (attempted > 0)
			editStats.push_back({attempted, accepted});

		if (persistence && std::chrono::duration<double>(now - lastFlush).count() >= 2.0)
		{
			// The flushWorld capture is main-thread (like the engine's
			// shutdown/reload paths) and its cost BELONGS in frameMs.
			mgr.flushWorld();
			lastFlush = Clock::now();
		}

		const double ms = std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count();
		frameMs.push_back(ms);

		// Pace the loop at ~60 Hz; the sleep is excluded from frameMs.
		constexpr auto kFrameBudget = std::chrono::milliseconds(16);
		const auto spent = Clock::now() - frameStart;
		if (spent < kFrameBudget)
			std::this_thread::sleep_for(kFrameBudget - spent);
	}

	std::cout << "  session wall=" << std::chrono::duration<double>(Clock::now() - t0).count()
	          << "s frames=" << frameMs.size()
	          << " chunks=" << mgr.chunkCount()
	          << " gen=" << mgr.pendingGenJobs()
	          << " mesh=" << mgr.pendingMeshJobs() << std::endl;
	{
		int generated = 0, unloaded = 0;
		for (Chunk *c : mgr.activeChunksSnapshot())
			(c && c->getState() >= ChunkState::GENERATED) ? ++generated : ++unloaded;
	}
	if (persistence)
	{
		// Periodic flush (issue #180 review round 6, item 9): the save
		// worker must be measured UNDER load, not only on the final close.
		if (mgr.flushWorld())
		if (outStatus)
			*outStatus = mgr.worldPersistence()->status();
		mgr.closeWorld();
	}

	int attemptedTotal = 0, acceptedTotal = 0;
	for (const auto &e : editStats)
	{
		attemptedTotal += e.first;
		acceptedTotal += e.second;
	}
	std::cout << "  edits: attempted=" << attemptedTotal << " accepted=" << acceptedTotal
	          << std::endl;
	const FrameStats fs = summarize(frameMs);
	std::printf("  frames=%zu avg=%.3fms p50=%.3f p95=%.3f p99=%.3f max=%.3f\n",
	            fs.count, fs.avg, fs.p50, fs.p95, fs.p99, fs.max);
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

	std::printf("A. baseline (persistence off, same edits/streaming):\n");
	if (runSession(false, durationSec, editsPerSec, seed, root, nullptr) != 0)
		return 1;

	WorldPersistence::Status st;
	std::printf("B. persistence active (open world + same edits, flush every 2s):\n");
	if (runSession(true, durationSec, editsPerSec, seed, root, &st) != 0)
		return 1;
	printSaveStats(st);

	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	return 0;
}
