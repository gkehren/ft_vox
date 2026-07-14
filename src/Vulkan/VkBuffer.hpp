#pragma once

#include "Vulkan/VkAllocator.hpp"

#include <cstddef>
#include <cstdint>

struct AllocatedBuffer
{
	VkBuffer buffer{VK_NULL_HANDLE};
	VmaAllocation allocation{VK_NULL_HANDLE};
	VkDeviceSize size{0};
	VmaAllocationInfo info{};
};

/// GPU-only or host-visible buffer via VMA.
AllocatedBuffer createBuffer(VmaAllocator allocator,
							 VkDeviceSize size,
							 VkBufferUsageFlags usage,
							 VmaMemoryUsage memoryUsage,
							 VmaAllocationCreateFlags flags = 0);

void destroyBuffer(VmaAllocator allocator, AllocatedBuffer &buf);

/// Map a host-visible allocation (must have been created with HOST_ACCESS flags).
void *mapBuffer(VmaAllocator allocator, AllocatedBuffer &buf);
void unmapBuffer(VmaAllocator allocator, AllocatedBuffer &buf);

/// Write CPU bytes into a host-visible buffer (maps, memcpy, unmaps).
void writeBuffer(VmaAllocator allocator, AllocatedBuffer &buf, const void *data, VkDeviceSize size,
				 VkDeviceSize offset = 0);
