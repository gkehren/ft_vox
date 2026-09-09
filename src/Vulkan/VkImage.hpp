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

/// 2D array image (e.g. cascaded shadow maps). View type is 2D_ARRAY when arrayLayers > 1.
AllocatedImage createImage2DArray(VmaAllocator allocator,
								  VkDevice device,
								  uint32_t width,
								  uint32_t height,
								  uint32_t arrayLayers,
								  VkFormat format,
								  VkImageUsageFlags usage,
								  VmaMemoryUsage memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
								  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

/// Create a single-layer view into an existing array image (for cascade depth attachments).
VkImageView createImageView2DLayer(VkDevice device, VkImage image, VkFormat format,
								   VkImageAspectFlags aspect, uint32_t layer);

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

/// Stage a complete CPU-generated mip chain into a GPU 2D image in one submit
/// (UNDEFINED → TRANSFER_DST → SHADER_READ). `mips` holds every level tightly
/// packed, level 0 first (the layout texture_mips::generateLayerChain
/// produces); one vkCmdCopyBufferToImage writes all levels, so the descriptor
/// set only becomes usable once every level is resident. `dataSize` must equal
/// the sum of all level byte sizes (texture_mips::chainBytes(w, h)).
void uploadImage2DMipChain(VmaAllocator allocator,
						   ImmediateCommands &imm,
						   AllocatedImage &image,
						   const void *mips,
						   VkDeviceSize dataSize,
						   VkImageLayout finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
