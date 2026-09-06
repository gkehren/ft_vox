#pragma once

#include "Vulkan/VkBuffer.hpp"
#include "Vulkan/GpuResourceRetire.hpp"
#include "Engine/WorkloadTelemetry.hpp"

#include <cstdint>
#include <utility>
#include <vector>

// Shared device-local geometry arena (issue #109). Meshes no longer own one
// VMA vertex/index allocation each: every stream (opaque vertices, opaque
// indices, water vertices, water indices) suballocates aligned ranges from a
// bounded set of large pages. Ranges stay valid until explicitly retired;
// freeing is frame-aware, so streaming and edits never need vkDeviceWaitIdle.
//
// - Allocation: per-page first-fit free list, ranges aligned to the arena's
//   alignment (vertex arenas align to sizeof(Vertex) so a range's first
//   vertex index is exact; index arenas align to 4).
// - Growth: when no free block fits, a new page is created. There is never a
//   blocking realloc or compaction; pages whose last block was freed are
//   destroyed through the frame-aware retire queue.
// - Failure: allocate() returns false when VMA cannot back a new page. The
//   caller has recorded no commands yet, so nothing else has to be undone.
// - Metrics: page count, live/free bytes and a high-water mark are published
//   through the telemetry registry (arena.* gauges, arena.growEvents).
class MeshArena
{
public:
	static constexpr uint32_t kNoPage = UINT32_MAX;

	struct Range
	{
		uint32_t page{kNoPage};
		uint32_t offset{0}; // bytes, aligned to the arena's alignment
		uint32_t bytes{0};
		bool empty() const { return page == kNoPage; }
	};

	// Frame-aware retirement delay: framesInFlight + 1. A range retired
	// during frame F's upload was last referenced by frame F-1's commands
	// (still in flight); with `framesInFlight` frames in flight it is safe
	// once frame F+framesInFlight has begun - the extra +1 keeps one frame
	// of margin, mirroring GpuResourceRetire's own safety window.
	void init(VmaAllocator allocator, GpuResourceRetire &retire, VkDeviceSize pageSize,
			  VkBufferUsageFlags usage, uint32_t alignment, telemetry::Gauge gauge,
			  uint32_t framesInFlight);
	void shutdown(); // immediate destroy of every page (device idle expected)

	// Returns false only when a new page would be required and its backing
	// allocation fails. Pure CPU bookkeeping: no commands, no GPU mutation.
	bool allocate(uint32_t bytes, Range &out);
	// Frame-aware free: the range's bytes return to the free list after
	// (framesInFlight + 1) beginFrame() calls.
	void retire(const Range &range);
	// Immediate free (bootstrap/shutdown paths only: no in-flight frame can
	// still reference the range).
	void freeImmediate(const Range &range);
	// Drives delayed frees, page destruction and arena telemetry. Call once
	// per rendered frame, in frame order.
	void beginFrame(uint64_t frameNumber);

	// Test-only deterministic failure injection (issue #109 review): the
	// next `count` page creations or allocations report failure exactly as
	// a VMA OOM would, without exhausting real device memory.
	void setFailNextPageAllocations(uint32_t count) { m_failNextPages = count; }
	void setFailNextAllocations(uint32_t count) { m_failNextAllocs = count; }

	VkBuffer pageBuffer(uint32_t page) const;
	// Mutable page access for the ImmediateCommands bootstrap upload path.
	AllocatedBuffer &pageBufferRef(uint32_t page) { return m_pages[static_cast<size_t>(page)].buf; }
	VkDeviceSize pageSize(uint32_t page) const { return m_pages[static_cast<size_t>(page)].buf.size; }

	struct Metrics
	{
		uint32_t pages{0};
		uint32_t liveBlocks{0};
		uint64_t liveBytes{0};
		uint64_t freeBytes{0};
		uint64_t highWaterBytes{0};
	};
	Metrics metrics() const;
	// Frames a retired range waits in the pending queue before its bytes
	// return to the free list (= framesInFlight + 1, set by init). Exposed
	// so tests can pin the wrapper's delay plumbing.
	uint32_t retireDelay() const { return m_retireDelay; }
	// Live bytes: published ranges + ranges still pending retirement (both
	// consume the arena until they return to the free list).
	uint64_t liveBytes() const { return m_liveBytes; }
	uint64_t highWaterBytes() const { return m_highWater; }
	uint32_t liveBlocks() const { return m_liveBlocks; }

private:
	using FreeBlock = std::pair<uint32_t, uint32_t>; // (offset, bytes), sorted by offset
	struct Page
	{
		AllocatedBuffer buf{};
		std::vector<FreeBlock> freeList;
		bool alive{false};
	};
	struct PendingFree
	{
		Range range;
		uint64_t frame{0};
	};

	bool createPage(uint32_t atLeastBytes);
	void releasePage(Page &page);
	void insertFree(uint32_t page, uint32_t offset, uint32_t bytes);

	VmaAllocator m_allocator{VK_NULL_HANDLE};
	GpuResourceRetire *m_retire{nullptr};
	VkBufferUsageFlags m_usage{0};
	uint32_t m_alignment{1};
	VkDeviceSize m_pageSize{0};
	telemetry::Gauge m_gauge{};
	uint32_t m_retireDelay{3};
	std::vector<Page> m_pages;
	std::vector<PendingFree> m_pending;
	uint64_t m_frame{0};
	uint64_t m_highWater{0};
	uint64_t m_liveBytes{0};
	uint32_t m_liveBlocks{0};
	uint32_t m_failNextPages{0};
	uint32_t m_failNextAllocs{0};
};

// The four per-stream arenas shared by every chunk (issue #109). Owned by
// WorldRenderer; the chunk upload path suballocates from them. The arena.*
// telemetry gauges are published HERE, aggregated over all four streams
// (per-arena metrics are still reachable through MeshArena::metrics).
struct MeshArenas
{
	MeshArena opaqueVertex;
	MeshArena opaqueIndex;
	MeshArena waterVertex;
	MeshArena waterIndex;

	void init(VmaAllocator allocator, GpuResourceRetire &retire, uint32_t vertexAlignment,
	          uint32_t framesInFlight,
	          VkDeviceSize vertexPageSize = 128ull * 1024ull * 1024ull,
	          VkDeviceSize indexPageSize = 64ull * 1024ull * 1024ull);
	void shutdown();
	void beginFrame(uint64_t frameNumber);

private:
	uint64_t m_totalHighWater{0};
};
