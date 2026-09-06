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

/// View-direction biased squared distance for load selection: chunks in front of
/// the camera count as closer (load sooner and farther out), chunks behind count
/// as farther. frontBias in [0,1): ahead reach ~ dist / sqrt(1-bias), behind
/// ~ dist / sqrt(1+bias). camForwardXZ must be normalized (or zero = no bias).
inline float biasedLoadDistSq(const glm::vec3 &camPos, const glm::vec3 &chunkCenter,
							  const glm::vec2 &camForwardXZ, float frontBias)
{
	const float dx = chunkCenter.x - camPos.x;
	const float dz = chunkCenter.z - camPos.z;
	const float distSq = dx * dx + dz * dz;
	const float bias = glm::clamp(frontBias, 0.f, 0.9f);
	if (bias <= 0.f || distSq < 1e-6f)
		return distSq;
	const float invLen = 1.0f / std::sqrt(distSq);
	const float facing = (dx * invLen) * camForwardXZ.x + (dz * invLen) * camForwardXZ.y;
	return distSq * (1.0f - bias * glm::clamp(facing, -1.f, 1.f));
}

/// Drop candidates beyond maxDistSq (recomputed against camPos, with optional
/// view-direction bias — see biasedLoadDistSq).
inline void pruneLoadCandidatesByDistance(std::vector<LoadCandidate> &candidates,
										  const glm::vec3 &camPos, float maxDistSq,
										  const glm::vec2 &camForwardXZ = glm::vec2(0.f),
										  float frontBias = 0.f)
{
	candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
									[&](const LoadCandidate &c) {
										const glm::vec3 center(
											c.pos.x * CHUNK_SIZE + CHUNK_SIZE * 0.5f, 0.f,
											c.pos.z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
										return biasedLoadDistSq(camPos, center, camForwardXZ, frontBias) > maxDistSq;
									}),
					 candidates.end());
	// Refresh distSq after prune (camera may have moved).
	for (auto &c : candidates)
	{
		const glm::vec3 center(
			c.pos.x * CHUNK_SIZE + CHUNK_SIZE * 0.5f, 0.f,
			c.pos.z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
		c.distSq = biasedLoadDistSq(camPos, center, camForwardXZ, frontBias);
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

// -----------------------------------------------------------------------------
// Incremental streaming structures & helpers (issue #108)
// -----------------------------------------------------------------------------

/// Minimum dot-product between camera forward vectors before front-bias
/// requires queue/footprint reconciliation (~10 degrees).
constexpr float kStreamHeadingCosThreshold = 0.985f;

/// Intra-chunk movement granularity for streaming reconciliation (issue #108
/// review): the desired footprint is a function of the exact camera position,
/// so maintenance must not go stale for a whole 16-block chunk. Reconcile
/// whenever the camera enters a new world-space anchor cell (4 blocks → at
/// most 4 reconciliations per axis per chunk, still far below a full scan).
constexpr int kStreamingAnchorBlocks = 4;

/// Quantized world-space anchor cell of the camera XZ position. Floor
/// division so negative coordinates quantize symmetrically.
inline glm::ivec2 streamingMovementAnchor(const glm::vec3 &pos)
{
	return {
		static_cast<int>(std::floor(pos.x / static_cast<float>(kStreamingAnchorBlocks))),
		static_cast<int>(std::floor(pos.z / static_cast<float>(kStreamingAnchorBlocks)))};
}

/// Bias below which front-bias reshaping is considered absent: at bias 0 the
/// desired region is rotation-invariant, so heading changes are no-ops.
constexpr float kStreamBiasEpsilon = 1e-4f;

/// Runtime front-bias value actually used by the footprint math (the UI may
/// hand in values beyond the clamp — compare/store the canonical value, not
/// the raw one, so clamped settings never trigger spurious rebuilds).
inline float normalizedStreamFrontBias(float bias)
{
	return glm::clamp(bias, 0.f, 0.9f);
}

/// True when two front-bias settings differ beyond float noise after
/// normalization. Single comparison rule for every invalidation check.
inline bool streamFrontBiasChanged(float a, float b)
{
	return std::abs(normalizedStreamFrontBias(a) - normalizedStreamFrontBias(b)) > kStreamBiasEpsilon;
}

/// Cheap main-thread streaming maintenance counters (issue #108): how frames
/// split between the zero-work fast path and the reconciliation paths, plus
/// queue churn. Published via ChunkManager::streamingMaintenanceStats() for
/// benchmarks and tests; the pure helpers above stay un-instrumented.
struct StreamingMaintenanceStats
{
	uint64_t zeroWork{0};
	uint64_t incrementalUpdates{0};
	uint64_t headingRebuilds{0};
	uint64_t fullRebuilds{0};
	uint64_t queueSorts{0};
	uint64_t enteringCandidates{0};
	uint64_t exitingCandidates{0};
	uint64_t unloadScans{0};
};

/// Contiguous interval of chunk X coordinates in a row Z that lie within the desired load region.
struct ChunkRowSpan
{
	int minX{1};
	int maxX{0};
	bool empty() const noexcept { return minX > maxX; }
	bool contains(int x) const noexcept { return x >= minX && x <= maxX; }
};

/// Bounded desired-chunk footprint represented as a span per row Z.
struct ChunkDesiredFootprint
{
	int minZ{1};
	int maxZ{0};
	std::vector<ChunkRowSpan> spans; // index: z - minZ

	bool empty() const noexcept { return minZ > maxZ || spans.empty(); }
	const ChunkRowSpan *spanForZ(int z) const noexcept
	{
		if (z < minZ || z > maxZ || spans.empty())
			return nullptr;
		return &spans[static_cast<size_t>(z - minZ)];
	}
	bool contains(int x, int z) const noexcept
	{
		const ChunkRowSpan *s = spanForZ(z);
		return s && s->contains(x);
	}
};

/// Newly entered and newly exited coordinates produced by an incremental update.
struct FootprintDiff
{
	std::vector<glm::ivec3> entering;
	std::vector<glm::ivec3> exiting;
};

/// Brute-force reference implementation for deterministic validation and testing.
inline std::vector<glm::ivec3> computeDesiredChunkSetBruteForce(
	const glm::ivec3 &camChunk,
	const glm::vec3 &camPos,
	const glm::vec2 &camForwardXZ,
	float frontBias,
	int maxRenderDistance)
{
	const float maxDistSq = static_cast<float>(maxRenderDistance) * static_cast<float>(maxRenderDistance);
	const float reachBlocks =
		static_cast<float>(maxRenderDistance) / std::sqrt(1.0f - glm::clamp(frontBias, 0.f, 0.9f));
	const int radius =
		static_cast<int>(std::ceil(reachBlocks / static_cast<float>(CHUNK_SIZE)));

	std::vector<glm::ivec3> res;
	for (int z = -radius; z <= radius; ++z)
	{
		for (int x = -radius; x <= radius; ++x)
		{
			const glm::ivec3 pos = camChunk + glm::ivec3(x, 0, z);
			const glm::vec3 center(
				pos.x * CHUNK_SIZE + CHUNK_SIZE * 0.5f, 0.f,
				pos.z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
			if (biasedLoadDistSq(camPos, center, camForwardXZ, frontBias) <= maxDistSq)
				res.push_back(pos);
		}
	}
	return res;
}

/// Convert footprint spans to an explicit coordinate list (for verification).
inline std::vector<glm::ivec3> footprintToCoordList(const ChunkDesiredFootprint &fp)
{
	std::vector<glm::ivec3> res;
	if (fp.empty())
		return res;
	for (int z = fp.minZ; z <= fp.maxZ; ++z)
	{
		const ChunkRowSpan *s = fp.spanForZ(z);
		if (s && !s->empty())
		{
			for (int x = s->minX; x <= s->maxX; ++x)
				res.push_back({x, 0, z});
		}
	}
	return res;
}

/// Compute full contiguous row span for row z.
inline ChunkRowSpan computeRowSpanFull(int z, const glm::ivec3 &camChunk, const glm::vec3 &camPos,
									  const glm::vec2 &camForwardXZ, float frontBias, float maxDistSq, int radius)
{
	const int xStart = camChunk.x - radius;
	const int xEnd = camChunk.x + radius;
	int minX = xEnd + 1;
	int maxX = xStart - 1;

	for (int x = xStart; x <= xEnd; ++x)
	{
		const glm::vec3 center(
			x * CHUNK_SIZE + CHUNK_SIZE * 0.5f, 0.f,
			z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
		if (biasedLoadDistSq(camPos, center, camForwardXZ, frontBias) <= maxDistSq)
		{
			minX = x;
			break;
		}
	}
	if (minX > xEnd)
		return ChunkRowSpan{1, 0};

	for (int x = xEnd; x >= minX; --x)
	{
		const glm::vec3 center(
			x * CHUNK_SIZE + CHUNK_SIZE * 0.5f, 0.f,
			z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
		if (biasedLoadDistSq(camPos, center, camForwardXZ, frontBias) <= maxDistSq)
		{
			maxX = x;
			break;
		}
	}
	return ChunkRowSpan{minX, maxX};
}

/// Incrementally update row span for row z starting from previous span.
inline ChunkRowSpan computeRowSpanIncremental(int z, const ChunkRowSpan &oldSpan,
											  const glm::ivec3 &camChunk, const glm::vec3 &camPos,
											  const glm::vec2 &camForwardXZ, float frontBias, float maxDistSq, int radius)
{
	const int xStart = camChunk.x - radius;
	const int xEnd = camChunk.x + radius;

	if (oldSpan.empty())
		return computeRowSpanFull(z, camChunk, camPos, camForwardXZ, frontBias, maxDistSq, radius);

	auto inRange = [&](int x) -> bool {
		if (x < xStart || x > xEnd)
			return false;
		const glm::vec3 center(
			x * CHUNK_SIZE + CHUNK_SIZE * 0.5f, 0.f,
			z * CHUNK_SIZE + CHUNK_SIZE * 0.5f);
		return biasedLoadDistSq(camPos, center, camForwardXZ, frontBias) <= maxDistSq;
	};

	const bool minInRange = inRange(oldSpan.minX);
	const bool maxInRange = inRange(oldSpan.maxX);

	// If neither endpoint is in range, row shifted completely or emptied (tip rows).
	if (!minInRange && !maxInRange)
		return computeRowSpanFull(z, camChunk, camPos, camForwardXZ, frontBias, maxDistSq, radius);

	int minX = oldSpan.minX;
	if (minInRange)
	{
		while (minX > xStart && inRange(minX - 1))
			--minX;
	}
	else
	{
		while (minX <= oldSpan.maxX && !inRange(minX))
			++minX;
	}

	int maxX = oldSpan.maxX;
	if (maxInRange)
	{
		while (maxX < xEnd && inRange(maxX + 1))
			++maxX;
	}
	else
	{
		while (maxX >= minX && !inRange(maxX))
			--maxX;
	}

	if (minX > maxX)
		return ChunkRowSpan{1, 0};

	return ChunkRowSpan{minX, maxX};
}

/// Compute full footprint from scratch.
inline ChunkDesiredFootprint computeDesiredFootprintFull(
	const glm::ivec3 &camChunk,
	const glm::vec3 &camPos,
	const glm::vec2 &camForwardXZ,
	float frontBias,
	int maxRenderDistance)
{
	const float maxDistSq = static_cast<float>(maxRenderDistance) * static_cast<float>(maxRenderDistance);
	const float reachBlocks =
		static_cast<float>(maxRenderDistance) / std::sqrt(1.0f - glm::clamp(frontBias, 0.f, 0.9f));
	const int radius =
		static_cast<int>(std::ceil(reachBlocks / static_cast<float>(CHUNK_SIZE)));

	ChunkDesiredFootprint fp;
	fp.minZ = camChunk.z - radius;
	fp.maxZ = camChunk.z + radius;
	const size_t rowCount = static_cast<size_t>(fp.maxZ - fp.minZ + 1);
	fp.spans.resize(rowCount);

	for (int z = fp.minZ; z <= fp.maxZ; ++z)
	{
		fp.spans[static_cast<size_t>(z - fp.minZ)] =
			computeRowSpanFull(z, camChunk, camPos, camForwardXZ, frontBias, maxDistSq, radius);
	}
	return fp;
}

/// Compute updated footprint incrementally from previous footprint.
inline ChunkDesiredFootprint computeDesiredFootprintIncremental(
	const ChunkDesiredFootprint &oldFootprint,
	const glm::ivec3 &camChunk,
	const glm::vec3 &camPos,
	const glm::vec2 &camForwardXZ,
	float frontBias,
	int maxRenderDistance)
{
	const float maxDistSq = static_cast<float>(maxRenderDistance) * static_cast<float>(maxRenderDistance);
	const float reachBlocks =
		static_cast<float>(maxRenderDistance) / std::sqrt(1.0f - glm::clamp(frontBias, 0.f, 0.9f));
	const int radius =
		static_cast<int>(std::ceil(reachBlocks / static_cast<float>(CHUNK_SIZE)));

	ChunkDesiredFootprint fp;
	fp.minZ = camChunk.z - radius;
	fp.maxZ = camChunk.z + radius;
	const size_t rowCount = static_cast<size_t>(fp.maxZ - fp.minZ + 1);
	fp.spans.resize(rowCount);

	for (int z = fp.minZ; z <= fp.maxZ; ++z)
	{
		const ChunkRowSpan *oldSpan = oldFootprint.spanForZ(z);
		ChunkRowSpan dummyEmpty{1, 0};
		const ChunkRowSpan &sOld = oldSpan ? *oldSpan : dummyEmpty;
		fp.spans[static_cast<size_t>(z - fp.minZ)] =
			computeRowSpanIncremental(z, sOld, camChunk, camPos, camForwardXZ, frontBias, maxDistSq, radius);
	}
	return fp;
}

/// Compute symmetric difference between old and new footprint.
inline FootprintDiff computeFootprintDiff(const ChunkDesiredFootprint &oldFootprint,
										  const ChunkDesiredFootprint &newFootprint)
{
	FootprintDiff diff;
	if (oldFootprint.empty() && newFootprint.empty())
		return diff;

	const int zMin = std::min(oldFootprint.empty() ? newFootprint.minZ : oldFootprint.minZ,
							  newFootprint.empty() ? oldFootprint.minZ : newFootprint.minZ);
	const int zMax = std::max(oldFootprint.empty() ? newFootprint.maxZ : oldFootprint.maxZ,
							  newFootprint.empty() ? oldFootprint.maxZ : newFootprint.maxZ);

	for (int z = zMin; z <= zMax; ++z)
	{
		const ChunkRowSpan *sOld = oldFootprint.spanForZ(z);
		const ChunkRowSpan *sNew = newFootprint.spanForZ(z);
		const bool hasOld = (sOld && !sOld->empty());
		const bool hasNew = (sNew && !sNew->empty());

		if (!hasOld && !hasNew)
			continue;
		if (!hasOld && hasNew)
		{
			for (int x = sNew->minX; x <= sNew->maxX; ++x)
				diff.entering.push_back({x, 0, z});
			continue;
		}
		if (hasOld && !hasNew)
		{
			for (int x = sOld->minX; x <= sOld->maxX; ++x)
				diff.exiting.push_back({x, 0, z});
			continue;
		}

		// Both old and new exist
		if (sNew->minX > sOld->maxX || sNew->maxX < sOld->minX)
		{
			// Disjoint
			for (int x = sOld->minX; x <= sOld->maxX; ++x)
				diff.exiting.push_back({x, 0, z});
			for (int x = sNew->minX; x <= sNew->maxX; ++x)
				diff.entering.push_back({x, 0, z});
			continue;
		}

		// Overlapping spans:
		// Left side
		if (sNew->minX < sOld->minX)
		{
			for (int x = sNew->minX; x < sOld->minX; ++x)
				diff.entering.push_back({x, 0, z});
		}
		else if (sNew->minX > sOld->minX)
		{
			for (int x = sOld->minX; x < sNew->minX; ++x)
				diff.exiting.push_back({x, 0, z});
		}

		// Right side
		if (sNew->maxX > sOld->maxX)
		{
			for (int x = sOld->maxX + 1; x <= sNew->maxX; ++x)
				diff.entering.push_back({x, 0, z});
		}
		else if (sNew->maxX < sOld->maxX)
		{
			for (int x = sNew->maxX + 1; x <= sOld->maxX; ++x)
				diff.exiting.push_back({x, 0, z});
		}
	}
	return diff;
}

