#pragma once

// Draw-time emitter for page-pair groups of indirect commands (issue #109
// review). Shared by OpaquePass / ShadowPass / WaterPass so the batching
// rules (multiDrawIndirect, maxDrawIndirectCount) exist in exactly one
// implementation instead of three that can drift.

#include "Renderer/IndirectDrawUtils.hpp"
#include "Renderer/VoxelDrawDataLayout.hpp"
#include "Chunk/Chunk.hpp"

#include <volk.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>

/// Record `commandCount` consecutive VkDrawIndexedIndirectCommand entries
/// starting at `firstCommand`, split into batches of at most `maxBatch`
/// (batchLimit from indirectBatchLimit). With maxBatch == 1 this degrades to
/// one draw per command - the spec-legal fallback - without any extra branch
/// at the call sites.
inline void issueIndirectRange(VkCommandBuffer cmd, VkBuffer indirectBuffer,
							   size_t firstCommand, size_t commandCount, uint32_t maxBatch)
{
	size_t done = 0;
	while (done < commandCount)
	{
		const uint32_t batch =
			static_cast<uint32_t>(std::min<size_t>(commandCount - done, maxBatch));
		vkCmdDrawIndexedIndirect(cmd, indirectBuffer,
								 (firstCommand + done) * sizeof(VkDrawIndexedIndirectCommand),
								 batch, sizeof(VkDrawIndexedIndirectCommand));
		done += batch;
	}
}

struct PageBatch
{
	uint64_t key{0};
	uint32_t first{0};
	uint32_t count{0};
};

/// Groups `count` draw commands by arena (vertexPage, indexPage) pair into
/// `dst` and `drawDataOut`. Populates `outBatches` with contiguous ranges
/// for each distinct page pair.
template <size_t MaxBatches = 32>
inline size_t groupIndirectDrawsByPage(
	const Chunk::IndirectDraw *src, size_t count, uint32_t baseInstance,
	VkDrawIndexedIndirectCommand *dst, VoxelDrawData *drawDataOut,
	std::array<PageBatch, MaxBatches> &outBatches)
{
	if (count == 0)
		return 0;

	const uint64_t firstKey = (uint64_t(src[0].vertexPage) << 32) | src[0].indexPage;
	bool single = true;
	for (size_t i = 1; i < count; ++i)
	{
		if (((uint64_t(src[i].vertexPage) << 32) | src[i].indexPage) != firstKey)
		{
			single = false;
			break;
		}
	}

	if (single)
	{
		outBatches[0] = {firstKey, 0, static_cast<uint32_t>(count)};
		for (size_t i = 0; i < count; ++i)
		{
			dst[i] = src[i].cmd;
			dst[i].firstInstance = static_cast<uint32_t>(baseInstance + i);
			if (drawDataOut)
			{
				drawDataOut[baseInstance + i].worldOrigin = src[i].chunkOrigin;
				drawDataOut[baseInstance + i].flags = 0;
			}
		}
		return 1;
	}

	// Multiple page pairs: linear 2-pass bucket grouping directly into mapped
	// destination. Replaces O(N log N) std::sort on 40-byte structs with O(N)
	// linear copy.
	uint32_t batchCount = 0;
	uint32_t lastB = 0;
	uint64_t lastKey = 0;
	for (size_t i = 0; i < count; ++i)
	{
		const uint64_t key = (uint64_t(src[i].vertexPage) << 32) | src[i].indexPage;
		uint32_t b = lastB;
		if (i == 0 || key != lastKey)
		{
			b = 0;
			while (b < batchCount && outBatches[b].key != key)
				++b;
			if (b == batchCount)
			{
				assert(batchCount < MaxBatches && "exceeded max page batches");
				outBatches[b].key = key;
				outBatches[b].count = 0;
				++batchCount;
			}
			lastB = b;
			lastKey = key;
		}
		outBatches[b].count++;
	}

	std::array<uint32_t, MaxBatches> writeOffsets;
	uint32_t runningFirst = 0;
	for (uint32_t b = 0; b < batchCount; ++b)
	{
		outBatches[b].first = runningFirst;
		writeOffsets[b] = runningFirst;
		runningFirst += outBatches[b].count;
	}

	lastB = 0;
	lastKey = outBatches[0].key;
	for (size_t i = 0; i < count; ++i)
	{
		const uint64_t key = (uint64_t(src[i].vertexPage) << 32) | src[i].indexPage;
		uint32_t b = lastB;
		if (key != lastKey)
		{
			b = 0;
			while (outBatches[b].key != key)
				++b;
			lastB = b;
			lastKey = key;
		}
		const uint32_t dstIdx = writeOffsets[b]++;
		dst[dstIdx] = src[i].cmd;
		dst[dstIdx].firstInstance = static_cast<uint32_t>(baseInstance + dstIdx);
		if (drawDataOut)
		{
			drawDataOut[baseInstance + dstIdx].worldOrigin = src[i].chunkOrigin;
			drawDataOut[baseInstance + dstIdx].flags = 0;
		}
	}

	return batchCount;
}
