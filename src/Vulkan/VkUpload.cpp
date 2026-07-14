#include "Vulkan/VkUpload.hpp"

#include <stdexcept>

void uploadBuffer(VmaAllocator allocator,
				  ImmediateCommands &imm,
				  AllocatedBuffer &dst,
				  const void *data,
				  VkDeviceSize size,
				  VkDeviceSize dstOffset)
{
	if (!data || size == 0)
		throw std::runtime_error("uploadBuffer: empty data");
	if (dstOffset + size > dst.size)
		throw std::runtime_error("uploadBuffer: range out of bounds");

	AllocatedBuffer staging = createBuffer(
		allocator,
		size,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VMA_MEMORY_USAGE_AUTO,
		VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);

	writeBuffer(allocator, staging, data, size, 0);

	imm.submitAndWait([&](VkCommandBuffer cmd) {
		VkBufferCopy copy{};
		copy.srcOffset = 0;
		copy.dstOffset = dstOffset;
		copy.size = size;
		vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &copy);
	});

	destroyBuffer(allocator, staging);
}
