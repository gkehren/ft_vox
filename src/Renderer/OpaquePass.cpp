#include "Renderer/OpaquePass.hpp"
#include "Vulkan/MeshArena.hpp"
#include "Vulkan/ImageBarrier.hpp"
#include "Vulkan/GraphicsPipelineBuilder.hpp"
#include "Vulkan/VkShader.hpp"
#include "utils.hpp"

#include <stdexcept>
#include <cstdlib>

namespace
{
auto beginR() { return vkCmdBeginRendering ? vkCmdBeginRendering : vkCmdBeginRenderingKHR; }
auto endR() { return vkCmdEndRendering ? vkCmdEndRendering : vkCmdEndRenderingKHR; }
} // namespace

void OpaquePass::init(VkContext &context)
{
	m_context = &context;
	createIndirectBuffers();
}

void OpaquePass::createIndirectBuffers()
{
	for (IndirectBatch &b : m_indirect)
	{
		b.buf = createBuffer(m_context->getAllocator(),
							 static_cast<VkDeviceSize>(kMaxIndirectCommands) * sizeof(VkDrawIndexedIndirectCommand),
							 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
							 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
								 VMA_ALLOCATION_CREATE_MAPPED_BIT);
		b.mapped = b.buf.info.pMappedData;
		if (!b.mapped)
			b.mapped = mapBuffer(m_context->getAllocator(), b.buf);
	}
}

void OpaquePass::destroyIndirectBuffers()
{
	if (!m_context)
		return;
	for (IndirectBatch &b : m_indirect)
	{
		if (b.buf.buffer != VK_NULL_HANDLE)
			destroyBuffer(m_context->getAllocator(), b.buf);
		b = {};
	}
}

void OpaquePass::shutdown()
{
	destroyPipeline();
	destroyIndirectBuffers();
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
						const VkClearColorValue &clearColor, uint32_t frameIndex, const MeshArenas &arenas,
						VkGpuProfiler *gpu)
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
	// Issue #109: one indirect command per live section, grouped by arena
	// page pair - the shared arenas are bound once per pair (in practice a
	// handful of binds per frame) instead of two binds per chunk.
	m_scratch.clear();
	for (Chunk *chunk : chunks)
		if (chunk)
			chunk->collectOpaqueDraws(m_scratch);
	const bool directDraws = std::getenv("FT_VOX_DRAW_DIRECT") != nullptr;
	if (directDraws)
	{
		// Bisect mode: bind per section and draw directly (no indirect
		// buffer). If the world renders correctly here, the arena data is
		// good and the bug is in the indirect command path.
		VkBuffer curV = VK_NULL_HANDLE, curI = VK_NULL_HANDLE;
		for (const Chunk::IndirectDraw &d : m_scratch)
		{
			VkBuffer vb = arenas.opaqueVertex.pageBuffer(d.vertexPage);
			VkBuffer ib = arenas.opaqueIndex.pageBuffer(d.indexPage);
			if (vb != curV || ib != curI)
			{
				VkDeviceSize voff = 0;
				vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &voff);
				vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
				curV = vb;
				curI = ib;
			}
			vkCmdDrawIndexed(cmd, d.cmd.indexCount, 1, d.cmd.firstIndex,
			                 d.cmd.vertexOffset, 0);
		}
		if (gpu) gpu->endPass(cmd, GpuPass::Opaque);
		return;
	}
	if (!m_scratch.empty())
	{
		const size_t count = std::min<size_t>(m_scratch.size(), kMaxIndirectCommands);
		// The overwhelming common case: every range lives in the same page
		// pair, so a linear same-key scan replaces the sort.
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
		auto *dst = static_cast<VkDrawIndexedIndirectCommand *>(m_indirect[frameIndex].mapped);
		for (size_t i = 0; i < count; ++i)
			dst[i] = m_scratch[i].cmd;
		// Flush only what was written: a full 1.25 MiB flush per frame
		// measurably regressed CPU record time.
		vmaFlushAllocation(m_context->getAllocator(), m_indirect[frameIndex].buf.allocation, 0,
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
			vkCmdDrawIndexedIndirect(cmd, m_indirect[frameIndex].buf.buffer,
									 first * sizeof(VkDrawIndexedIndirectCommand),
									 static_cast<uint32_t>(j - i), sizeof(VkDrawIndexedIndirectCommand));
			telemetry::registry().add(telemetry::ArenaBinds);
			if (std::getenv("FT_VOX_VALIDATE_INDIRECT") && m_debugFrames < 3)
			{
				const VkDeviceSize vbSize = arenas.opaqueVertex.pageSize(static_cast<uint32_t>(key >> 32));
				const VkDeviceSize ibSize = arenas.opaqueIndex.pageSize(static_cast<uint32_t>(key & 0xffffffffu));
				uint32_t worstEnd = 0, worstVOff = 0;
				uint32_t bad = 0;
				for (size_t k = i; k < j; ++k)
				{
					const uint32_t iStart = m_scratch[k].cmd.firstIndex * sizeof(uint32_t);
					const uint32_t iEnd = iStart + m_scratch[k].cmd.indexCount * sizeof(uint32_t);
					const uint32_t vBytes = static_cast<uint32_t>(m_scratch[k].cmd.vertexOffset) *
						sizeof(Vertex);
					if (iEnd > worstEnd) worstEnd = iEnd;
					if (vBytes > worstVOff) worstVOff = vBytes;
					if (iEnd > ibSize || vBytes > vbSize)
						++bad;
				}
				if (bad > 0)
					std::cerr << "[indirect] group out-of-page: bad=" << bad
					          << " ibSize=" << ibSize << " worstIndexEnd=" << worstEnd
					          << " vbSize=" << vbSize << " worstVertexBytes=" << worstVOff
					          << std::endl;
			}
			first += static_cast<uint32_t>(j - i);
			i = j;
		}
		telemetry::registry().add(telemetry::OpaqueDraws, count);
	}
	if (std::getenv("FT_VOX_VALIDATE_INDIRECT"))
		++m_debugFrames;
	if (gpu) gpu->endPass(cmd, GpuPass::Opaque);
	if (gpu) gpu->beginPass(cmd, GpuPass::Overlays);
	overlays.record(cmd, set0, chunks);
	if (gpu) gpu->endPass(cmd, GpuPass::Overlays);
	endRendering(cmd);
}
