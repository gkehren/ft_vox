#include "ChunkPool.hpp"

#include <Chunk/Chunk.hpp>
#include <utils.hpp>

#include <algorithm>
#include <iostream>

namespace
{
// Sizing bounds and growth rules live in StreamHelpers.hpp so the pool,
// the estimator and the UI read one set of constants.
constexpr size_t kMinPoolCapacity = kMinChunkPoolCapacity;
} // namespace

ChunkPool::ChunkPool(size_t initialCapacity)
{
	const size_t cap = std::max(initialCapacity, kMinPoolCapacity);
	std::lock_guard<std::mutex> lock(m_mutex);
	growUnlocked(cap);
	std::cout << "ChunkPool: pre-allocated " << m_capacity.load(std::memory_order_relaxed) << " chunks\n";
}

ChunkPool::~ChunkPool()
{
	auto& t = telemetry::registry();
	t.replace(telemetry::PoolCapacity, capacity(), 0);
	t.replace(telemetry::PoolAcquired, acquiredCount(), 0);
	t.replace(telemetry::PoolFree, freeCount(), 0);
}

void ChunkPool::publishTelemetry()
{
	auto& t = telemetry::registry();
	const std::array<size_t, 3> now{capacity(), acquiredCount(), freeCount()};
	for (size_t i = 0; i < now.size(); ++i)
		t.replace(static_cast<telemetry::Gauge>(telemetry::PoolCapacity + i), m_telemetry[i], now[i]);
	m_telemetry = now;
}

void ChunkPool::growUnlocked(size_t addCount)
{
	if (addCount == 0)
		return;

	const size_t cur = m_storage.size();
	if (cur >= kMaxChunkPoolCapacity)
		return;

	const size_t canAdd = std::min(addCount, kMaxChunkPoolCapacity - cur);
	m_storage.reserve(cur + canAdd);
	m_freeList.reserve(m_freeList.size() + canAdd);
	m_owned.reserve(cur + canAdd);

	for (size_t i = 0; i < canAdd; ++i)
	{
		m_storage.push_back(std::make_unique<Chunk>(glm::vec3(0.0f), ChunkState::UNLOADED, &m_voxelPool, &m_borderPool, &m_meshPool, &m_lightPool));
		Chunk *ptr = m_storage.back().get();
		m_freeList.push_back(ptr);
		m_owned.push_back(ptr);
	}

	std::sort(m_owned.begin(), m_owned.end());

	m_capacity.store(m_storage.size(), std::memory_order_relaxed);
	m_growEvents.fetch_add(1, std::memory_order_relaxed);
	publishTelemetry();
}

bool ChunkPool::ensureCapacity(size_t minCapacity, size_t maxGrowPerCall)
{
	minCapacity = std::min(std::max(minCapacity, kMinPoolCapacity), kMaxChunkPoolCapacity);

	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_storage.size() >= minCapacity)
		return false;

	// Incremental growth (chunkPoolGrowStep contract): slab-amortized, but
	// capped per call so a slider jump converges over several streaming
	// ticks instead of one massive allocation. Callers re-invoke each tick.
	// No per-step logging: growth is observable via growEvents(), the pool
	// telemetry gauges and the Engine's PoolGrow profiler scope — a console
	// line per increment would both spam convergences and pollute the very
	// timing the PoolGrow scope is meant to measure.
	const size_t add = chunkPoolGrowStep(m_storage.size(), minCapacity, maxGrowPerCall);
	const size_t before = m_storage.size();
	growUnlocked(add);
	return m_storage.size() > before;
}

Chunk *ChunkPool::acquire(const glm::vec3 &worldPosition)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_freeList.empty())
	{
		m_rejectCount.fetch_add(1, std::memory_order_relaxed);
		telemetry::registry().add(telemetry::PoolRejected);
		return nullptr;
	}

	Chunk *chunk = m_freeList.back();
	m_freeList.pop_back();
	chunk->reset(worldPosition, Chunk::ResetMode::ForGeneration);
	m_acquiredCount.fetch_add(1, std::memory_order_relaxed);
	publishTelemetry();
	return chunk;
}

void ChunkPool::release(Chunk *chunk)
{
	if (!chunk)
		return;

	// Drop GPU / CPU mesh data while not holding the pool mutex (can be heavy).
	chunk->reset(glm::vec3(0.0f));

	std::lock_guard<std::mutex> lock(m_mutex);

	if (std::binary_search(m_owned.begin(), m_owned.end(), chunk))
	{
		m_freeList.push_back(chunk);
	}
	else
	{
		// Stray pointer (legacy overflow or bug) — free to avoid leak.
		delete chunk;
	}
	const size_t acq = m_acquiredCount.load(std::memory_order_relaxed);
	if (acq > 0)
		m_acquiredCount.fetch_sub(1, std::memory_order_relaxed);
	publishTelemetry();
}
