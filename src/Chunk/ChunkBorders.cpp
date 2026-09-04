#include "ChunkBorders.hpp"
#include <Engine/WorkloadTelemetry.hpp>
#include <cassert>

BorderPool::BorderPool(size_t initialCapacity)
{
	if (initialCapacity > 0)
		reserve(initialCapacity);
}

BorderPool::~BorderPool()
{
	// unique_ptrs in m_storage automatically free all border blocks.
}

ChunkNeighborBorders *BorderPool::acquire()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_freeList.empty())
		{
			ChunkNeighborBorders *b = m_freeList.back();
			m_freeList.pop_back();
			m_activeCount.fetch_add(1, std::memory_order_relaxed);
			return b;
		}
	}

	// Allocate indeterminate border storage outside the mutex. The pool owns
	// allocation/ownership/reuse only - it does NOT impose logical content:
	// every borrower must initialize the block (resetToAir or a full
	// overwrite) before exposing border coordinates (issue #113 review).
	// Growth beyond the free list is one observable grow event; free-list
	// reuse and reserve() preallocation are not growth.
	auto block = std::make_unique_for_overwrite<ChunkNeighborBorders>();
	ChunkNeighborBorders *ptr = block.get();
	std::lock_guard<std::mutex> lock(m_mutex);
	m_storage.push_back(std::move(block));
	auto it = std::lower_bound(m_owned.begin(), m_owned.end(), ptr);
	m_owned.insert(it, ptr);
	m_activeCount.fetch_add(1, std::memory_order_relaxed);
	telemetry::registry().add(telemetry::BorderPoolGrow);
	return ptr;
}

void BorderPool::release(ChunkNeighborBorders *borders)
{
	assert(borders != nullptr && "Cannot release null ChunkNeighborBorders");
	if (!borders)
		return;

	std::lock_guard<std::mutex> lock(m_mutex);

	// Two protection levels (same policy as VoxelPool):
	// Debug: a programming error is fatal and immediately visible.
	// Release: refuse invalid ownership without corrupting the pool.
	const bool owned =
		std::binary_search(m_owned.begin(), m_owned.end(), borders);
	assert(owned && "ChunkNeighborBorders returned to wrong pool");
	if (!owned)
		return;

	const bool alreadyFree =
		std::find(m_freeList.begin(), m_freeList.end(), borders) != m_freeList.end();
	assert(!alreadyFree && "Double release of ChunkNeighborBorders detected");
	if (alreadyFree)
		return;

	m_freeList.push_back(borders);

	const size_t active = m_activeCount.load(std::memory_order_relaxed);
	assert(active > 0 && "BorderPool active count underflow");
	if (active > 0)
		m_activeCount.fetch_sub(1, std::memory_order_relaxed);
}

size_t BorderPool::capacity() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_storage.size();
}

size_t BorderPool::activeCount() const
{
	return m_activeCount.load(std::memory_order_relaxed);
}

size_t BorderPool::freeCount() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_freeList.size();
}

void BorderPool::reserve(size_t minCapacity)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_storage.size() >= minCapacity)
		return;

	const size_t addCount = minCapacity - m_storage.size();
	m_storage.reserve(m_storage.size() + addCount);
	m_freeList.reserve(m_freeList.size() + addCount);
	m_owned.reserve(m_owned.size() + addCount);

	for (size_t i = 0; i < addCount; ++i)
	{
		auto block = std::make_unique<ChunkNeighborBorders>();
		ChunkNeighborBorders *ptr = block.get();
		m_storage.push_back(std::move(block));
		m_freeList.push_back(ptr);
		m_owned.push_back(ptr);
	}
	std::sort(m_owned.begin(), m_owned.end());
}

BorderPool &BorderPool::defaultPool()
{
	static BorderPool s_defaultPool;
	return s_defaultPool;
}
