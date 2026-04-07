#pragma once

#include <vector>
#include <mutex>
#include <cstddef>
#include <atomic>
#include <glm/glm.hpp>

class Chunk;

/// Thread-safe object pool for Chunk objects.
/// Pre-allocates a configurable number of Chunk instances and recycles them
/// via acquire()/release() to avoid per-frame heap allocation overhead.
///
/// Overflow: if the pool is exhausted, acquire() falls back to heap allocation
/// and logs a warning. Overflow chunks are deleted on release().
class ChunkPool
{
public:
	/// @param capacity Number of chunks to pre-allocate.
	explicit ChunkPool(size_t capacity);
	~ChunkPool();

	// Non-copyable, non-movable
	ChunkPool(const ChunkPool &) = delete;
	ChunkPool &operator=(const ChunkPool &) = delete;

	/// Obtain a chunk from the pool, reset it to the given world position.
	/// Thread-safe.
	Chunk *acquire(const glm::vec3 &worldPosition);

	/// Return a chunk to the pool for reuse.
	/// GPU resources are released and internal buffers are cleared (capacity retained).
	/// Overflow chunks (allocated outside the pool) are deleted.
	/// Thread-safe.
	void release(Chunk *chunk);

	// --- Statistics (lock-free reads) ---
	size_t capacity() const { return m_capacity; }
	size_t acquiredCount() const { return m_acquiredCount.load(std::memory_order_relaxed); }
	size_t freeCount() const { return m_capacity - m_acquiredCount.load(std::memory_order_relaxed); }
	size_t overflowCount() const { return m_overflowCount.load(std::memory_order_relaxed); }

private:
	/// Returns true if the chunk pointer belongs to the pre-allocated storage.
	bool isPoolOwned(const Chunk *chunk) const;

	size_t m_capacity;
	std::vector<Chunk> m_storage;      // Contiguous pre-allocated chunk storage
	std::vector<Chunk *> m_freeList;   // Stack of available chunk pointers
	std::mutex m_mutex;

	std::atomic<size_t> m_acquiredCount{0};
	std::atomic<size_t> m_overflowCount{0};
};
