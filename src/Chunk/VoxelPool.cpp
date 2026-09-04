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

VoxelStorage *VoxelPool::acquire()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_freeList.empty())
		{
			VoxelStorage *s = m_freeList.back();
			m_freeList.pop_back();
			m_activeCount.fetch_add(1, std::memory_order_relaxed);
			return s;
		}
	}

	// Allocate uninitialized storage outside the mutex. Growing is an event,
	// not a gauge publication: the pool stays a low-level thread-safe
	// component and the main thread publishes the voxel.pool.* gauges as a
	// per-frame snapshot (issue #112 review).
	VoxelStorage *ptr = nullptr;
	{
		auto block = std::make_unique_for_overwrite<VoxelStorage>();
		ptr = block.get();
		std::lock_guard<std::mutex> lock(m_mutex);
		m_storage.push_back(std::move(block));
		auto it = std::lower_bound(m_owned.begin(), m_owned.end(), ptr);
		m_owned.insert(it, ptr);
		m_activeCount.fetch_add(1, std::memory_order_relaxed);
	}
	telemetry::registry().add(telemetry::VoxelPoolGrow);
	return ptr;
}

void VoxelPool::release(VoxelStorage *storage)
{
	assert(storage != nullptr && "Cannot release null VoxelStorage");
	if (!storage)
		return;

	std::lock_guard<std::mutex> lock(m_mutex);

	// Two protection levels (issue #112 review):
	// Debug: a programming error is fatal and immediately visible.
	// Release: refuse invalid ownership without corrupting the pool.
	const bool owned =
		std::binary_search(m_owned.begin(), m_owned.end(), storage);
	assert(owned && "VoxelStorage returned to wrong pool");
	if (!owned)
		return;

	const bool alreadyFree =
		std::find(m_freeList.begin(), m_freeList.end(), storage) != m_freeList.end();
	assert(!alreadyFree && "Double release of VoxelStorage detected");
	if (alreadyFree)
		return;

	m_freeList.push_back(storage);

	// Guard against silent underflow if a broken lifecycle double-frees.
	const size_t active = m_activeCount.load(std::memory_order_relaxed);
	assert(active > 0 && "VoxelPool active count underflow");
	if (active > 0)
		m_activeCount.fetch_sub(1, std::memory_order_relaxed);
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
}

VoxelPool &VoxelPool::defaultPool()
{
	static VoxelPool s_defaultPool;
	return s_defaultPool;
}
