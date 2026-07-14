#include "Vulkan/VkImage.hpp"

#include <cstring>
#include <stdexcept>

AllocatedImage createImage2D(VmaAllocator allocator,
							 VkDevice device,
							 uint32_t width,
							 uint32_t height,
							 VkFormat format,
							 VkImageUsageFlags usage,
							 VmaMemoryUsage memoryUsage,
							 uint32_t mipLevels,
							 VkSampleCountFlagBits samples,
							 VkImageAspectFlags aspect)
{
	if (allocator == VK_NULL_HANDLE || width == 0 || height == 0)
		throw std::runtime_error("createImage2D: invalid arguments");

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent = {width, height, 1};
	imageInfo.mipLevels = mipLevels;
	imageInfo.arrayLayers = 1;
	imageInfo.format = format;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = usage;
	imageInfo.samples = samples;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo{};
	allocInfo.usage = memoryUsage;
	allocInfo.flags = 0;

	AllocatedImage out{};
	out.format = format;
	out.width = width;
	out.height = height;
	out.mipLevels = mipLevels;
	out.arrayLayers = 1;

	if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &out.image, &out.allocation, nullptr) != VK_SUCCESS)
		throw std::runtime_error("vmaCreateImage failed");

	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = out.image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;
	viewInfo.subresourceRange.aspectMask = aspect;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = mipLevels;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = 1;

	if (vkCreateImageView(device, &viewInfo, nullptr, &out.view) != VK_SUCCESS)
	{
		vmaDestroyImage(allocator, out.image, out.allocation);
		throw std::runtime_error("vkCreateImageView failed");
	}

	return out;
}

void destroyImage(VmaAllocator allocator, VkDevice device, AllocatedImage &image)
{
	if (device != VK_NULL_HANDLE && image.view != VK_NULL_HANDLE)
		vkDestroyImageView(device, image.view, nullptr);
	if (allocator != VK_NULL_HANDLE && image.image != VK_NULL_HANDLE)
		vmaDestroyImage(allocator, image.image, image.allocation);
	image = {};
}

void cmdTransitionImageLayout(VkCommandBuffer cmd,
							  VkImage image,
							  VkImageLayout oldLayout,
							  VkImageLayout newLayout,
							  VkImageAspectFlags aspect,
							  uint32_t mipLevels,
							  uint32_t arrayLayers)
{
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = aspect;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = mipLevels;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = arrayLayers;

	VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

	if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
	{
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
			 newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
	{
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask =
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	}
	else
	{
		// Conservative general path
		barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	}

	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void uploadImage2D(VmaAllocator allocator,
				   ImmediateCommands &imm,
				   AllocatedImage &image,
				   const void *pixels,
				   VkDeviceSize dataSize,
				   VkImageLayout finalLayout)
{
	if (!pixels || dataSize == 0)
		throw std::runtime_error("uploadImage2D: empty pixel data");

	AllocatedBuffer staging = createBuffer(
		allocator,
		dataSize,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VMA_MEMORY_USAGE_AUTO,
		VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);

	// Prefer mapped pointer from allocation info when available
	void *mapped = staging.info.pMappedData;
	if (!mapped)
		mapped = mapBuffer(allocator, staging);
	std::memcpy(mapped, pixels, static_cast<size_t>(dataSize));
	if (!staging.info.pMappedData)
		unmapBuffer(allocator, staging);

	imm.submitAndWait([&](VkCommandBuffer cmd) {
		cmdTransitionImageLayout(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED,
								 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
								 image.mipLevels, image.arrayLayers);

		VkBufferImageCopy region{};
		region.bufferOffset = 0;
		region.bufferRowLength = 0;
		region.bufferImageHeight = 0;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = 1;
		region.imageOffset = {0, 0, 0};
		region.imageExtent = {image.width, image.height, 1};

		vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		cmdTransitionImageLayout(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, finalLayout,
								 VK_IMAGE_ASPECT_COLOR_BIT, image.mipLevels, image.arrayLayers);
	});

	destroyBuffer(allocator, staging);
}
