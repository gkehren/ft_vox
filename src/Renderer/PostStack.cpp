#include "Renderer/PostStack.hpp"
#include "Renderer/PostDefaults.hpp"
#include "Renderer/Lighting.hpp"
#include "Renderer/ColorSpace.hpp"
#include "Vulkan/ImageBarrier.hpp"
#include "Vulkan/VkShader.hpp"
#include "Vulkan/VkUpload.hpp"
#include "utils.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <array>
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace
{
std::string spvPath(const char *name) { return resolveSpvPath(name); }
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
	glm::vec4 p0;
	glm::vec4 p1;
	glm::vec4 p2;
};
struct SsaoPC
{
	glm::vec4 p0; // xy = inv full-res resolution, z = radius (view meters), w = unused
	glm::vec4 p1; // x = directions (4..8), y = steps (1..4), zw unused
	glm::mat4 invProj;
};
struct UpsamplePC
{
	glm::vec4 p0; // xy = inv full-res resolution, zw unused
	glm::mat4 invProj;
};
struct CompPC
{
	glm::vec4 p0;
	glm::vec4 p1;
	glm::vec4 p2;
	glm::vec4 p3;
	glm::vec4 p4; // x=filmGrain, y=vignette, z=encodeSrgb, w=ssaoDebugView (0=off,1=final,2=raw,3=normals)
	glm::vec4 p5; // x=useAutoExposure, yzw unused
};
struct DownPC
{
	glm::vec4 stepUV; // xy = 1/destination size: luminance tap spread in UV
	glm::vec4 params; // x = sourceIsLog (stage 0 samples HDR, later stages .r)
};
struct AdaptPC
{
	glm::vec4 p0; // x=dt, y=speedUp, z=speedDown, w=useSeed
	glm::vec4 p1; // x=seedExposure, y=middleGrey, z=compensationEv, w=minEv
	glm::vec4 p2; // x=maxEv
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
	destroyExposureBuffers();
	if (m_postPool)
		vkDestroyDescriptorPool(m_context->getDevice(), m_postPool, nullptr);
	if (m_postSetLayout)
		vkDestroyDescriptorSetLayout(m_context->getDevice(), m_postSetLayout, nullptr);
	if (m_godSetLayout)
		vkDestroyDescriptorSetLayout(m_context->getDevice(), m_godSetLayout, nullptr);
	if (m_compositeSetLayout)
		vkDestroyDescriptorSetLayout(m_context->getDevice(), m_compositeSetLayout, nullptr);
	if (m_exposureSetLayout)
		vkDestroyDescriptorSetLayout(m_context->getDevice(), m_exposureSetLayout, nullptr);
	if (m_ssaoUpLayout)
		vkDestroyDescriptorSetLayout(m_context->getDevice(), m_ssaoUpLayout, nullptr);
	if (m_linearSampler)
		vkDestroySampler(m_context->getDevice(), m_linearSampler, nullptr);
	if (m_nearestSampler)
		vkDestroySampler(m_context->getDevice(), m_nearestSampler, nullptr);
	if (m_quadVBO.buffer)
		destroyBuffer(m_context->getAllocator(), m_quadVBO);
	destroyDefaultImages();
	m_postPool = VK_NULL_HANDLE;
	m_postSetLayout = m_godSetLayout = m_compositeSetLayout = m_ssaoUpLayout = VK_NULL_HANDLE;
	m_exposureSetLayout = VK_NULL_HANDLE;
	m_linearSampler = m_nearestSampler = VK_NULL_HANDLE;
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

	si.magFilter = si.minFilter = VK_FILTER_NEAREST;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	if (vkCreateSampler(m_context->getDevice(), &si, nullptr, &m_nearestSampler) != VK_SUCCESS)
		throw std::runtime_error("post nearest sampler failed");
}

void PostStack::createFullscreenQuad(ImmediateCommands &imm)
{
	const float verts[] = {
		-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1,
		1, -1, 1, 0, 1, 1, 1, 1, -1, 1, 0, 1,
	};
	m_quadVBO = createBuffer(m_context->getAllocator(), sizeof(verts),
							 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
							 VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
	uploadBuffer(m_context->getAllocator(), imm, m_quadVBO, verts, sizeof(verts));
}


void PostStack::createTargets(uint32_t w, uint32_t h)
{
	m_width = w;
	m_height = h;
	const uint32_t hw = std::max(1u, w / 2);
	const uint32_t hh = std::max(1u, h / 2);

	auto makeColor = [&](uint32_t W, uint32_t H, VkFormat fmt, VkImageUsageFlags extra = 0) {
		return createImage2D(m_context->getAllocator(), m_context->getDevice(), W, H, fmt,
							 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra,
							 VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
	};

	m_hdr = makeColor(w, h, m_hdrFormat, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	m_godSource = makeColor(w, h, m_hdrFormat);
	m_sceneDepth = createImage2D(m_context->getAllocator(), m_context->getDevice(), w, h, m_depthFormat,
								 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
									 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
								 VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
	m_bloom[0] = makeColor(hw, hh, m_hdrFormat);
	m_bloom[1] = makeColor(hw, hh, m_hdrFormat);
	m_godRays = makeColor(hw, hh, m_hdrFormat);
	m_ssao = makeColor(hw, hh, VK_FORMAT_R8G8B8A8_UNORM); // r = AO, gb = encoded normal
	m_ssaoUp = makeColor(w, h, VK_FORMAT_R8_UNORM);		  // bilateral-upsampled final AO

	// Auto-exposure metering chain (issue #140). Small R32F log-luminance
	// targets; m_lum1 carries the adapted exposure for tooling readback. The
	// chain resolution is fixed, so a swapchain resize does not disturb the
	// adaptation (the history buffer also persists — see createExposureBuffers).
	auto makeLum = [&](uint32_t W, uint32_t H) {
		return createImage2D(m_context->getAllocator(), m_context->getDevice(), W, H,
							 VK_FORMAT_R32_SFLOAT,
							 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
								 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
							 VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
	};
	m_lum[0] = makeLum(64, 64);
	m_lum[1] = makeLum(16, 16);
	m_lum[2] = makeLum(4, 4);
	m_lum1 = makeLum(1, 1);

	auto write1 = [&](VkDescriptorSet set, VkImageView view, VkSampler samp = VK_NULL_HANDLE) {
		VkDescriptorImageInfo ii{samp ? samp : m_linearSampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = set;
		w.dstBinding = 0;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w.pImageInfo = &ii;
		vkUpdateDescriptorSets(m_context->getDevice(), 1, &w, 0, nullptr);
	};
	write1(m_setExtract, m_hdr.view);
	write1(m_setBlur[0], m_bloom[0].view);
	write1(m_setBlur[1], m_bloom[1].view);
	write1(m_setSsao, m_sceneDepth.view, m_nearestSampler);
	// R32F metering chain: NEAREST — linear filtering of 32-bit float formats
	// is an optional format feature, while nearest always works. Stage taps
	// land on spread texel centers, so point sampling is fine for metering.
	write1(m_setLumSrc[0], m_lum[0].view, m_nearestSampler);
	write1(m_setLumSrc[1], m_lum[1].view, m_nearestSampler);

	// ssaoUpsample: b0 half-res AO+normals (linear), b1 full-res depth (nearest)
	{
		VkDescriptorImageInfo imgs[2] = {
			{m_linearSampler, m_ssao.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{m_nearestSampler, m_sceneDepth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		};
		std::array<VkWriteDescriptorSet, 2> ws{};
		for (int i = 0; i < 2; ++i)
		{
			ws[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			ws[i].dstSet = m_setSsaoUp;
			ws[i].dstBinding = static_cast<uint32_t>(i);
			ws[i].descriptorCount = 1;
			ws[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			ws[i].pImageInfo = &imgs[i];
		}
		vkUpdateDescriptorSets(m_context->getDevice(), 2, ws.data(), 0, nullptr);
	}

	{
		VkDescriptorImageInfo imgs[2] = {
			{m_linearSampler, m_godSource.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			{m_nearestSampler, m_sceneDepth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		};
		std::array<VkWriteDescriptorSet, 2> ws{};
		for (int i = 0; i < 2; ++i)
		{
			ws[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			ws[i].dstSet = m_setGodRays;
			ws[i].dstBinding = static_cast<uint32_t>(i);
			ws[i].descriptorCount = 1;
			ws[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			ws[i].pImageInfo = &imgs[i];
		}
		vkUpdateDescriptorSets(m_context->getDevice(), 2, ws.data(), 0, nullptr);
	}

	// Adaptation sets: m_lum[2] sampler + shared history SSBO + per-slot
	// snapshot SSBO. Buffers persist across resize (created once in init).
	writeExposureDescriptors();

	// Every frame-in-flight owns its composite set, so descriptor writes occur
	// only after that frame's fence has been waited.
	for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
		writeCompositeDescriptors(postCompositeSources(false, false, false),
								  frame);
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
	destroyImage(m_context->getAllocator(), m_context->getDevice(), m_ssao);
	destroyImage(m_context->getAllocator(), m_context->getDevice(), m_ssaoUp);
	destroyImage(m_context->getAllocator(), m_context->getDevice(), m_lum[0]);
	destroyImage(m_context->getAllocator(), m_context->getDevice(), m_lum[1]);
	destroyImage(m_context->getAllocator(), m_context->getDevice(), m_lum[2]);
	destroyImage(m_context->getAllocator(), m_context->getDevice(), m_lum1);
}

void PostStack::createPipelines(VkFormat swapchainFormat, VkColorSpaceKHR swapchainColorSpace)
{
	m_swapchainFormat = swapchainFormat;
	m_swapchainColorSpace = swapchainColorSpace;
	m_outputTransfer = colorspace::classifyOutputTransfer(swapchainFormat, swapchainColorSpace);
	if (m_outputTransfer == colorspace::OutputTransfer::Unsupported)
		throw std::runtime_error(
			"PostStack: unsupported swapchain color space for the SDR sRGB pipeline "
			"(expected SRGB_NONLINEAR with an sRGB or UNORM 8-bit format)");
	m_swapchainRequiresSrgbEncode =
		colorspace::outputTransferRequiresShaderEncode(m_outputTransfer);
	if (!m_postSetLayout)
	{
		VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		li.bindingCount = 1;
		li.pBindings = &b;
		vkCreateDescriptorSetLayout(m_context->getDevice(), &li, nullptr, &m_postSetLayout);

		std::array<VkDescriptorSetLayoutBinding, 2> gb{};
		gb[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		gb[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		li.bindingCount = 2;
		li.pBindings = gb.data();
		vkCreateDescriptorSetLayout(m_context->getDevice(), &li, nullptr, &m_godSetLayout);

		std::array<VkDescriptorSetLayoutBinding, 6> cb{};
		for (int i = 0; i < 5; ++i)
			cb[i] = {static_cast<uint32_t>(i), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		cb[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}; // auto-exposure history (issue #140)
		li.bindingCount = 6;
		li.pBindings = cb.data();
		vkCreateDescriptorSetLayout(m_context->getDevice(), &li, nullptr, &m_compositeSetLayout);

		// Exposure adaptation (issue #140): 4x4 log-lum sampler + shared
		// history SSBO + per-frame-in-flight snapshot SSBO (CPU readout).
		{
			std::array<VkDescriptorSetLayoutBinding, 3> eb{};
			eb[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
			eb[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
			eb[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
			li.bindingCount = 3;
			li.pBindings = eb.data();
			vkCreateDescriptorSetLayout(m_context->getDevice(), &li, nullptr, &m_exposureSetLayout);
		}

		std::array<VkDescriptorSetLayoutBinding, 2> ub{};
		ub[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		ub[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		li.bindingCount = 2;
		li.pBindings = ub.data();
		vkCreateDescriptorSetLayout(m_context->getDevice(), &li, nullptr, &m_ssaoUpLayout);

		std::array<VkDescriptorPoolSize, 2> ps{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32},
												{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6}}};
		VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		pi.poolSizeCount = 2;
		pi.pPoolSizes = ps.data();
		pi.maxSets = 20;
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
		alloc(m_postSetLayout, m_setSsao);
		alloc(m_ssaoUpLayout, m_setSsaoUp);
		alloc(m_postSetLayout, m_setLumSrc[0]);
		alloc(m_postSetLayout, m_setLumSrc[1]);
		alloc(m_godSetLayout, m_setGodRays);
		for (VkDescriptorSet &set : m_setComposite)
			alloc(m_compositeSetLayout, set);
		for (VkDescriptorSet &set : m_exposureSets)
			alloc(m_exposureSetLayout, set);

		VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
		VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		pl.setLayoutCount = 1;
		pl.pSetLayouts = &m_postSetLayout;
		pl.pushConstantRangeCount = 1;
		pl.pPushConstantRanges = &pcr;
		vkCreatePipelineLayout(m_context->getDevice(), &pl, nullptr, &m_postLayout1);
		vkCreatePipelineLayout(m_context->getDevice(), &pl, nullptr, &m_ssaoLayout);

		pl.pSetLayouts = &m_godSetLayout;
		vkCreatePipelineLayout(m_context->getDevice(), &pl, nullptr, &m_godLayout);

		pl.pSetLayouts = &m_ssaoUpLayout;
		vkCreatePipelineLayout(m_context->getDevice(), &pl, nullptr, &m_ssaoUpPipeLayout);

		pl.pSetLayouts = &m_compositeSetLayout;
		vkCreatePipelineLayout(m_context->getDevice(), &pl, nullptr, &m_compositeLayout);

		pl.pSetLayouts = &m_exposureSetLayout;
		vkCreatePipelineLayout(m_context->getDevice(), &pl, nullptr, &m_exposureLayout);
	}

	auto load = [&](const char *n) { return loadShaderModule(m_context->getDevice(), spvPath(n)); };
	VkShaderModule fsVert = load("fullscreen.vert.spv");
	VkShaderModule extractF = load("bloomExtract.frag.spv");
	VkShaderModule blurF = load("bloomBlur.frag.spv");
	VkShaderModule godF = load("godRays.frag.spv");
	VkShaderModule ssaoF = load("ssao.frag.spv");
	VkShaderModule ssaoUpF = load("ssaoUpsample.frag.spv");
	VkShaderModule compF = load("composite.frag.spv");
	VkShaderModule downF = load("luminance_downsample.frag.spv");
	// exposure_adapt.frag writes SSBOs from the fragment stage: without
	// fragmentStoresAndAtomics the SPIR-V contract (NonWritable) would be
	// violated, so the pipeline is not created at all and the engine stays
	// on the manual exposure path (autoExposureActive gates recording).
	VkShaderModule adaptF = VK_NULL_HANDLE;
	if (m_context->fragmentStoresAndAtomics())
		adaptF = load("exposure_adapt.frag.spv");

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

	const VkFormat ssaoFmt = VK_FORMAT_R8G8B8A8_UNORM; // half-res AO+normals
	const VkFormat ssaoUpFmt = VK_FORMAT_R8_UNORM;	 // full-res final AO
	makeFS(extractF, m_postLayout1, m_hdrFormat, m_extractPipe);
	makeFS(blurF, m_postLayout1, m_hdrFormat, m_blurPipe);
	makeFS(godF, m_godLayout, m_hdrFormat, m_godRaysPipe);
	makeFS(ssaoF, m_ssaoLayout, ssaoFmt, m_ssaoPipe);
	makeFS(ssaoUpF, m_ssaoUpPipeLayout, ssaoUpFmt, m_ssaoUpPipe);
	makeFS(compF, m_compositeLayout, swapchainFormat, m_compositePipe);
	const VkFormat lumFmt = VK_FORMAT_R32_SFLOAT;
	makeFS(downF, m_postLayout1, lumFmt, m_downsamplePipe);
	if (adaptF)
		makeFS(adaptF, m_exposureLayout, lumFmt, m_adaptPipe);


	for (auto m : {fsVert, extractF, blurF, godF, ssaoF, ssaoUpF, compF, downF})
		destroyShaderModule(m_context->getDevice(), m);
	if (adaptF)
		destroyShaderModule(m_context->getDevice(), adaptF);
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
	d(m_ssaoPipe);
	d(m_ssaoUpPipe);
	d(m_compositePipe);
	d(m_downsamplePipe);
	d(m_adaptPipe);
	auto dl = [&](VkPipelineLayout &l) {
		if (l)
			vkDestroyPipelineLayout(m_context->getDevice(), l, nullptr);
		l = VK_NULL_HANDLE;
	};
	dl(m_postLayout1);
	dl(m_godLayout);
	dl(m_ssaoLayout);
	dl(m_ssaoUpPipeLayout);
	dl(m_compositeLayout);
	dl(m_exposureLayout);
}

void PostStack::init(VkContext &context, ImmediateCommands &imm, VkDescriptorSetLayout frameSetLayout,
					 VkFormat swapchainFormat, VkColorSpaceKHR swapchainColorSpace,
					 uint32_t width, uint32_t height)
{
	m_context = &context;
	m_frameSetLayout = frameSetLayout;
	createSamplers();
	createFullscreenQuad(imm);
	createDefaultImages(imm);
	createPipelines(swapchainFormat, swapchainColorSpace);
	createExposureBuffers();
	createTargets(width, height);
}

void PostStack::resize(uint32_t width, uint32_t height, VkFormat swapchainFormat,
					   VkColorSpaceKHR swapchainColorSpace)
{
	if (!m_context || (width == m_width && height == m_height &&
					   swapchainFormat == m_swapchainFormat && swapchainColorSpace == m_swapchainColorSpace))
		return;
	m_context->waitIdle();
	destroyTargets();
	if (swapchainFormat != m_swapchainFormat || swapchainColorSpace != m_swapchainColorSpace)
	{
		destroyPipelines();
		createPipelines(swapchainFormat, swapchainColorSpace);
	}
	createTargets(width, height);
}





void PostStack::destroyDefaultImages()
{
	if (!m_context)
		return;
	destroyImage(m_context->getAllocator(), m_context->getDevice(), m_defaultBlack);
	destroyImage(m_context->getAllocator(), m_context->getDevice(), m_defaultWhiteR8);
}

void PostStack::createDefaultImages(ImmediateCommands &imm)
{
	destroyDefaultImages();
	// 1×1 black HDR
	m_defaultBlack = createImage2D(m_context->getAllocator(), m_context->getDevice(), 1, 1, m_hdrFormat,
								   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
								   VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
	// 1×1 white R8 (SSAO = 1)
	m_defaultWhiteR8 = createImage2D(m_context->getAllocator(), m_context->getDevice(), 1, 1, VK_FORMAT_R8_UNORM,
									 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
									 VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

	// Clear via transfer (portable for R16F HDR and R8).
	imm.submitAndWait([&](VkCommandBuffer cmd) {
		vkbar::cmdTransitionColor(cmd, m_defaultBlack.image, VK_IMAGE_LAYOUT_UNDEFINED,
								  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
								  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		vkbar::cmdTransitionColor(cmd, m_defaultWhiteR8.image, VK_IMAGE_LAYOUT_UNDEFINED,
								  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
								  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkClearColorValue black{{0.f, 0.f, 0.f, 0.f}};
		VkClearColorValue white{{1.f, 1.f, 1.f, 1.f}};
		VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdClearColorImage(cmd, m_defaultBlack.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
		vkCmdClearColorImage(cmd, m_defaultWhiteR8.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &r);
		vkbar::cmdTransitionColor(cmd, m_defaultBlack.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
								  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
								  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
								  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		vkbar::cmdTransitionColor(cmd, m_defaultWhiteR8.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
								  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
								  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
								  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	});
	for (PostCompositeSources &sources : m_lastCompositeSrc)
		sources = {true, true, true}; // force rewrite per frame
}

void PostStack::writeCompositeDescriptors(const PostCompositeSources &src,
									 uint32_t frameIndex)
{
	if (!m_context || frameIndex >= kFramesInFlight ||
		m_setComposite[frameIndex] == VK_NULL_HANDLE)
		return;
	VkImageView bloomView = src.bloomUseDefault ? m_defaultBlack.view : m_bloom[0].view;
	VkImageView godView = src.godRaysUseDefault ? m_defaultBlack.view : m_godRays.view;
	VkImageView ssaoView = src.ssaoUseDefault ? m_defaultWhiteR8.view : m_ssaoUp.view; // final AO
	VkImageView ssaoRawView = src.ssaoUseDefault ? m_defaultWhiteR8.view : m_ssao.view; // raw AO+normals
	VkDescriptorImageInfo imgs[5] = {
		{m_linearSampler, m_hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		{m_linearSampler, bloomView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		{m_linearSampler, godView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		{m_linearSampler, ssaoView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		{m_linearSampler, ssaoRawView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
	};
	std::array<VkWriteDescriptorSet, 5> ws{};
	for (int i = 0; i < 5; ++i)
	{
		ws[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		ws[i].dstSet = m_setComposite[frameIndex];
		ws[i].dstBinding = static_cast<uint32_t>(i);
		ws[i].descriptorCount = 1;
		ws[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		ws[i].pImageInfo = &imgs[i];
	}
	vkUpdateDescriptorSets(m_context->getDevice(), 5, ws.data(), 0, nullptr);
	m_lastCompositeSrc[frameIndex] = src;
}


void PostStack::recordPost(VkCommandBuffer cmd, VkImage swapchainImage, VkImageView swapchainView,
						   VkExtent2D extent, uint32_t frameIndex,
						   VkDescriptorSet /*frameSet0*/,
						   const PostProcessSettings &settings, const glm::vec2 &sunScreen,
						   float sunVisibility, float time, const glm::mat4 &projection,
						   float frameDt, VkGpuProfiler *profiler)
{
	const auto beginRendering = beginR();
	const auto endRendering = endR();
	const uint32_t hw = std::max(1u, extent.width / 2);
	const uint32_t hh = std::max(1u, extent.height / 2);
	const VkExtent2D half{hw, hh};

	// HDR + god source + depth → shader read
	vkbar::cmdTransitionColor(cmd, m_hdr.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	vkbar::cmdTransitionColor(cmd, m_godSource.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	vkbar::cmdTransitionDepth(cmd, m_sceneDepth.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
					VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	// HDR luminance metering + exposure adaptation (issue #140). Meters the
	// raw scene HDR — before bloom/god-rays are added in composite — and
	// produces the exposure state the composite consumes.
	if (profiler)
		profiler->beginPass(cmd, GpuPass::Exposure);
	recordExposure(cmd, frameIndex, settings, frameDt);
	if (profiler)
		profiler->endPass(cmd, GpuPass::Exposure);


	// --- SSAO half-res + bilateral upsample (skip both when disabled — composite
	// treats ao=1 via flag); timed as its own GpuPass inside the Post scope ---
	if (settings.ssaoEnabled)
	{
		if (profiler)
			profiler->beginPass(cmd, GpuPass::Ssao);
		vkbar::cmdTransitionColor(cmd, m_ssao.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		SsaoPC spc{};
		spc.p0 = glm::vec4(1.f / static_cast<float>(extent.width), 1.f / static_cast<float>(extent.height),
						   settings.ssaoRadius, 0.f); // w unused (shader reserves it)
		spc.p1 = glm::vec4(float(settings.ssaoDirections), float(settings.ssaoSteps), 0.f, 0.f);
		spc.invProj = glm::inverse(projection);
		fsDraw(cmd, m_ssaoPipe, m_ssaoLayout, m_setSsao, m_ssao.view, half, &spc, sizeof(spc));
		vkbar::cmdTransitionColor(cmd, m_ssao.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
						VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

		// Depth-aware bilateral upsample → full-res final AO
		vkbar::cmdTransitionColor(cmd, m_ssaoUp.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		UpsamplePC upc{};
		upc.p0 = glm::vec4(1.f / static_cast<float>(extent.width), 1.f / static_cast<float>(extent.height), 0.f, 0.f);
		upc.invProj = spc.invProj;
		fsDraw(cmd, m_ssaoUpPipe, m_ssaoUpPipeLayout, m_setSsaoUp, m_ssaoUp.view, extent, &upc, sizeof(upc));
		vkbar::cmdTransitionColor(cmd, m_ssaoUp.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
						VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		if (profiler)
			profiler->endPass(cmd, GpuPass::Ssao);
	}

	// Bloom (skip when disabled)
	if (settings.bloomEnabled)
	{
		vkbar::cmdTransitionColor(cmd, m_bloom[0].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		ExtractPC epc{};
		epc.bloomThreshold = settings.bloomThreshold;
		fsDraw(cmd, m_extractPipe, m_postLayout1, m_setExtract, m_bloom[0].view, half, &epc, sizeof(epc));
		vkbar::cmdTransitionColor(cmd, m_bloom[0].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
						VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

		const int blurIters = settings.bloomBlurIterations > 0 ? settings.bloomBlurIterations : 3;
		int readIdx = 0;
		for (int i = 0; i < blurIters; ++i)
		{
			for (int horizontal = 1; horizontal >= 0; --horizontal)
			{
				const int writeIdx = 1 - readIdx;
				vkbar::cmdTransitionColor(cmd, m_bloom[writeIdx].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
				BlurPC bpc{};
				bpc.data = glm::vec4(1.f / static_cast<float>(hw), 1.f / static_cast<float>(hh),
									 horizontal ? 1.f : 0.f, 0.f);
				fsDraw(cmd, m_blurPipe, m_postLayout1, m_setBlur[readIdx], m_bloom[writeIdx].view, half, &bpc, sizeof(bpc));
				vkbar::cmdTransitionColor(cmd, m_bloom[writeIdx].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
								VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
								VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
				readIdx = writeIdx;
			}
		}
	}

	// Depth-aware god rays — composite samples only when this pass actually ran
	// (lighting::godRaysPassActive; never sample UNDEFINED/stale m_godRays at night).
	const bool godRaysProduced = lighting::godRaysPassActive(settings.godRaysEnabled, sunVisibility);
	if (godRaysProduced)
	{
		vkbar::cmdTransitionColor(cmd, m_godRays.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		GodPC gpc{};
		gpc.p0 = glm::vec4(sunScreen.x, sunScreen.y, settings.godRaysDensity, settings.godRaysWeight);
		gpc.p1 = glm::vec4(settings.godRaysDecay, settings.godRaysExposure, sunVisibility, time);
		gpc.p2 = glm::vec4(settings.godRaysDramaticBoost,
						   settings.godRaysDynamicBoostEnabled ? 1.f : 0.f,
						   settings.godRaysBoostPreview ? 1.f : 0.f,
						   settings.godRaysDepthOcclusion ? 1.f : 0.f);
		fsDraw(cmd, m_godRaysPipe, m_godLayout, m_setGodRays, m_godRays.view, half, &gpc, sizeof(gpc));
		vkbar::cmdTransitionColor(cmd, m_godRays.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
						VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}

	// Composite — bind 1×1 defaults for skipped effects (no clear of half-res targets)
	const PostCompositeSources compositeSources =
		postCompositeSources(settings, godRaysProduced);
	if (frameIndex < kFramesInFlight &&
		compositeSources != m_lastCompositeSrc[frameIndex])
		writeCompositeDescriptors(compositeSources, frameIndex);

	// Composite
	vkbar::cmdTransitionColor(cmd, swapchainImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	CompPC cpc{};
	cpc.p0 = glm::vec4(settings.exposure, settings.bloomIntensity, settings.gamma,
					   static_cast<float>(settings.toneMapper));
	cpc.p1 = glm::vec4(settings.bloomEnabled ? 1.f : 0.f,
					   settings.fxaaEnabled ? 1.f : 0.f,
					   godRaysProduced ? 1.f : 0.f,
					   settings.postSaturation);
	cpc.p2 = glm::vec4(1.f / static_cast<float>(extent.width),
					   1.f / static_cast<float>(extent.height),
					   settings.postContrast,
					   settings.ssaoEnabled ? 1.f : 0.f);
	cpc.p3 = glm::vec4(settings.ssaoIntensity,
					   settings.underwater ? 1.f : 0.f,
					   settings.underwaterStrength,
					   time);
	cpc.p4 = glm::vec4(settings.filmGrain, settings.vignette,
					   m_swapchainRequiresSrgbEncode ? 1.0f : 0.0f,
					   static_cast<float>(settings.ssaoDebugView));
	cpc.p5 = glm::vec4(autoExposureActive(settings) ? 1.0f : 0.0f, 0.f, 0.f, 0.f);
	fsDraw(cmd, m_compositePipe, m_compositeLayout,
		   m_setComposite[frameIndex % kFramesInFlight], swapchainView, extent,
		   &cpc, sizeof(cpc));
}


// --- Auto exposure (issue #140) -------------------------------------------

bool PostStack::autoExposureActive(const PostProcessSettings &settings) const
{
	// Without fragment SSBO stores there is no adaptation state: fall back to
	// the deterministic manual path instead of failing device creation.
	return settings.autoExposureEnabled && m_context->fragmentStoresAndAtomics();
}

void PostStack::createExposureBuffers()
{
	// ONE shared adaptation history: the temporal state is a single logical
	// value, not a per-frame-in-flight one. Cross-submission ordering is
	// provided by the pre-adapt barrier plus the same-queue in-order execution
	// guarantee (see recordExposure). The per-frame-in-flight snapshots are
	// CPU-visible copies for the debug UI, read after that slot's fence has
	// been waited. All buffers persist across swapchain resize - the metering
	// chain has a fixed resolution, so adaptation survives resize by design.
	auto createStateBuffer = [&] {
		AllocatedBuffer buf = createBuffer(m_context->getAllocator(),
										   sizeof(autoexposure::ExposureGpuState),
										   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
										   VMA_MEMORY_USAGE_AUTO,
										   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
											   VMA_ALLOCATION_CREATE_MAPPED_BIT);
		void *mapped = buf.info.pMappedData ? buf.info.pMappedData : mapBuffer(m_context->getAllocator(), buf);
		const autoexposure::ExposureGpuState initial{}; // adaptedExposure = manual default
		std::memcpy(mapped, &initial, sizeof(initial));
		return buf;
	};
	m_exposureHistory = createStateBuffer();
	for (AllocatedBuffer &buf : m_exposureSnapshot)
		buf = createStateBuffer();
	m_forceSeed = true;

	// Composite sets bind the (persistent) history buffer once.
	for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
	{
		VkDescriptorBufferInfo bi{m_exposureHistory.buffer, 0, sizeof(autoexposure::ExposureGpuState)};
		VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = m_setComposite[frame];
		w.dstBinding = 5;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		w.pBufferInfo = &bi;
		vkUpdateDescriptorSets(m_context->getDevice(), 1, &w, 0, nullptr);
	}
}

void PostStack::destroyExposureBuffers()
{
	if (!m_context)
		return;
	if (m_exposureHistory.buffer)
		destroyBuffer(m_context->getAllocator(), m_exposureHistory);
	for (AllocatedBuffer &buf : m_exposureSnapshot)
	{
		if (buf.buffer)
			destroyBuffer(m_context->getAllocator(), buf);
	}
}

void PostStack::writeExposureDescriptors()
{
	for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
	{
		VkDescriptorImageInfo ii{m_nearestSampler, m_lum[2].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkDescriptorBufferInfo history{m_exposureHistory.buffer, 0, sizeof(autoexposure::ExposureGpuState)};
		VkDescriptorBufferInfo snapshot{m_exposureSnapshot[frame].buffer, 0, sizeof(autoexposure::ExposureGpuState)};
		VkWriteDescriptorSet ws[3]{};
		const VkDescriptorType types[3] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
										   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
										   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
		VkDescriptorBufferInfo *infos[3] = {nullptr, &history, &snapshot};
		for (int i = 0; i < 3; ++i)
		{
			ws[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			ws[i].dstSet = m_exposureSets[frame];
			ws[i].dstBinding = static_cast<uint32_t>(i);
			ws[i].descriptorCount = 1;
			ws[i].descriptorType = types[i];
			ws[i].pImageInfo = i == 0 ? &ii : nullptr;
			ws[i].pBufferInfo = infos[i];
		}
		vkUpdateDescriptorSets(m_context->getDevice(), 3, ws, 0, nullptr);
	}
}

void PostStack::fsDraw(VkCommandBuffer cmd, VkPipeline pipe, VkPipelineLayout layout,
					   VkDescriptorSet set, VkImageView outView, VkExtent2D outExt,
					   const void *pc, uint32_t pcSize)
{
	const auto beginRendering = beginR();
	const auto endRendering = endR();
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
	VkViewport vport{0, 0, (float)outExt.width, (float)outExt.height, 0, 1};
	VkRect2D sc{{0, 0}, outExt};
	vkCmdSetViewport(cmd, 0, 1, &vport);
	vkCmdSetScissor(cmd, 0, 1, &sc);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
	if (pc && pcSize)
		vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcSize, pc);
	VkDeviceSize off = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_quadVBO.buffer, &off);
	vkCmdDraw(cmd, 6, 1, 0, 0);
	endRendering(cmd);
}

void PostStack::recordExposure(VkCommandBuffer cmd, uint32_t frameIndex,
							   const PostProcessSettings &settings, float frameDt)
{
	if (frameIndex >= kFramesInFlight)
		return;

	const bool useAuto = autoExposureActive(settings);
	const bool seed = m_forceSeed || (useAuto && !m_lastAutoEnabled);
	m_forceSeed = false;
	m_lastAutoEnabled = useAuto;
	if (!useAuto)
		return;

	// Long stalls (window drag, debugger break) must not slew the adaptation.
	const float dt = std::clamp(frameDt, 0.0f, 0.1f);

	// HDR -> 64x64 -> 16x16 -> 4x4 log-luminance reduction. m_setExtract
	// already binds the HDR scene target; m_lum views feed the next stages.
	// Only the first stage converts luminance to log2; later stages average
	// the already-converted .r channel.
	const VkExtent2D sizes[3] = {{64, 64}, {16, 16}, {4, 4}};
	VkDescriptorSet srcSets[3] = {m_setExtract, m_setLumSrc[0], m_setLumSrc[1]};
	for (int i = 0; i < 3; ++i)
	{
		vkbar::cmdTransitionColor(cmd, m_lum[i].image, VK_IMAGE_LAYOUT_UNDEFINED,
								  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								  0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
								  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		DownPC dpc{};
		dpc.stepUV = glm::vec4(1.f / float(sizes[i].width), 1.f / float(sizes[i].height), 0.f, 0.f);
		dpc.params = glm::vec4(i == 0 ? 0.f : 1.f, 0.f, 0.f, 0.f);
		fsDraw(cmd, m_downsamplePipe, m_postLayout1, srcSets[i], m_lum[i].view, sizes[i],
			   &dpc, sizeof(dpc));
		vkbar::cmdTransitionColor(cmd, m_lum[i].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
								  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
								  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}

	// The history buffer was last written by the previous submission of this
	// queue; submissions execute in order, so this barrier (executed after
	// that write) provides the memory dependency for the read below and for
	// the read-modify-write the adapt pass performs.
	VkBufferMemoryBarrier pre{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
	pre.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	pre.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	pre.buffer = m_exposureHistory.buffer;
	pre.offset = 0;
	pre.size = sizeof(autoexposure::ExposureGpuState);
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
						 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 1, &pre, 0, nullptr);

	// Adaptation: 4x4 meter -> target -> temporal adaptation -> history +
	// snapshot SSBOs (+1x1 R32F debug target carrying the same values).
	vkbar::cmdTransitionColor(cmd, m_lum1.image, VK_IMAGE_LAYOUT_UNDEFINED,
							  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							  0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
							  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	AdaptPC apc{};
	apc.p0 = glm::vec4(dt, settings.autoExposureSpeedUp, settings.autoExposureSpeedDown, seed ? 1.f : 0.f);
	apc.p1 = glm::vec4(settings.exposure, settings.autoExposureMiddleGrey,
					   settings.exposureCompensation, settings.autoExposureMinEv);
	apc.p2 = glm::vec4(settings.autoExposureMaxEv, 0.f, 0.f, 0.f);
	fsDraw(cmd, m_adaptPipe, m_exposureLayout, m_exposureSets[frameIndex], m_lum1.view, {1, 1},
		   &apc, sizeof(apc));
	vkbar::cmdTransitionColor(cmd, m_lum1.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
							  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
							  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	// History + snapshot writes (adaptation) -> shader reads (composite and
	// the next frame's adaptation).
	VkBufferMemoryBarrier post[2]{};
	for (int i = 0; i < 2; ++i)
	{
		post[i] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
		post[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		post[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		post[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		post[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		post[i].size = sizeof(autoexposure::ExposureGpuState);
	}
	post[0].buffer = m_exposureHistory.buffer;
	post[1].buffer = m_exposureSnapshot[frameIndex].buffer;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
						 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 2, post, 0, nullptr);
}

void PostStack::refreshExposureReadout(uint32_t frameIndex)
{
	// On-demand debug readback (issue #140: no per-frame GPU->CPU traffic).
	// Call only after the frame slot's fence has been waited (beginFrame, or
	// a synchronous tooling submit), ideally throttled - see GameUI.
	if (frameIndex >= kFramesInFlight)
		return;
	AllocatedBuffer &snapshot = m_exposureSnapshot[frameIndex];
	if (!snapshot.buffer)
		return;
	if (void *mapped = snapshot.info.pMappedData)
	{
		vmaInvalidateAllocation(m_context->getAllocator(), snapshot.allocation, 0, VK_WHOLE_SIZE);
		std::memcpy(&m_exposureReadout, mapped, sizeof(m_exposureReadout));
	}
}

void PostStack::recordExposureProbe(VkCommandBuffer cmd, uint32_t frameIndex,
									const PostProcessSettings &settings)
{
	// dt = 0: the adaptation history is a strict no-op, only the metered /
	// target / clamp outputs of this synthetic frame are produced.
	recordExposure(cmd, frameIndex, settings, 0.0f);
}
