#include "Renderer/PostStack.hpp"
#include "Vulkan/VkShader.hpp"
#include "Vulkan/VkUpload.hpp"
#include "utils.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <algorithm>

namespace
{
std::string spvPath(const char *name)
{
	const char *prefixes[] = {"./ressources/shaders/spv/", "ressources/shaders/spv/", "../ressources/shaders/spv/"};
	for (const char *p : prefixes)
	{
		std::string path = std::string(p) + name;
		std::ifstream f(path, std::ios::binary);
		if (f)
			return path;
	}
	return std::string(RES_PATH) + "shaders/spv/" + name;
}
auto beginR() { return vkCmdBeginRendering ? vkCmdBeginRendering : vkCmdBeginRenderingKHR; }
auto endR() { return vkCmdEndRendering ? vkCmdEndRendering : vkCmdEndRenderingKHR; }

struct ExtractPC
{
	float bloomThreshold;
	float pad[3]{};
};
struct BlurPC
{
	glm::vec4 data; // xy=texelSize, z=horizontal
};
struct GodPC
{
	glm::vec4 p0; // xy=sun, z=density, w=weight
	glm::vec4 p1; // decay, exposure, sunVis, time
	glm::vec4 p2; // dramaticBoost, dynamicBoost, boostPreview, pad
};
struct CompPC
{
	glm::vec4 p0; // exposure, bloomIntensity, gamma, toneMapper
	glm::vec4 p1; // bloomOn, fxaaOn, godRaysOn, postSaturation
	glm::vec4 p2; // texelSize.xy, postContrast, pad
};
} // namespace

PostStack::~PostStack() { shutdown(); }

void PostStack::shutdown()
{
	if (!m_context)
		return;
	m_context->waitIdle();
	destroyPipelines();
	destroyTargets();
	if (m_postPool)
		vkDestroyDescriptorPool(m_context->getDevice(), m_postPool, nullptr);
	if (m_postSetLayout)
		vkDestroyDescriptorSetLayout(m_context->getDevice(), m_postSetLayout, nullptr);
	if (m_compositeSetLayout)
		vkDestroyDescriptorSetLayout(m_context->getDevice(), m_compositeSetLayout, nullptr);
	if (m_linearSampler)
		vkDestroySampler(m_context->getDevice(), m_linearSampler, nullptr);
	if (m_quadVBO.buffer)
		destroyBuffer(m_context->getAllocator(), m_quadVBO);
	if (m_skyVBO.buffer)
		destroyBuffer(m_context->getAllocator(), m_skyVBO);
	m_postPool = VK_NULL_HANDLE;
	m_postSetLayout = m_compositeSetLayout = VK_NULL_HANDLE;
	m_linearSampler = VK_NULL_HANDLE;
	m_context = nullptr;
}

void PostStack::createSamplers()
{
	VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	si.magFilter = si.minFilter = VK_FILTER_LINEAR;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	if (vkCreateSampler(m_context->getDevice(), &si, nullptr, &m_linearSampler) != VK_SUCCESS)
		throw std::runtime_error("post sampler failed");
}

void PostStack::createFullscreenQuad(ImmediateCommands &imm)
{
	const float verts[] = {
		-1, -1, 0, 0,  1, -1, 1, 0,  -1, 1, 0, 1,
		 1, -1, 1, 0,  1,  1, 1, 1,  -1, 1, 0, 1,
	};
	m_quadVBO = createBuffer(m_context->getAllocator(), sizeof(verts),
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
	uploadBuffer(m_context->getAllocator(), imm, m_quadVBO, verts, sizeof(verts));
}

void PostStack::createSkyGeometry(ImmediateCommands &imm)
{
	// Unit cube (36 verts) — positions only
	float v[] = {
		-1,-1,-1,  1,-1,-1,  1, 1,-1,  1, 1,-1, -1, 1,-1, -1,-1,-1,
		-1,-1, 1,  1,-1, 1,  1, 1, 1,  1, 1, 1, -1, 1, 1, -1,-1, 1,
		-1, 1,-1,  1, 1,-1,  1, 1, 1,  1, 1, 1, -1, 1, 1, -1, 1,-1,
		-1,-1,-1,  1,-1,-1,  1,-1, 1,  1,-1, 1, -1,-1, 1, -1,-1,-1,
		 1,-1,-1,  1, 1,-1,  1, 1, 1,  1, 1, 1,  1,-1, 1,  1,-1,-1,
		-1,-1,-1, -1, 1,-1, -1, 1, 1, -1, 1, 1, -1,-1, 1, -1,-1,-1,
	};
	m_skyVBO = createBuffer(m_context->getAllocator(), sizeof(v),
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
	uploadBuffer(m_context->getAllocator(), imm, m_skyVBO, v, sizeof(v));
}

void PostStack::createTargets(uint32_t w, uint32_t h)
{
	m_width = w;
	m_height = h;
	const uint32_t hw = std::max(1u, w / 2);
	const uint32_t hh = std::max(1u, h / 2);

	auto makeColor = [&](uint32_t W, uint32_t H, VkFormat fmt) {
		return createImage2D(m_context->getAllocator(), m_context->getDevice(), W, H, fmt,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
	};

	m_hdr = makeColor(w, h, m_hdrFormat);
	m_godSource = makeColor(w, h, m_hdrFormat);
	m_sceneDepth = createImage2D(m_context->getAllocator(), m_context->getDevice(), w, h, m_depthFormat,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
		1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
	m_bloom[0] = makeColor(hw, hh, m_hdrFormat);
	m_bloom[1] = makeColor(hw, hh, m_hdrFormat);
	m_godRays = makeColor(hw, hh, m_hdrFormat);

	// Update descriptor sets
	auto write1 = [&](VkDescriptorSet set, VkImageView view) {
		VkDescriptorImageInfo ii{m_linearSampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = set;
		w.dstBinding = 0;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w.pImageInfo = &ii;
		vkUpdateDescriptorSets(m_context->getDevice(), 1, &w, 0, nullptr);
	};
	write1(m_setExtract, m_hdr.view);
	// Fixed blur sets: set i always samples bloom[i] (no mid-frame updates).
	write1(m_setBlur[0], m_bloom[0].view);
	write1(m_setBlur[1], m_bloom[1].view);
	write1(m_setGodRays, m_godSource.view);

	// Final bloom lives in bloom[0] after 10 ping-pong passes (even count).
	VkDescriptorImageInfo imgs[3] = {
		{m_linearSampler, m_hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		{m_linearSampler, m_bloom[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		{m_linearSampler, m_godRays.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	};
	std::array<VkWriteDescriptorSet, 3> ws{};
	for (int i = 0; i < 3; ++i)
	{
		ws[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		ws[i].dstSet = m_setComposite;
		ws[i].dstBinding = static_cast<uint32_t>(i);
		ws[i].descriptorCount = 1;
		ws[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		ws[i].pImageInfo = &imgs[i];
	}
	vkUpdateDescriptorSets(m_context->getDevice(), 3, ws.data(), 0, nullptr);
}

void PostStack::destroyTargets()
{
	if (!m_context)
		return;
	destroyImage(m_context->getAllocator(), m_context->getDevice(), m_hdr);
	destroyImage(m_context->getAllocator(), m_context->getDevice(), m_godSource);
	destroyImage(m_context->getAllocator(), m_context->getDevice(), m_sceneDepth);
	destroyImage(m_context->getAllocator(), m_context->getDevice(), m_bloom[0]);
	destroyImage(m_context->getAllocator(), m_context->getDevice(), m_bloom[1]);
	destroyImage(m_context->getAllocator(), m_context->getDevice(), m_godRays);
}

void PostStack::createPipelines(VkFormat swapchainFormat)
{
	m_swapchainFormat = swapchainFormat;
	// set layouts + pool if needed
	if (!m_postSetLayout)
	{
		VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		li.bindingCount = 1;
		li.pBindings = &b;
		vkCreateDescriptorSetLayout(m_context->getDevice(), &li, nullptr, &m_postSetLayout);

		std::array<VkDescriptorSetLayoutBinding, 3> cb{};
		for (int i = 0; i < 3; ++i)
			cb[i] = {static_cast<uint32_t>(i), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		li.bindingCount = 3;
		li.pBindings = cb.data();
		vkCreateDescriptorSetLayout(m_context->getDevice(), &li, nullptr, &m_compositeSetLayout);

		std::array<VkDescriptorPoolSize, 1> ps{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}}};
		VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		pi.poolSizeCount = 1;
		pi.pPoolSizes = ps.data();
		pi.maxSets = 16;
		vkCreateDescriptorPool(m_context->getDevice(), &pi, nullptr, &m_postPool);

		auto alloc = [&](VkDescriptorSetLayout lay, VkDescriptorSet &out) {
			VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			ai.descriptorPool = m_postPool;
			ai.descriptorSetCount = 1;
			ai.pSetLayouts = &lay;
			vkAllocateDescriptorSets(m_context->getDevice(), &ai, &out);
		};
		alloc(m_postSetLayout, m_setExtract);
		alloc(m_postSetLayout, m_setBlur[0]);
		alloc(m_postSetLayout, m_setBlur[1]);
		alloc(m_postSetLayout, m_setGodRays);
		alloc(m_compositeSetLayout, m_setComposite);

		// post layout: set0 = one sampler, push constants
		VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
		VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		pl.setLayoutCount = 1;
		pl.pSetLayouts = &m_postSetLayout;
		pl.pushConstantRangeCount = 1;
		pl.pPushConstantRanges = &pcr;
		vkCreatePipelineLayout(m_context->getDevice(), &pl, nullptr, &m_postLayout1);

		pl.pSetLayouts = &m_compositeSetLayout;
		vkCreatePipelineLayout(m_context->getDevice(), &pl, nullptr, &m_compositeLayout);

		// sky: frame set only
		pl.pSetLayouts = &m_frameSetLayout;
		pl.pushConstantRangeCount = 0;
		vkCreatePipelineLayout(m_context->getDevice(), &pl, nullptr, &m_skyLayout);
	}

	auto load = [&](const char *n) { return loadShaderModule(m_context->getDevice(), spvPath(n)); };
	VkShaderModule fsVert = load("fullscreen.vert.spv");
	VkShaderModule extractF = load("bloomExtract.frag.spv");
	VkShaderModule blurF = load("bloomBlur.frag.spv");
	VkShaderModule godF = load("godRays.frag.spv");
	VkShaderModule compF = load("composite.frag.spv");
	VkShaderModule skyV = load("skybox.vert.spv");
	VkShaderModule skyF = load("skybox.frag.spv");

	VkVertexInputBindingDescription bind{0, 4 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
	std::array<VkVertexInputAttributeDescription, 2> attrs = {{
		{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
		{1, 0, VK_FORMAT_R32G32_SFLOAT, 8},
	}};
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &bind;
	vi.vertexAttributeDescriptionCount = 2;
	vi.pVertexAttributeDescriptions = attrs.data();

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
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = VK_FALSE;
	VkPipelineColorBlendAttachmentState ba{};
	ba.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &ba;
	std::array<VkDynamicState, 2> dynS = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dyn.dynamicStateCount = 2;
	dyn.pDynamicStates = dynS.data();

	auto makeFS = [&](VkShaderModule frag, VkPipelineLayout layout, VkFormat colorFmt, VkPipeline &out) {
		VkPipelineShaderStageCreateInfo stages[2] = {
			{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, fsVert, "main", nullptr},
			{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
		};
		VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		ri.colorAttachmentCount = 1;
		ri.pColorAttachmentFormats = &colorFmt;
		VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gi.pNext = &ri;
		gi.stageCount = 2;
		gi.pStages = stages;
		gi.pVertexInputState = &vi;
		gi.pInputAssemblyState = &ia;
		gi.pViewportState = &vp;
		gi.pRasterizationState = &rs;
		gi.pMultisampleState = &ms;
		gi.pDepthStencilState = &ds;
		gi.pColorBlendState = &cb;
		gi.pDynamicState = &dyn;
		gi.layout = layout;
		if (vkCreateGraphicsPipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, &gi, nullptr, &out) != VK_SUCCESS)
			throw std::runtime_error("post pipeline failed");
	};

	makeFS(extractF, m_postLayout1, m_hdrFormat, m_extractPipe);
	makeFS(blurF, m_postLayout1, m_hdrFormat, m_blurPipe);
	makeFS(godF, m_postLayout1, m_hdrFormat, m_godRaysPipe);
	makeFS(compF, m_compositeLayout, swapchainFormat, m_compositePipe);

	// Sky pipeline
	{
		VkVertexInputBindingDescription sb{0, sizeof(float) * 3, VK_VERTEX_INPUT_RATE_VERTEX};
		VkVertexInputAttributeDescription sa{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
		VkPipelineVertexInputStateCreateInfo svi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		svi.vertexBindingDescriptionCount = 1;
		svi.pVertexBindingDescriptions = &sb;
		svi.vertexAttributeDescriptionCount = 1;
		svi.pVertexAttributeDescriptions = &sa;

		VkPipelineDepthStencilStateCreateInfo sds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
		sds.depthTestEnable = VK_TRUE;
		sds.depthWriteEnable = VK_FALSE;
		sds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

		VkPipelineColorBlendAttachmentState sba[2]{};
		sba[0].colorWriteMask = sba[1].colorWriteMask = 0xF;
		VkPipelineColorBlendStateCreateInfo scb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		scb.attachmentCount = 2;
		scb.pAttachments = sba;

		VkPipelineShaderStageCreateInfo stages[2] = {
			{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, skyV, "main", nullptr},
			{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, skyF, "main", nullptr},
		};
		std::array<VkFormat, 2> cols = {m_hdrFormat, m_hdrFormat};
		VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
		ri.colorAttachmentCount = 2;
		ri.pColorAttachmentFormats = cols.data();
		ri.depthAttachmentFormat = m_depthFormat;

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
		gi.layout = m_skyLayout;
		if (vkCreateGraphicsPipelines(m_context->getDevice(), VK_NULL_HANDLE, 1, &gi, nullptr, &m_skyPipeline) != VK_SUCCESS)
			throw std::runtime_error("sky pipeline failed");
	}

	for (auto m : {fsVert, extractF, blurF, godF, compF, skyV, skyF})
		destroyShaderModule(m_context->getDevice(), m);
}

void PostStack::destroyPipelines()
{
	if (!m_context)
		return;
	auto d = [&](VkPipeline &p) {
		if (p)
			vkDestroyPipeline(m_context->getDevice(), p, nullptr);
		p = VK_NULL_HANDLE;
	};
	d(m_extractPipe);
	d(m_blurPipe);
	d(m_godRaysPipe);
	d(m_compositePipe);
	d(m_skyPipeline);
	if (m_postLayout1)
		vkDestroyPipelineLayout(m_context->getDevice(), m_postLayout1, nullptr);
	if (m_compositeLayout)
		vkDestroyPipelineLayout(m_context->getDevice(), m_compositeLayout, nullptr);
	if (m_skyLayout)
		vkDestroyPipelineLayout(m_context->getDevice(), m_skyLayout, nullptr);
	m_postLayout1 = m_compositeLayout = m_skyLayout = VK_NULL_HANDLE;
}

void PostStack::init(VkContext &context, ImmediateCommands &imm, VkDescriptorSetLayout frameSetLayout,
					 VkFormat swapchainFormat, uint32_t width, uint32_t height)
{
	m_context = &context;
	m_frameSetLayout = frameSetLayout;
	createSamplers();
	createFullscreenQuad(imm);
	createSkyGeometry(imm);
	createPipelines(swapchainFormat);
	createTargets(width, height);
}

void PostStack::resize(uint32_t width, uint32_t height, VkFormat swapchainFormat)
{
	if (!m_context || (width == m_width && height == m_height && swapchainFormat == m_swapchainFormat))
		return;
	m_context->waitIdle();
	destroyTargets();
	if (swapchainFormat != m_swapchainFormat)
	{
		destroyPipelines();
		createPipelines(swapchainFormat);
	}
	createTargets(width, height);
}

void PostStack::transitionColor(VkCommandBuffer cmd, VkImage image, VkImageLayout oldL, VkImageLayout newL,
								VkAccessFlags srcA, VkAccessFlags dstA,
								VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
{
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.oldLayout = oldL;
	b.newLayout = newL;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	b.srcAccessMask = srcA;
	b.dstAccessMask = dstA;
	vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void PostStack::recordSky(VkCommandBuffer cmd, VkDescriptorSet frameSet0, VkExtent2D extent)
{
	// Expects HDR color already in COLOR_ATTACHMENT with scene loaded, god source may need clear,
	// depth loaded. Caller sets up dual-attachment rendering OR we begin our own pass.
	// This method begins a dual-attachment pass with LOAD on HDR, CLEAR on god, LOAD depth.
	const auto beginRendering = beginR();
	const auto endRendering = endR();

	// Transition god source undefined -> color
	transitionColor(cmd, m_godSource.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
					VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

	VkRenderingAttachmentInfo cols[2]{};
	cols[0].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	cols[0].imageView = m_hdr.view;
	cols[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	cols[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	cols[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;

	cols[1].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	cols[1].imageView = m_godSource.view;
	cols[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	cols[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	cols[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	cols[1].clearValue.color = {{0, 0, 0, 0}};

	VkRenderingAttachmentInfo depth{};
	depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	depth.imageView = m_sceneDepth.view;
	depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

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

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyLayout, 0, 1, &frameSet0, 0, nullptr);
	VkDeviceSize off = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_skyVBO.buffer, &off);
	vkCmdDraw(cmd, 36, 1, 0, 0);

	endRendering(cmd);
}

void PostStack::recordPost(VkCommandBuffer cmd, VkImage swapchainImage, VkImageView swapchainView,
						   VkExtent2D extent, VkDescriptorSet /*frameSet0*/,
						   const PostProcessSettings &settings, const glm::vec2 &sunScreen,
						   float sunVisibility, float time)
{
	const auto beginRendering = beginR();
	const auto endRendering = endR();
	const uint32_t hw = std::max(1u, extent.width / 2);
	const uint32_t hh = std::max(1u, extent.height / 2);
	const VkExtent2D half{hw, hh};

	// HDR + god source → shader read
	transitionColor(cmd, m_hdr.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	transitionColor(cmd, m_godSource.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	auto fsDraw = [&](VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSet set, VkImageView outView,
					  VkExtent2D outExt, const void *pc, uint32_t pcSize) {
		// Caller must transition target image to COLOR_ATTACHMENT before calling.
		VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		ca.imageView = outView;
		ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		ca.clearValue.color = {{0, 0, 0, 0}};
		VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
		ri.renderArea = {{0, 0}, outExt};
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &ca;
		beginRendering(cmd, &ri);
		VkViewport vp{0, 0, (float)outExt.width, (float)outExt.height, 0, 1};
		VkRect2D sc{{0, 0}, outExt};
		vkCmdSetViewport(cmd, 0, 1, &vp);
		vkCmdSetScissor(cmd, 0, 1, &sc);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
		if (pc && pcSize)
			vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcSize, pc);
		VkDeviceSize off = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &m_quadVBO.buffer, &off);
		vkCmdDraw(cmd, 6, 1, 0, 0);
		endRendering(cmd);
	};

	// Bloom extract → bloom[0]
	transitionColor(cmd, m_bloom[0].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	ExtractPC epc{};
	epc.bloomThreshold = settings.bloomThreshold;
	fsDraw(m_extractPipe, m_postLayout1, m_setExtract, m_bloom[0].view, half, &epc, sizeof(epc));
	transitionColor(cmd, m_bloom[0].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	// Blur ping-pong: fixed descriptor sets (no mid-pass vkUpdateDescriptorSets — illegal while recorded).
	// m_setBlur[i] permanently samples m_bloom[i].
	int readIdx = 0;
	for (int i = 0; i < 5; ++i)
	{
		for (int horizontal = 1; horizontal >= 0; --horizontal)
		{
			const int writeIdx = 1 - readIdx;
			transitionColor(cmd, m_bloom[writeIdx].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

			BlurPC bpc{};
			bpc.data = glm::vec4(1.f / static_cast<float>(hw), 1.f / static_cast<float>(hh),
								 horizontal ? 1.f : 0.f, 0.f);
			fsDraw(m_blurPipe, m_postLayout1, m_setBlur[readIdx], m_bloom[writeIdx].view, half, &bpc, sizeof(bpc));
			transitionColor(cmd, m_bloom[writeIdx].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
							VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
							VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
			readIdx = writeIdx;
		}
	}
	// 10 swaps → final bloom in bloom[0]. Composite binding 1 is fixed to bloom[0] in createTargets.

	// God rays
	transitionColor(cmd, m_godRays.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	GodPC gpc{};
	gpc.p0 = glm::vec4(sunScreen.x, sunScreen.y, settings.godRaysDensity, settings.godRaysWeight);
	gpc.p1 = glm::vec4(settings.godRaysDecay, settings.godRaysExposure, sunVisibility, time);
	gpc.p2 = glm::vec4(settings.godRaysDramaticBoost,
					   settings.godRaysDynamicBoostEnabled ? 1.f : 0.f,
					   settings.godRaysBoostPreview ? 1.f : 0.f, 0.f);
	fsDraw(m_godRaysPipe, m_postLayout1, m_setGodRays, m_godRays.view, half, &gpc, sizeof(gpc));
	transitionColor(cmd, m_godRays.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	// Composite → swapchain
	transitionColor(cmd, swapchainImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	CompPC cpc{};
	cpc.p0 = glm::vec4(settings.exposure, settings.bloomIntensity, settings.gamma,
					   static_cast<float>(settings.toneMapper));
	cpc.p1 = glm::vec4(settings.bloomEnabled ? 1.f : 0.f,
					   settings.fxaaEnabled ? 1.f : 0.f,
					   settings.godRaysEnabled ? 1.f : 0.f,
					   settings.postSaturation);
	cpc.p2 = glm::vec4(1.f / static_cast<float>(extent.width),
					   1.f / static_cast<float>(extent.height),
					   settings.postContrast, 0.f);
	fsDraw(m_compositePipe, m_compositeLayout, m_setComposite, swapchainView, extent, &cpc, sizeof(cpc));
}
