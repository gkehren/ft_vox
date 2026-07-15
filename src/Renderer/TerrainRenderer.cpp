#include "Renderer/TerrainRenderer.hpp"
#include "Engine/Profiler.hpp"
#include "Vulkan/VkShader.hpp"
#include "Vulkan/VkUpload.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
std::string spvPath(const char *name) { return resolveSpvPath(name); }

auto beginRenderingFn()
{
	return vkCmdBeginRendering ? vkCmdBeginRendering : vkCmdBeginRenderingKHR;
}
auto endRenderingFn()
{
	return vkCmdEndRendering ? vkCmdEndRendering : vkCmdEndRenderingKHR;
}

VkPipelineVertexInputStateCreateInfo makeVertexInput(
	VkVertexInputBindingDescription &binding,
	std::array<VkVertexInputAttributeDescription, 4> &attrs)
{
	binding.binding = 0;
	binding.stride = sizeof(Vertex);
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)};
	attrs[1] = {1, 0, VK_FORMAT_R32_UINT, offsetof(Vertex, packedData)};
	attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord)};
	attrs[3] = {3, 0, VK_FORMAT_R32_UINT, offsetof(Vertex, packedBiomeColor)};

	VkPipelineVertexInputStateCreateInfo vi{};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &binding;
	vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
	vi.pVertexAttributeDescriptions = attrs.data();
	return vi;
}
} // namespace

TerrainRenderer::~TerrainRenderer()
{
	shutdown();
}

void TerrainRenderer::shutdown()
{
	if (!m_context)
		return;

	m_context->waitIdle();
	destroyFrameResources();
	destroyPipelines();
	m_overlays.shutdown();
	m_post.shutdown();

	if (m_descriptorPool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(m_context->getDevice(), m_descriptorPool, nullptr);
	if (m_setLayout0 != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_context->getDevice(), m_setLayout0, nullptr);
	if (m_setLayout1 != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_context->getDevice(), m_setLayout1, nullptr);

	m_descriptorPool = VK_NULL_HANDLE;
	m_setLayout0 = VK_NULL_HANDLE;
	m_setLayout1 = VK_NULL_HANDLE;
	m_set1 = VK_NULL_HANDLE;

	destroyShadowResources();
	m_textures.shutdown();
	m_context = nullptr;
	m_imm = nullptr;
}

void TerrainRenderer::init(VkContext &context, VkSwapchain &swapchain, ImmediateCommands &imm)
{
	m_context = &context;
	m_imm = &imm;
	m_lightDir = glm::normalize(glm::vec3(0.4f, 1.0f, 0.2f));

	m_textures.initialize(context, imm);
	createShadowResources();
	createDescriptors();
	const auto extent = swapchain.getExtent();
	m_post.init(context, imm, m_setLayout0, swapchain.getImageFormat(), extent.width, extent.height);
	m_overlays.init(context, imm, m_setLayout0, m_post.hdrFormat(), m_post.depthFormat());
	createPipelines(swapchain);
	createFrameResources();
	writeSet1Descriptors();

	std::cout << "TerrainRenderer ready (shadows + water + sky + post + overlays)\n";
}

void TerrainRenderer::onSwapchainRecreate(VkSwapchain &swapchain)
{
	m_context->waitIdle();
	const auto extent = swapchain.getExtent();
	m_post.resize(extent.width, extent.height, swapchain.getImageFormat());
	destroyPipelines();
	createPipelines(swapchain);
	m_overlays.recreatePipelines(m_post.hdrFormat(), m_post.depthFormat());
}

void TerrainRenderer::createShadowResources()
{
	m_shadowMap = createImage2D(
		m_context->getAllocator(), m_context->getDevice(),
		kShadowMapSize, kShadowMapSize, m_depthFormat,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);

	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_NEAREST;
	samplerInfo.minFilter = VK_FILTER_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // outside = lit
	samplerInfo.unnormalizedCoordinates = VK_FALSE;
	samplerInfo.compareEnable = VK_FALSE;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	if (vkCreateSampler(m_context->getDevice(), &samplerInfo, nullptr, &m_shadowSampler) != VK_SUCCESS)
		throw std::runtime_error("Failed to create shadow sampler");
}

void TerrainRenderer::destroyShadowResources()
{
	if (!m_context)
		return;
	if (m_shadowSampler != VK_NULL_HANDLE)
	{
		vkDestroySampler(m_context->getDevice(), m_shadowSampler, nullptr);
		m_shadowSampler = VK_NULL_HANDLE;
	}
	if (m_shadowMap.image != VK_NULL_HANDLE)
		destroyImage(m_context->getAllocator(), m_context->getDevice(), m_shadowMap);
}

void TerrainRenderer::createDescriptors()
{
	VkDescriptorSetLayoutBinding uboBinding{};
	uboBinding.binding = 0;
	uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	uboBinding.descriptorCount = 1;
	uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo layout0{};
	layout0.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout0.bindingCount = 1;
	layout0.pBindings = &uboBinding;
	if (vkCreateDescriptorSetLayout(m_context->getDevice(), &layout0, nullptr, &m_setLayout0) != VK_SUCCESS)
		throw std::runtime_error("Failed to create set layout 0");

	std::array<VkDescriptorSetLayoutBinding, 2> set1Bindings{};
	set1Bindings[0].binding = 0;
	set1Bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	set1Bindings[0].descriptorCount = 1;
	set1Bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	set1Bindings[1].binding = 1;
	set1Bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	set1Bindings[1].descriptorCount = 1;
	set1Bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo layout1{};
	layout1.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout1.bindingCount = static_cast<uint32_t>(set1Bindings.size());
	layout1.pBindings = set1Bindings.data();
	if (vkCreateDescriptorSetLayout(m_context->getDevice(), &layout1, nullptr, &m_setLayout1) != VK_SUCCESS)
		throw std::runtime_error("Failed to create set layout 1");

	std::array<VkDescriptorPoolSize, 2> poolSizes{};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[0].descriptorCount = kMaxFramesInFlight;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSizes[1].descriptorCount = 2; // atlas + shadow

	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	poolInfo.pPoolSizes = poolSizes.data();
	poolInfo.maxSets = kMaxFramesInFlight + 1;
	if (vkCreateDescriptorPool(m_context->getDevice(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
		throw std::runtime_error("Failed to create descriptor pool");

	VkDescriptorSetAllocateInfo alloc1{};
	alloc1.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc1.descriptorPool = m_descriptorPool;
	alloc1.descriptorSetCount = 1;
	alloc1.pSetLayouts = &m_setLayout1;
	if (vkAllocateDescriptorSets(m_context->getDevice(), &alloc1, &m_set1) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate descriptor set 1");

	std::array<VkDescriptorSetLayout, 2> setLayouts = {m_setLayout0, m_setLayout1};
	VkPipelineLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
	layoutInfo.pSetLayouts = setLayouts.data();
	if (vkCreatePipelineLayout(m_context->getDevice(), &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
		throw std::runtime_error("Failed to create pipeline layout");
}

void TerrainRenderer::writeSet1Descriptors()
{
	VkDescriptorImageInfo atlasInfo{};
	atlasInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	atlasInfo.imageView = m_textures.getImageView();
	atlasInfo.sampler = m_textures.getSampler();

	VkDescriptorImageInfo shadowInfo{};
	shadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	shadowInfo.imageView = m_shadowMap.view;
	shadowInfo.sampler = m_shadowSampler;

	std::array<VkWriteDescriptorSet, 2> writes{};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = m_set1;
	writes[0].dstBinding = 0;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[0].descriptorCount = 1;
	writes[0].pImageInfo = &atlasInfo;

	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = m_set1;
	writes[1].dstBinding = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[1].descriptorCount = 1;
	writes[1].pImageInfo = &shadowInfo;

	vkUpdateDescriptorSets(m_context->getDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void TerrainRenderer::destroyPipelines()
{
	if (!m_context)
		return;
	if (m_opaquePipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(m_context->getDevice(), m_opaquePipeline, nullptr);
	if (m_waterPipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(m_context->getDevice(), m_waterPipeline, nullptr);
	if (m_shadowPipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(m_context->getDevice(), m_shadowPipeline, nullptr);
	if (m_pipelineLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(m_context->getDevice(), m_pipelineLayout, nullptr);
	m_opaquePipeline = m_waterPipeline = m_shadowPipeline = VK_NULL_HANDLE;
	m_pipelineLayout = VK_NULL_HANDLE;
}

void TerrainRenderer::createPipelines(VkSwapchain &swapchain)
{
	// Recreate layout if destroyed with pipelines
	if (m_pipelineLayout == VK_NULL_HANDLE)
	{
		std::array<VkDescriptorSetLayout, 2> setLayouts = {m_setLayout0, m_setLayout1};
		VkPipelineLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
		layoutInfo.pSetLayouts = setLayouts.data();
		if (vkCreatePipelineLayout(m_context->getDevice(), &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
			throw std::runtime_error("Failed to create pipeline layout");
	}

	VkShaderModule terrainVert = loadShaderModule(m_context->getDevice(), spvPath("terrain.vert.spv"));
	VkShaderModule terrainFrag = loadShaderModule(m_context->getDevice(), spvPath("terrain.frag.spv"));
	VkShaderModule shadowVert = loadShaderModule(m_context->getDevice(), spvPath("shadow.vert.spv"));
	VkShaderModule shadowFrag = loadShaderModule(m_context->getDevice(), spvPath("shadow.frag.spv"));

	VkVertexInputBindingDescription binding{};
	std::array<VkVertexInputAttributeDescription, 4> attrs{};
	auto vertexInput = makeVertexInput(binding, attrs);

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic{};
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
	dynamic.pDynamicStates = dynamicStates.data();

	// --- Opaque pipeline ---
	{
		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
					 VK_SHADER_STAGE_VERTEX_BIT, terrainVert, "main", nullptr};
		stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
					 VK_SHADER_STAGE_FRAGMENT_BIT, terrainFrag, "main", nullptr};

		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_BACK_BIT;
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;

		VkPipelineDepthStencilStateCreateInfo depthStencil{};
		depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depthStencil.depthTestEnable = VK_TRUE;
		depthStencil.depthWriteEnable = VK_TRUE;
		depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

		VkPipelineColorBlendAttachmentState blendAtt{};
		blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
								  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		blendAtt.blendEnable = VK_FALSE;

		VkPipelineColorBlendStateCreateInfo blend{};
		blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		blend.attachmentCount = 1;
		blend.pAttachments = &blendAtt;

		VkPipelineRenderingCreateInfo renderingInfo{};
		renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
		VkFormat colorFormat = m_post.hdrFormat();
		renderingInfo.colorAttachmentCount = 1;
		renderingInfo.pColorAttachmentFormats = &colorFormat;
		renderingInfo.depthAttachmentFormat = m_post.depthFormat();

		VkGraphicsPipelineCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		info.pNext = &renderingInfo;
		info.stageCount = 2;
		info.pStages = stages;
		info.pVertexInputState = &vertexInput;
		info.pInputAssemblyState = &inputAssembly;
		info.pViewportState = &viewportState;
		info.pRasterizationState = &raster;
		info.pMultisampleState = &multisample;
		info.pDepthStencilState = &depthStencil;
		info.pColorBlendState = &blend;
		info.pDynamicState = &dynamic;
		info.layout = m_pipelineLayout;
		if (vkCreateGraphicsPipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, &info, nullptr, &m_opaquePipeline) != VK_SUCCESS)
			throw std::runtime_error("Failed to create opaque pipeline");
	}

	// --- Water pipeline (alpha blend, no depth write, no cull) ---
	{
		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
					 VK_SHADER_STAGE_VERTEX_BIT, terrainVert, "main", nullptr};
		stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
					 VK_SHADER_STAGE_FRAGMENT_BIT, terrainFrag, "main", nullptr};

		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;

		VkPipelineDepthStencilStateCreateInfo depthStencil{};
		depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depthStencil.depthTestEnable = VK_TRUE;
		depthStencil.depthWriteEnable = VK_FALSE;
		depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

		VkPipelineColorBlendAttachmentState blendAtt{};
		blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
								  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		blendAtt.blendEnable = VK_TRUE;
		blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blendAtt.colorBlendOp = VK_BLEND_OP_ADD;
		blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blendAtt.alphaBlendOp = VK_BLEND_OP_ADD;

		VkPipelineColorBlendStateCreateInfo blend{};
		blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		blend.attachmentCount = 1;
		blend.pAttachments = &blendAtt;

		VkPipelineRenderingCreateInfo renderingInfo{};
		renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
		VkFormat colorFormat = m_post.hdrFormat();
		renderingInfo.colorAttachmentCount = 1;
		renderingInfo.pColorAttachmentFormats = &colorFormat;
		renderingInfo.depthAttachmentFormat = m_post.depthFormat();

		VkGraphicsPipelineCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		info.pNext = &renderingInfo;
		info.stageCount = 2;
		info.pStages = stages;
		info.pVertexInputState = &vertexInput;
		info.pInputAssemblyState = &inputAssembly;
		info.pViewportState = &viewportState;
		info.pRasterizationState = &raster;
		info.pMultisampleState = &multisample;
		info.pDepthStencilState = &depthStencil;
		info.pColorBlendState = &blend;
		info.pDynamicState = &dynamic;
		info.layout = m_pipelineLayout;
		if (vkCreateGraphicsPipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, &info, nullptr, &m_waterPipeline) != VK_SUCCESS)
			throw std::runtime_error("Failed to create water pipeline");
	}

	// --- Shadow pipeline (depth only, front cull for peter-panning) ---
	{
		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
					 VK_SHADER_STAGE_VERTEX_BIT, shadowVert, "main", nullptr};
		stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
					 VK_SHADER_STAGE_FRAGMENT_BIT, shadowFrag, "main", nullptr};

		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_FRONT_BIT; // reduce shadow acne (same as old GL)
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;
		raster.depthBiasEnable = VK_TRUE;
		raster.depthBiasConstantFactor = 1.25f;
		raster.depthBiasSlopeFactor = 1.75f;

		VkPipelineDepthStencilStateCreateInfo depthStencil{};
		depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depthStencil.depthTestEnable = VK_TRUE;
		depthStencil.depthWriteEnable = VK_TRUE;
		depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

		VkPipelineColorBlendStateCreateInfo blend{};
		blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		blend.attachmentCount = 0;

		VkPipelineRenderingCreateInfo renderingInfo{};
		renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
		renderingInfo.colorAttachmentCount = 0;
		renderingInfo.depthAttachmentFormat = m_depthFormat;

		VkGraphicsPipelineCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		info.pNext = &renderingInfo;
		info.stageCount = 2;
		info.pStages = stages;
		info.pVertexInputState = &vertexInput;
		info.pInputAssemblyState = &inputAssembly;
		info.pViewportState = &viewportState;
		info.pRasterizationState = &raster;
		info.pMultisampleState = &multisample;
		info.pDepthStencilState = &depthStencil;
		info.pColorBlendState = &blend;
		info.pDynamicState = &dynamic;
		info.layout = m_pipelineLayout;
		if (vkCreateGraphicsPipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, &info, nullptr, &m_shadowPipeline) != VK_SUCCESS)
			throw std::runtime_error("Failed to create shadow pipeline");
	}

	destroyShaderModule(m_context->getDevice(), terrainVert);
	destroyShaderModule(m_context->getDevice(), terrainFrag);
	destroyShaderModule(m_context->getDevice(), shadowVert);
	destroyShaderModule(m_context->getDevice(), shadowFrag);
}

void TerrainRenderer::createFrameResources()
{
	for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
	{
		auto &f = m_frames[i];
		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = m_context->getGraphicsQueueFamily();
		if (vkCreateCommandPool(m_context->getDevice(), &poolInfo, nullptr, &f.pool) != VK_SUCCESS)
			throw std::runtime_error("Failed to create terrain command pool");

		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = f.pool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(m_context->getDevice(), &allocInfo, &f.cmd) != VK_SUCCESS)
			throw std::runtime_error("Failed to allocate terrain command buffer");

		VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		if (vkCreateSemaphore(m_context->getDevice(), &semInfo, nullptr, &f.imageAvailable) != VK_SUCCESS ||
			vkCreateSemaphore(m_context->getDevice(), &semInfo, nullptr, &f.renderFinished) != VK_SUCCESS ||
			vkCreateFence(m_context->getDevice(), &fenceInfo, nullptr, &f.inFlight) != VK_SUCCESS)
			throw std::runtime_error("Failed to create terrain frame sync objects");

		f.ubo = createBuffer(
			m_context->getAllocator(), sizeof(TerrainRenderer::FrameUBO),
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
			VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
		f.uboMapped = f.ubo.info.pMappedData ? f.ubo.info.pMappedData : mapBuffer(m_context->getAllocator(), f.ubo);

		VkDescriptorSetAllocateInfo setAlloc{};
		setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		setAlloc.descriptorPool = m_descriptorPool;
		setAlloc.descriptorSetCount = 1;
		setAlloc.pSetLayouts = &m_setLayout0;
		if (vkAllocateDescriptorSets(m_context->getDevice(), &setAlloc, &f.descriptorSet0) != VK_SUCCESS)
			throw std::runtime_error("Failed to allocate frame descriptor set");

		VkDescriptorBufferInfo bufferInfo{};
		bufferInfo.buffer = f.ubo.buffer;
		bufferInfo.offset = 0;
		bufferInfo.range = sizeof(TerrainRenderer::FrameUBO);

		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = f.descriptorSet0;
		write.dstBinding = 0;
		write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		write.descriptorCount = 1;
		write.pBufferInfo = &bufferInfo;
		vkUpdateDescriptorSets(m_context->getDevice(), 1, &write, 0, nullptr);
	}
}

void TerrainRenderer::destroyFrameResources()
{
	if (!m_context)
		return;
	for (auto &f : m_frames)
	{
		if (f.inFlight)
			vkDestroyFence(m_context->getDevice(), f.inFlight, nullptr);
		if (f.imageAvailable)
			vkDestroySemaphore(m_context->getDevice(), f.imageAvailable, nullptr);
		if (f.renderFinished)
			vkDestroySemaphore(m_context->getDevice(), f.renderFinished, nullptr);
		if (f.pool)
			vkDestroyCommandPool(m_context->getDevice(), f.pool, nullptr);
		if (f.ubo.buffer)
		{
			if (f.uboMapped && !f.ubo.info.pMappedData)
				unmapBuffer(m_context->getAllocator(), f.ubo);
			destroyBuffer(m_context->getAllocator(), f.ubo);
		}
		f = {};
	}
	m_imagesInFlight.clear();
}

glm::mat4 TerrainRenderer::computeLightSpaceMatrix(const glm::vec3 &cameraPos, const glm::vec3 &lightDir)
{
	const glm::vec3 dir = glm::normalize(lightDir);
	const glm::vec3 lightPos = cameraPos + dir * 500.0f;
	const glm::mat4 lightView = glm::lookAt(lightPos, cameraPos, glm::vec3(0.0f, 1.0f, 0.0f));
	// Ortho matches old OpenGL shadow volume; depth 0..1 via GLM_FORCE_DEPTH_ZERO_TO_ONE
	const glm::mat4 lightProj = glm::ortho(-512.0f, 512.0f, -512.0f, 512.0f, 1.0f, 1000.0f);
	return lightProj * lightView;
}

void TerrainRenderer::updateFrameUBO(uint32_t frameIndex, const Camera &camera, float aspectW, float aspectH, float farPlane,
									 float time, const ShaderParameters &params)
{
	m_time = time;
	m_lastCamPos = camera.getPosition();

	const float dayTime = params.dayTime;
	const float sunAngle = dayTime * 6.2831853f - 1.5707963f;
	const glm::vec3 sunDir = glm::normalize(glm::vec3(std::cos(sunAngle), std::sin(sunAngle) * 0.85f + 0.15f, 0.35f));
	const glm::vec3 moonDir = -sunDir;
	m_lightDir = params.lightDirection;

	const float day = params.dayFactor;
	const float night = params.nightFactor;
	const float sunset = params.sunsetFactor;

	FrameUBO ubo{};
	ubo.view = camera.getViewMatrix();
	ubo.projection = camera.getProjectionMatrix(aspectW, aspectH, farPlane);
	ubo.lightSpaceMatrix = computeLightSpaceMatrix(camera.getPosition(), m_lightDir);
	ubo.viewPos = glm::vec4(camera.getPosition(), 1.0f);
	ubo.lightDirection = glm::vec4(m_lightDir, 0.0f);
	ubo.fogColor = glm::vec4(params.fogColor, 1.0f);
	ubo.fogParams = glm::vec4(params.fogStart, params.fogEnd, params.fogDensity, 0.0f);
	// colorBoost in lightParams.w + visualParams.z; saturation/contrast in visualParams
	packFrameLightVisual(params, ubo.lightParams, ubo.visualParams);
	ubo.sunDir = glm::vec4(sunDir, 0.0f);
	ubo.moonDir = glm::vec4(moonDir, 0.0f);
	ubo.skyParams = glm::vec4(time, day, sunset, night);

	std::memcpy(m_frames[frameIndex].uboMapped, &ubo, sizeof(FrameUBO));
}

bool TerrainRenderer::beginAcquire(VkSwapchain &swapchain, uint32_t &outImageIndex, uint32_t frameIndex)
{
	auto &f = m_frames[frameIndex];
	vkWaitForFences(m_context->getDevice(), 1, &f.inFlight, VK_TRUE, UINT64_MAX);

	VkResult result = vkAcquireNextImageKHR(m_context->getDevice(), swapchain.getSwapchain(),
											UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &outImageIndex);
	if (result == VK_ERROR_OUT_OF_DATE_KHR)
		return false;
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
		throw std::runtime_error("Failed to acquire swapchain image");

	if (m_imagesInFlight.size() != swapchain.getImageCount())
		m_imagesInFlight.assign(swapchain.getImageCount(), VK_NULL_HANDLE);
	if (m_imagesInFlight[outImageIndex] != VK_NULL_HANDLE)
		vkWaitForFences(m_context->getDevice(), 1, &m_imagesInFlight[outImageIndex], VK_TRUE, UINT64_MAX);
	m_imagesInFlight[outImageIndex] = f.inFlight;

	vkResetFences(m_context->getDevice(), 1, &f.inFlight);
	vkResetCommandBuffer(f.cmd, 0);
	return true;
}

void TerrainRenderer::recordFrame(uint32_t frameIndex,
								  uint32_t imageIndex,
								  VkSwapchain &swapchain,
								  const std::vector<Chunk *> &chunks,
								  const std::vector<Chunk *> &shadowChunks,
								  const VkClearColorValue &clearColor,
								  const std::function<void(VkCommandBuffer)> &preRecord,
								  const std::function<void(VkCommandBuffer)> &imguiDraw)
{
	auto &f = m_frames[frameIndex];
	const VkExtent2D extent = swapchain.getExtent();
	const auto beginRendering = beginRenderingFn();
	const auto endRendering = endRenderingFn();
	if (!beginRendering || !endRendering)
		throw std::runtime_error("Dynamic rendering entry points unavailable");

	VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	if (vkBeginCommandBuffer(f.cmd, &beginInfo) != VK_SUCCESS)
		throw std::runtime_error("Failed to begin terrain command buffer");

	// Mesh staging copies (same queue, before any vertex use).
	// Engine wraps preRecord with PROFILE_SCOPE("MeshUpload").
	if (preRecord)
		preRecord(f.cmd);

	std::array<VkDescriptorSet, 2> sets = {f.descriptorSet0, m_set1};

	// ========== Shadow pass ==========
	{
		PROFILE_SCOPE("Shadow");
		VkImageMemoryBarrier toDepth{};
		toDepth.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		toDepth.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toDepth.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		toDepth.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDepth.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDepth.image = m_shadowMap.image;
		toDepth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
		toDepth.srcAccessMask = 0;
		toDepth.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
							 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
							 0, 0, nullptr, 0, nullptr, 1, &toDepth);

		VkRenderingAttachmentInfo depthAtt{};
		depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		depthAtt.imageView = m_shadowMap.view;
		depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		depthAtt.clearValue.depthStencil = {1.0f, 0};

		VkRenderingInfo shadowInfo{};
		shadowInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
		shadowInfo.renderArea = {{0, 0}, {kShadowMapSize, kShadowMapSize}};
		shadowInfo.layerCount = 1;
		shadowInfo.pDepthAttachment = &depthAtt;

		beginRendering(f.cmd, &shadowInfo);

		VkViewport vp{0.f, 0.f, static_cast<float>(kShadowMapSize), static_cast<float>(kShadowMapSize), 0.f, 1.f};
		VkRect2D scissor{{0, 0}, {kShadowMapSize, kShadowMapSize}};
		vkCmdSetViewport(f.cmd, 0, 1, &vp);
		vkCmdSetScissor(f.cmd, 0, 1, &scissor);

		vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
		vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0,
								static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);

		for (Chunk *chunk : shadowChunks)
		{
			if (chunk)
				chunk->drawShadow(f.cmd);
		}

		endRendering(f.cmd);

		// Depth attachment → shader read
		VkImageMemoryBarrier toSample = toDepth;
		toSample.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		toSample.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
							 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
							 0, 0, nullptr, 0, nullptr, 1, &toSample);
	}

	// ========== HDR scene pass (opaque + water) ==========
	{
		PROFILE_SCOPE("Scene");
		VkImageMemoryBarrier toColor{};
		toColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toColor.image = m_post.hdrColor().image;
		toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		toColor.srcAccessMask = 0;
		toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
							 0, 0, nullptr, 0, nullptr, 1, &toColor);

		VkImageMemoryBarrier toDepth{};
		toDepth.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		toDepth.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toDepth.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		toDepth.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDepth.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDepth.image = m_post.sceneDepth().image;
		toDepth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
		toDepth.srcAccessMask = 0;
		toDepth.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
							 0, 0, nullptr, 0, nullptr, 1, &toDepth);

		VkRenderingAttachmentInfo colorAtt{};
		colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		colorAtt.imageView = m_post.hdrColor().view;
		colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAtt.clearValue.color = clearColor;

		VkRenderingAttachmentInfo depthAtt{};
		depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		depthAtt.imageView = m_post.sceneDepth().view;
		depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		depthAtt.clearValue.depthStencil = {1.0f, 0};

		VkRenderingInfo renderingInfo{};
		renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
		renderingInfo.renderArea = {{0, 0}, extent};
		renderingInfo.layerCount = 1;
		renderingInfo.colorAttachmentCount = 1;
		renderingInfo.pColorAttachments = &colorAtt;
		renderingInfo.pDepthAttachment = &depthAtt;

		beginRendering(f.cmd, &renderingInfo);

		VkViewport viewport{};
		viewport.x = 0.0f;
		viewport.y = static_cast<float>(extent.height);
		viewport.width = static_cast<float>(extent.width);
		viewport.height = -static_cast<float>(extent.height);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		vkCmdSetViewport(f.cmd, 0, 1, &viewport);
		VkRect2D scissor{{0, 0}, extent};
		vkCmdSetScissor(f.cmd, 0, 1, &scissor);

		vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0,
								static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);

		vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_opaquePipeline);
		for (Chunk *chunk : chunks)
		{
			if (chunk)
				chunk->draw(f.cmd);
		}

		{
			struct WaterEntry
			{
				Chunk *chunk;
				float dist2;
			};
			std::vector<WaterEntry> waterChunks;
			waterChunks.reserve(chunks.size());
			const auto *ubo = static_cast<const FrameUBO *>(f.uboMapped);
			const glm::vec3 camPos = glm::vec3(ubo->viewPos);
			for (Chunk *chunk : chunks)
			{
				if (chunk && chunk->hasWaterMesh())
				{
					const glm::vec3 c = chunk->getPosition() + glm::vec3(CHUNK_SIZE * 0.5f, 0.f, CHUNK_SIZE * 0.5f);
					const glm::vec3 d = c - camPos;
					waterChunks.push_back({chunk, glm::dot(d, d)});
				}
			}
			std::sort(waterChunks.begin(), waterChunks.end(),
					  [](const WaterEntry &a, const WaterEntry &b) { return a.dist2 > b.dist2; });

			vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterPipeline);
			for (const auto &e : waterChunks)
				e.chunk->drawWater(f.cmd);
		}

		// Overlays (chunk borders, voxel highlight, players) into HDR before sky/post
		m_overlays.record(f.cmd, f.descriptorSet0, chunks);

		endRendering(f.cmd);
	}

	// ========== Sky MRT (HDR + god-ray source) ==========
	{
		PROFILE_SCOPE("Sky");
		m_post.recordSky(f.cmd, f.descriptorSet0, extent);
	}

	// ========== Post: bloom / god rays / composite → swapchain ==========
	{
		PROFILE_SCOPE("Post");
		const auto *ubo = static_cast<const FrameUBO *>(f.uboMapped);
		glm::vec4 sunClip = ubo->projection * ubo->view * glm::vec4(m_lastCamPos + glm::vec3(ubo->sunDir) * 500.f, 1.f);
		glm::vec2 sunScreen(0.5f, 0.5f);
		float sunVisibility = 0.f;
		if (sunClip.w > 0.f)
		{
			glm::vec3 ndc = glm::vec3(sunClip) / sunClip.w;
			// Account for negative viewport Y flip in main pass (screen Y is flipped vs NDC)
			// Same UV space as the fullscreen post pass (no extra Y flip).
			sunScreen = glm::vec2(ndc.x * 0.5f + 0.5f, ndc.y * 0.5f + 0.5f);
			sunVisibility = glm::smoothstep(-0.10f, 0.04f, ubo->sunDir.y);
			if (sunScreen.x < 0.f || sunScreen.x > 1.f || sunScreen.y < 0.f || sunScreen.y > 1.f)
				sunVisibility = 0.f;
		}

		m_post.recordPost(f.cmd, swapchain.getImages()[imageIndex], swapchain.getImageViews()[imageIndex],
						  extent, f.descriptorSet0, m_postSettings, sunScreen, sunVisibility, m_time);
	}

	// ========== ImGui on swapchain (load) ==========
	if (imguiDraw)
	{
		PROFILE_SCOPE("ImGuiDraw");
		VkRenderingAttachmentInfo colorAtt{};
		colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		colorAtt.imageView = swapchain.getImageViews()[imageIndex];
		colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

		VkRenderingInfo ri{};
		ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
		ri.renderArea = {{0, 0}, extent};
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &colorAtt;
		beginRendering(f.cmd, &ri);
		VkViewport vp{0, 0, (float)extent.width, (float)extent.height, 0, 1};
		VkRect2D sc{{0, 0}, extent};
		vkCmdSetViewport(f.cmd, 0, 1, &vp);
		vkCmdSetScissor(f.cmd, 0, 1, &sc);
		imguiDraw(f.cmd);
		endRendering(f.cmd);
	}

	// Swapchain → present
	VkImageMemoryBarrier toPresent{};
	toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toPresent.image = swapchain.getImages()[imageIndex];
	toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	toPresent.dstAccessMask = 0;
	vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
						 0, 0, nullptr, 0, nullptr, 1, &toPresent);

	if (vkEndCommandBuffer(f.cmd) != VK_SUCCESS)
		throw std::runtime_error("Failed to end terrain command buffer");
}

bool TerrainRenderer::submitPresent(VkSwapchain &swapchain, uint32_t imageIndex, uint32_t frameIndex)
{
	auto &f = m_frames[frameIndex];

	// Wait for color output AND shadow sampling readiness is handled in-cmd.
	// Still wait at COLOR_ATTACHMENT for WSI acquire semaphore.
	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &f.imageAvailable;
	submitInfo.pWaitDstStageMask = &waitStage;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &f.cmd;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &f.renderFinished;

	if (vkQueueSubmit(m_context->getGraphicsQueue(), 1, &submitInfo, f.inFlight) != VK_SUCCESS)
		throw std::runtime_error("Failed to submit terrain frame");

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &f.renderFinished;
	VkSwapchainKHR sc = swapchain.getSwapchain();
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &sc;
	presentInfo.pImageIndices = &imageIndex;

	VkResult result = vkQueuePresentKHR(m_context->getPresentQueue(), &presentInfo);
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
		return false;
	if (result != VK_SUCCESS)
		throw std::runtime_error("Failed to present");
	return true;
}
