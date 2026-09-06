// IndirectDrawEmit bucket grouping (issue #122 review): stability,
// firstInstance/drawData coherence, single-key fast path and the >128
// page-pair fallback. Header-only: no Vulkan device, no function loading.

#include "Renderer/IndirectDrawEmit.hpp"

#include <array>
#include <iostream>
#include <vector>

static int g_fails = 0;

#define CHECK(cond, msg)                                                       \
	do                                                                         \
	{                                                                          \
		if (!(cond))                                                           \
		{                                                                      \
			std::cerr << "FAIL: " << msg << " (" << __LINE__ << ")\n";         \
			++g_fails;                                                         \
		}                                                                      \
	} while (0)

namespace
{
Chunk::IndirectDraw makeDraw(uint32_t vertexPage, uint32_t indexPage, uint32_t firstIndex,
                             glm::ivec3 origin)
{
	Chunk::IndirectDraw d;
	d.cmd = {100u + firstIndex, 1, firstIndex, 0, 0};
	d.vertexPage = vertexPage;
	d.indexPage = indexPage;
	d.chunkOrigin = origin;
	return d;
}

uint64_t keyOf(uint32_t vertexPage, uint32_t indexPage)
{
	return (uint64_t(vertexPage) << 32) | indexPage;
}
} // namespace

int main()
{
	constexpr uint32_t base = voxel_draw::kWaterBase;
	// The helper indexes drawData[baseInstance + i]: the table must span the
	// whole layout. Heap-allocated once, reused by every scenario.
	std::vector<VoxelDrawData> drawData(voxel_draw::kEntryCount);
	std::vector<VkDrawIndexedIndirectCommand> dstBuf(voxel_draw::kCommandsPerPass);

	// A) Single bucket: one distinct page pair.
	{
		std::vector<Chunk::IndirectDraw> src = {
			makeDraw(1, 2, 0, {1, 0, 0}),
			makeDraw(1, 2, 1, {1, 0, 0}),
			makeDraw(1, 2, 2, {2, 0, 0}),
			makeDraw(1, 2, 3, {2, 0, 0}),
		};
		VkDrawIndexedIndirectCommand *dst = dstBuf.data();
		std::fill(dst, dst + 8, VkDrawIndexedIndirectCommand{});
		std::fill(drawData.begin(), drawData.begin() + base + 8, VoxelDrawData{});
		std::array<PageBatch, 128> batches{};
		const GroupedDraws g = emitGroupedIndirectDraws(src.data(), src.size(), base,
		                                                dst, drawData.data(), batches);
		CHECK(!g.usedFallback && g.batchCount == 1, "A: single bucket detected");
		CHECK(batches[0].key == keyOf(1, 2) && batches[0].first == 0 && batches[0].count == 4,
		      "A: single bucket covers everything");
		for (size_t i = 0; i < src.size(); ++i)
			CHECK(dst[i].firstIndex == src[i].cmd.firstIndex, "A: order preserved");
	}

	// B) Interleaved buckets with stability: A B A B A -> A A A B B, each
	// bucket keeping the relative input order.
	{
		std::vector<Chunk::IndirectDraw> src;
		for (uint32_t i = 0; i < 3; ++i)
			src.push_back(makeDraw(0, 0, i, {7, 0, 0}));          // A0 A1 A2
		for (uint32_t i = 0; i < 2; ++i)
			src.push_back(makeDraw(1, 1, 10 + i, {8, 0, 0}));     // B0 B1
		// Interleave: A B A B A
		std::vector<Chunk::IndirectDraw> interleaved = {
			src[0], src[3], src[1], src[4], src[2]};
		VkDrawIndexedIndirectCommand *dst = dstBuf.data();
		std::fill(dst, dst + 8, VkDrawIndexedIndirectCommand{});
		std::fill(drawData.begin(), drawData.begin() + base + 8, VoxelDrawData{});
		std::array<PageBatch, 128> batches{};
		const GroupedDraws g = emitGroupedIndirectDraws(interleaved.data(), interleaved.size(), base,
		                                                dst, drawData.data(), batches);
		CHECK(!g.usedFallback && g.batchCount == 2, "B: two buckets");
		CHECK(batches[0].key == keyOf(0, 0) && batches[0].first == 0 && batches[0].count == 3,
		      "B: first-seen bucket first");
		CHECK(batches[1].key == keyOf(1, 1) && batches[1].first == 3 && batches[1].count == 2,
		      "B: second bucket after");
		for (size_t i = 0; i < 3; ++i)
			CHECK(dst[i].firstIndex == i, "B: A entries stable");
		for (size_t i = 0; i < 2; ++i)
			CHECK(dst[3 + i].firstIndex == 10 + i, "B: B entries stable");
	}

	// C) firstInstance == base + final slot and drawData coherence: grouped
	// A0 B0 A1 must index the table through the FINAL position.
	{
		std::vector<Chunk::IndirectDraw> interleaved = {
			makeDraw(0, 0, 0, {100, 0, 0}),   // A0
			makeDraw(1, 1, 1, {200, 0, 0}),   // B0
			makeDraw(0, 0, 2, {100, 0, 0}),   // A1
		};
		VkDrawIndexedIndirectCommand *dst = dstBuf.data();
		std::fill(dst, dst + 8, VkDrawIndexedIndirectCommand{});
		std::fill(drawData.begin(), drawData.begin() + base + 8, VoxelDrawData{});
		std::array<PageBatch, 128> batches{};
		const GroupedDraws g = emitGroupedIndirectDraws(interleaved.data(), interleaved.size(), base,
		                                                dst, drawData.data(), batches);
		CHECK(g.batchCount == 2 && !g.usedFallback, "C: grouped");
		// Final order: A0 A1 B0.
		CHECK(dst[0].firstIndex == 0 && dst[1].firstIndex == 2 && dst[2].firstIndex == 1,
		      "C: grouped final order");
		CHECK(dst[0].firstInstance == base && dst[1].firstInstance == base + 1 &&
		      dst[2].firstInstance == base + 2,
		      "C: firstInstance follows final slot");
		CHECK(drawData[base].worldOrigin == glm::ivec3(100, 0, 0) &&
		      drawData[base + 1].worldOrigin == glm::ivec3(100, 0, 0) &&
		      drawData[base + 2].worldOrigin == glm::ivec3(200, 0, 0),
		      "C: drawData entries match their draws");
	}

	// D) Single-key fast path is consistent with a multi-bucket run of one
	// bucket: same dst, same firstInstance series.
	{
		std::vector<Chunk::IndirectDraw> src;
		for (uint32_t i = 0; i < 3; ++i)
			src.push_back(makeDraw(3, 4, i, {9, 0, 0}));
		VkDrawIndexedIndirectCommand *dst = dstBuf.data();
		std::fill(dst, dst + 8, VkDrawIndexedIndirectCommand{});
		std::fill(drawData.begin(), drawData.begin() + base + 8, VoxelDrawData{});
		std::array<PageBatch, 128> batches{};
		const GroupedDraws g = emitGroupedIndirectDraws(src.data(), src.size(), base,
		                                                dst, drawData.data(), batches);
		CHECK(g.batchCount == 1 && !g.usedFallback, "D: fast path");
		for (size_t i = 0; i < src.size(); ++i)
		{
			CHECK(dst[i].firstInstance == base + static_cast<uint32_t>(i),
			      "D: sequential firstInstance");
			CHECK(drawData[base + i].worldOrigin == glm::ivec3(9, 0, 0), "D: drawData filled");
		}
	}

	// E) Exactly MaxBatches distinct pairs: bucket path succeeds.
	{
		constexpr size_t kPairs = 128;
		std::vector<Chunk::IndirectDraw> src;
		for (size_t p = 0; p < kPairs; ++p)
			src.push_back(makeDraw(static_cast<uint32_t>(p), static_cast<uint32_t>(p),
			                       static_cast<uint32_t>(p), {static_cast<int>(p), 0, 0}));
		std::fill(drawData.begin(), drawData.end(), VoxelDrawData{});
		VkDrawIndexedIndirectCommand *dst = dstBuf.data();
		std::array<PageBatch, 128> batches{};
		const GroupedDraws g = emitGroupedIndirectDraws(src.data(), src.size(), base,
		                                                dst, drawData.data(), batches);
		CHECK(!g.usedFallback && g.batchCount == kPairs, "E: 128 buckets succeed");
		for (size_t p = 0; p < kPairs; ++p)
			CHECK(dst[p].firstIndex == p, "E: identity order (input already one per pair)");
	}

	// F) MaxBatches + 1 distinct pairs: safe sorted fallback, nothing lost.
	{
		constexpr size_t kPairs = 129;
		std::vector<Chunk::IndirectDraw> src;
		for (size_t p = 0; p < kPairs; ++p)
			src.push_back(makeDraw(static_cast<uint32_t>(p), static_cast<uint32_t>(p),
			                       static_cast<uint32_t>(p), {static_cast<int>(p), 0, 0}));
		// Shuffle deterministically so the fallback sort has real work.
		std::vector<Chunk::IndirectDraw> shuffled;
		for (size_t i = 0; i < kPairs; ++i)
			shuffled.push_back(src[(i * 7919u) % kPairs]);
		std::fill(drawData.begin(), drawData.end(), VoxelDrawData{});
		VkDrawIndexedIndirectCommand *dst = dstBuf.data();
		std::array<PageBatch, 128> batches{};
		const GroupedDraws g = emitGroupedIndirectDraws(shuffled.data(), shuffled.size(), base,
		                                                dst, drawData.data(), batches);
		CHECK(g.usedFallback && g.batchCount == 0, "F: 129 buckets take the fallback");
		// Sorted fallback: every command present exactly once, sequential
		// firstInstance, drawData coherent with each draw.
		std::array<bool, kPairs> seen{};
		for (size_t i = 0; i < kPairs; ++i)
		{
			const uint32_t fi = dst[i].firstIndex;
			CHECK(fi < kPairs && !seen[fi], "F: no command lost or duplicated");
			seen[fi] = true;
			CHECK(dst[i].firstInstance == base + static_cast<uint32_t>(i), "F: sequential firstInstance");
			CHECK(drawData[base + i].worldOrigin.x == static_cast<int>(fi),
			      "F: drawData matches its draw");
		}
		for (size_t p = 0; p < kPairs; ++p)
			CHECK(seen[p], "F: all 129 commands emitted");
		// Sorted => keys ascending => the caller's contiguous-run scan sees
		// exactly 129 runs.
		size_t runs = 1;
		for (size_t i = 1; i < kPairs; ++i)
			if (keyOf(dst[i].firstIndex, dst[i].firstIndex) !=
			    keyOf(dst[i - 1].firstIndex, dst[i - 1].firstIndex))
				++runs;
		CHECK(runs == kPairs, "F: sorted keys produce one run per pair");
	}

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "PASS: indirect draw emit - bucket grouping stable, firstInstance/drawData "
	             "coherent, fallback safe\n";
	return 0;
}
