#include "ChunkPool.hpp"

#include <Chunk/Chunk.hpp>
#include <utils.hpp>

#include <algorithm>
#include <iostream>

namespace
{
constexpr size_t kMinPoolCapacity = 64;
constexpr size_t kGrowSlabMin = 128;
// Soft cap so a mis-set slider cannot allocate unbounded RAM in one go.
constexpr size_t kMaxPoolCapacity = 16384;
} // namespace

ChunkPool::ChunkPool(size_t initialCapacity)
{
	const size_t cap = std::max(initialCapacity, kMinPoolCapacity);
	std::lock_guard<std::mutex> lock(m_mutex);
	growUnlocked(cap);
	std::cout << "ChunkPool: pre-allocated " << m_capacity.load(std::memory_order_relaxed) << " chunks\n";
}

ChunkPool::~ChunkPool() = default;

void ChunkPool::growUnlocked(size_t addCount)
{
	if (addCount == 0)
		return;

	const size_t cur = m_storage.size();
	if (cur >= kMaxPoolCapacity)
		return;

	const size_t canAdd = std::min(addCount, kMaxPoolCapacity - cur);
	m_storage.reserve(cur + canAdd);
	m_freeList.reserve(m_freeList.size() + canAdd);
	m_owned.reserve(cur + canAdd);

	for (size_t i = 0; i < canAdd; ++i)
	{
		m_storage.push_back(std::make_unique<Chunk>(glm::vec3(0.0f)));
		Chunk *ptr = m_storage.back().get();
		m_freeList.push_back(ptr);
		m_owned.push_back(ptr);
	}

	std::sort(m_owned.begin(), m_owned.end());

	m_capacity.store(m_storage.size(), std::memory_order_relaxed);
	m_growEvents.fetch_add(1, std::memory_order_relaxed);
}

bool ChunkPool::ensureCapacity(size_t minCapacity)
{
	minCapacity = std::min(std::max(minCapacity, kMinPoolCapacity), kMaxPoolCapacity);

	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_storage.size() >= minCapacity)
		return false;

	const size_t need = minCapacity - m_storage.size();
	// Grow in slabs to amortize lock + allocation cost when the slider jumps.
	const size_t add = std::max(need, kGrowSlabMin);
	const size_t before = m_storage.size();
	growUnlocked(add);
	const size_t after = m_storage.size();
	if (after > before)
	{
		std::cout << "ChunkPool: grew " << before << " -> " << after
				  << " (target >= " << minCapacity << ")\n";
	}
	return after > before;
}

Chunk *ChunkPool::acquire(const glm::vec3 &worldPosition)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_freeList.empty())
	{
		m_rejectCount.fetch_add(1, std::memory_order_relaxed);
		return nullptr;
	}

	Chunk *chunk = m_freeList.back();
	m_freeList.pop_back();
	chunk->reset(worldPosition, Chunk::ResetMode::ForGeneration);
	m_acquiredCount.fetch_add(1, std::memory_order_relaxed);
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
}
