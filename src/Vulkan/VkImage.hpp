#pragma once

#include "Vulkan/VkAllocator.hpp"
#include "Vulkan/VkBuffer.hpp"
#include "Vulkan/VkCommands.hpp"

#include <cstdint>

struct AllocatedImage
{
	VkImage image{VK_NULL_HANDLE};
	VkImageView view{VK_NULL_HANDLE};
	VmaAllocation allocation{VK_NULL_HANDLE};
	VkFormat format{VK_FORMAT_UNDEFINED};
	uint32_t width{0};
	uint32_t height{0};
	uint32_t mipLevels{1};
	uint32_t arrayLayers{1};
};

AllocatedImage createImage2D(VmaAllocator allocator,
							 VkDevice device,
							 uint32_t width,
							 uint32_t height,
							 VkFormat format,
							 VkImageUsageFlags usage,
							 VmaMemoryUsage memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
							 uint32_t mipLevels = 1,
							 VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
							 VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

void destroyImage(VmaAllocator allocator, VkDevice device, AllocatedImage &image);

/// Transition image layout with a pipeline barrier (recorded into cmd).
void cmdTransitionImageLayout(VkCommandBuffer cmd,
							  VkImage image,
							  VkImageLayout oldLayout,
							  VkImageLayout newLayout,
							  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
							  uint32_t mipLevels = 1,
							  uint32_t arrayLayers = 1);

/// Stage CPU pixel data into a GPU-optimal 2D image (UNDEFINED → TRANSFER_DST → SHADER_READ).
/// `dataSize` must equal width * height * bytesPerPixel for a tightly packed buffer.
void uploadImage2D(VmaAllocator allocator,
				   ImmediateCommands &imm,
				   AllocatedImage &image,
				   const void *pixels,
				   VkDeviceSize dataSize,
				   VkImageLayout finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
