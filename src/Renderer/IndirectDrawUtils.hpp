#pragma once

// Pure helpers governing how a page-pair group of indirect draw commands is
// submitted (issue #109 review): a group may only be batched into one
// vkCmdDrawIndexedIndirect when the device enables multiDrawIndirect AND the
// batch stays under VkPhysicalDeviceLimits::maxDrawIndirectCount. Deliberately
// Vulkan-free so the splitting logic is unit-testable headless.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

/// How many commands one vkCmdDrawIndexedIndirect may carry:
/// 1 without multiDrawIndirect, otherwise the hardware ceiling (never 0).
/// FT_VOX_MAX_INDIRECT_BATCH (diagnostic only, like FT_VOX_DRAW_DIRECT)
/// clamps the limit down to force the splitting path on any GPU.
inline uint32_t indirectBatchLimit(bool multiDrawIndirect, uint32_t hardwareMax)
{
	if (!multiDrawIndirect)
		return 1;
	uint32_t limit = hardwareMax;
	if (const char *env = std::getenv("FT_VOX_MAX_INDIRECT_BATCH"))
	{
		const long parsed = std::atol(env);
		if (parsed >= 1)
			limit = std::min(limit, static_cast<uint32_t>(parsed));
	}
	return std::max(1u, limit);
}

/// Split `total` commands into batches of at most `maxBatch`: sum == total,
/// every batch in [1, maxBatch], and the split only produces a smaller final
/// batch. Exposed for headless unit tests of the contract the draw-time
/// splitter must uphold.
inline std::vector<uint32_t> splitIndirectBatchCounts(size_t total, uint32_t maxBatch)
{
	std::vector<uint32_t> counts;
	if (total == 0)
		return counts;
	const size_t capped = std::max<size_t>(1, maxBatch);
	for (size_t done = 0; done < total; done += capped)
		counts.push_back(static_cast<uint32_t>(std::min(total - done, capped)));
	return counts;
}
