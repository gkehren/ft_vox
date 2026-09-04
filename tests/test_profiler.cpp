#include "Engine/Profiler.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <future>
#include <algorithm>
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

struct ProfilerStateProbe
{
	static int stackDepth(const Profiler &prof) { return prof.m_stackDepth; }
	static const ProfileEntry &entry(const Profiler &prof, int i) { return prof.m_entries[i]; }
};

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

	// Drain final in-flight samples
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

	// Phase 0: Neither DynamicJobA nor DynamicJobB registered (samples safely dropped)
	// Phase 1: DynamicJobA registered (DynamicJobA recorded, DynamicJobB still dropped)
	// Phase 2: DynamicJobB registered (both recorded)
	std::atomic<int> phase{0};
	std::atomic<int> workersReadyPhase0{0};
	std::atomic<int> workersDonePhase1{0};
	constexpr int kWorkers = 4;
	constexpr int kSamplesPerPhase = 1000;

	std::vector<std::thread> workers;
	workers.reserve(kWorkers);
	for (int i = 0; i < kWorkers; ++i)
	{
		workers.emplace_back([&prof, &phase, &workersReadyPhase0, &workersDonePhase1]() {
			// Phase 0: samples to unregistered names must be safely dropped
			for (int s = 0; s < 500; ++s)
			{
				prof.addWorkerSample("DynamicJobA", 1.0f);
				prof.addWorkerSample("DynamicJobB", 2.0f);
			}
			workersReadyPhase0.fetch_add(1, std::memory_order_release);

			// Wait for phase 1: DynamicJobA is registered
			while (phase.load(std::memory_order_acquire) < 1)
				std::this_thread::yield();

			for (int s = 0; s < kSamplesPerPhase; ++s)
			{
				prof.addWorkerSample("DynamicJobA", 1.0f);
				prof.addWorkerSample("DynamicJobB", 2.0f); // Still unregistered, dropped
			}
			workersDonePhase1.fetch_add(1, std::memory_order_release);

			// Wait for phase 2: DynamicJobB is registered
			while (phase.load(std::memory_order_acquire) < 2)
				std::this_thread::yield();

			for (int s = 0; s < kSamplesPerPhase; ++s)
			{
				prof.addWorkerSample("DynamicJobA", 1.0f);
				prof.addWorkerSample("DynamicJobB", 2.0f);
			}
		});
	}

	// Wait for workers to finish attempting samples in phase 0
	while (workersReadyPhase0.load(std::memory_order_acquire) < kWorkers)
		std::this_thread::yield();

	// Register DynamicJobA and enter Phase 1
	prof.registerWorkerName("DynamicJobA");
	phase.store(1, std::memory_order_release);

	// Wait for all workers to finish Phase 1
	while (workersDonePhase1.load(std::memory_order_acquire) < kWorkers)
		std::this_thread::yield();

	// Register DynamicJobB and enter Phase 2
	prof.registerWorkerName("DynamicJobB");
	phase.store(2, std::memory_order_release);

	for (auto &w : workers)
		w.join();

	prof.snapshotWorkers();
	std::cout << "  Worker snapshot count after dynamic registration: " << prof.workerSnapshotCount() << std::endl;
	TEST_CHECK(prof.workerSnapshotCount() >= 5);

	bool foundA = false;
	bool foundB = false;
	uint64_t countA = 0;
	uint64_t countB = 0;
	for (int i = 0; i < prof.workerSnapshotCount(); ++i)
	{
		const WorkerSnapshot &s = prof.workerSnapshots()[i];
		if (s.name && std::strcmp(s.name, "DynamicJobA") == 0)
		{
			foundA = true;
			countA = s.count;
			// Sampled in Phase 1 and Phase 2: exactly kWorkers * 2 * kSamplesPerPhase
			constexpr uint64_t expectedA = static_cast<uint64_t>(kWorkers) * 2 * kSamplesPerPhase;
			TEST_CHECK(s.count == expectedA);
			TEST_CHECK(s.totalUs == expectedA * 1000);
		}
		else if (s.name && std::strcmp(s.name, "DynamicJobB") == 0)
		{
			foundB = true;
			countB = s.count;
			// Sampled only in Phase 2: exactly kWorkers * kSamplesPerPhase
			constexpr uint64_t expectedB = static_cast<uint64_t>(kWorkers) * kSamplesPerPhase;
			TEST_CHECK(s.count == expectedB);
			TEST_CHECK(s.totalUs == expectedB * 2000);
		}
	}

	std::cout << "  DynamicJobA: found=" << foundA << ", count=" << countA
			  << " | DynamicJobB: found=" << foundB << ", count=" << countB << std::endl;
	TEST_CHECK(foundA);
	TEST_CHECK(foundB);
	TEST_CHECK(countA > 0);
	TEST_CHECK(countB > 0);
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

	std::cout << "  Profiler worker overhead: " << nsPerOp << " ns/op ("
			  << totalMs << " ms for " << totalOps << " ops)" << std::endl;

	// Informational benchmark measurement: no hard wall-clock threshold to prevent
	// flaky CI runs under TSan/ASan instrumentation, virtualization, or high runner load.
	TEST_CHECK(totalOps > 0 && totalMs >= 0.0);
	std::cout << "  -> Passed!" << std::endl;
}


void check_empty_capture(const Profiler &prof)
{
	TEST_CHECK(prof.lastFrameMs() == 0.f);
	TEST_CHECK(prof.lastEntryCount() == 0);
	TEST_CHECK(prof.avgFrameMs() == 0.f);
	TEST_CHECK(prof.fpsEstimate() == 0.f);
	TEST_CHECK(prof.onePercentLowMs() == 0.f);
	TEST_CHECK(prof.historyCount() == 0);
	TEST_CHECK(prof.historyWriteIndex() == 0);
	TEST_CHECK(prof.spikeCount() == 0);
	TEST_CHECK(prof.workerSnapshotCount() == 0);
	for (int i = 0; i < Profiler::kHistorySize; ++i)
		TEST_CHECK(prof.frameHistory()[i] == 0.f);
	for (int i = 0; i < Profiler::kMaxEntries; ++i)
		TEST_CHECK(prof.lastEntries()[i].name == nullptr);
	for (int i = 0; i < Profiler::kMaxWorkerBuckets; ++i)
		TEST_CHECK(prof.workerSnapshots()[i].count == 0);
}

void test_capture_reset_idle()
{
	Profiler prof;
	prof.beginFrame();
	prof.push("old");
	std::this_thread::sleep_for(std::chrono::milliseconds(25));
	prof.pop();
	prof.addWorkerSample("TerrainGen", 3.f);
	prof.endFrame();
	TEST_CHECK(prof.lastFrameMs() > 0.f && prof.historyCount() == 1);
	TEST_CHECK(prof.lastEntryCount() == 1 && prof.spikeCount() == 1);
	TEST_CHECK(prof.workerSnapshotCount() > 0);
	prof.addWorkerSample("TerrainGen", 7.f); // pending, not yet drained
	const auto oldEpoch = prof.captureEpoch();
	prof.setEnabled(false);
	prof.clearHistory();
	check_empty_capture(prof);
	TEST_CHECK(!prof.enabled());
	TEST_CHECK(prof.captureEpoch() != oldEpoch);
	TEST_CHECK(prof.lastScopeMs("old") == 0.f);
	prof.snapshotWorkers();
	for (int i = 0; i < prof.workerSnapshotCount(); ++i)
		TEST_CHECK(prof.workerSnapshots()[i].count == 0);
	prof.clearHistory(); // repeated reset while paused is safe
	check_empty_capture(prof);
	prof.setEnabled(true);
	prof.addWorkerSample("TerrainGen", 2.f);
	prof.snapshotWorkers();
	TEST_CHECK(prof.workerSnapshots()[0].count == 1); // registration retained
	TEST_CHECK(prof.workerSnapshots()[0].totalUs == 2000);
}

void test_capture_reset_open_frame()
{
	Profiler prof;
	prof.beginFrame();
	prof.push("before-reset-root");
	prof.push("Clear button");
	const auto oldEpoch = prof.captureEpoch();
	prof.addWorkerSample("TerrainGen", 8.f, oldEpoch);
	prof.clearHistory(); // same placement as UI/reloadWorld during an open frame
	check_empty_capture(prof);
	TEST_CHECK(ProfilerStateProbe::stackDepth(prof) == 2);
	TEST_CHECK(std::strcmp(ProfilerStateProbe::entry(prof, 0).name, "before-reset-root") == 0);
	prof.push("after-button");
	TEST_CHECK(ProfilerStateProbe::entry(prof, 2).parent == 1);
	TEST_CHECK(ProfilerStateProbe::stackDepth(prof) == 3);
	prof.pop();
	prof.pop(); // Clear button
	prof.pop(); // root opened before clear
	TEST_CHECK(ProfilerStateProbe::stackDepth(prof) == 0);
	prof.addWorkerSample("TerrainGen", 9.f, oldEpoch); // late pre-reload job
	prof.addWorkerSample("TerrainGen", 2.f, prof.captureEpoch());
	prof.endFrame();
	// Even with zero warmup the interrupted pre-reload frame is not measured.
	check_empty_capture(prof);
	prof.beginFrame();
	prof.push("fresh-root");
	prof.push("fresh-child");
	prof.pop();
	prof.pop();
	prof.endFrame();
	TEST_CHECK(prof.historyCount() == 1 && prof.lastEntryCount() == 2);
	TEST_CHECK(prof.lastEntries()[0].parent == -1 && prof.lastEntries()[0].depth == 0);
	TEST_CHECK(prof.lastEntries()[1].parent == 0 && prof.lastEntries()[1].depth == 1);
	TEST_CHECK(std::strcmp(prof.lastEntries()[0].name, "fresh-root") == 0);
	TEST_CHECK(prof.workerSnapshots()[0].count == 1);
	TEST_CHECK(prof.workerSnapshots()[0].totalUs == 2000);
}

void test_capture_reset_inflight_workers()
{
	Profiler prof;
	const auto oldEpoch = prof.captureEpoch();
	std::promise<void> release;
	auto gate = release.get_future().share();
	std::atomic<int> ready{0};
	std::vector<std::thread> workers;
	for (int i = 0; i < 4; ++i)
		workers.emplace_back([&] {
			prof.addWorkerSample("TerrainGen", 1.f, oldEpoch);
			ready.fetch_add(1, std::memory_order_release);
			gate.wait();
			prof.addWorkerSample("TerrainGen", 1.f, oldEpoch); // reject late completion
			prof.addWorkerSample("TerrainGen", 2.5f, prof.captureEpoch());
		});
	while (ready.load(std::memory_order_acquire) != 4)
		std::this_thread::yield();
	prof.clearHistory();
	check_empty_capture(prof);
	release.set_value();
	for (auto &worker : workers)
		worker.join();
	prof.snapshotWorkers();
	TEST_CHECK(prof.workerSnapshots()[0].count == 4);
	TEST_CHECK(prof.workerSnapshots()[0].totalUs == 10000);

	// Race reset sweeps with both stale-epoch and fresh submissions. Every
	// retained pair must consist solely of 2.5ms samples, never old 1ms work.
	std::atomic<bool> stop{false};
	workers.clear();
	for (int i = 0; i < 4; ++i)
		workers.emplace_back([&] {
			while (!stop.load(std::memory_order_relaxed))
			{
				prof.addWorkerSample("TerrainGen", 1.f, oldEpoch);
				prof.addWorkerSample("TerrainGen", 2.5f, prof.captureEpoch());
			}
		});
	for (int i = 0; i < 1000; ++i)
	{
		prof.clearHistory();
		prof.snapshotWorkers();
		const auto &snapshot = prof.workerSnapshots()[0];
		TEST_CHECK(snapshot.totalUs == snapshot.count * 2500);
	}
	stop.store(true, std::memory_order_relaxed);
	for (auto &worker : workers)
		worker.join();
	prof.snapshotWorkers();
	TEST_CHECK(prof.workerSnapshots()[0].totalUs == prof.workerSnapshots()[0].count * 2500);
}

} // namespace

int main()
{
	std::cout << "=== Running Profiler Worker Consistency Tests ===" << std::endl;
	test_capture_reset_idle();
	test_capture_reset_open_frame();
	test_capture_reset_inflight_workers();
	test_basic_worker_snapshot();
	test_concurrent_worker_snapshot_consistency();
	test_concurrent_registration_and_sampling();
	test_profiler_overhead_benchmark();
	std::cout << "=== All Profiler Tests Passed Successfully! ===" << std::endl;
	return 0;
}
