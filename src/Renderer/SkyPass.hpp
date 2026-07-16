#pragma once

#include "Vulkan/VkContext.hpp"
#include "Vulkan/VkBuffer.hpp"
#include "Vulkan/VkImage.hpp"
#include "Vulkan/VkCommands.hpp"

#include <volk.h>
#include <cstdint>

/// Procedural sky cube draw into HDR + god-source MRT (depth-tested, no depth write).
class SkyPass
{
public:
	void init(VkContext &context, ImmediateCommands &imm, VkDescriptorSetLayout frameSetLayout,
			  VkFormat hdrFormat, VkFormat depthFormat);
	void shutdown();
	void recreatePipeline(VkFormat hdrFormat, VkFormat depthFormat);

	/// HDR + godSource as color attachments (load HDR, clear god); depth load from scene.
	void record(VkCommandBuffer cmd, VkDescriptorSet frameSet0, VkExtent2D extent,
				AllocatedImage &hdr, AllocatedImage &godSource, AllocatedImage &sceneDepth);

private:
	void createGeometry(ImmediateCommands &imm);
	void createPipeline(VkFormat hdrFormat, VkFormat depthFormat);
	void destroyPipeline();

	VkContext *m_context{nullptr};
	VkDescriptorSetLayout m_frameSetLayout{VK_NULL_HANDLE};
	AllocatedBuffer m_vbo{};
	VkPipelineLayout m_layout{VK_NULL_HANDLE};
	VkPipeline m_pipeline{VK_NULL_HANDLE};
	VkFormat m_hdrFormat{VK_FORMAT_R16G16B16A16_SFLOAT};
	VkFormat m_depthFormat{VK_FORMAT_D32_SFLOAT};
};
