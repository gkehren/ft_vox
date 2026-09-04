#include "Vulkan/VkBuffer.hpp"

#include <cstring>
#include <stdexcept>

AllocatedBuffer createBuffer(VmaAllocator allocator,
							 VkDeviceSize size,
							 VkBufferUsageFlags usage,
							 VmaMemoryUsage memoryUsage,
							 VmaAllocationCreateFlags flags)
{
	if (allocator == VK_NULL_HANDLE || size == 0)
		throw std::runtime_error("createBuffer: invalid allocator or size");

	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo{};
	allocInfo.usage = memoryUsage;
	allocInfo.flags = flags;

	AllocatedBuffer out{};
	out.size = size;
	if (vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &out.buffer, &out.allocation, &out.info) != VK_SUCCESS)
		throw std::runtime_error("vmaCreateBuffer failed");
	return out;
}

void destroyBuffer(VmaAllocator allocator, AllocatedBuffer &buf)
{
	if (allocator != VK_NULL_HANDLE && buf.buffer != VK_NULL_HANDLE)
	{
        if (buf.meshKind >= 0) {
            auto& t = telemetry::registry();
            t.replace(buf.retired ? telemetry::RetiredBytes : static_cast<telemetry::Gauge>(buf.meshKind), buf.size, 0);
            if (buf.retired) t.replace(telemetry::RetiredBuffers, 1, 0);
            t.add(telemetry::AllocDestroyed);
        }
		vmaDestroyBuffer(allocator, buf.buffer, buf.allocation);
	}
	buf = {};
}

void *mapBuffer(VmaAllocator allocator, AllocatedBuffer &buf)
{
	void *mapped = nullptr;
	if (vmaMapMemory(allocator, buf.allocation, &mapped) != VK_SUCCESS)
		throw std::runtime_error("vmaMapMemory failed");
	return mapped;
}

void unmapBuffer(VmaAllocator allocator, AllocatedBuffer &buf)
{
	vmaUnmapMemory(allocator, buf.allocation);
}

void writeBuffer(VmaAllocator allocator, AllocatedBuffer &buf, const void *data, VkDeviceSize size,
				 VkDeviceSize offset)
{
	if (offset + size > buf.size)
		throw std::runtime_error("writeBuffer: range out of bounds");
	void *mapped = mapBuffer(allocator, buf);
	std::memcpy(static_cast<char *>(mapped) + offset, data, static_cast<size_t>(size));
	unmapBuffer(allocator, buf);
}

void trackMeshBuffer(AllocatedBuffer& buffer, telemetry::Gauge kind)
{
    buffer.meshKind = static_cast<int>(kind);
    telemetry::registry().replace(kind, 0, buffer.size);
    telemetry::registry().add(telemetry::AllocCreated);
}
