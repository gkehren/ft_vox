#include "ChunkMeshResult.hpp"
#include <Engine/WorkloadTelemetry.hpp>
#include <cassert>

void MeshBuildResult::beginBuild(Chunk *chunkOwner, uint64_t chunkGeneration)
{
	owner = chunkOwner;
	generation = chunkGeneration;
	opaqueVertices.clear();
	opaqueIndices.clear();
	waterVertices.clear();
	waterIndices.clear();
}

void MeshBuildResult::detach()
{
	owner = nullptr;
	generation = 0;
	// Drop content (sizes only - capacity stays with the pool block so the
	// next borrower does not start from zero allocations).
	opaqueVertices.clear();
	opaqueIndices.clear();
	waterVertices.clear();
	waterIndices.clear();
}

MeshResultPool::MeshResultPool(size_t initialCapacity)
{
	if (initialCapacity > 0)
		reserve(initialCapacity);
}

MeshResultPool::~MeshResultPool() = default;

MeshBuildResult *MeshResultPool::acquire()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_freeList.empty())
		{
			MeshBuildResult *r = m_freeList.back();
			m_freeList.pop_back();
			m_activeCount.fetch_add(1, std::memory_order_relaxed);
			return r;
		}
	}

	// Allocate outside the mutex. The pool owns allocation/ownership/reuse
	// only - it does not impose logical content: the borrower stamps
	// identity via beginBuild() and the mesher fills the vectors.
	auto block = std::make_unique_for_overwrite<MeshBuildResult>();
	MeshBuildResult *ptr = block.get();
	ptr->homePool = this;
	std::lock_guard<std::mutex> lock(m_mutex);
	m_storage.push_back(std::move(block));
	auto it = std::lower_bound(m_owned.begin(), m_owned.end(), ptr);
	m_owned.insert(it, ptr);
	m_activeCount.fetch_add(1, std::memory_order_relaxed);
	telemetry::registry().add(telemetry::MeshPoolGrow);
	return ptr;
}

void MeshResultPool::release(MeshBuildResult *result)
{
	assert(result != nullptr && "Cannot release null MeshBuildResult");
	if (!result)
		return;

	std::lock_guard<std::mutex> lock(m_mutex);

	// Two protection levels (same policy as VoxelPool/BorderPool):
	// Debug: a programming error is fatal and immediately visible.
	// Release: refuse invalid ownership without corrupting the pool.
	const bool owned = std::binary_search(m_owned.begin(), m_owned.end(), result);
	assert(owned && "MeshBuildResult returned to wrong pool");
	if (!owned)
		return;

	const bool alreadyFree =
		std::find(m_freeList.begin(), m_freeList.end(), result) != m_freeList.end();
	assert(!alreadyFree && "Double release of MeshBuildResult detected");
	if (alreadyFree)
		return;

	// Bookkeeping, not content policy: detach stale identity/payload so a
	// released block cannot alias a live chunk and size telemetry stays
	// accurate. Capacity is retained for the next borrower.
	result->detach();
	m_freeList.push_back(result);

	const size_t active = m_activeCount.load(std::memory_order_relaxed);
	assert(active > 0 && "MeshResultPool active count underflow");
	if (active > 0)
		m_activeCount.fetch_sub(1, std::memory_order_relaxed);
}

size_t MeshResultPool::capacity() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_storage.size();
}

size_t MeshResultPool::activeCount() const
{
	return m_activeCount.load(std::memory_order_relaxed);
}

size_t MeshResultPool::freeCount() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_freeList.size();
}

MeshResultPoolStats MeshResultPool::stats() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	MeshResultPoolStats s;
	s.capacity = m_storage.size();
	s.free = m_freeList.size();
	s.active = s.capacity > s.free ? s.capacity - s.free : 0;
	for (const auto &block : m_storage)
	{
		s.opaqueVertexCapacity += block->opaqueVertices.capacity() * sizeof(Vertex);
		s.opaqueIndexCapacity += block->opaqueIndices.capacity() * sizeof(uint32_t);
		s.waterVertexCapacity += block->waterVertices.capacity() * sizeof(Vertex);
		s.waterIndexCapacity += block->waterIndices.capacity() * sizeof(uint32_t);
		// Sizes only carry meaning while the block is lent out; released
		// blocks were detached by release().
		s.opaqueVertexSize += block->opaqueVertices.size() * sizeof(Vertex);
		s.opaqueIndexSize += block->opaqueIndices.size() * sizeof(uint32_t);
		s.waterVertexSize += block->waterVertices.size() * sizeof(Vertex);
		s.waterIndexSize += block->waterIndices.size() * sizeof(uint32_t);
	}
	return s;
}

void MeshResultPool::reserve(size_t minCapacity)
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
		auto block = std::make_unique<MeshBuildResult>();
		MeshBuildResult *ptr = block.get();
		ptr->homePool = this;
		m_storage.push_back(std::move(block));
		m_freeList.push_back(ptr);
		m_owned.push_back(ptr);
	}
	std::sort(m_owned.begin(), m_owned.end());
}

MeshResultPool &MeshResultPool::defaultPool()
{
	static MeshResultPool s_defaultPool;
	return s_defaultPool;
}
