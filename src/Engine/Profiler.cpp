#include "Engine/Profiler.hpp"

#include <algorithm>
#include <cmath>
namespace
{
Profiler g_profiler;
} // namespace

Profiler::Profiler()
{
	registerWorkerName("TerrainGen");
	registerWorkerName("MeshBuild");
	registerWorkerName("MeshLOD");
	registerWorkerName("TerrainQueue");
	registerWorkerName("MeshQueue");
	registerWorkerName("BiomeMap");
}

Profiler &GetProfiler()
{
	return g_profiler;
}

void Profiler::registerWorkerName(const char *name)
{
	if (!name)
		return;
	std::lock_guard<std::mutex> lock(m_workerRegisterMutex);
	const int count = m_workerBucketCount.load(std::memory_order_relaxed);
	for (int i = 0; i < count; ++i)
	{
		if (m_workers[static_cast<size_t>(i)].name &&
			std::strcmp(m_workers[static_cast<size_t>(i)].name, name) == 0)
			return;
	}
	if (count >= kMaxWorkerBuckets)
		return;
	m_workers[static_cast<size_t>(count)].name = name;
	m_workerBucketCount.store(count + 1, std::memory_order_release);
}

void Profiler::beginFrame()
{
	if (!enabled())
	{
		m_inFrame = false;
		return;
	}

	m_frameStart = Clock::now();
	m_entryCount = 0;
	m_stackDepth = 0;
	m_inFrame = true;
}

void Profiler::endFrame()
{
	if (!m_inFrame)
		return;

	// Close any leaked scopes so the frame is still usable.
	while (m_stackDepth > 0)
		closeScope();

	const float frameMs = nowMs();
	finalizeFrame(frameMs);
	m_inFrame = false;
}

void Profiler::push(const char *name)
{
	if (!m_inFrame || !enabled())
		return;
	openScope(name);
}

void Profiler::pop()
{
	if (!m_inFrame || !enabled())
		return;
	closeScope();
}

void Profiler::openScope(const char *name)
{
	if (m_entryCount >= kMaxEntries || m_stackDepth >= kMaxDepth)
		return;

	const int16_t parent = m_stackDepth > 0 ? m_stack[static_cast<size_t>(m_stackDepth - 1)] : int16_t{-1};
	const int idx = m_entryCount++;
	ProfileEntry &e = m_entries[static_cast<size_t>(idx)];
	e.name = name ? name : "?";
	e.startMs = nowMs();
	e.durationMs = 0.f;
	e.depth = static_cast<uint8_t>(m_stackDepth);
	e.parent = parent;
	m_stack[static_cast<size_t>(m_stackDepth++)] = static_cast<int16_t>(idx);
}

void Profiler::closeScope()
{
	if (m_stackDepth <= 0)
		return;
	const int16_t idx = m_stack[static_cast<size_t>(--m_stackDepth)];
	if (idx < 0 || idx >= m_entryCount)
		return;
	ProfileEntry &e = m_entries[static_cast<size_t>(idx)];
	e.durationMs = nowMs() - e.startMs;
	if (e.durationMs < 0.f)
		e.durationMs = 0.f;
}

void Profiler::addWorkerSample(const char *name, float ms)
{
	if (!name || !enabled())
		return;

	if (std::isnan(ms) || ms < 0.f)
		ms = 0.f;
	const auto us = static_cast<uint64_t>(static_cast<double>(ms) * 1000.0);

	// Pointer compare first (string literals), then strcmp. Buckets pre-registered on main thread.
	const int n = m_workerBucketCount.load(std::memory_order_acquire);
	for (int i = 0; i < n; ++i)
	{
		WorkerBucket &b = m_workers[static_cast<size_t>(i)];
		const char *bn = b.name;
		if (bn == name || (bn && std::strcmp(bn, name) == 0))
		{
			std::lock_guard<std::mutex> lock(b.mutex);
			b.count += 1;
			b.totalUs += us;
			return;
		}
	}
	// Unknown name — drop sample (call registerWorkerName first).
}

void Profiler::clearHistory()
{
	m_history.fill(0.f);
	m_historyWrite = 0;
	m_historyCount = 0;
	m_avgFrameMs = 0.f;
	m_onePercentLowMs = 0.f;
	m_avgAccumMs = 0.0;
	m_avgSamples = 0;
	m_spikeCount = 0;
	m_spikes = {};
}

float Profiler::lastScopeMs(const char *name) const
{
	if (!name)
		return 0.f;
	for (int i = 0; i < m_lastEntryCount; ++i)
	{
		const char *n = m_lastEntries[static_cast<size_t>(i)].name;
		if (n && std::strcmp(n, name) == 0)
			return m_lastEntries[static_cast<size_t>(i)].durationMs;
	}
	return 0.f;
}

void Profiler::snapshotWorkers()
{
	m_workerSnapCount = 0;
	const int n = m_workerBucketCount.load(std::memory_order_acquire);
	for (int i = 0; i < n && m_workerSnapCount < kMaxWorkerBuckets; ++i)
	{
		WorkerBucket &b = m_workers[static_cast<size_t>(i)];
		if (!b.name)
			continue;

		uint64_t c = 0;
		uint64_t us = 0;
		{
			std::lock_guard<std::mutex> lock(b.mutex);
			c = b.count;
			us = b.totalUs;
			b.count = 0;
			b.totalUs = 0;
		}

		WorkerSnapshot &s = m_workerSnaps[static_cast<size_t>(m_workerSnapCount++)];
		s.name = b.name;
		s.count = c;
		s.totalUs = us;
		s.totalMs = static_cast<float>(us) / 1000.f;
		s.avgMs = c > 0 ? s.totalMs / static_cast<float>(c) : 0.f;
	}
}

void Profiler::updateAverages(float frameMs)
{
	m_history[static_cast<size_t>(m_historyWrite)] = frameMs;
	m_historyWrite = (m_historyWrite + 1) % kHistorySize;
	if (m_historyCount < kHistorySize)
		++m_historyCount;

	// Rolling mean over last ~60 samples (or all if fewer)
	const int window = std::min(m_historyCount, 60);
	float sum = 0.f;
	for (int i = 0; i < window; ++i)
	{
		int idx = m_historyWrite - 1 - i;
		if (idx < 0)
			idx += kHistorySize;
		sum += m_history[static_cast<size_t>(idx)];
	}
	m_avgFrameMs = window > 0 ? sum / static_cast<float>(window) : frameMs;

	// 1% low: 99th percentile of history (slow frames) — sort a temp copy of recent samples
	if (m_historyCount > 0)
	{
		std::array<float, kHistorySize> tmp{};
		const int n = m_historyCount;
		for (int i = 0; i < n; ++i)
		{
			int idx = m_historyWrite - 1 - i;
			if (idx < 0)
				idx += kHistorySize;
			tmp[static_cast<size_t>(i)] = m_history[static_cast<size_t>(idx)];
		}
		std::sort(tmp.begin(), tmp.begin() + n);
		// 1% low FPS ≈ high frame times: take the value at 99th percentile (slow end)
		const int p99 = std::clamp(static_cast<int>(std::ceil(n * 0.99f)) - 1, 0, n - 1);
		m_onePercentLowMs = tmp[static_cast<size_t>(p99)];
	}
}

void Profiler::recordSpike(float frameMs)
{
	if (frameMs < kSpikeThresholdMs)
		return;

	// Find heaviest root-level (or any) scope
	const char *topName = nullptr;
	float topMs = 0.f;
	for (int i = 0; i < m_lastEntryCount; ++i)
	{
		const ProfileEntry &e = m_lastEntries[static_cast<size_t>(i)];
		if (e.durationMs > topMs)
		{
			topMs = e.durationMs;
			topName = e.name;
		}
	}

	SpikeRecord rec{frameMs, topName, topMs};
	if (m_spikeCount < kSpikeLogSize)
	{
		m_spikes[static_cast<size_t>(m_spikeCount++)] = rec;
	}
	else
	{
		// Shift left, append
		for (int i = 1; i < kSpikeLogSize; ++i)
			m_spikes[static_cast<size_t>(i - 1)] = m_spikes[static_cast<size_t>(i)];
		m_spikes[static_cast<size_t>(kSpikeLogSize - 1)] = rec;
	}
}

void Profiler::finalizeFrame(float frameMs)
{
	// Copy hierarchy for UI
	m_lastEntryCount = m_entryCount;
	for (int i = 0; i < m_entryCount; ++i)
		m_lastEntries[static_cast<size_t>(i)] = m_entries[static_cast<size_t>(i)];
	m_lastFrameMs = frameMs;

	snapshotWorkers();
	updateAverages(frameMs);
	recordSpike(frameMs);
}
