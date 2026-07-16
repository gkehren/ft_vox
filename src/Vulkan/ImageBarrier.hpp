#pragma once

#include <volk.h>
#include <cstdint>

/// Single shared image-layout transition API for the frame graph and post chain.
namespace vkbar
{

inline VkImageMemoryBarrier makeBarrier(VkImage image, VkImageLayout oldL, VkImageLayout newL,
										VkAccessFlags srcA, VkAccessFlags dstA,
										VkImageAspectFlags aspect, uint32_t baseLayer = 0,
										uint32_t layerCount = 1, uint32_t mipLevels = 1)
{
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.oldLayout = oldL;
	b.newLayout = newL;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {aspect, 0, mipLevels, baseLayer, layerCount};
	b.srcAccessMask = srcA;
	b.dstAccessMask = dstA;
	return b;
}

inline void cmdBarrier(VkCommandBuffer cmd, const VkImageMemoryBarrier &b,
					   VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
{
	vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

inline void cmdBarriers(VkCommandBuffer cmd, const VkImageMemoryBarrier *barriers, uint32_t count,
						VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
{
	if (count == 0)
		return;
	vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, count, barriers);
}

inline void cmdTransitionColor(VkCommandBuffer cmd, VkImage image, VkImageLayout oldL, VkImageLayout newL,
							   VkAccessFlags srcA, VkAccessFlags dstA,
							   VkPipelineStageFlags srcS, VkPipelineStageFlags dstS,
							   uint32_t layerCount = 1)
{
	const auto b = makeBarrier(image, oldL, newL, srcA, dstA, VK_IMAGE_ASPECT_COLOR_BIT, 0, layerCount);
	cmdBarrier(cmd, b, srcS, dstS);
}

inline void cmdTransitionDepth(VkCommandBuffer cmd, VkImage image, VkImageLayout oldL, VkImageLayout newL,
							   VkAccessFlags srcA, VkAccessFlags dstA,
							   VkPipelineStageFlags srcS, VkPipelineStageFlags dstS,
							   uint32_t layerCount = 1)
{
	const auto b = makeBarrier(image, oldL, newL, srcA, dstA, VK_IMAGE_ASPECT_DEPTH_BIT, 0, layerCount);
	cmdBarrier(cmd, b, srcS, dstS);
}

} // namespace vkbar
