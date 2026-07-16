#pragma once

#include <volk.h>
#include <array>
#include <stdexcept>
#include <vector>

/// Defaults for IA / multisample / dynamic viewport+scissor; fill the rest and build.
class GraphicsPipelineBuilder
{
public:
	GraphicsPipelineBuilder()
	{
		m_ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		m_ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		m_vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		m_vp.viewportCount = 1;
		m_vp.scissorCount = 1;

		m_ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		m_ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		m_dynStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		m_dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		m_dyn.dynamicStateCount = static_cast<uint32_t>(m_dynStates.size());
		m_dyn.pDynamicStates = m_dynStates.data();

		m_raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		m_raster.polygonMode = VK_POLYGON_MODE_FILL;
		m_raster.cullMode = VK_CULL_MODE_BACK_BIT;
		m_raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		m_raster.lineWidth = 1.f;

		m_depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		m_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	}

	GraphicsPipelineBuilder &setLayout(VkPipelineLayout layout)
	{
		m_layout = layout;
		return *this;
	}

	GraphicsPipelineBuilder &setShaderStages(const VkPipelineShaderStageCreateInfo *stages, uint32_t count)
	{
		m_stages.assign(stages, stages + count);
		return *this;
	}

	GraphicsPipelineBuilder &setVertexInput(const VkPipelineVertexInputStateCreateInfo *vi)
	{
		m_vi = vi;
		return *this;
	}

	GraphicsPipelineBuilder &setRaster(const VkPipelineRasterizationStateCreateInfo &r)
	{
		m_raster = r;
		return *this;
	}

	GraphicsPipelineBuilder &setDepth(const VkPipelineDepthStencilStateCreateInfo &d)
	{
		m_depth = d;
		m_hasDepth = true;
		return *this;
	}

	GraphicsPipelineBuilder &setColorBlendAttachments(const VkPipelineColorBlendAttachmentState *atts,
													  uint32_t count)
	{
		m_blendAtts.assign(atts, atts + count);
		m_blend.attachmentCount = count;
		m_blend.pAttachments = m_blendAtts.empty() ? nullptr : m_blendAtts.data();
		return *this;
	}

	GraphicsPipelineBuilder &setNoColorAttachments()
	{
		m_blendAtts.clear();
		m_blend.attachmentCount = 0;
		m_blend.pAttachments = nullptr;
		return *this;
	}

	GraphicsPipelineBuilder &setRenderingFormats(const VkFormat *colorFmts, uint32_t colorCount,
												 VkFormat depthFmt = VK_FORMAT_UNDEFINED)
	{
		m_colorFmts.assign(colorFmts, colorFmts + colorCount);
		m_depthFmt = depthFmt;
		return *this;
	}

	VkPipeline build(VkDevice device) const
	{
		VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		rendering.colorAttachmentCount = static_cast<uint32_t>(m_colorFmts.size());
		rendering.pColorAttachmentFormats = m_colorFmts.empty() ? nullptr : m_colorFmts.data();
		rendering.depthAttachmentFormat = m_depthFmt;

		VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		info.pNext = &rendering;
		info.stageCount = static_cast<uint32_t>(m_stages.size());
		info.pStages = m_stages.data();
		info.pVertexInputState = m_vi;
		info.pInputAssemblyState = &m_ia;
		info.pViewportState = &m_vp;
		info.pRasterizationState = &m_raster;
		info.pMultisampleState = &m_ms;
		info.pDepthStencilState = m_hasDepth ? &m_depth : nullptr;
		info.pColorBlendState = &m_blend;
		info.pDynamicState = &m_dyn;
		info.layout = m_layout;

		VkPipeline pipe = VK_NULL_HANDLE;
		if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipe) != VK_SUCCESS)
			throw std::runtime_error("GraphicsPipelineBuilder::build failed");
		return pipe;
	}

private:
	VkPipelineLayout m_layout{VK_NULL_HANDLE};
	std::vector<VkPipelineShaderStageCreateInfo> m_stages;
	const VkPipelineVertexInputStateCreateInfo *m_vi{nullptr};
	VkPipelineInputAssemblyStateCreateInfo m_ia{};
	VkPipelineViewportStateCreateInfo m_vp{};
	VkPipelineMultisampleStateCreateInfo m_ms{};
	VkPipelineRasterizationStateCreateInfo m_raster{};
	VkPipelineDepthStencilStateCreateInfo m_depth{};
	bool m_hasDepth{false};
	std::vector<VkPipelineColorBlendAttachmentState> m_blendAtts;
	VkPipelineColorBlendStateCreateInfo m_blend{};
	std::array<VkDynamicState, 2> m_dynStates{};
	VkPipelineDynamicStateCreateInfo m_dyn{};
	std::vector<VkFormat> m_colorFmts;
	VkFormat m_depthFmt{VK_FORMAT_UNDEFINED};
};
