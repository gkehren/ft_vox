#include "VoxelPool.hpp"
#include <Engine/WorkloadTelemetry.hpp>
#include <algorithm>
#include <cassert>

VoxelPool::VoxelPool(size_t initialCapacity)
{
	if (initialCapacity > 0)
		reserve(initialCapacity);
}

VoxelPool::~VoxelPool()
{
	// unique_ptrs in m_storage automatically free all VoxelStorage blocks.
}

void VoxelPool::publishTelemetry(size_t cap, size_t act, size_t fre)
{
	// Publish snapshot outside m_mutex to avoid lock hierarchy inversions
	auto &t = telemetry::registry();
	if (!t.enabled)
		return;
	const std::array<size_t, 3> now{cap, act, fre};
	for (size_t i = 0; i < now.size(); ++i)
		t.replace(static_cast<telemetry::Gauge>(telemetry::PoolCapacity + i), m_telemetry[i], now[i]);
	m_telemetry = now;
}

VoxelStorage *VoxelPool::acquire()
{
	size_t cap = 0, act = 0, fre = 0;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_freeList.empty())
		{
			VoxelStorage *s = m_freeList.back();
			m_freeList.pop_back();
			m_activeCount.fetch_add(1, std::memory_order_relaxed);
			cap = m_storage.size();
			act = m_activeCount.load(std::memory_order_relaxed);
			fre = m_freeList.size();
			publishTelemetry(cap, act, fre);
			return s;
		}
	}

	// Allocate uninitialized storage outside the mutex
	auto block = std::make_unique_for_overwrite<VoxelStorage>();
	VoxelStorage *ptr = block.get();

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_storage.push_back(std::move(block));
		auto it = std::lower_bound(m_owned.begin(), m_owned.end(), ptr);
		m_owned.insert(it, ptr);
		m_activeCount.fetch_add(1, std::memory_order_relaxed);
		cap = m_storage.size();
		act = m_activeCount.load(std::memory_order_relaxed);
		fre = m_freeList.size();
	}
	publishTelemetry(cap, act, fre);
	return ptr;
}

void VoxelPool::release(VoxelStorage *storage)
{
	assert(storage != nullptr && "Cannot release null VoxelStorage");
	size_t cap = 0, act = 0, fre = 0;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		assert(std::binary_search(m_owned.begin(), m_owned.end(), storage) &&
			   "VoxelStorage does not belong to this VoxelPool");
		assert(std::find(m_freeList.begin(), m_freeList.end(), storage) == m_freeList.end() &&
			   "Double release of VoxelStorage detected");

		m_freeList.push_back(storage);
		const size_t acq = m_activeCount.load(std::memory_order_relaxed);
		if (acq > 0)
			m_activeCount.fetch_sub(1, std::memory_order_relaxed);

		cap = m_storage.size();
		act = m_activeCount.load(std::memory_order_relaxed);
		fre = m_freeList.size();
	}
	publishTelemetry(cap, act, fre);
}

size_t VoxelPool::capacity() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_storage.size();
}

size_t VoxelPool::activeCount() const
{
	return m_activeCount.load(std::memory_order_relaxed);
}

size_t VoxelPool::freeCount() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_freeList.size();
}

void VoxelPool::reserve(size_t minCapacity)
{
	size_t cap = 0, act = 0, fre = 0;
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
			auto block = std::make_unique_for_overwrite<VoxelStorage>();
			VoxelStorage *ptr = block.get();
			m_storage.push_back(std::move(block));
			m_freeList.push_back(ptr);
			m_owned.push_back(ptr);
		}
		std::sort(m_owned.begin(), m_owned.end());

		cap = m_storage.size();
		act = m_activeCount.load(std::memory_order_relaxed);
		fre = m_freeList.size();
	}
	publishTelemetry(cap, act, fre);
}

VoxelPool &VoxelPool::defaultPool()
{
	static VoxelPool s_defaultPool;
	return s_defaultPool;
}
