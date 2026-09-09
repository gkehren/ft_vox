#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <utils.hpp>
#include <Chunk/LightSample.hpp>

/// Decoupled voxel lighting storage for a single chunk.
/// Size: exactly CHUNK_VOLUME * 2 bytes = 128 KiB (65,536 voxels * 16 bits).
/// Packing matches lighting::packVoxelLight:
///   bits 0-3: block R
///   bits 4-7: block G
///   bits 8-11: block B
///   bits 12-15: sky light
struct ChunkLightStorage
{
	std::array<uint16_t, CHUNK_VOLUME> voxels;

	uint16_t *data() { return voxels.data(); }
	const uint16_t *data() const { return voxels.data(); }
	size_t size() const { return CHUNK_VOLUME; }
	uint16_t &operator[](size_t idx) { return voxels[idx]; }
	const uint16_t &operator[](size_t idx) const { return voxels[idx]; }

	void reset()
	{
		voxels.fill(0);
	}
};

/// Reusable pool of pointer-stable ChunkLightStorage blocks.
///
/// Follows the same contract as VoxelPool: backs active meshed chunks with
/// 128 KiB light memory on demand without steady-state heap allocations during streaming.
class ChunkLightPool
{
public:
	explicit ChunkLightPool(size_t initialCapacity = 0)
	{
		if (initialCapacity > 0)
			reserve(initialCapacity);
	}

	~ChunkLightPool() = default;

	ChunkLightPool(const ChunkLightPool &) = delete;
	ChunkLightPool &operator=(const ChunkLightPool &) = delete;
	ChunkLightPool(ChunkLightPool &&) = delete;
	ChunkLightPool &operator=(ChunkLightPool &&) = delete;

	ChunkLightStorage *acquire()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_freeList.empty())
			{
				ChunkLightStorage *ptr = m_freeList.back();
				m_freeList.pop_back();
				m_activeCount.fetch_add(1, std::memory_order_relaxed);
				return ptr;
			}
		}
		auto block = std::make_unique<ChunkLightStorage>();
		ChunkLightStorage *ptr = block.get();
		std::lock_guard<std::mutex> lock(m_mutex);
		m_storage.push_back(std::move(block));
		m_owned.push_back(ptr);
		m_activeCount.fetch_add(1, std::memory_order_relaxed);
		return ptr;
	}

	void release(ChunkLightStorage *storage)
	{
		assert(storage != nullptr && "Cannot release null ChunkLightStorage");
		if (!storage)
			return;
		std::lock_guard<std::mutex> lock(m_mutex);
		const bool owned =
			std::find(m_owned.begin(), m_owned.end(), storage) != m_owned.end();
		assert(owned && "ChunkLightStorage returned to wrong pool");
		if (!owned)
			return;
		const bool alreadyFree =
			std::find(m_freeList.begin(), m_freeList.end(), storage) != m_freeList.end();
		assert(!alreadyFree && "Double release of ChunkLightStorage detected");
		if (alreadyFree)
			return;
		m_freeList.push_back(storage);
		const size_t active = m_activeCount.load(std::memory_order_relaxed);
		assert(active > 0 && "ChunkLightPool active count underflow");
		if (active > 0)
			m_activeCount.fetch_sub(1, std::memory_order_relaxed);
	}

	size_t capacity() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_storage.size();
	}

	size_t activeCount() const
	{
		return m_activeCount.load(std::memory_order_relaxed);
	}

	size_t freeCount() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_freeList.size();
	}

	void reserve(size_t minCapacity)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_storage.size() >= minCapacity)
			return;
		const size_t addCount = minCapacity - m_storage.size();
		m_storage.reserve(m_storage.size() + addCount);
		m_freeList.reserve(m_freeList.size() + addCount);
		for (size_t i = 0; i < addCount; ++i)
		{
			auto block = std::make_unique<ChunkLightStorage>();
			ChunkLightStorage *ptr = block.get();
			m_storage.push_back(std::move(block));
			m_freeList.push_back(ptr);
			m_owned.push_back(ptr);
		}
	}

	/// Trim unused free blocks down to targetFreeBlocks to return memory to the OS.
	void trim(size_t targetFreeBlocks)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_freeList.size() <= targetFreeBlocks)
			return;

		std::vector<ChunkLightStorage *> toRemove;
		while (m_freeList.size() > targetFreeBlocks)
		{
			toRemove.push_back(m_freeList.back());
			m_freeList.pop_back();
		}

		for (ChunkLightStorage *ptr : toRemove)
		{
			auto itOwned = std::find(m_owned.begin(), m_owned.end(), ptr);
			if (itOwned != m_owned.end())
				m_owned.erase(itOwned);

			auto itStorage = std::find_if(m_storage.begin(), m_storage.end(),
				[ptr](const std::unique_ptr<ChunkLightStorage> &b) { return b.get() == ptr; });
			if (itStorage != m_storage.end())
				m_storage.erase(itStorage);
		}
	}

	static ChunkLightPool &defaultPool()
	{
		static ChunkLightPool pool;
		return pool;
	}

private:
	mutable std::mutex m_mutex;
	std::vector<std::unique_ptr<ChunkLightStorage>> m_storage;
	std::vector<ChunkLightStorage *> m_freeList;
	std::vector<const ChunkLightStorage *> m_owned;
	std::atomic<size_t> m_activeCount{0};
};
