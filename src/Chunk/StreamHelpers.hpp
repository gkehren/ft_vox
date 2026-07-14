#pragma once

/// Pure helpers for Phase D streaming CPU optimizations (no Vulkan).
/// Used by ChunkManager / Chunk / TerrainGenerator and unit-tested in isolation.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <utils.hpp>

struct LoadCandidate
{
	glm::ivec3 pos{0};
	float distSq{0.f};
};

/// Nearest-first ordering for chunk load selection.
inline void sortLoadCandidatesNearestFirst(std::vector<LoadCandidate> &candidates)
{
	std::sort(candidates.begin(), candidates.end(),
			  [](const LoadCandidate &a, const LoadCandidate &b) { return a.distSq < b.distSq; });
}

/// Drop candidates beyond maxDistSq (recomputed against camPos).
inline void pruneLoadCandidatesByDistance(std::vector<LoadCandidate> &candidates,
										  const glm::vec3 &camPos, float maxDistSq)
{
	candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
									[&](const LoadCandidate &c) {
										const glm::vec3 center(
											c.pos.x * CHUNK_SIZE + CHUNK_SIZE * 0.5f, 0.f,
											c.pos.z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
										const float dx = camPos.x - center.x;
										const float dz = camPos.z - center.z;
										return (dx * dx + dz * dz) > maxDistSq;
									}),
					 candidates.end());
	// Refresh distSq after prune (camera may have moved).
	for (auto &c : candidates)
	{
		const glm::vec3 center(
			c.pos.x * CHUNK_SIZE + CHUNK_SIZE * 0.5f, 0.f,
			c.pos.z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
		const float dx = camPos.x - center.x;
		const float dz = camPos.z - center.z;
		c.distSq = dx * dx + dz * dz;
	}
}

/// Y range for 3D cave/surface noise: bedrock..surface+margin (clamped).
/// Reduces GenUniformGrid3D volume vs full CHUNK_HEIGHT.
inline void computeCaveYRange(int maxSurfaceHeight, int &yMin, int &ySize, int margin = 16)
{
	constexpr int kBedrock = 0;
	const int surface = std::max(1, maxSurfaceHeight);
	yMin = kBedrock;
	const int yMaxExclusive = std::min(CHUNK_HEIGHT, surface + margin + 1);
	ySize = std::max(1, yMaxExclusive - yMin);
}

/// Exclusive upper Y for column/border fill after 3D noise is bounded.
/// Must cover SEA_LEVEL so oceans still get WATER when surface+margin < sea level.
inline int computeColumnFillEnd(int yNoiseEnd, int seaLevel, int chunkHeight = CHUNK_HEIGHT)
{
	return std::min(chunkHeight, std::max(yNoiseEnd, seaLevel + 1));
}

/// Production bounds used by TerrainGenerator for a whole chunk (or border strip).
/// `maxSurfaceHeight` is the chunk-wide (or strip-wide) max heightmap surface —
/// not a per-column solidTop. Tests must use the same chunk-wide max.
struct ChunkYFillBounds
{
	int yMin{0};	   // inclusive start for noise/fill
	int yNoiseEnd{0};  // exclusive end of 3D noise band (= yMin + ySize)
	int yFillEnd{0};   // exclusive end of column fill (>= yNoiseEnd, covers sea)
	int ySize{0};	   // yNoiseEnd - yMin
};

inline ChunkYFillBounds computeChunkYFillBounds(int maxSurfaceHeight, int seaLevel,
												int margin = 16, int chunkHeight = CHUNK_HEIGHT)
{
	ChunkYFillBounds b{};
	computeCaveYRange(maxSurfaceHeight, b.yMin, b.ySize, margin);
	b.yNoiseEnd = b.yMin + b.ySize;
	b.yFillEnd = computeColumnFillEnd(b.yNoiseEnd, seaLevel, chunkHeight);
	return b;
}

/// Remaining work items allowed given elapsed ms and a frame time cap.
/// Returns 0 when the time budget is exhausted.
inline int remainingCountBudget(int requested, double elapsedMs, double maxStreamMs)
{
	if (requested <= 0)
		return 0;
	if (maxStreamMs <= 0.0)
		return requested; // disabled
	if (elapsedMs >= maxStreamMs)
		return 0;
	return requested;
}

/// Steady-state + headroom ChunkPool size for a view distance.
/// Matches ChunkManager unload radius = unloadFactor * maxRenderDistance.
inline size_t estimateChunkPoolCapacity(int maxRenderDistanceBlocks,
										float unloadFactor = 1.5f,
										float margin = 1.15f)
{
	constexpr size_t kMin = 64;
	constexpr size_t kMax = 16384;
	const int maxRd = maxRenderDistanceBlocks < 16 ? 16 : maxRenderDistanceBlocks;
	const float uf = unloadFactor < 1.f ? 1.f : unloadFactor;
	const float mg = margin < 1.f ? 1.f : margin;
	const float unloadBlocks = static_cast<float>(maxRd) * uf;
	const float radiusChunks = unloadBlocks / static_cast<float>(CHUNK_SIZE) + 1.f;
	const double area =
		3.14159265358979323846 * static_cast<double>(radiusChunks) * static_cast<double>(radiusChunks);
	const size_t disk = static_cast<size_t>(std::ceil(area * static_cast<double>(mg)));
	const size_t headroom = 96 + static_cast<size_t>(std::ceil(radiusChunks * 2.5f));
	size_t needed = disk + headroom;
	if (needed < kMin)
		needed = kMin;
	if (needed > kMax)
		needed = kMax;
	return needed;
}

/// Occupancy bounds for non-air voxels.
/// index = y * (CHUNK_SIZE*CHUNK_SIZE) + z * CHUNK_SIZE + x  (matches Chunk::getIndex).
/// Returns false if the chunk is empty (all air).
inline bool computeOccupancyY(const uint8_t *types, int &outMinY, int &outMaxY)
{
	if (!types)
		return false;
	const uint8_t air = static_cast<uint8_t>(AIR);
	int minY = CHUNK_HEIGHT;
	int maxY = -1;
	for (int y = 0; y < CHUNK_HEIGHT; ++y)
	{
		const int layerBase = y * CHUNK_SIZE * CHUNK_SIZE;
		bool any = false;
		for (int i = 0; i < CHUNK_SIZE * CHUNK_SIZE; ++i)
		{
			if (types[layerBase + i] != air)
			{
				any = true;
				break;
			}
		}
		if (any)
		{
			minY = std::min(minY, y);
			maxY = std::max(maxY, y);
		}
	}
	if (maxY < 0)
		return false;
	outMinY = minY;
	outMaxY = maxY;
	return true;
}
