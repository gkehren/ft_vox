#pragma once

#include "Vulkan/VkContext.hpp"
#include "Vulkan/VkImage.hpp"
#include "Chunk/Chunk.hpp"
#include "Renderer/OverlayRenderer.hpp"

#include <volk.h>
#include <vector>
#include <cstdint>

/// Opaque HDR terrain + overlays into color/depth targets.
class OpaquePass
{
public:
	void init(VkContext &context);
	void shutdown();

	void createPipeline(VkPipelineLayout layout, const VkPipelineVertexInputStateCreateInfo &vertexInput,
						VkFormat colorFmt, VkFormat depthFmt);
	void destroyPipeline();

	void record(VkCommandBuffer cmd, VkExtent2D extent, VkDescriptorSet set0, VkDescriptorSet set1,
				VkPipelineLayout layout, AllocatedImage &hdr, AllocatedImage &depth,
				const std::vector<Chunk *> &chunks, OverlayRenderer &overlays,
				const VkClearColorValue &clearColor);

private:
	VkContext *m_context{nullptr};
	VkPipeline m_pipeline{VK_NULL_HANDLE};
};
