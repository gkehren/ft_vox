#pragma once

#include <vector>
#include <mutex>
#include <memory>
#include <cstddef>
#include <atomic>
#include <array>
#include <glm/glm.hpp>
#include <Chunk/VoxelPool.hpp>

class Chunk;

/// Thread-safe object pool for Chunk instances with **pointer-stable** storage.
///
/// Storage is a list of heap-owned Chunks (`unique_ptr`), so capacity can grow
/// via ensureCapacity() without invalidating existing Chunk* handles held by
/// ChunkManager.
///
/// When the free list is empty, acquire() returns nullptr (no heap spam).
/// Callers should ensureCapacity() from the stream distance settings and apply
/// load back-pressure when freeCount() is 0.
class ChunkPool
{
public:
	/// @param initialCapacity Number of chunks to pre-allocate (clamped to >= 1).
	explicit ChunkPool(size_t initialCapacity);
	~ChunkPool();

	ChunkPool(const ChunkPool &) = delete;
	ChunkPool &operator=(const ChunkPool &) = delete;

	/// Grow pool so capacity() >= minCapacity. Never shrinks. Thread-safe.
	/// Returns true if new chunks were allocated.
	bool ensureCapacity(size_t minCapacity);

	/// Obtain a chunk reset to worldPosition, or nullptr if the free list is empty.
	/// Must call generateTerrain() before consuming voxel data.
	/// Thread-safe. Does not heap-allocate overflow chunks.
	Chunk *acquire(const glm::vec3 &worldPosition);

	/// Return a chunk to the free list (pool-owned) or delete (unknown pointer).
	/// Thread-safe.
	void release(Chunk *chunk);

	// --- Statistics (lock-free reads; freeCount may lag slightly under concurrent use) ---
	size_t capacity() const { return m_capacity.load(std::memory_order_relaxed); }
	size_t acquiredCount() const { return m_acquiredCount.load(std::memory_order_relaxed); }
	size_t freeCount() const
	{
		const size_t cap = m_capacity.load(std::memory_order_relaxed);
		const size_t acq = m_acquiredCount.load(std::memory_order_relaxed);
		return cap > acq ? cap - acq : 0;
	}
	/// How many times acquire() returned nullptr because the free list was empty.
	size_t rejectCount() const { return m_rejectCount.load(std::memory_order_relaxed); }
	/// Number of successful grow operations (ensureCapacity that allocated).
	size_t growEvents() const { return m_growEvents.load(std::memory_order_relaxed); }

	size_t voxelStorageCapacity() const { return m_voxelPool.capacity(); }
	size_t voxelStorageActive() const { return m_voxelPool.activeCount(); }
	size_t voxelStorageFree() const { return m_voxelPool.freeCount(); }
	VoxelPool &voxelPool() { return m_voxelPool; }
	const VoxelPool &voxelPool() const { return m_voxelPool; }

private:
	void publishTelemetry(); // caller holds m_mutex
	std::array<size_t, 3> m_telemetry{};
	void growUnlocked(size_t addCount); // caller holds m_mutex

	// Must be declared before m_storage so it outlives all Chunk destructors
	// (members destroy in reverse order): retiring chunks release their
	// VoxelStorage back into this pool.
	VoxelPool m_voxelPool;

	/// All pool-owned chunks; unique_ptr keeps addresses stable across vector growth.
	std::vector<std::unique_ptr<Chunk>> m_storage;
	std::vector<Chunk *> m_freeList;
	std::vector<const Chunk *> m_owned;
	std::mutex m_mutex;

	std::atomic<size_t> m_capacity{0};
	std::atomic<size_t> m_acquiredCount{0};
	std::atomic<size_t> m_rejectCount{0};
	std::atomic<size_t> m_growEvents{0};
};

// estimateChunkPoolCapacity() lives in StreamHelpers.hpp (shared with tests / UI).
#include <Chunk/StreamHelpers.hpp>
