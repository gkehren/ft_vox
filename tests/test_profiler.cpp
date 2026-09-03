#include "Engine/Profiler.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

#define TEST_CHECK(expr)                                                               \
	do                                                                                 \
	{                                                                                  \
		if (!(expr))                                                                   \
		{                                                                              \
			std::cerr << "Assertion failed: " #expr " at " __FILE__ ":" << __LINE__    \
					  << std::endl;                                                    \
			std::abort();                                                              \
		}                                                                              \
	} while (0)

namespace
{

void test_basic_worker_snapshot()
{
	std::cout << "[Test 1] Basic worker snapshot..." << std::endl;
	Profiler prof;
	TEST_CHECK(prof.enabled());

	// Initial snapshot before samples should have 0 counts
	prof.snapshotWorkers();
	TEST_CHECK(prof.workerSnapshotCount() >= 3);
	for (int i = 0; i < prof.workerSnapshotCount(); ++i)
	{
		const WorkerSnapshot &s = prof.workerSnapshots()[i];
		TEST_CHECK(s.count == 0);
		TEST_CHECK(s.totalUs == 0);
		TEST_CHECK(s.totalMs == 0.f);
		TEST_CHECK(s.avgMs == 0.f);
	}

	// Add 3 samples to TerrainGen (1.5ms each = 1500 us)
	prof.addWorkerSample("TerrainGen", 1.5f);
	prof.addWorkerSample("TerrainGen", 1.5f);
	prof.addWorkerSample("TerrainGen", 1.5f);

	// Add 2 samples to MeshBuild (2.0ms each = 2000 us)
	prof.addWorkerSample("MeshBuild", 2.0f);
	prof.addWorkerSample("MeshBuild", 2.0f);

	prof.snapshotWorkers();

	bool foundTerrain = false;
	bool foundMesh = false;
	for (int i = 0; i < prof.workerSnapshotCount(); ++i)
	{
		const WorkerSnapshot &s = prof.workerSnapshots()[i];
		if (s.name && std::strcmp(s.name, "TerrainGen") == 0)
		{
			foundTerrain = true;
			TEST_CHECK(s.count == 3);
			TEST_CHECK(s.totalUs == 4500);
			TEST_CHECK(std::abs(s.totalMs - 4.5f) < 1e-4f);
			TEST_CHECK(std::abs(s.avgMs - 1.5f) < 1e-4f);
		}
		else if (s.name && std::strcmp(s.name, "MeshBuild") == 0)
		{
			foundMesh = true;
			TEST_CHECK(s.count == 2);
			TEST_CHECK(s.totalUs == 4000);
			TEST_CHECK(std::abs(s.totalMs - 4.0f) < 1e-4f);
			TEST_CHECK(std::abs(s.avgMs - 2.0f) < 1e-4f);
		}
	}
	TEST_CHECK(foundTerrain && foundMesh);

	// Next snapshot without new samples should drain to 0
	prof.snapshotWorkers();
	for (int i = 0; i < prof.workerSnapshotCount(); ++i)
	{
		const WorkerSnapshot &s = prof.workerSnapshots()[i];
		TEST_CHECK(s.count == 0);
		TEST_CHECK(s.totalUs == 0);
	}

	std::cout << "  -> Passed!" << std::endl;
}

void test_concurrent_worker_snapshot_consistency()
{
	std::cout << "[Test 2] Concurrent worker snapshot consistency stress..." << std::endl;
	Profiler prof;

	constexpr int kNumThreads = 6;
	constexpr int kItersPerThread = 40000;

	struct JobConfig
	{
		const char *name;
		float ms;
		uint64_t expectedUs;
	};

	const JobConfig jobs[3] = {
		{"TerrainGen", 1.0f, 1000},
		{"MeshBuild", 2.5f, 2500},
		{"MeshLOD", 0.5f, 500}
	};

	std::atomic<bool> doneWorkers{false};
	std::atomic<uint64_t> snapshotSplits{0};

	uint64_t globalCount[3] = {0, 0, 0};
	uint64_t globalUs[3] = {0, 0, 0};

	auto recordSnapshot = [&](const Profiler &p) {
		const int sc = p.workerSnapshotCount();
		const WorkerSnapshot *snaps = p.workerSnapshots();
		for (int i = 0; i < sc; ++i)
		{
			const WorkerSnapshot &s = snaps[i];
			if (!s.name)
				continue;
			for (int j = 0; j < 3; ++j)
			{
				if (std::strcmp(s.name, jobs[j].name) == 0)
				{
					if (s.count > 0)
					{
						// Individual snapshot check: count and totalUs must NEVER split!
						if (s.count * jobs[j].expectedUs != s.totalUs)
						{
							snapshotSplits.fetch_add(1, std::memory_order_relaxed);
						}
					}
					globalCount[j] += s.count;
					globalUs[j] += s.totalUs;
					break;
				}
			}
		}
	};

	// Dedicated thread repeatedly snapshotting while workers are hammering
	std::thread snapThread([&]() {
		while (!doneWorkers.load(std::memory_order_relaxed))
		{
			prof.snapshotWorkers();
			recordSnapshot(prof);
			std::this_thread::yield();
		}
	});

	std::vector<std::thread> workers;
	workers.reserve(kNumThreads);
	for (int t = 0; t < kNumThreads; ++t)
	{
		workers.emplace_back([&prof, t, &jobs]() {
			const int jobIdx = t % 3;
			for (int i = 0; i < kItersPerThread; ++i)
			{
				prof.addWorkerSample(jobs[jobIdx].name, jobs[jobIdx].ms);
			}
		});
	}

	for (auto &w : workers)
		w.join();

	doneWorkers.store(true, std::memory_order_relaxed);
	snapThread.join();

	// Drain both double-buffer slots
	prof.snapshotWorkers();
	recordSnapshot(prof);
	prof.snapshotWorkers();
	recordSnapshot(prof);

	std::cout << "  Snapshot split violations detected: " << snapshotSplits.load() << std::endl;
	TEST_CHECK(snapshotSplits.load() == 0);

	for (int j = 0; j < 3; ++j)
	{
		int threadsForJob = 0;
		for (int t = 0; t < kNumThreads; ++t)
		{
			if (t % 3 == j)
				++threadsForJob;
		}
		const uint64_t expectedCount = static_cast<uint64_t>(threadsForJob) * kItersPerThread;
		const uint64_t expectedTotalUs = expectedCount * jobs[j].expectedUs;

		std::cout << "  Job [" << jobs[j].name << "]: count=" << globalCount[j]
				  << " (expected " << expectedCount << "), totalUs=" << globalUs[j]
				  << " (expected " << expectedTotalUs << ")" << std::endl;

		TEST_CHECK(globalCount[j] == expectedCount);
		TEST_CHECK(globalUs[j] == expectedTotalUs);
	}

	std::cout << "  -> Passed!" << std::endl;
}

void test_concurrent_registration_and_sampling()
{
	std::cout << "[Test 3] Concurrent registration and sampling..." << std::endl;
	Profiler prof;

	std::atomic<bool> startFlag{false};
	std::atomic<bool> stopFlag{false};

	std::vector<std::thread> workers;
	for (int i = 0; i < 4; ++i)
	{
		workers.emplace_back([&prof, &startFlag, &stopFlag]() {
			while (!startFlag.load(std::memory_order_relaxed))
				std::this_thread::yield();
			while (!stopFlag.load(std::memory_order_relaxed))
			{
				prof.addWorkerSample("TerrainGen", 1.0f);
				prof.addWorkerSample("DynamicJobA", 1.0f);
				prof.addWorkerSample("DynamicJobB", 1.0f);
			}
		});
	}

	std::thread regThread([&prof, &startFlag, &stopFlag]() {
		while (!startFlag.load(std::memory_order_relaxed))
			std::this_thread::yield();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		prof.registerWorkerName("DynamicJobA");
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		prof.registerWorkerName("DynamicJobB");
	});

	startFlag.store(true, std::memory_order_release);
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	stopFlag.store(true, std::memory_order_release);

	regThread.join();
	for (auto &w : workers)
		w.join();

	prof.snapshotWorkers();
	std::cout << "  Worker snapshot count after dynamic registration: " << prof.workerSnapshotCount() << std::endl;
	TEST_CHECK(prof.workerSnapshotCount() >= 3);
	std::cout << "  -> Passed!" << std::endl;
}

void test_profiler_overhead_benchmark()
{
	std::cout << "[Test 4] Profiler overhead benchmark..." << std::endl;
	Profiler prof;

	constexpr int kThreads = 4;
	constexpr int kIters = 250000;
	const int totalOps = kThreads * kIters;

	auto start = std::chrono::steady_clock::now();
	std::vector<std::thread> workers;
	workers.reserve(kThreads);
	for (int t = 0; t < kThreads; ++t)
	{
		workers.emplace_back([&prof, t]() {
			const char *names[3] = {"TerrainGen", "MeshBuild", "MeshLOD"};
			const char *name = names[t % 3];
			for (int i = 0; i < kIters; ++i)
			{
				prof.addWorkerSample(name, 1.0f);
			}
		});
	}
	for (auto &w : workers)
		w.join();
	auto end = std::chrono::steady_clock::now();

	const double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
	const double nsPerOp = (totalMs * 1e6) / static_cast<double>(totalOps);

	std::cout << "  Total time: " << totalMs << " ms for " << totalOps
			  << " ops -> " << nsPerOp << " ns/op" << std::endl;

	// Ensure overhead is reasonably low (< 1000 ns/op under multi-threaded contention)
	TEST_CHECK(nsPerOp < 1000.0);
	std::cout << "  -> Passed!" << std::endl;
}

} // namespace

int main()
{
	std::cout << "=== Running Profiler Worker Consistency Tests ===" << std::endl;
	test_basic_worker_snapshot();
	test_concurrent_worker_snapshot_consistency();
	test_concurrent_registration_and_sampling();
	test_profiler_overhead_benchmark();
	std::cout << "=== All Profiler Tests Passed Successfully! ===" << std::endl;
	return 0;
}
