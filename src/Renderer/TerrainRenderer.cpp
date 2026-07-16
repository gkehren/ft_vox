#include "Renderer/TerrainRenderer.hpp"
#include "Engine/Profiler.hpp"
#include "Vulkan/VkShader.hpp"
#include "Vulkan/VkUpload.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
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

	if (m_pipelineLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(m_context->getDevice(), m_pipelineLayout, nullptr);
	if (m_shadowPipelineLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(m_context->getDevice(), m_shadowPipelineLayout, nullptr);
	if (m_waterPipelineLayout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(m_context->getDevice(), m_waterPipelineLayout, nullptr);
	m_pipelineLayout = m_shadowPipelineLayout = m_waterPipelineLayout = VK_NULL_HANDLE;

	if (m_descriptorPool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(m_context->getDevice(), m_descriptorPool, nullptr);
	if (m_setLayout0 != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_context->getDevice(), m_setLayout0, nullptr);
	if (m_setLayout1 != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_context->getDevice(), m_setLayout1, nullptr);
	if (m_setLayout2 != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(m_context->getDevice(), m_setLayout2, nullptr);

	m_descriptorPool = VK_NULL_HANDLE;
	m_setLayout0 = m_setLayout1 = m_setLayout2 = VK_NULL_HANDLE;
	m_set1 = m_set2Water = VK_NULL_HANDLE;

	if (m_sceneSampler != VK_NULL_HANDLE)
	{
		vkDestroySampler(m_context->getDevice(), m_sceneSampler, nullptr);
		m_sceneSampler = VK_NULL_HANDLE;
	}
	if (m_sceneHistory.image != VK_NULL_HANDLE)
		destroyImage(m_context->getAllocator(), m_context->getDevice(), m_sceneHistory);
	if (m_depthHistory.image != VK_NULL_HANDLE)
		destroyImage(m_context->getAllocator(), m_context->getDevice(), m_depthHistory);

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

	// Scene history for water refraction + depth history for shore foam
	m_sceneHistory = createImage2D(
		m_context->getAllocator(), m_context->getDevice(),
		extent.width, extent.height, VK_FORMAT_R16G16B16A16_SFLOAT,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
	m_depthHistory = createImage2D(
		m_context->getAllocator(), m_context->getDevice(),
		extent.width, extent.height, m_depthFormat,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
	{
		VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		si.magFilter = si.minFilter = VK_FILTER_LINEAR;
		si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		if (vkCreateSampler(m_context->getDevice(), &si, nullptr, &m_sceneSampler) != VK_SUCCESS)
			throw std::runtime_error("scene sampler failed");
	}

	m_post.init(context, imm, m_setLayout0, swapchain.getImageFormat(), extent.width, extent.height);
	m_overlays.init(context, imm, m_setLayout0, m_post.hdrFormat(), m_post.depthFormat());
	createPipelines(swapchain);
	createFrameResources();
	writeSet1Descriptors();
	writeWaterSceneDescriptors();

	std::cout << "TerrainRenderer ready (CSM + water refraction + SSAO + sky + post)\n";
}

void TerrainRenderer::onSwapchainRecreate(VkSwapchain &swapchain)
{
	m_context->waitIdle();
	const auto extent = swapchain.getExtent();
	m_post.resize(extent.width, extent.height, swapchain.getImageFormat());
	if (m_sceneHistory.image != VK_NULL_HANDLE)
		destroyImage(m_context->getAllocator(), m_context->getDevice(), m_sceneHistory);
	if (m_depthHistory.image != VK_NULL_HANDLE)
		destroyImage(m_context->getAllocator(), m_context->getDevice(), m_depthHistory);
	m_sceneHistory = createImage2D(
		m_context->getAllocator(), m_context->getDevice(),
		extent.width, extent.height, VK_FORMAT_R16G16B16A16_SFLOAT,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
	m_depthHistory = createImage2D(
		m_context->getAllocator(), m_context->getDevice(),
		extent.width, extent.height, m_depthFormat,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
	writeWaterSceneDescriptors();
	destroyPipelines();
	createPipelines(swapchain);
	m_overlays.recreatePipelines(m_post.hdrFormat(), m_post.depthFormat());
}

void TerrainRenderer::createShadowResources()
{
	m_shadowMap = createImage2DArray(
		m_context->getAllocator(), m_context->getDevice(),
		kShadowMapSize, kShadowMapSize, static_cast<uint32_t>(kCascadeCount), m_depthFormat,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, VK_IMAGE_ASPECT_DEPTH_BIT);

	for (int i = 0; i < kCascadeCount; ++i)
	{
		m_shadowLayerViews[i] = createImageView2DLayer(
			m_context->getDevice(), m_shadowMap.image, m_depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT,
			static_cast<uint32_t>(i));
	}

	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
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
	for (auto &v : m_shadowLayerViews)
	{
		if (v != VK_NULL_HANDLE)
		{
			vkDestroyImageView(m_context->getDevice(), v, nullptr);
			v = VK_NULL_HANDLE;
		}
	}
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

	std::array<VkDescriptorSetLayoutBinding, 2> set2Bindings = set1Bindings;
	VkDescriptorSetLayoutCreateInfo layout2 = layout1;
	layout2.pBindings = set2Bindings.data();
	if (vkCreateDescriptorSetLayout(m_context->getDevice(), &layout2, nullptr, &m_setLayout2) != VK_SUCCESS)
		throw std::runtime_error("Failed to create set layout 2");

	std::array<VkDescriptorPoolSize, 2> poolSizes{};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[0].descriptorCount = kMaxFramesInFlight;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSizes[1].descriptorCount = 8;

	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	poolInfo.pPoolSizes = poolSizes.data();
	poolInfo.maxSets = kMaxFramesInFlight + 4;
	if (vkCreateDescriptorPool(m_context->getDevice(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
		throw std::runtime_error("Failed to create descriptor pool");

	VkDescriptorSetAllocateInfo alloc1{};
	alloc1.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc1.descriptorPool = m_descriptorPool;
	alloc1.descriptorSetCount = 1;
	alloc1.pSetLayouts = &m_setLayout1;
	if (vkAllocateDescriptorSets(m_context->getDevice(), &alloc1, &m_set1) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate descriptor set 1");

	VkDescriptorSetAllocateInfo alloc2 = alloc1;
	alloc2.pSetLayouts = &m_setLayout2;
	if (vkAllocateDescriptorSets(m_context->getDevice(), &alloc2, &m_set2Water) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate water descriptor set");

	// Opaque layout: set0 + set1
	{
		std::array<VkDescriptorSetLayout, 2> setLayouts = {m_setLayout0, m_setLayout1};
		VkPipelineLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
		layoutInfo.pSetLayouts = setLayouts.data();
		if (vkCreatePipelineLayout(m_context->getDevice(), &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
			throw std::runtime_error("Failed to create pipeline layout");
	}
	// Shadow: push constant mat4
	{
		VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
		VkPipelineLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutInfo.pushConstantRangeCount = 1;
		layoutInfo.pPushConstantRanges = &pcr;
		if (vkCreatePipelineLayout(m_context->getDevice(), &layoutInfo, nullptr, &m_shadowPipelineLayout) != VK_SUCCESS)
			throw std::runtime_error("Failed to create shadow pipeline layout");
	}
	// Water: set0 + set1 + set2, push invScreenSize for gl_FragCoord UV
	{
		std::array<VkDescriptorSetLayout, 3> setLayouts = {m_setLayout0, m_setLayout1, m_setLayout2};
		VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 4};
		VkPipelineLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
		layoutInfo.pSetLayouts = setLayouts.data();
		layoutInfo.pushConstantRangeCount = 1;
		layoutInfo.pPushConstantRanges = &pcr;
		if (vkCreatePipelineLayout(m_context->getDevice(), &layoutInfo, nullptr, &m_waterPipelineLayout) != VK_SUCCESS)
			throw std::runtime_error("Failed to create water pipeline layout");
	}
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

void TerrainRenderer::writeWaterSceneDescriptors()
{
	if (m_set2Water == VK_NULL_HANDLE || m_sceneHistory.view == VK_NULL_HANDLE)
		return;

	VkDescriptorImageInfo colorInfo{};
	colorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	colorInfo.imageView = m_sceneHistory.view;
	colorInfo.sampler = m_sceneSampler;

	// Depth history copy (real depth) — live depth is the attachment during water pass.
	VkDescriptorImageInfo depthInfo{};
	depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	depthInfo.imageView = m_depthHistory.view != VK_NULL_HANDLE ? m_depthHistory.view : m_sceneHistory.view;
	depthInfo.sampler = m_sceneSampler;

	std::array<VkWriteDescriptorSet, 2> writes{};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = m_set2Water;
	writes[0].dstBinding = 0;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[0].descriptorCount = 1;
	writes[0].pImageInfo = &colorInfo;

	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = m_set2Water;
	writes[1].dstBinding = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[1].descriptorCount = 1;
	writes[1].pImageInfo = &depthInfo;

	vkUpdateDescriptorSets(m_context->getDevice(), 2, writes.data(), 0, nullptr);
}

void TerrainRenderer::destroyPipelines()
{
	if (!m_context)
		return;
	auto dPipe = [&](VkPipeline &p) {
		if (p)
			vkDestroyPipeline(m_context->getDevice(), p, nullptr);
		p = VK_NULL_HANDLE;
	};
	dPipe(m_opaquePipeline);
	dPipe(m_waterPipeline);
	dPipe(m_shadowPipeline);
	// Keep layouts alive across swapchain recreate (owned by createDescriptors)
}

void TerrainRenderer::createPipelines(VkSwapchain & /*swapchain*/)
{
	VkShaderModule terrainVert = loadShaderModule(m_context->getDevice(), spvPath("terrain.vert.spv"));
	VkShaderModule terrainFrag = loadShaderModule(m_context->getDevice(), spvPath("terrain.frag.spv"));
	VkShaderModule waterVert = loadShaderModule(m_context->getDevice(), spvPath("water.vert.spv"));
	VkShaderModule waterFrag = loadShaderModule(m_context->getDevice(), spvPath("water.frag.spv"));
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

	// --- Opaque ---
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
		blendAtt.colorWriteMask = 0xF;

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

	// --- Water ---
	{
		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
					 VK_SHADER_STAGE_VERTEX_BIT, waterVert, "main", nullptr};
		stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
					 VK_SHADER_STAGE_FRAGMENT_BIT, waterFrag, "main", nullptr};

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
		blendAtt.colorWriteMask = 0xF;
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
		info.layout = m_waterPipelineLayout;
		if (vkCreateGraphicsPipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, &info, nullptr, &m_waterPipeline) != VK_SUCCESS)
			throw std::runtime_error("Failed to create water pipeline");
	}

	// --- Shadow (depth only, push constant matrix) ---
	{
		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
					 VK_SHADER_STAGE_VERTEX_BIT, shadowVert, "main", nullptr};
		stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
					 VK_SHADER_STAGE_FRAGMENT_BIT, shadowFrag, "main", nullptr};

		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_FRONT_BIT;
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;
		raster.depthBiasEnable = VK_TRUE;
		// Stronger slope-scale bias for coplanar voxel faces (reduces acne without peter-panning as badly as huge constant bias)
		raster.depthBiasConstantFactor = 1.75f;
		raster.depthBiasSlopeFactor = 2.5f;

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
		info.layout = m_shadowPipelineLayout;
		if (vkCreateGraphicsPipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, &info, nullptr, &m_shadowPipeline) != VK_SUCCESS)
			throw std::runtime_error("Failed to create shadow pipeline");
	}

	for (auto m : {terrainVert, terrainFrag, waterVert, waterFrag, shadowVert, shadowFrag})
		destroyShaderModule(m_context->getDevice(), m);
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

void TerrainRenderer::updateFrameUBO(uint32_t frameIndex, const Camera &camera, float aspectW, float aspectH,
									 float farPlane, float time, const ShaderParameters &params,
									 float shadowCascadeFar, bool underwater)
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

	const float nearPlane = 0.1f;
	const float cascadeFar = std::max(shadowCascadeFar, 64.f);
	const float aspect = (aspectH > 1e-5f) ? (aspectW / aspectH) : (16.f / 9.f);
	glm::vec4 splits{};
	// Frustum-slice CSM using real camera basis (not camera-centered spheres)
	tier1::buildCascadeUBOFromFront(camera.getPosition(), camera.getFront(), glm::vec3(0.f, 1.f, 0.f),
									m_lightDir, nearPlane, cascadeFar, aspect, tier1::kDefaultFovYDegrees,
									m_cascadeMatrices, splits, nullptr);

	FrameUBO ubo{};
	ubo.view = camera.getViewMatrix();
	ubo.projection = camera.getProjectionMatrix(aspectW, aspectH, farPlane);
	ubo.cascadeMatrix0 = m_cascadeMatrices[0];
	ubo.cascadeMatrix1 = m_cascadeMatrices[1];
	ubo.cascadeMatrix2 = m_cascadeMatrices[2];
	ubo.viewPos = glm::vec4(camera.getPosition(), 1.0f);
	ubo.lightDirection = glm::vec4(m_lightDir, 0.0f);
	ubo.fogColor = glm::vec4(params.fogColor, 1.0f);
	ubo.fogParams = glm::vec4(params.fogStart, params.fogEnd, params.fogDensity, params.fogHeightFalloff);
	packFrameLightVisual(params, ubo.lightParams, ubo.visualParams);
	ubo.sunDir = glm::vec4(sunDir, 0.0f);
	ubo.moonDir = glm::vec4(moonDir, 0.0f);
	ubo.skyParams = glm::vec4(time, day, sunset, night);
	ubo.cascadeSplits = splits;
	// Cool base color in rgb; strength in w (shader multiplies by nightFactor * w)
	ubo.moonAmbient = glm::vec4(0.22f, 0.30f, 0.48f, params.moonAmbientStrength);
	ubo.tier1Params = glm::vec4(params.blockLightScale, params.emissiveScale, params.fogBaseY,
								underwater ? 1.0f : 0.0f);
	ubo.waterParams = glm::vec4(params.waterWaveStrength, params.waterRefraction,
								params.waterSpecular, params.waterFoamStrength);

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

	if (preRecord)
		preRecord(f.cmd);

	std::array<VkDescriptorSet, 2> sets = {f.descriptorSet0, m_set1};

	// ========== Cascaded shadow passes ==========
	{
		PROFILE_SCOPE("Shadow");
		VkImageMemoryBarrier toDepth{};
		toDepth.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		toDepth.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toDepth.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		toDepth.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDepth.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDepth.image = m_shadowMap.image;
		toDepth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, static_cast<uint32_t>(kCascadeCount)};
		toDepth.srcAccessMask = 0;
		toDepth.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
							 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
							 0, 0, nullptr, 0, nullptr, 1, &toDepth);

		for (int c = 0; c < kCascadeCount; ++c)
		{
			VkRenderingAttachmentInfo depthAtt{};
			depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
			depthAtt.imageView = m_shadowLayerViews[c];
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
			vkCmdPushConstants(f.cmd, m_shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
							   sizeof(glm::mat4), &m_cascadeMatrices[c]);

			for (Chunk *chunk : shadowChunks)
			{
				if (chunk)
					chunk->drawShadow(f.cmd);
			}

			endRendering(f.cmd);
		}

		VkImageMemoryBarrier toSample = toDepth;
		toSample.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		toSample.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		toSample.subresourceRange.layerCount = static_cast<uint32_t>(kCascadeCount);
		vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
							 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
							 0, 0, nullptr, 0, nullptr, 1, &toSample);
	}

	// ========== Opaque HDR scene ==========
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

		m_overlays.record(f.cmd, f.descriptorSet0, chunks);
		endRendering(f.cmd);
	}

	// ========== Copy opaque HDR + depth → history for water ==========
	{
		VkImageMemoryBarrier barriers[4]{};
		// HDR → transfer src
		barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barriers[0].srcQueueFamilyIndex = barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[0].image = m_post.hdrColor().image;
		barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

		// scene history → transfer dst
		barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barriers[1].srcQueueFamilyIndex = barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[1].image = m_sceneHistory.image;
		barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		barriers[1].srcAccessMask = 0;
		barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		// live depth → transfer src
		barriers[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[2].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		barriers[2].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barriers[2].srcQueueFamilyIndex = barriers[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[2].image = m_post.sceneDepth().image;
		barriers[2].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
		barriers[2].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		barriers[2].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

		// depth history → transfer dst
		barriers[3].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[3].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[3].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barriers[3].srcQueueFamilyIndex = barriers[3].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[3].image = m_depthHistory.image;
		barriers[3].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
		barriers[3].srcAccessMask = 0;
		barriers[3].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

		vkCmdPipelineBarrier(f.cmd,
							 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
							 VK_PIPELINE_STAGE_TRANSFER_BIT,
							 0, 0, nullptr, 0, nullptr, 4, barriers);

		VkImageCopy colorCopy{};
		colorCopy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		colorCopy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		colorCopy.extent = {extent.width, extent.height, 1};
		vkCmdCopyImage(f.cmd, m_post.hdrColor().image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					   m_sceneHistory.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &colorCopy);

		VkImageCopy depthCopy{};
		depthCopy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
		depthCopy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
		depthCopy.extent = {extent.width, extent.height, 1};
		vkCmdCopyImage(f.cmd, m_post.sceneDepth().image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					   m_depthHistory.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depthCopy);

		// History → shader read; HDR → color attachment; live depth → attachment for water depth test
		VkImageMemoryBarrier after[4]{};
		after[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		after[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		after[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		after[0].srcQueueFamilyIndex = after[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		after[0].image = m_sceneHistory.image;
		after[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		after[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		after[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		after[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		after[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		after[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		after[1].srcQueueFamilyIndex = after[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		after[1].image = m_depthHistory.image;
		after[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
		after[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		after[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		after[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		after[2].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		after[2].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		after[2].srcQueueFamilyIndex = after[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		after[2].image = m_post.hdrColor().image;
		after[2].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		after[2].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		after[2].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

		after[3].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		after[3].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		after[3].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		after[3].srcQueueFamilyIndex = after[3].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		after[3].image = m_post.sceneDepth().image;
		after[3].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
		after[3].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		after[3].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
							 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
								 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
							 0, 0, nullptr, 0, nullptr, 4, after);
	}

	// ========== Water pass (samples history color + depth history; depth-tests on live depth) ==========
	{
		VkRenderingAttachmentInfo colorAtt{};
		colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		colorAtt.imageView = m_post.hdrColor().view;
		colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

		VkRenderingAttachmentInfo depthAtt{};
		depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		depthAtt.imageView = m_post.sceneDepth().view;
		depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

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

		std::array<VkDescriptorSet, 3> waterSets = {f.descriptorSet0, m_set1, m_set2Water};
		vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterPipelineLayout, 0,
								static_cast<uint32_t>(waterSets.size()), waterSets.data(), 0, nullptr);
		vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterPipeline);
		// invScreenSize for gl_FragCoord UV (framebuffer space matches history images)
		const float invScreen[4] = {
			1.f / static_cast<float>(std::max(1u, extent.width)),
			1.f / static_cast<float>(std::max(1u, extent.height)),
			0.f, 0.f};
		vkCmdPushConstants(f.cmd, m_waterPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
						   sizeof(invScreen), invScreen);

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
		for (const auto &e : waterChunks)
			e.chunk->drawWater(f.cmd);

		endRendering(f.cmd);
	}

	// ========== Sky MRT ==========
	{
		PROFILE_SCOPE("Sky");
		m_post.recordSky(f.cmd, f.descriptorSet0, extent);
	}

	// ========== Post: SSAO / bloom / god rays / composite ==========
	{
		PROFILE_SCOPE("Post");
		const auto *ubo = static_cast<const FrameUBO *>(f.uboMapped);
		glm::vec4 sunClip = ubo->projection * ubo->view * glm::vec4(m_lastCamPos + glm::vec3(ubo->sunDir) * 500.f, 1.f);
		glm::vec2 sunScreen(0.5f, 0.5f);
		float sunVisibility = 0.f;
		if (sunClip.w > 0.f)
		{
			glm::vec3 ndc = glm::vec3(sunClip) / sunClip.w;
			sunScreen = glm::vec2(ndc.x * 0.5f + 0.5f, ndc.y * 0.5f + 0.5f);
			sunVisibility = glm::smoothstep(-0.10f, 0.04f, ubo->sunDir.y);
			if (sunScreen.x < 0.f || sunScreen.x > 1.f || sunScreen.y < 0.f || sunScreen.y > 1.f)
				sunVisibility = 0.f;
		}

		const bool underwater = ubo->tier1Params.w > 0.5f;
		m_postSettings.underwater = underwater;
		m_post.recordPost(f.cmd, swapchain.getImages()[imageIndex], swapchain.getImageViews()[imageIndex],
						  extent, f.descriptorSet0, m_postSettings, sunScreen, sunVisibility, m_time,
						  ubo->projection);
	}

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

	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = &f.imageAvailable;
	submit.pWaitDstStageMask = &waitStage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &f.cmd;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &f.renderFinished;

	if (vkQueueSubmit(m_context->getGraphicsQueue(), 1, &submit, f.inFlight) != VK_SUCCESS)
		throw std::runtime_error("Failed to submit terrain command buffer");

	VkPresentInfoKHR present{};
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &f.renderFinished;
	VkSwapchainKHR sc = swapchain.getSwapchain();
	present.swapchainCount = 1;
	present.pSwapchains = &sc;
	present.pImageIndices = &imageIndex;

	VkResult result = vkQueuePresentKHR(m_context->getPresentQueue(), &present);
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
		return false;
	if (result != VK_SUCCESS)
		throw std::runtime_error("Failed to present");
	return true;
}
