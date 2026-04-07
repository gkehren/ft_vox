#include "ChunkPool.hpp"
#include <Chunk/Chunk.hpp>
#include <iostream>

ChunkPool::ChunkPool(size_t capacity)
	: m_capacity(capacity)
{
	// Pre-allocate all chunks at a dummy position; they'll be reset() on acquire.
	m_storage.reserve(capacity);
	m_freeList.reserve(capacity);

	for (size_t i = 0; i < capacity; ++i)
	{
		m_storage.emplace_back(glm::vec3(0.0f));
	}

	// Populate free-list (all chunks start as available)
	for (size_t i = 0; i < capacity; ++i)
	{
		m_freeList.push_back(&m_storage[i]);
	}

	std::cout << "ChunkPool: pre-allocated " << capacity << " chunks ("
			  << (capacity * sizeof(Chunk)) / (1024 * 1024) << " MB storage)" << std::endl;
}

ChunkPool::~ChunkPool()
{
	// m_storage destructor handles cleanup of pool-owned chunks.
	// Any leaked overflow chunks are not tracked here — caller is responsible.
}

Chunk *ChunkPool::acquire(const glm::vec3 &worldPosition)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (!m_freeList.empty())
	{
		Chunk *chunk = m_freeList.back();
		m_freeList.pop_back();
		chunk->reset(worldPosition);
		m_acquiredCount.fetch_add(1, std::memory_order_relaxed);
		return chunk;
	}

	// Pool exhausted — fall back to heap allocation
	m_overflowCount.fetch_add(1, std::memory_order_relaxed);
	m_acquiredCount.fetch_add(1, std::memory_order_relaxed);
	std::cerr << "ChunkPool: WARNING — pool exhausted, heap-allocating chunk (overflow #"
			  << m_overflowCount.load(std::memory_order_relaxed) << ")" << std::endl;

	Chunk *overflow = new Chunk(worldPosition);
	return overflow;
}

void ChunkPool::release(Chunk *chunk)
{
	if (!chunk)
		return;

	// Reset the chunk (releases GPU resources, clears buffers but keeps capacity)
	chunk->reset(glm::vec3(0.0f));

	std::lock_guard<std::mutex> lock(m_mutex);

	if (isPoolOwned(chunk))
	{
		m_freeList.push_back(chunk);
	}
	else
	{
		// Overflow chunk — delete it since it's not in our storage
		delete chunk;
	}
	m_acquiredCount.fetch_sub(1, std::memory_order_relaxed);
}

bool ChunkPool::isPoolOwned(const Chunk *chunk) const
{
	if (m_storage.empty())
		return false;
	const Chunk *begin = m_storage.data();
	const Chunk *end = begin + m_storage.size();
	return chunk >= begin && chunk < end;
}
