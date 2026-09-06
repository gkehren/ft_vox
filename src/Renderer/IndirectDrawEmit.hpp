#pragma once

// Draw-time emitter for page-pair groups of indirect commands (issue #109
// review). Shared by OpaquePass / ShadowPass / WaterPass so the batching
// rules (multiDrawIndirect, maxDrawIndirectCount) exist in exactly one
// implementation instead of three that can drift.

#include "Renderer/IndirectDrawUtils.hpp"

#include <volk.h>

#include <algorithm>
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
