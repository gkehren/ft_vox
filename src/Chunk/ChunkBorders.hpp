#pragma once

#include <algorithm>
#include <cassert>
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>
#include <utils.hpp>

// Compact neighbor border storage (issue #103).
//
// The previous representation was a dense padded 18 x (CHUNK_HEIGHT+2) x 18
// byte shell (83,592 bytes per chunk) even though the rebuild path only ever
// populates the four horizontal neighbor faces plus four diagonal corner
// columns, and generation never writes the y = -1 / y = CHUNK_HEIGHT padding
// rows (fill bounds start at kBedrock = 0 and stop at CHUNK_HEIGHT).
//
// One block holds everything the mesher can sample across chunk borders:
//   - four faces of CHUNK_HEIGHT * CHUNK_SIZE bytes (x or z = -1 / CHUNK_SIZE)
//   - four diagonal corner columns of CHUNK_HEIGHT bytes (used by AO)
// 4 * 4096 + 4 * 256 = 17,408 bytes: ~4.8x smaller than the dense shell, and
// blocks live in a pool so chunks do not retain border memory after upload.
struct ChunkNeighborBorders
{
	static constexpr size_t kFaceSize = static_cast<size_t>(CHUNK_HEIGHT) * CHUNK_SIZE;

	// Face values indexed [y * CHUNK_SIZE + t]:
	//   west  -> neighbor at local x = -1,          t = z
	//   east  -> neighbor at local x = CHUNK_SIZE,  t = z
	//   south -> neighbor at local z = -1,          t = x
	//   north -> neighbor at local z = CHUNK_SIZE,  t = x
	// No default member initializers: pool blocks are created with
	// make_unique_for_overwrite and initialized by the consumer (AIR via
	// resetToAir(), or fully overwritten by generation).
	std::array<uint8_t, kFaceSize> west;
	std::array<uint8_t, kFaceSize> east;
	std::array<uint8_t, kFaceSize> south;
	std::array<uint8_t, kFaceSize> north;

	// Diagonal neighbor columns indexed [y]:
	//   SW -> (x = -1, z = -1), SE -> (x = CHUNK_SIZE, z = -1),
	//   NW -> (x = -1, z = CHUNK_SIZE), NE -> (x = CHUNK_SIZE, z = CHUNK_SIZE)
	std::array<uint8_t, CHUNK_HEIGHT> cornerSW;
	std::array<uint8_t, CHUNK_HEIGHT> cornerSE;
	std::array<uint8_t, CHUNK_HEIGHT> cornerNW;
	std::array<uint8_t, CHUNK_HEIGHT> cornerNE;

	// IMPORTANT: unwritten cells hold indeterminate bytes (the zero value of
	// uint8_t is BEDROCK, TextureType 0 - not AIR). Every borrower must call
	// resetToAir() before exposing border coordinates, so unwritten cells
	// read as AIR.
	void resetToAir()
	{
		west.fill(static_cast<uint8_t>(AIR));
		east.fill(static_cast<uint8_t>(AIR));
		south.fill(static_cast<uint8_t>(AIR));
		north.fill(static_cast<uint8_t>(AIR));
		cornerSW.fill(static_cast<uint8_t>(AIR));
		cornerSE.fill(static_cast<uint8_t>(AIR));
		cornerNW.fill(static_cast<uint8_t>(AIR));
		cornerNE.fill(static_cast<uint8_t>(AIR));
	}

	// Border sampling for meshing with x/z in [-1, CHUNK_SIZE] and y in
	// [0, CHUNK_HEIGHT). Vertical out-of-range reads as AIR (generation never
	// writes those rows). Corner coordinates (both x and z on a border)
	// resolve to the matching diagonal column. In-chunk and out-of-neighbor
	// coordinates read as AIR.
	uint8_t at(int x, int y, int z) const
	{
		if (y < 0 || y >= static_cast<int>(CHUNK_HEIGHT))
			return static_cast<uint8_t>(AIR);

		if (x < -1 || x > static_cast<int>(CHUNK_SIZE) ||
			z < -1 || z > static_cast<int>(CHUNK_SIZE))
			return static_cast<uint8_t>(AIR);

		const bool westSide = x == -1;
		const bool eastSide = x == static_cast<int>(CHUNK_SIZE);
		const bool southSide = z == -1;
		const bool northSide = z == static_cast<int>(CHUNK_SIZE);

		if (!westSide && !eastSide && !southSide && !northSide)
			return static_cast<uint8_t>(AIR); // in-chunk coordinate

		const size_t yi = static_cast<size_t>(y);
		if (westSide || eastSide)
		{
			const size_t t = static_cast<size_t>(z);
			if (southSide)
				return westSide ? cornerSW[yi] : cornerSE[yi];
			if (northSide)
				return westSide ? cornerNW[yi] : cornerNE[yi];
			return westSide ? west[yi * CHUNK_SIZE + t] : east[yi * CHUNK_SIZE + t];
		}
		const size_t t = static_cast<size_t>(x);
		return southSide ? south[yi * CHUNK_SIZE + t] : north[yi * CHUNK_SIZE + t];
	}

	// Writable variant used by edits at chunk boundaries. Coordinates must
	// lie exactly on the 1-thick border; anything else is a programming
	// error (asserted in Debug).
	uint8_t &mutableAt(int x, int y, int z)
	{
		assert(y >= 0 && y < static_cast<int>(CHUNK_HEIGHT));
		assert(x >= -1 && x <= static_cast<int>(CHUNK_SIZE));
		assert(z >= -1 && z <= static_cast<int>(CHUNK_SIZE));

		const bool onBorder =
			x == -1 || x == static_cast<int>(CHUNK_SIZE) ||
			z == -1 || z == static_cast<int>(CHUNK_SIZE);
		assert(onBorder &&
			   "ChunkNeighborBorders::mutableAt requires border coordinate");

		const size_t yi = static_cast<size_t>(y);
		const bool westSide = x == -1;
		const bool eastSide = x == static_cast<int>(CHUNK_SIZE);
		const bool southSide = z == -1;
		const bool northSide = z == static_cast<int>(CHUNK_SIZE);
		if (westSide || eastSide)
		{
			const size_t t = static_cast<size_t>(z);
			if (southSide)
				return westSide ? cornerSW[yi] : cornerSE[yi];
			if (northSide)
				return westSide ? cornerNW[yi] : cornerNE[yi];
			return westSide ? west[yi * CHUNK_SIZE + t] : east[yi * CHUNK_SIZE + t];
		}
		const size_t t = static_cast<size_t>(x);
		return southSide ? south[yi * CHUNK_SIZE + t] : north[yi * CHUNK_SIZE + t];
	}
};

// Pool of pointer-stable ChunkNeighborBorders blocks (same contract as
// VoxelPool): borrowed for generation/meshing, returned after upload so no
// per-chunk border memory is retained. Thread-safe; growth beyond the free
// list allocates outside the mutex. Invalid releases are fatal asserts in
// Debug and refused without corrupting the pool in Release.
class BorderPool
{
public:
	explicit BorderPool(size_t initialCapacity = 0);
	~BorderPool();

	BorderPool(const BorderPool &) = delete;
	BorderPool &operator=(const BorderPool &) = delete;
	BorderPool(BorderPool &&) = delete;
	BorderPool &operator=(BorderPool &&) = delete;

	ChunkNeighborBorders *acquire();
	void release(ChunkNeighborBorders *borders);

	size_t capacity() const;
	size_t activeCount() const;
	size_t freeCount() const;

	void reserve(size_t minCapacity);

	static BorderPool &defaultPool();

private:
	mutable std::mutex m_mutex;
	std::vector<std::unique_ptr<ChunkNeighborBorders>> m_storage;
	std::vector<ChunkNeighborBorders *> m_freeList;
	std::vector<const ChunkNeighborBorders *> m_owned;
	std::atomic<size_t> m_activeCount{0};
};
