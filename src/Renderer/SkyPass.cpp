#include "Renderer/SkyPass.hpp"
#include "Vulkan/ImageBarrier.hpp"
#include "Vulkan/VkShader.hpp"
#include "Vulkan/VkUpload.hpp"
#include "utils.hpp"

#include <array>
#include <stdexcept>

namespace
{
auto beginR() { return vkCmdBeginRendering ? vkCmdBeginRendering : vkCmdBeginRenderingKHR; }
auto endR() { return vkCmdEndRendering ? vkCmdEndRendering : vkCmdEndRenderingKHR; }
} // namespace

void SkyPass::init(VkContext &context, ImmediateCommands &imm, VkDescriptorSetLayout frameSetLayout,
				   VkFormat hdrFormat, VkFormat depthFormat)
{
	m_context = &context;
	m_frameSetLayout = frameSetLayout;
	m_hdrFormat = hdrFormat;
	m_depthFormat = depthFormat;
	createGeometry(imm);

	VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &m_frameSetLayout;
	if (vkCreatePipelineLayout(m_context->getDevice(), &pl, nullptr, &m_layout) != VK_SUCCESS)
		throw std::runtime_error("SkyPass pipeline layout failed");

	createPipeline(hdrFormat, depthFormat);
}

void SkyPass::shutdown()
{
	destroyPipeline();
	if (m_context && m_layout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(m_context->getDevice(), m_layout, nullptr);
		m_layout = VK_NULL_HANDLE;
	}
	if (m_context && m_vbo.buffer != VK_NULL_HANDLE)
		destroyBuffer(m_context->getAllocator(), m_vbo);
	m_context = nullptr;
}

void SkyPass::createGeometry(ImmediateCommands &imm)
{
	float v[] = {
		-1, -1, -1, 1, -1, -1, 1, 1, -1, 1, 1, -1, -1, 1, -1, -1, -1, -1,
		-1, -1, 1, 1, -1, 1, 1, 1, 1, 1, 1, 1, -1, 1, 1, -1, -1, 1,
		-1, 1, -1, 1, 1, -1, 1, 1, 1, 1, 1, 1, -1, 1, 1, -1, 1, -1,
		-1, -1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, -1, -1, 1, -1, -1, -1,
		1, -1, -1, 1, 1, -1, 1, 1, 1, 1, 1, 1, 1, -1, 1, 1, -1, -1,
		-1, -1, -1, -1, 1, -1, -1, 1, 1, -1, 1, 1, -1, -1, 1, -1, -1, -1,
	};
	m_vbo = createBuffer(m_context->getAllocator(), sizeof(v),
						 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
						 VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
	uploadBuffer(m_context->getAllocator(), imm, m_vbo, v, sizeof(v));
}

void SkyPass::createPipeline(VkFormat hdrFormat, VkFormat depthFormat)
{
	destroyPipeline();
	m_hdrFormat = hdrFormat;
	m_depthFormat = depthFormat;

	VkShaderModule skyV = loadShaderModule(m_context->getDevice(), resolveSpvPath("skybox.vert.spv"));
	VkShaderModule skyF = loadShaderModule(m_context->getDevice(), resolveSpvPath("skybox.frag.spv"));

	VkVertexInputBindingDescription sb{0, sizeof(float) * 3, VK_VERTEX_INPUT_RATE_VERTEX};
	VkVertexInputAttributeDescription sa{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
	VkPipelineVertexInputStateCreateInfo svi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	svi.vertexBindingDescriptionCount = 1;
	svi.pVertexBindingDescriptions = &sb;
	svi.vertexAttributeDescriptionCount = 1;
	svi.pVertexAttributeDescriptions = &sa;

	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo sds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	sds.depthTestEnable = VK_TRUE;
	sds.depthWriteEnable = VK_FALSE;
	sds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

	VkPipelineColorBlendAttachmentState sba[2]{};
	sba[0].colorWriteMask = sba[1].colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo scb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	scb.attachmentCount = 2;
	scb.pAttachments = sba;

	std::array<VkDynamicState, 2> dynS = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dyn.dynamicStateCount = 2;
	dyn.pDynamicStates = dynS.data();

	VkPipelineShaderStageCreateInfo stages[2] = {
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, skyV, "main", nullptr},
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, skyF, "main", nullptr},
	};
	std::array<VkFormat, 2> cols = {hdrFormat, hdrFormat};
	VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	ri.colorAttachmentCount = 2;
	ri.pColorAttachmentFormats = cols.data();
	ri.depthAttachmentFormat = depthFormat;

	VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gi.pNext = &ri;
	gi.stageCount = 2;
	gi.pStages = stages;
	gi.pVertexInputState = &svi;
	gi.pInputAssemblyState = &ia;
	gi.pViewportState = &vp;
	gi.pRasterizationState = &rs;
	gi.pMultisampleState = &ms;
	gi.pDepthStencilState = &sds;
	gi.pColorBlendState = &scb;
	gi.pDynamicState = &dyn;
	gi.layout = m_layout;
	if (vkCreateGraphicsPipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, &gi, nullptr, &m_pipeline) != VK_SUCCESS)
		throw std::runtime_error("SkyPass pipeline failed");

	destroyShaderModule(m_context->getDevice(), skyV);
	destroyShaderModule(m_context->getDevice(), skyF);
}

void SkyPass::destroyPipeline()
{
	if (m_context && m_pipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(m_context->getDevice(), m_pipeline, nullptr);
		m_pipeline = VK_NULL_HANDLE;
	}
}

void SkyPass::recreatePipeline(VkFormat hdrFormat, VkFormat depthFormat)
{
	createPipeline(hdrFormat, depthFormat);
}

void SkyPass::record(VkCommandBuffer cmd, VkDescriptorSet frameSet0, VkExtent2D extent,
					 AllocatedImage &hdr, AllocatedImage &godSource, AllocatedImage &sceneDepth)
{
	const auto beginRendering = beginR();
	const auto endRendering = endR();

	vkbar::cmdTransitionColor(cmd, godSource.image, VK_IMAGE_LAYOUT_UNDEFINED,
							  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
							  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

	VkRenderingAttachmentInfo cols[2]{};
	cols[0].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	cols[0].imageView = hdr.view;
	cols[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	cols[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	cols[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;

	cols[1].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	cols[1].imageView = godSource.view;
	cols[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	cols[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	cols[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	cols[1].clearValue.color = {{0, 0, 0, 0}};

	VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	depth.imageView = sceneDepth.view;
	depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

	VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = {{0, 0}, extent};
	ri.layerCount = 1;
	ri.colorAttachmentCount = 2;
	ri.pColorAttachments = cols;
	ri.pDepthAttachment = &depth;
	beginRendering(cmd, &ri);

	VkViewport viewport{0, (float)extent.height, (float)extent.width, -(float)extent.height, 0, 1};
	VkRect2D scissor{{0, 0}, extent};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &frameSet0, 0, nullptr);
	VkDeviceSize off = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_vbo.buffer, &off);
	vkCmdDraw(cmd, 36, 1, 0, 0);

	endRendering(cmd);
}
