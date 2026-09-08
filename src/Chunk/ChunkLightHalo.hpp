#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <utils.hpp>

class Chunk;

// Transient cross-chunk block-light context (issue #141, review fix).
//
// Block light must not stop at chunk borders: during one chunk's mesh build
// the RGB block-light BFS needs up to 15 voxels of context beyond each
// border (the maximum 0..15 attenuation radius). One ChunkLightHalo holds
// the RING of neighbor voxel types around the center chunk - the center is
// always read from the Chunk itself. The light field computed over
// center+ring is only sampled inside the center chunk (plus the 1-voxel
// face-sampling shell), so nothing persistent is stored per chunk: halos
// live in a pool, are borrowed at mesh dispatch on the main thread (same
// pattern as ChunkNeighborBorders) and are released when the worker job
// finishes.
//
// Only the 4 side neighbors + 4 diagonals are needed: block light uses a
// 6-neighbour Manhattan BFS, and any path from a diagonal chunk into the
// center crosses the center-relative corner squares which the diagonals
// directly fill. Chunks two steps away or more can never contribute (their
// nearest cell is already 16+ steps out, i.e. attenuated to zero).
struct ChunkLightHalo
{
	static constexpr int kRadius = 15;
	static constexpr int kExtent = CHUNK_SIZE + 2 * kRadius; // 46
	static constexpr size_t kVolume =
		static_cast<size_t>(kExtent) * static_cast<size_t>(kExtent) * CHUNK_HEIGHT;

	// IMPORTANT: unwritten cells must read as AIR; the zero value of uint8_t
	// is BEDROCK (TextureType 0, opaque), so every borrower calls
	// resetToAir() first and the fill then overwrites the whole ring.
	// Center cells are never written (kept AIR - the consumer reads the
	// center from the Chunk).
	std::array<uint8_t, kVolume> voxels;

	// Emissive sources found in the ring while filling, so the worker's
	// seeding pass does not rescan ~476K ring cells.
	struct Emissive
	{
		uint32_t hidx;
		uint16_t emRGB4;
	};
	std::vector<Emissive> emissives;

	void resetToAir()
	{
		voxels.fill(static_cast<uint8_t>(AIR));
		emissives.clear();
	}

	// Light-field index for center-relative coordinates:
	//   hidx = hx + kExtent * (y + CHUNK_HEIGHT * hz)
	// with hx = x + kRadius, hz = z + kRadius. The same indexing is used by
	// the thread-local block-light planes (Chunk.cpp) so face sampling can
	// read the 1-voxel shell outside the chunk without a special case.
	static size_t hidx(int x, int y, int z)
	{
		return static_cast<size_t>(x + kRadius) +
			   static_cast<size_t>(kExtent) *
				   (static_cast<size_t>(y) +
					static_cast<size_t>(CHUNK_HEIGHT) *
						static_cast<size_t>(z + kRadius));
	}

	// Ring voxel at center-relative coordinates. AIR outside the ring and
	// for center cells (never stored here).
	uint8_t voxelAt(int x, int y, int z) const
	{
		if (y < 0 || y >= static_cast<int>(CHUNK_HEIGHT))
			return static_cast<uint8_t>(AIR);
		if (x < -kRadius || x >= CHUNK_SIZE + kRadius ||
			z < -kRadius || z >= CHUNK_SIZE + kRadius)
			return static_cast<uint8_t>(AIR);
		if (x >= 0 && x < static_cast<int>(CHUNK_SIZE) &&
			z >= 0 && z < static_cast<int>(CHUNK_SIZE))
			return static_cast<uint8_t>(AIR); // center: read from the Chunk
		return voxels[hidx(x, y, z)];
	}
};

// Fill the ring from the (up to) 8 horizontal neighbors around `center`.
// Defined in Chunk.cpp (needs the complete Chunk type). Missing or
// unreadable (UNLOADED - their generation worker is still filling the
// backing) neighbors contribute AIR - their chunks dirty this one through
// the arrival/edit light rules when their content lands. A neighbor in
// transit for a MESH job is read normally: its voxels are immutable while
// the mesh worker owns it (edits are deferred), and treating it as AIR
// would blank halos whenever one batch dispatches two adjacent chunks
// (issue #141 review round 3).
void fillLightHaloFromNeighbors(ChunkLightHalo &halo, const Chunk *center,
								const Chunk *west, const Chunk *east,
								const Chunk *south, const Chunk *north,
								const Chunk *southWest, const Chunk *southEast,
								const Chunk *northWest, const Chunk *northEast);

// Pool of pointer-stable ChunkLightHalo blocks (same contract as
// BorderPool): borrowed at mesh dispatch, returned when the worker job
// finishes, so no per-chunk halo memory is retained between jobs.
// Thread-safe; growth beyond the free list allocates outside the mutex.
// Invalid releases are fatal asserts in Debug and refused without
// corrupting the pool in Release.
class LightHaloPool
{
public:
	explicit LightHaloPool(size_t initialCapacity = 0)
	{
		if (initialCapacity > 0)
			reserve(initialCapacity);
	}

	ChunkLightHalo *acquire()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_freeList.empty())
			{
				ChunkLightHalo *halo = m_freeList.back();
				m_freeList.pop_back();
				m_activeCount.fetch_add(1, std::memory_order_relaxed);
				return halo;
			}
		}
		// Allocate indeterminate storage outside the mutex; borrowers must
		// resetToAir() before exposing ring coordinates (zero would be
		// BEDROCK, not AIR).
		auto block = std::make_unique_for_overwrite<ChunkLightHalo>();
		ChunkLightHalo *ptr = block.get();
		std::lock_guard<std::mutex> lock(m_mutex);
		m_storage.push_back(std::move(block));
		m_owned.push_back(ptr);
		m_activeCount.fetch_add(1, std::memory_order_relaxed);
		return ptr;
	}

	void release(ChunkLightHalo *halo)
	{
		assert(halo != nullptr && "Cannot release null ChunkLightHalo");
		if (!halo)
			return;
		std::lock_guard<std::mutex> lock(m_mutex);
		const bool owned =
			std::find(m_owned.begin(), m_owned.end(), halo) != m_owned.end();
		assert(owned && "ChunkLightHalo returned to wrong pool");
		if (!owned)
			return;
		const bool alreadyFree =
			std::find(m_freeList.begin(), m_freeList.end(), halo) != m_freeList.end();
		assert(!alreadyFree && "Double release of ChunkLightHalo detected");
		if (alreadyFree)
			return;
		m_freeList.push_back(halo);
		const size_t active = m_activeCount.load(std::memory_order_relaxed);
		assert(active > 0 && "LightHaloPool active count underflow");
		if (active > 0)
			m_activeCount.fetch_sub(1, std::memory_order_relaxed);
	}

private:
	void reserve(size_t minCapacity)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_storage.reserve(m_storage.size() + minCapacity);
		m_freeList.reserve(m_freeList.size() + minCapacity);
		for (size_t i = 0; i < minCapacity; ++i)
		{
			auto block = std::make_unique<ChunkLightHalo>();
			ChunkLightHalo *ptr = block.get();
			m_storage.push_back(std::move(block));
			m_freeList.push_back(ptr);
			m_owned.push_back(ptr);
		}
	}

	mutable std::mutex m_mutex;
	std::vector<std::unique_ptr<ChunkLightHalo>> m_storage;
	std::vector<ChunkLightHalo *> m_freeList;
	std::vector<const ChunkLightHalo *> m_owned;
	std::atomic<size_t> m_activeCount{0};
};
