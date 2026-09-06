#include "Renderer/OpaquePass.hpp"
#include "Renderer/MobRenderer.hpp"
#include "Renderer/IndirectDrawEmit.hpp"
#include "Renderer/VoxelDrawDataLayout.hpp"
#include "Vulkan/MeshArena.hpp"
#include "Vulkan/ImageBarrier.hpp"
#include "Vulkan/GraphicsPipelineBuilder.hpp"
#include "Vulkan/VkShader.hpp"
#include "utils.hpp"

#include <cassert>
#include <iostream>
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
						VoxelDrawData *drawDataOut, AllocatedBuffer &drawDataBuffer,
						VkGpuProfiler *gpu, MobRenderer *mobs)
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
	size_t demandedCount = 0;  // commands the visible chunks want (pre-truncation)
	size_t count = 0;          // commands actually emitted this frame
	if (m_scratch.size() < kMaxIndirectCommands)
		m_scratch.resize(kMaxIndirectCommands);
	Chunk::IndirectDraw *outPtr = m_scratch.data();
	for (Chunk *chunk : chunks)
	{
		if (!chunk || !chunk->hasRenderableOpaqueDraws())
			continue;
		const uint32_t n = chunk->getCachedOpaqueDrawCount();
		demandedCount += n;
		if (count + n <= kMaxIndirectCommands)
		{
			std::memcpy(outPtr + count, chunk->cachedOpaqueDraws(), n * sizeof(Chunk::IndirectDraw));
			count += n;
		}
		else
		{
			// Not silent: at larger view distances or denser worlds this
			// would silently drop geometry (issue #109 review phase 24).
			static bool warned = false;
			if (!warned)
			{
				warned = true;
				std::cerr << "[indirect] opaque command capacity exceeded: demanded=" << demandedCount
				          << " emitted=" << count << " capacity=" << kMaxIndirectCommands
				          << " chunkCommands=" << n << " - chunk skipped" << std::endl;
			}
		}
	}
	m_lastCommands = static_cast<uint32_t>(std::min<size_t>(demandedCount, UINT32_MAX));
	const uint32_t baseInstance = voxel_draw::kOpaqueBase;
	const bool directDraws = std::getenv("FT_VOX_DRAW_DIRECT") != nullptr;
	if (directDraws)
	{
		// Bisect mode: bind per section and draw directly (no indirect
		// buffer). If the world renders correctly here, the arena data is
		// good and the bug is in the indirect command path. Only the draw
		// dispatch changes: overlays and the rendering scope close exactly
		// like the indirect path, so the rest of the frame graph is
		// identical. Uses `count` — the scratch is pre-sized to
		// kMaxIndirectCommands, so its size() says nothing about how many
		// entries are valid.
		assert(count <= kMaxIndirectCommands);
		assert(baseInstance + count <= voxel_draw::kEntryCount);
		if (drawDataOut)
		{
			for (size_t i = 0; i < count; ++i)
			{
				drawDataOut[baseInstance + i].worldOrigin = m_scratch[i].chunkOrigin;
				drawDataOut[baseInstance + i].flags = 0;
			}
			vmaFlushAllocation(m_context->getAllocator(), drawDataBuffer.allocation,
							   baseInstance * sizeof(VoxelDrawData), count * sizeof(VoxelDrawData));
		}
		VkBuffer curV = VK_NULL_HANDLE, curI = VK_NULL_HANDLE;
		for (size_t i = 0; i < count; ++i)
		{
			const Chunk::IndirectDraw &d = m_scratch[i];
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
			                 d.cmd.vertexOffset, static_cast<uint32_t>(baseInstance + i));
		}
	}
	else if (count > 0)
	{
		assert(count <= kMaxIndirectCommands && "OpaquePass: indirect command capacity exceeded");
		assert(baseInstance + count <= voxel_draw::kEntryCount);
		// Computed once per record: batches obey multiDrawIndirect and
		// maxDrawIndirectCount (see IndirectDrawUtils.hpp).
		const uint32_t batchLimit = indirectBatchLimit(
			m_context->hasMultiDrawIndirect(), m_context->maxDrawIndirectCount());

		auto *dst = static_cast<VkDrawIndexedIndirectCommand *>(m_indirect[frameIndex].mapped);
		std::array<PageBatch, 128> batches{};
		const GroupedDraws grouped = emitGroupedIndirectDraws(
			m_scratch.data(), count, baseInstance, dst, drawDataOut, batches);

		// Flush only what was written: a full 1.25 MiB flush per frame
		// measurably regressed CPU record time.
		vmaFlushAllocation(m_context->getAllocator(), m_indirect[frameIndex].buf.allocation, 0,
						   count * sizeof(VkDrawIndexedIndirectCommand));
		if (drawDataOut)
		{
			vmaFlushAllocation(m_context->getAllocator(), drawDataBuffer.allocation,
							   baseInstance * sizeof(VoxelDrawData), count * sizeof(VoxelDrawData));
		}

		// One call per batch: drawCount stays under the hardware ceiling
		// and degrades to per-command draws without multiDrawIndirect.
		if (!grouped.usedFallback)
		{
			for (size_t b = 0; b < grouped.batchCount; ++b)
			{
				const uint64_t key = batches[b].key;
				VkBuffer vb = arenas.opaqueVertex.pageBuffer(static_cast<uint32_t>(key >> 32));
				VkBuffer ib = arenas.opaqueIndex.pageBuffer(static_cast<uint32_t>(key & 0xffffffffu));
				VkDeviceSize voff = 0;
				vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &voff);
				vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
				issueIndirectRange(cmd, m_indirect[frameIndex].buf.buffer, batches[b].first,
								   batches[b].count, batchLimit);
				telemetry::registry().add(telemetry::ArenaBinds);
			}
		}
		else
		{
			// Rare >128-page-pair fallback: src was sorted by key and dst is
			// sequential — emit contiguous same-key runs.
			size_t i = 0;
			uint32_t first = 0;
			while (i < count)
			{
				const uint64_t key = (uint64_t(m_scratch[i].vertexPage) << 32) | m_scratch[i].indexPage;
				size_t j = i;
				while (j < count &&
					   ((uint64_t(m_scratch[j].vertexPage) << 32) | m_scratch[j].indexPage) == key)
					++j;
				VkBuffer vb = arenas.opaqueVertex.pageBuffer(static_cast<uint32_t>(key >> 32));
				VkBuffer ib = arenas.opaqueIndex.pageBuffer(static_cast<uint32_t>(key & 0xffffffffu));
				VkDeviceSize voff = 0;
				vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &voff);
				vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
				issueIndirectRange(cmd, m_indirect[frameIndex].buf.buffer, first, j - i, batchLimit);
				telemetry::registry().add(telemetry::ArenaBinds);
				first += static_cast<uint32_t>(j - i);
				i = j;
			}
		}
		telemetry::registry().add(telemetry::OpaqueDraws, count);
	}
	if (std::getenv("FT_VOX_VALIDATE_INDIRECT"))
		++m_debugFrames;
	if (gpu) gpu->endPass(cmd, GpuPass::Opaque);
	if (gpu) gpu->beginPass(cmd, GpuPass::Mobs);
    if (mobs) mobs->record(cmd, frameIndex, set0);
    if (gpu) gpu->endPass(cmd, GpuPass::Mobs);
	if (gpu) gpu->beginPass(cmd, GpuPass::Overlays);
	overlays.record(cmd, set0, chunks);
	if (gpu) gpu->endPass(cmd, GpuPass::Overlays);
	endRendering(cmd);
}
