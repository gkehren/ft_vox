#include "Renderer/ShadowPass.hpp"
#include "Vulkan/MeshArena.hpp"
#include <algorithm>
#include "Vulkan/ImageBarrier.hpp"
#include "Vulkan/GraphicsPipelineBuilder.hpp"
#include "Vulkan/VkShader.hpp"
#include "utils.hpp"
#include <glm/gtc/matrix_access.hpp>

#include <stdexcept>

namespace
{
static_assert(ShadowPass::kCascadeCount == telemetry::Shadow2 - telemetry::Shadow0 + 1,
              "Update workload telemetry when changing the cascade count");
auto beginR() { return vkCmdBeginRendering ? vkCmdBeginRendering : vkCmdBeginRenderingKHR; }
auto endR() { return vkCmdEndRendering ? vkCmdEndRendering : vkCmdEndRenderingKHR; }
} // namespace

void ShadowPass::init(VkContext &context)
{
	m_context = &context;
	createResources();
	createIndirectBuffers();
}

void ShadowPass::createIndirectBuffers()
{
	for (auto &frameSlots : m_indirect)
		for (auto &b : frameSlots)
		{
			b.buf = createBuffer(m_context->getAllocator(),
								 static_cast<VkDeviceSize>(kMaxIndirectCommands) *
									 sizeof(VkDrawIndexedIndirectCommand),
								 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
								 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
									 VMA_ALLOCATION_CREATE_MAPPED_BIT);
			b.mapped = b.buf.info.pMappedData;
			if (!b.mapped)
				b.mapped = mapBuffer(m_context->getAllocator(), b.buf);
		}
}

void ShadowPass::destroyIndirectBuffers()
{
	if (!m_context)
		return;
	for (auto &frameSlots : m_indirect)
		for (auto &b : frameSlots)
		{
			if (b.buf.buffer != VK_NULL_HANDLE)
				destroyBuffer(m_context->getAllocator(), b.buf);
			b = {};
		}
}

void ShadowPass::shutdown()
{
	destroyPipeline();
	destroyResources();
	destroyIndirectBuffers();
	m_context = nullptr;
}

void ShadowPass::createResources()
{
	m_shadowMap = createImage2DArray(
		m_context->getAllocator(), m_context->getDevice(), kShadowMapSize, kShadowMapSize,
		static_cast<uint32_t>(kCascadeCount), m_depthFormat,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, VK_IMAGE_ASPECT_DEPTH_BIT);
	for (int i = 0; i < kCascadeCount; ++i)
	{
		m_shadowLayerViews[static_cast<size_t>(i)] = createImageView2DLayer(
			m_context->getDevice(), m_shadowMap.image, m_depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT,
			static_cast<uint32_t>(i));
	}
	VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	si.magFilter = si.minFilter = VK_FILTER_LINEAR;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
	si.compareEnable = VK_FALSE;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	if (vkCreateSampler(m_context->getDevice(), &si, nullptr, &m_shadowSampler) != VK_SUCCESS)
		throw std::runtime_error("Failed to create shadow sampler");
}

void ShadowPass::destroyResources()
{
	if (!m_context)
		return;
	for (auto &v : m_shadowLayerViews)
	{
		if (v != VK_NULL_HANDLE)
			vkDestroyImageView(m_context->getDevice(), v, nullptr);
		v = VK_NULL_HANDLE;
	}
	if (m_shadowSampler != VK_NULL_HANDLE)
	{
		vkDestroySampler(m_context->getDevice(), m_shadowSampler, nullptr);
		m_shadowSampler = VK_NULL_HANDLE;
	}
	if (m_shadowMap.image != VK_NULL_HANDLE)
		destroyImage(m_context->getAllocator(), m_context->getDevice(), m_shadowMap);
}

void ShadowPass::createPipeline(VkPipelineLayout layout, const VkPipelineVertexInputStateCreateInfo &vertexInput)
{
	m_layout = layout;
	VkShaderModule vert = loadShaderModule(m_context->getDevice(), resolveSpvPath("shadow.vert.spv"));
	VkShaderModule frag = loadShaderModule(m_context->getDevice(), resolveSpvPath("shadow.frag.spv"));
	VkPipelineShaderStageCreateInfo stages[2] = {
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
	};
	VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_FRONT_BIT;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.f;
	raster.depthBiasEnable = VK_TRUE;
	raster.depthBiasConstantFactor = 1.75f;
	raster.depthBiasSlopeFactor = 2.5f;

	VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	depth.depthTestEnable = VK_TRUE;
	depth.depthWriteEnable = VK_TRUE;
	depth.depthCompareOp = VK_COMPARE_OP_LESS;

	m_pipeline = GraphicsPipelineBuilder()
					 .setLayout(layout)
					 .setShaderStages(stages, 2)
					 .setVertexInput(&vertexInput)
					 .setRaster(raster)
					 .setDepth(depth)
					 .setNoColorAttachments()
					 .setRenderingFormats(nullptr, 0, m_depthFormat)
					 .build(m_context->getDevice());

	destroyShaderModule(m_context->getDevice(), vert);
	destroyShaderModule(m_context->getDevice(), frag);
}

void ShadowPass::destroyPipeline()
{
	if (m_context && m_pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(m_context->getDevice(), m_pipeline, nullptr);
	m_pipeline = VK_NULL_HANDLE;
}

void ShadowPass::record(VkCommandBuffer cmd, uint32_t frameIndex, const std::vector<Chunk *> &shadowChunks,
						const std::array<glm::mat4, kCascadeCount> &cascades, float time,
						const MeshArenas &arenas)
{
	const auto beginRendering = beginR();
	const auto endRendering = endR();

	vkbar::cmdTransitionDepth(cmd, m_shadowMap.image, VK_IMAGE_LAYOUT_UNDEFINED,
							  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
							  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
							  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
							  static_cast<uint32_t>(kCascadeCount));

	struct ShadowPC
	{
		glm::mat4 lightSpace;
		float time;
		float pad0 = 0.f, pad1 = 0.f, pad2 = 0.f;
	};
	static_assert(sizeof(ShadowPC) == 80, "ShadowPC must match shadow.vert");

	for (int c = 0; c < kCascadeCount; ++c)
	{
		VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		depthAtt.imageView = m_shadowLayerViews[static_cast<size_t>(c)];
		depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		depthAtt.clearValue.depthStencil = {1.0f, 0};

		VkRenderingInfo shadowInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
		shadowInfo.renderArea = {{0, 0}, {kShadowMapSize, kShadowMapSize}};
		shadowInfo.layerCount = 1;
		shadowInfo.pDepthAttachment = &depthAtt;
		beginRendering(cmd, &shadowInfo);

		VkViewport vp{0.f, 0.f, static_cast<float>(kShadowMapSize), static_cast<float>(kShadowMapSize), 0.f, 1.f};
		VkRect2D scissor{{0, 0}, {kShadowMapSize, kShadowMapSize}};
		vkCmdSetViewport(cmd, 0, 1, &vp);
		vkCmdSetScissor(cmd, 0, 1, &scissor);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

		ShadowPC pc{};
		pc.lightSpace = cascades[static_cast<size_t>(c)];
		pc.time = time;
		vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ShadowPC), &pc);

		std::array<glm::vec4, 6> planes{};
		const glm::mat4 &mat = pc.lightSpace;
		planes[0] = glm::row(mat, 3) + glm::row(mat, 0);
		planes[1] = glm::row(mat, 3) - glm::row(mat, 0);
		planes[2] = glm::row(mat, 3) + glm::row(mat, 1);
		planes[3] = glm::row(mat, 3) - glm::row(mat, 1);
		planes[4] = glm::row(mat, 3) + glm::row(mat, 2);
		planes[5] = glm::row(mat, 3) - glm::row(mat, 2);

		std::array<glm::vec3, 6> planeNormals;
		std::array<float, 6> planeOffsets;
		for (size_t i = 0; i < 6; ++i)
		{
			auto &p = planes[i];
			const float len = glm::length(glm::vec3(p));
			if (len > 1e-6f)
				p /= len;

			planeNormals[i] = glm::vec3(p);
			const glm::vec3 optOffset(
				p.x >= 0.f ? static_cast<float>(CHUNK_SIZE) : 0.f,
				p.y >= 0.f ? static_cast<float>(CHUNK_HEIGHT) : 0.f,
				p.z >= 0.f ? static_cast<float>(CHUNK_SIZE) : 0.f
			);
			planeOffsets[i] = p.w + glm::dot(planeNormals[i], optOffset);
		}

		// Same cascade culling as before, but the visible chunks now
		// contribute indirect commands into the shared arenas (issue #109):
		// the cascade binds the arena pages once per page pair instead of
		// rebinding per-chunk buffers.
		m_scratch.clear();
		for (Chunk *chunk : shadowChunks)
		{
			if (!chunk || chunk->getOpaqueIndexCount() == 0)
				continue;

			const glm::vec3 aabbMin = chunk->getPosition();
			bool visible = true;
			for (size_t i = 0; i < 6; ++i)
			{
				if (glm::dot(planeNormals[i], aabbMin) + planeOffsets[i] < 0.f)
				{
					visible = false;
					break;
				}
			}
			if (visible)
				chunk->collectOpaqueDraws(m_scratch);
		}
		if (!m_scratch.empty())
		{
			const size_t count =
				std::min<size_t>(m_scratch.size(), kMaxIndirectCommands);
			const uint64_t firstKey =
				(uint64_t(m_scratch[0].vertexPage) << 32) | m_scratch[0].indexPage;
			bool single = true;
			for (const Chunk::IndirectDraw &d : m_scratch)
				if (((uint64_t(d.vertexPage) << 32) | d.indexPage) != firstKey)
				{
					single = false;
					break;
				}
			if (!single)
				std::sort(m_scratch.begin(), m_scratch.end(),
				          [](const Chunk::IndirectDraw &a, const Chunk::IndirectDraw &b)
				          {
					          const uint64_t ka = (uint64_t(a.vertexPage) << 32) | a.indexPage;
					          const uint64_t kb = (uint64_t(b.vertexPage) << 32) | b.indexPage;
					          return ka < kb;
				          });
			auto *dst = static_cast<VkDrawIndexedIndirectCommand *>(
				m_indirect[frameIndex][static_cast<size_t>(c)].mapped);
			for (size_t i = 0; i < count; ++i)
				dst[i] = m_scratch[i].cmd;
			vmaFlushAllocation(m_context->getAllocator(),
							   m_indirect[frameIndex][static_cast<size_t>(c)].buf.allocation, 0,
							   count * sizeof(VkDrawIndexedIndirectCommand));

			size_t i = 0;
			uint32_t first = 0;
			while (i < count)
			{
				const uint64_t key = (uint64_t(m_scratch[i].vertexPage) << 32) | m_scratch[i].indexPage;
				size_t j = i;
				while (j < count &&
					   (uint64_t(m_scratch[j].vertexPage) << 32 | m_scratch[j].indexPage) == key)
					++j;
				VkBuffer vb = arenas.opaqueVertex.pageBuffer(static_cast<uint32_t>(key >> 32));
				VkBuffer ib = arenas.opaqueIndex.pageBuffer(static_cast<uint32_t>(key & 0xffffffffu));
				VkDeviceSize voff = 0;
				vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &voff);
				vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
				vkCmdDrawIndexedIndirect(cmd, m_indirect[frameIndex][static_cast<size_t>(c)].buf.buffer,
										 first * sizeof(VkDrawIndexedIndirectCommand),
										 static_cast<uint32_t>(j - i), sizeof(VkDrawIndexedIndirectCommand));
				telemetry::registry().add(telemetry::ArenaBinds);
				first += static_cast<uint32_t>(j - i);
				i = j;
			}
			telemetry::registry().add(static_cast<telemetry::Event>(telemetry::Shadow0 + c), count);
		}
		endRendering(cmd);
	}

	vkbar::cmdTransitionDepth(cmd, m_shadowMap.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
							  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
							  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
							  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
							  static_cast<uint32_t>(kCascadeCount));
}
