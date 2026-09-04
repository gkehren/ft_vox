#include "Renderer/WorldRenderer.hpp"
#include "Renderer/MaterialTable.hpp"
#include "Engine/Profiler.hpp"
#include "Vulkan/ImageBarrier.hpp"
#include "Vulkan/VkUpload.hpp"
#include "utils.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace
{
VkPipelineVertexInputStateCreateInfo makeVertexInput(VkVertexInputBindingDescription &binding,
													 std::array<VkVertexInputAttributeDescription, 4> &attrs)
{
	binding.binding = 0;
	binding.stride = sizeof(Vertex);
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)};
	attrs[1] = {1, 0, VK_FORMAT_R32_UINT, offsetof(Vertex, packedData)};
	attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord)};
	attrs[3] = {3, 0, VK_FORMAT_R32_UINT, offsetof(Vertex, packedBiomeColor)};
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &binding;
	vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
	vi.pVertexAttributeDescriptions = attrs.data();
	return vi;
}

auto beginR() { return vkCmdBeginRendering ? vkCmdBeginRendering : vkCmdBeginRenderingKHR; }
auto endR() { return vkCmdEndRendering ? vkCmdEndRendering : vkCmdEndRenderingKHR; }
} // namespace

WorldRenderer::~WorldRenderer() { shutdown(); }

void WorldRenderer::shutdown()
{
	if (!m_context)
		return;
	m_context->waitIdle();
	destroyPipelines();
	destroyFrameUbos();
	m_sky.shutdown();
	m_water.shutdown();
	m_opaque.shutdown();
	m_shadow.shutdown();
	m_overlays.shutdown();
	m_post.shutdown();

	if (m_materialUbo.buffer != VK_NULL_HANDLE)
		destroyBuffer(m_context->getAllocator(), m_materialUbo);
	m_materialMapped = nullptr;

	auto destroyLayout = [&](VkPipelineLayout &l) {
		if (l != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(m_context->getDevice(), l, nullptr);
		l = VK_NULL_HANDLE;
	};
	destroyLayout(m_pipelineLayout);
	destroyLayout(m_shadowPipelineLayout);
	destroyLayout(m_waterPipelineLayout);

	if (m_descriptorPool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(m_context->getDevice(), m_descriptorPool, nullptr);
	auto destroySetLayout = [&](VkDescriptorSetLayout &l) {
		if (l != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(m_context->getDevice(), l, nullptr);
		l = VK_NULL_HANDLE;
	};
	destroySetLayout(m_setLayout0);
	destroySetLayout(m_setLayout1);
	destroySetLayout(m_setLayout2);
	m_descriptorPool = VK_NULL_HANDLE;
	m_set1 = m_set2Water = VK_NULL_HANDLE;
	m_textures.shutdown();
	m_context = nullptr;
	m_imm = nullptr;
}

void WorldRenderer::createDescriptors()
{
	// set0: FrameUBO + MaterialTable
	std::array<VkDescriptorSetLayoutBinding, 2> set0{};
	set0[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
			   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	set0[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
			   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	VkDescriptorSetLayoutCreateInfo layout0{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	layout0.bindingCount = static_cast<uint32_t>(set0.size());
	layout0.pBindings = set0.data();
	if (vkCreateDescriptorSetLayout(m_context->getDevice(), &layout0, nullptr, &m_setLayout0) != VK_SUCCESS)
		throw std::runtime_error("Failed to create set layout 0");

	std::array<VkDescriptorSetLayoutBinding, 2> set1{};
	set1[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	set1[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	VkDescriptorSetLayoutCreateInfo layout1{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	layout1.bindingCount = static_cast<uint32_t>(set1.size());
	layout1.pBindings = set1.data();
	if (vkCreateDescriptorSetLayout(m_context->getDevice(), &layout1, nullptr, &m_setLayout1) != VK_SUCCESS)
		throw std::runtime_error("Failed to create set layout 1");
	if (vkCreateDescriptorSetLayout(m_context->getDevice(), &layout1, nullptr, &m_setLayout2) != VK_SUCCESS)
		throw std::runtime_error("Failed to create set layout 2");

	std::array<VkDescriptorPoolSize, 2> poolSizes{};
	poolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxFramesInFlight * 2 + 2};
	poolSizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
	VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	poolInfo.pPoolSizes = poolSizes.data();
	poolInfo.maxSets = kMaxFramesInFlight + 4;
	if (vkCreateDescriptorPool(m_context->getDevice(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
		throw std::runtime_error("Failed to create descriptor pool");

	VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	alloc.descriptorPool = m_descriptorPool;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts = &m_setLayout1;
	if (vkAllocateDescriptorSets(m_context->getDevice(), &alloc, &m_set1) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate set1");
	alloc.pSetLayouts = &m_setLayout2;
	if (vkAllocateDescriptorSets(m_context->getDevice(), &alloc, &m_set2Water) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate set2");
}

void WorldRenderer::createPipelineLayouts()
{
	{
		std::array<VkDescriptorSetLayout, 2> layouts = {m_setLayout0, m_setLayout1};
		VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		li.setLayoutCount = 2;
		li.pSetLayouts = layouts.data();
		if (vkCreatePipelineLayout(m_context->getDevice(), &li, nullptr, &m_pipelineLayout) != VK_SUCCESS)
			throw std::runtime_error("pipeline layout failed");
	}
	{
		VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 80};
		VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		li.pushConstantRangeCount = 1;
		li.pPushConstantRanges = &pcr;
		if (vkCreatePipelineLayout(m_context->getDevice(), &li, nullptr, &m_shadowPipelineLayout) != VK_SUCCESS)
			throw std::runtime_error("shadow layout failed");
	}
	{
		std::array<VkDescriptorSetLayout, 3> layouts = {m_setLayout0, m_setLayout1, m_setLayout2};
		VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 4};
		VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		li.setLayoutCount = 3;
		li.pSetLayouts = layouts.data();
		li.pushConstantRangeCount = 1;
		li.pPushConstantRanges = &pcr;
		if (vkCreatePipelineLayout(m_context->getDevice(), &li, nullptr, &m_waterPipelineLayout) != VK_SUCCESS)
			throw std::runtime_error("water layout failed");
	}
}

void WorldRenderer::createFrameUbos()
{
	for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
	{
		auto &f = m_frameUbos[i];
		f.ubo = createBuffer(m_context->getAllocator(), sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
							 VMA_MEMORY_USAGE_CPU_TO_GPU,
							 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
								 VMA_ALLOCATION_CREATE_MAPPED_BIT);
		f.uboMapped = f.ubo.info.pMappedData;
		if (!f.uboMapped)
			f.uboMapped = mapBuffer(m_context->getAllocator(), f.ubo);

		VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		alloc.descriptorPool = m_descriptorPool;
		alloc.descriptorSetCount = 1;
		alloc.pSetLayouts = &m_setLayout0;
		if (vkAllocateDescriptorSets(m_context->getDevice(), &alloc, &f.descriptorSet0) != VK_SUCCESS)
			throw std::runtime_error("Failed to allocate frame set0");

		VkDescriptorBufferInfo frameBi{f.ubo.buffer, 0, sizeof(FrameUBO)};
		VkDescriptorBufferInfo matBi{m_materialUbo.buffer, 0, sizeof(materials::MaterialTableUBO)};
		VkWriteDescriptorSet writes[2]{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = f.descriptorSet0;
		writes[0].dstBinding = 0;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writes[0].descriptorCount = 1;
		writes[0].pBufferInfo = &frameBi;
		writes[1] = writes[0];
		writes[1].dstBinding = 1;
		writes[1].pBufferInfo = &matBi;
		vkUpdateDescriptorSets(m_context->getDevice(), 2, writes, 0, nullptr);
	}
}

void WorldRenderer::destroyFrameUbos()
{
	if (!m_context)
		return;
	for (auto &f : m_frameUbos)
	{
		if (f.ubo.buffer != VK_NULL_HANDLE)
			destroyBuffer(m_context->getAllocator(), f.ubo);
		f = {};
	}
}

void WorldRenderer::writeSet1Descriptors()
{
	VkDescriptorImageInfo texInfo{};
	texInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	texInfo.imageView = m_textures.getImageView();
	texInfo.sampler = m_textures.getSampler();
	VkDescriptorImageInfo shadowInfo{};
	shadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	shadowInfo.imageView = m_shadow.arrayView();
	shadowInfo.sampler = m_shadow.sampler();
	VkWriteDescriptorSet writes[2]{};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = m_set1;
	writes[0].dstBinding = 0;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[0].descriptorCount = 1;
	writes[0].pImageInfo = &texInfo;
	writes[1] = writes[0];
	writes[1].dstBinding = 1;
	writes[1].pImageInfo = &shadowInfo;
	vkUpdateDescriptorSets(m_context->getDevice(), 2, writes, 0, nullptr);
}

void WorldRenderer::writeMaterialDescriptors()
{
	const auto table = materials::buildGpuTable();
	if (m_materialMapped)
		std::memcpy(m_materialMapped, &table, sizeof(table));
}

void WorldRenderer::createPipelines()
{
	VkVertexInputBindingDescription binding{};
	std::array<VkVertexInputAttributeDescription, 4> attrs{};
	auto vertexInput = makeVertexInput(binding, attrs);
	const VkFormat colorFmt = m_post.hdrFormat();
	const VkFormat depthFmt = m_post.depthFormat();
	m_shadow.createPipeline(m_shadowPipelineLayout, vertexInput);
	m_opaque.createPipeline(m_pipelineLayout, vertexInput, colorFmt, depthFmt);
	m_water.createPipeline(m_waterPipelineLayout, vertexInput, colorFmt, depthFmt);
}

void WorldRenderer::destroyPipelines()
{
	m_water.destroyPipeline();
	m_opaque.destroyPipeline();
	m_shadow.destroyPipeline();
}

TextureAtlasLoadReport WorldRenderer::reloadResourcePack(const std::string &resourcePackRoot)
{
	if (!m_context || !m_imm)
		throw std::runtime_error("WorldRenderer::reloadResourcePack before init");
	// Failure-atomic: TextureManager keeps the previous atlas if rebuild throws.
	const TextureAtlasLoadReport report = m_textures.initialize(*m_context, *m_imm, resourcePackRoot);
	writeSet1Descriptors();
	return report;
}

void WorldRenderer::init(VkContext &context, VkSwapchain &swapchain, ImmediateCommands &imm,
						 const std::string &resourcePackRoot)
{
	m_context = &context;
	m_imm = &imm;
	m_lightDir = glm::normalize(glm::vec3(0.4f, 1.0f, 0.2f));

	m_textures.initialize(context, imm, resourcePackRoot);
	m_shadow.init(context);
	m_opaque.init(context);
	createDescriptors();
	createPipelineLayouts();

	m_materialUbo = createBuffer(m_context->getAllocator(), sizeof(materials::MaterialTableUBO),
								 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU,
								 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
									 VMA_ALLOCATION_CREATE_MAPPED_BIT);
	m_materialMapped = m_materialUbo.info.pMappedData;
	if (!m_materialMapped)
		m_materialMapped = mapBuffer(m_context->getAllocator(), m_materialUbo);
	writeMaterialDescriptors();

	const auto extent = swapchain.getExtent();
	m_water.init(context, imm, extent.width, extent.height, VK_FORMAT_D32_SFLOAT);
	m_post.init(context, imm, m_setLayout0, swapchain.getImageFormat(), extent.width, extent.height);
	m_sky.init(context, imm, m_setLayout0, m_post.hdrFormat(), m_post.depthFormat());
	m_overlays.init(context, imm, m_setLayout0, m_post.hdrFormat(), m_post.depthFormat());
	createPipelines();
	createFrameUbos();
	writeSet1Descriptors();
	m_water.writeSceneDescriptors(m_set2Water, m_water.sceneSampler());
	std::cout << "WorldRenderer ready (Shadow/Opaque/Water/Sky + post)\n";
}

void WorldRenderer::onSwapchainRecreate(VkSwapchain &swapchain)
{
	m_context->waitIdle();
	const auto extent = swapchain.getExtent();
	m_post.resize(extent.width, extent.height, swapchain.getImageFormat());
	m_water.resize(extent.width, extent.height, m_post.depthFormat());
	m_water.writeSceneDescriptors(m_set2Water, m_water.sceneSampler());
	m_sky.recreatePipeline(m_post.hdrFormat(), m_post.depthFormat());
	destroyPipelines();
	createPipelines();
	m_overlays.recreatePipelines(m_post.hdrFormat(), m_post.depthFormat());
}

void WorldRenderer::updateFrameUBO(uint32_t frameIndex, const Camera &camera, float aspectW, float aspectH,
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

	const float cascadeFar = std::max(shadowCascadeFar, 64.f);
	const float aspect = (aspectH > 1e-5f) ? (aspectW / aspectH) : (16.f / 9.f);
	glm::vec4 splits{};
	shadow::buildCascadeUBOFromFront(camera.getPosition(), camera.getFront(), glm::vec3(0.f, 1.f, 0.f), m_lightDir,
									0.1f, cascadeFar, aspect, shadow::kDefaultFovYDegrees, m_cascadeMatrices, splits,
									nullptr);

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
	ubo.skyParams = glm::vec4(time, params.dayFactor, params.sunsetFactor, params.nightFactor);
	ubo.cascadeSplits = splits;
	ubo.moonAmbient = glm::vec4(0.22f, 0.30f, 0.48f, params.moonAmbientStrength);
	ubo.lightingParams = glm::vec4(params.blockLightScale, params.emissiveScale, params.fogBaseY,
								underwater ? 1.0f : 0.0f);
	ubo.waterParams = glm::vec4(params.waterWaveStrength, params.waterRefraction, params.waterSpecular,
								params.waterFoamStrength);

	std::memcpy(m_frameUbos[frameIndex].uboMapped, &ubo, sizeof(FrameUBO));
}

void WorldRenderer::recordFrame(VkCommandBuffer cmd, uint32_t frameIndex, uint32_t imageIndex,
								  VkSwapchain &swapchain, const std::vector<Chunk *> &chunks,
								  const std::vector<Chunk *> &shadowChunks, const VkClearColorValue &clearColor,
								  const std::function<void(VkCommandBuffer)> &preRecord,
								  const std::function<void(VkCommandBuffer)> &imguiDraw,
                                  VkGpuProfiler *gpu, uint64_t benchmarkTag)
{
	const VkExtent2D extent = swapchain.getExtent();
	const auto beginRendering = beginR();
	const auto endRendering = endR();
	if (!beginRendering || !endRendering)
		throw std::runtime_error("Dynamic rendering entry points unavailable");

	VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
		throw std::runtime_error("Failed to begin terrain command buffer");

	if (gpu) gpu->beginRecording(cmd, frameIndex, benchmarkTag);
	if (gpu) gpu->beginPass(cmd, GpuPass::Upload);
	if (preRecord)
		preRecord(cmd);
	if (gpu) gpu->endPass(cmd, GpuPass::Upload);

	const VkDescriptorSet set0 = m_frameUbos[frameIndex].descriptorSet0;

	{
		PROFILE_SCOPE("Shadow");
		if (gpu) gpu->beginPass(cmd, GpuPass::Shadow);
		m_shadow.record(cmd, shadowChunks, m_cascadeMatrices, m_time);
		if (gpu) gpu->endPass(cmd, GpuPass::Shadow);
	}
	{
		PROFILE_SCOPE("Scene");
		m_opaque.record(cmd, extent, set0, m_set1, m_pipelineLayout, m_post.hdrColor(), m_post.sceneDepth(), chunks,
						m_overlays, clearColor, gpu);
	}
	{
		const auto *ubo = static_cast<const FrameUBO *>(m_frameUbos[frameIndex].uboMapped);
		if (gpu) gpu->beginPass(cmd, GpuPass::Water);
		m_water.record(cmd, extent, set0, m_set1, m_set2Water, m_waterPipelineLayout, m_post.hdrColor(),
					   m_post.sceneDepth(), chunks, glm::vec3(ubo->viewPos));
		if (gpu) gpu->endPass(cmd, GpuPass::Water);
	}
	{
		PROFILE_SCOPE("Sky");
		if (gpu) gpu->beginPass(cmd, GpuPass::Sky);
		m_sky.record(cmd, set0, extent, m_post.hdrColor(), m_post.godSource(), m_post.sceneDepth());
		if (gpu) gpu->endPass(cmd, GpuPass::Sky);
	}
	{
		PROFILE_SCOPE("Post");
		const auto *ubo = static_cast<const FrameUBO *>(m_frameUbos[frameIndex].uboMapped);
		glm::vec4 sunClip =
			ubo->projection * ubo->view * glm::vec4(m_lastCamPos + glm::vec3(ubo->sunDir) * 500.f, 1.f);
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
		m_postSettings.underwater = ubo->lightingParams.w > 0.5f;
		if (gpu) gpu->beginPass(cmd, GpuPass::Post);
		m_post.recordPost(cmd, swapchain.getImages()[imageIndex],
						  swapchain.getImageViews()[imageIndex], extent,
						  frameIndex, set0, m_postSettings, sunScreen,
						  sunVisibility, m_time, ubo->projection);
		if (gpu) gpu->endPass(cmd, GpuPass::Post);
	}

	if (imguiDraw)
	{
		PROFILE_SCOPE("ImGuiDraw");
		if (gpu) gpu->beginPass(cmd, GpuPass::ImGui);
		VkRenderingAttachmentInfo colorAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		colorAtt.imageView = swapchain.getImageViews()[imageIndex];
		colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
		ri.renderArea = {{0, 0}, extent};
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &colorAtt;
		beginRendering(cmd, &ri);
		VkViewport vp{0, 0, (float)extent.width, (float)extent.height, 0, 1};
		VkRect2D sc{{0, 0}, extent};
		vkCmdSetViewport(cmd, 0, 1, &vp);
		vkCmdSetScissor(cmd, 0, 1, &sc);
		imguiDraw(cmd);
		endRendering(cmd);
		if (gpu) gpu->endPass(cmd, GpuPass::ImGui);
	}

	vkbar::cmdTransitionColor(cmd, swapchain.getImages()[imageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
							  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

	if (gpu) gpu->endRecording(cmd);
	if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
		throw std::runtime_error("Failed to end terrain command buffer");
}
