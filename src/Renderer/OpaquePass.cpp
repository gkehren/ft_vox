#include "Renderer/OpaquePass.hpp"
#include "Vulkan/ImageBarrier.hpp"
#include "Vulkan/GraphicsPipelineBuilder.hpp"
#include "Vulkan/VkShader.hpp"
#include "utils.hpp"

#include <stdexcept>

namespace
{
auto beginR() { return vkCmdBeginRendering ? vkCmdBeginRendering : vkCmdBeginRenderingKHR; }
auto endR() { return vkCmdEndRendering ? vkCmdEndRendering : vkCmdEndRenderingKHR; }
} // namespace

void OpaquePass::init(VkContext &context) { m_context = &context; }

void OpaquePass::shutdown()
{
	destroyPipeline();
	m_context = nullptr;
}

void OpaquePass::createPipeline(VkPipelineLayout layout, const VkPipelineVertexInputStateCreateInfo &vertexInput,
								VkFormat colorFmt, VkFormat depthFmt)
{
	VkShaderModule vert = loadShaderModule(m_context->getDevice(), resolveSpvPath("terrain.vert.spv"));
	VkShaderModule frag = loadShaderModule(m_context->getDevice(), resolveSpvPath("terrain.frag.spv"));
	VkPipelineShaderStageCreateInfo stages[2] = {
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
	};
	VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_BACK_BIT;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.f;

	VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	depth.depthTestEnable = VK_TRUE;
	depth.depthWriteEnable = VK_TRUE;
	depth.depthCompareOp = VK_COMPARE_OP_LESS;

	VkPipelineColorBlendAttachmentState blendAtt{};
	blendAtt.colorWriteMask = 0xF;

	m_pipeline = GraphicsPipelineBuilder()
					 .setLayout(layout)
					 .setShaderStages(stages, 2)
					 .setVertexInput(&vertexInput)
					 .setRaster(raster)
					 .setDepth(depth)
					 .setColorBlendAttachments(&blendAtt, 1)
					 .setRenderingFormats(&colorFmt, 1, depthFmt)
					 .build(m_context->getDevice());

	destroyShaderModule(m_context->getDevice(), vert);
	destroyShaderModule(m_context->getDevice(), frag);
}

void OpaquePass::destroyPipeline()
{
	if (m_context && m_pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(m_context->getDevice(), m_pipeline, nullptr);
	m_pipeline = VK_NULL_HANDLE;
}

void OpaquePass::record(VkCommandBuffer cmd, VkExtent2D extent, VkDescriptorSet set0, VkDescriptorSet set1,
						VkPipelineLayout layout, AllocatedImage &hdr, AllocatedImage &depth,
						const std::vector<Chunk *> &chunks, OverlayRenderer &overlays,
						const VkClearColorValue &clearColor, VkGpuProfiler *gpu)
{
	if (gpu) gpu->beginPass(cmd, GpuPass::Opaque);
	const auto beginRendering = beginR();
	const auto endRendering = endR();

	vkbar::cmdTransitionColor(cmd, hdr.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							  0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
							  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	vkbar::cmdTransitionDepth(cmd, depth.image, VK_IMAGE_LAYOUT_UNDEFINED,
							  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
							  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
							  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);

	VkRenderingAttachmentInfo colorAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	colorAtt.imageView = hdr.view;
	colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAtt.clearValue.color = clearColor;

	VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	depthAtt.imageView = depth.view;
	depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	depthAtt.clearValue.depthStencil = {1.0f, 0};

	VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = {{0, 0}, extent};
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &colorAtt;
	ri.pDepthAttachment = &depthAtt;
	beginRendering(cmd, &ri);

	VkViewport viewport{0.f, static_cast<float>(extent.height), static_cast<float>(extent.width),
						-static_cast<float>(extent.height), 0.f, 1.f};
	VkRect2D scissor{{0, 0}, extent};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	VkDescriptorSet sets[2] = {set0, set1};
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets, 0, nullptr);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
	for (Chunk *chunk : chunks)
	{
		if (chunk)
			chunk->draw(cmd);
	}
	if (gpu) gpu->endPass(cmd, GpuPass::Opaque);
	if (gpu) gpu->beginPass(cmd, GpuPass::Overlays);
	overlays.record(cmd, set0, chunks);
	if (gpu) gpu->endPass(cmd, GpuPass::Overlays);
	endRendering(cmd);
}
