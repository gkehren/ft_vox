#pragma once

#include <array>
#include <memory>
#include <vector>
#include <mutex>
#include <cstddef>
#include <atomic>
#include <utils.hpp>

/// Decoupled voxel backing storage for a single chunk.
/// Size: exactly CHUNK_VOLUME bytes (65,536 voxels).
struct VoxelStorage
{
	std::array<Voxel, CHUNK_VOLUME> voxels;

	Voxel *data() { return voxels.data(); }
	const Voxel *data() const { return voxels.data(); }
	size_t size() const { return CHUNK_VOLUME; }
	Voxel &operator[](size_t idx) { return voxels[idx]; }
	const Voxel &operator[](size_t idx) const { return voxels[idx]; }
};

/// Reusable pool of pointer-stable VoxelStorage blocks.
///
/// Backs active chunks with 64 KiB voxel memory on demand without allocating
/// storage for free pool chunk slots. Reuses returned blocks from a free list
/// so no steady-state heap allocation occurs during continuous streaming.
class VoxelPool
{
public:
	explicit VoxelPool(size_t initialCapacity = 0);
	~VoxelPool();

	VoxelPool(const VoxelPool &) = delete;
	VoxelPool &operator=(const VoxelPool &) = delete;
	VoxelPool(VoxelPool &&) = delete;
	VoxelPool &operator=(VoxelPool &&) = delete;

	/// Acquire a VoxelStorage block. Reuses from free list if available,
	/// or allocates a new block if free list is empty. Thread-safe.
	VoxelStorage *acquire();

	/// Return a VoxelStorage block to the pool for future reuse. Thread-safe.
	void release(VoxelStorage *storage);

	/// Total number of VoxelStorage blocks currently allocated in this pool.
	size_t capacity() const;

	/// Number of VoxelStorage blocks currently in use.
	size_t activeCount() const;

	/// Number of VoxelStorage blocks available in the free list.
	size_t freeCount() const;

	/// Pre-allocate up to minCapacity blocks. Thread-safe.
	void reserve(size_t minCapacity);

	/// Default global pool used by standalone chunks outside of ChunkPool.
	static VoxelPool &defaultPool();

private:
	void publishTelemetry(size_t cap, size_t act, size_t fre);

	mutable std::mutex m_mutex;
	std::vector<std::unique_ptr<VoxelStorage>> m_storage;
	std::vector<VoxelStorage *> m_freeList;
	std::vector<const VoxelStorage *> m_owned;
	std::atomic<size_t> m_activeCount{0};
	std::array<size_t, 3> m_telemetry{};
};
