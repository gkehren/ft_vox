#include "Vulkan/MeshArena.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>

void MeshArena::init(VmaAllocator allocator, GpuResourceRetire &retire, VkDeviceSize pageSize,
					 VkBufferUsageFlags usage, uint32_t alignment, telemetry::Gauge gauge)
{
	m_allocator = allocator;
	m_retire = &retire;
	m_pageSize = pageSize;
	m_usage = usage;
	m_alignment = alignment;
	m_gauge = gauge;
}

void MeshArena::shutdown()
{
	for (Page &p : m_pages)
	{
		if (p.alive)
			destroyBuffer(m_allocator, p.buf);
		p = {};
	}
	m_pages.clear();
	m_pending.clear();
	m_frame = 0;
	m_highWater = 0;
}

bool MeshArena::createPage(uint32_t atLeastBytes)
{
	// Dedicated oversize pages round up to whole m_pageSize units so the
	// arena keeps a predictable page quantum.
	VkDeviceSize size = m_pageSize;
	while (size < atLeastBytes)
		size *= 2;

	Page p;
	p.buf = createBuffer(m_allocator, size,
						 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | m_usage,
						 VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
	if (p.buf.buffer == VK_NULL_HANDLE)
		return false;
	trackMeshBuffer(p.buf, m_gauge);
	telemetry::registry().add(telemetry::ArenaGrow);
	p.alive = true;
	p.freeList.emplace_back(0, static_cast<uint32_t>(size));
	m_pages.push_back(std::move(p));
	return true;
}

bool MeshArena::allocate(uint32_t bytes, Range &out)
{
	out = {};
	if (bytes == 0)
		return true;
	const uint32_t aligned =
		(bytes + m_alignment - 1) / m_alignment * m_alignment;

	for (size_t pi = 0; pi < m_pages.size(); ++pi)
	{
		Page &p = m_pages[pi];
		if (!p.alive)
			continue;
		for (size_t bi = 0; bi < p.freeList.size(); ++bi)
		{
			const auto [off, cap] = p.freeList[bi];
			const uint32_t alignedOff =
				(off + m_alignment - 1) / m_alignment * m_alignment;
			const uint32_t pad = alignedOff - off;
			if (pad + aligned > cap)
				continue;
			// First fit: shrink or remove the block, keeping the list
			// sorted by offset for O(1) coalescing on free.
			// Shrink the block, keeping any alignment padding free: the
			// page must be able to reconstruct its full capacity exactly
			// once every live range is freed, or it can never be released.
			const uint32_t restOff = alignedOff + aligned;
			const uint32_t restBytes = cap - pad - aligned;
			if (pad == 0)
			{
				if (restBytes == 0)
					p.freeList.erase(p.freeList.begin() + static_cast<long>(bi));
				else
					p.freeList[bi] = {restOff, restBytes};
			}
			else
			{
				p.freeList[bi] = {off, pad};
				if (restBytes > 0)
					p.freeList.insert(p.freeList.begin() + static_cast<long>(bi) + 1,
					                  {restOff, restBytes});
			}
			out.page = static_cast<uint32_t>(pi);
			out.offset = alignedOff;
			out.bytes = aligned;
			++m_liveBlocks;
			return true;
		}
	}

	// No existing block fits: grow by adding a page (never a blocking
	// realloc; issue #109 arena requirements).
	if (!createPage(aligned))
		return false;
	Page &p = m_pages.back();
	out.page = static_cast<uint32_t>(m_pages.size() - 1);
	out.offset = 0;
	out.bytes = aligned;
	p.freeList.clear();
	p.freeList.emplace_back(aligned, static_cast<uint32_t>(p.buf.size) - aligned);
	++m_liveBlocks;
	return true;
}

void MeshArena::insertFree(uint32_t page, uint32_t offset, uint32_t bytes)
{
	Page &p = m_pages[static_cast<size_t>(page)];
	auto &list = p.freeList;
	auto it = std::lower_bound(list.begin(), list.end(), offset,
							   [](const FreeBlock &b, uint32_t off) { return b.first < off; });
	it = list.insert(it, {offset, bytes});
	// Coalesce with the previous and next blocks.
	if (it != list.begin())
	{
		auto prev = it - 1;
		if (prev->first + prev->second == offset)
		{
			prev->second += bytes;
			list.erase(it);
			it = prev;
		}
	}
	if (it + 1 != list.end() && it->first + it->second == (it + 1)->first)
	{
		it->second += (it + 1)->second;
		list.erase(it + 1);
	}
}

void MeshArena::retire(const Range &range)
{
	if (range.empty())
		return;
	m_pending.push_back({range, m_frame + kRetireDelay});
}

void MeshArena::freeImmediate(const Range &range)
{
	if (range.empty())
		return;
	Page &p = m_pages[static_cast<size_t>(range.page)];
#ifndef NDEBUG
	// Double-free detector (debug): a range already fully free must not be
	// freed again, or the free list exceeds the page capacity.
	for (const auto &[off, bytes] : p.freeList)
	{
		if (range.offset >= off && range.offset + range.bytes <= off + bytes)
		{
			std::cerr << "MeshArena double-free: page=" << range.page
			          << " offset=" << range.offset << " bytes=" << range.bytes
			          << std::endl;
			assert(false && "MeshArena::freeImmediate on an already-free range");
		}
	}
#endif
	if (m_liveBlocks > 0)
		--m_liveBlocks;
	insertFree(range.page, range.offset, range.bytes);
	if (p.freeList.size() == 1 && p.freeList.front() == std::make_pair(0u, static_cast<uint32_t>(p.buf.size)))
		releasePage(p);
}

void MeshArena::beginFrame(uint64_t frameNumber)
{
	m_frame = frameNumber;
	// Release delayed frees whose safety delay has elapsed.
	for (size_t i = m_pending.size(); i-- > 0;)
	{
		if (m_pending[static_cast<size_t>(i)].frame <= frameNumber)
		{
			const Range r = m_pending[static_cast<size_t>(i)].range;
			m_pending.erase(m_pending.begin() + static_cast<long>(i));
			Page &p = m_pages[static_cast<size_t>(r.page)];
			if (!p.alive)
				continue;
			if (m_liveBlocks > 0)
				--m_liveBlocks;
			insertFree(r.page, r.offset, r.bytes);
		}
	}
	// Pages whose whole capacity is back in one free block have no live
	// range left; hand the buffer to the frame-aware retire queue.
	for (Page &p : m_pages)
	{
		if (p.alive && p.freeList.size() == 1 &&
			p.freeList.front() == std::make_pair(0u, static_cast<uint32_t>(p.buf.size)))
			releasePage(p);
	}

	// Publish arena metrics (issue #101/#109): page count, free bytes and
	// live-bytes high-water for this stream.
	Metrics m = metrics();
	telemetry::registry().set(telemetry::ArenaPages, m.pages);
	telemetry::registry().set(telemetry::ArenaFreeBytes, m.freeBytes);
	telemetry::registry().set(telemetry::ArenaHighWater, m_highWater);
}

void MeshArena::releasePage(Page &p)
{
	// The retire queue delays the actual destruction past the frames that
	// may still bind this page.
	if (m_retire)
		m_retire->retireBuffer(p.buf);
	else
		destroyBuffer(m_allocator, p.buf);
	p.buf = {};
	p.alive = false;
}

VkBuffer MeshArena::pageBuffer(uint32_t page) const
{
	return m_pages[static_cast<size_t>(page)].buf.buffer;
}

MeshArena::Metrics MeshArena::metrics() const
{
	Metrics m;
	m.liveBlocks = m_liveBlocks;
	for (const Page &p : m_pages)
	{
		if (!p.alive)
			continue;
		++m.pages;
		uint64_t free = 0;
		for (const auto &[off, bytes] : p.freeList)
			free += bytes;
		const uint64_t live = static_cast<uint64_t>(p.buf.size) - free;
		m.freeBytes += free;
		m.liveBytes += live;
	}
	m.highWaterBytes = m_highWater;
	return m;
}

void MeshArenas::init(VmaAllocator allocator, GpuResourceRetire &retire, uint32_t vertexAlignment)
{
	constexpr VkDeviceSize kVertexPageSize = 128ull * 1024ull * 1024ull; // 128 MiB
	constexpr VkDeviceSize kIndexPageSize = 64ull * 1024ull * 1024ull;	 // 64 MiB
	opaqueVertex.init(allocator, retire, kVertexPageSize,
					  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexAlignment, telemetry::GpuOpaqueVertex);
	opaqueIndex.init(allocator, retire, kIndexPageSize,
					 VK_BUFFER_USAGE_INDEX_BUFFER_BIT, sizeof(uint32_t), telemetry::GpuOpaqueIndex);
	waterVertex.init(allocator, retire, kVertexPageSize,
					 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexAlignment, telemetry::GpuWaterVertex);
	waterIndex.init(allocator, retire, kIndexPageSize,
					VK_BUFFER_USAGE_INDEX_BUFFER_BIT, sizeof(uint32_t), telemetry::GpuWaterIndex);
}

void MeshArenas::shutdown()
{
	opaqueVertex.shutdown();
	opaqueIndex.shutdown();
	waterVertex.shutdown();
	waterIndex.shutdown();
}

void MeshArenas::beginFrame(uint64_t frameNumber)
{
	opaqueVertex.beginFrame(frameNumber);
	opaqueIndex.beginFrame(frameNumber);
	waterVertex.beginFrame(frameNumber);
	waterIndex.beginFrame(frameNumber);
}
