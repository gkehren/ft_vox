#include "Renderer/WaterPass.hpp"
#include "Renderer/IndirectDrawEmit.hpp"
#include "Renderer/VoxelDrawDataLayout.hpp"
#include "Vulkan/MeshArena.hpp"
#include "Vulkan/ImageBarrier.hpp"
#include "Vulkan/GraphicsPipelineBuilder.hpp"
#include "Vulkan/VkShader.hpp"
#include "Vulkan/VkCommands.hpp"
#include "utils.hpp"

#include <cassert>

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace
{
auto beginR() { return vkCmdBeginRendering ? vkCmdBeginRendering : vkCmdBeginRenderingKHR; }
auto endR() { return vkCmdEndRendering ? vkCmdEndRendering : vkCmdEndRenderingKHR; }
} // namespace

void WaterPass::init(VkContext &context, ImmediateCommands & /*imm*/, uint32_t width, uint32_t height,
					 VkFormat depthFmt)
{
	m_context = &context;
	m_depthFormat = depthFmt;
	createHistory(width, height, depthFmt);
	createIndirectBuffers();
	VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	si.magFilter = si.minFilter = VK_FILTER_LINEAR;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	if (vkCreateSampler(m_context->getDevice(), &si, nullptr, &m_sceneSampler) != VK_SUCCESS)
		throw std::runtime_error("scene sampler failed");
}

void WaterPass::shutdown()
{
	destroyPipeline();
	destroyIndirectBuffers();
	if (m_context && m_sceneSampler != VK_NULL_HANDLE)
	{
		vkDestroySampler(m_context->getDevice(), m_sceneSampler, nullptr);
		m_sceneSampler = VK_NULL_HANDLE;
	}
	destroyHistory();
	m_context = nullptr;
}

void WaterPass::createHistory(uint32_t w, uint32_t h, VkFormat depthFmt)
{
	m_sceneHistory = createImage2D(
		m_context->getAllocator(), m_context->getDevice(), w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
	m_depthHistory = createImage2D(
		m_context->getAllocator(), m_context->getDevice(), w, h, depthFmt,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 1,
		VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
}

void WaterPass::destroyHistory()
{
	if (!m_context)
		return;
	if (m_sceneHistory.image != VK_NULL_HANDLE)
		destroyImage(m_context->getAllocator(), m_context->getDevice(), m_sceneHistory);
	if (m_depthHistory.image != VK_NULL_HANDLE)
		destroyImage(m_context->getAllocator(), m_context->getDevice(), m_depthHistory);
}

void WaterPass::resize(uint32_t width, uint32_t height, VkFormat depthFmt)
{
	destroyHistory();
	m_depthFormat = depthFmt;
	createHistory(width, height, depthFmt);
}

void WaterPass::createPipeline(VkPipelineLayout layout, const VkPipelineVertexInputStateCreateInfo &vertexInput,
							   VkFormat colorFmt, VkFormat depthFmt)
{
	VkShaderModule vert = loadShaderModule(m_context->getDevice(), resolveSpvPath("water.vert.spv"));
	VkShaderModule frag = loadShaderModule(m_context->getDevice(), resolveSpvPath("water.frag.spv"));
	VkPipelineShaderStageCreateInfo stages[2] = {
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
	};
	VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.f;

	VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	depth.depthTestEnable = VK_TRUE;
	// Single-composition water: the fragment outputs the fully
	// composited surface color (refraction + absorption + reflection + foam)
	// with alpha = 1, so blending is disabled and depth write is enabled.
	// The depth write resolves the nearest visible water surface independent
	// of draw order and lets the sky pass (LESS_OR_EQUAL depth test), SSAO,
	// god-ray occlusion and the camera-underwater composite see the surface
	// instead of the geometry behind it.
	depth.depthWriteEnable = VK_TRUE;
	depth.depthCompareOp = VK_COMPARE_OP_LESS;

	VkPipelineColorBlendAttachmentState blendAtt{};
	blendAtt.colorWriteMask = 0xF;
	blendAtt.blendEnable = VK_FALSE;

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

void WaterPass::destroyPipeline()
{
	if (m_context && m_pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(m_context->getDevice(), m_pipeline, nullptr);
	m_pipeline = VK_NULL_HANDLE;
}

void WaterPass::writeSceneDescriptors(VkDescriptorSet set2, VkSampler sampler)
{
	if (set2 == VK_NULL_HANDLE || m_sceneHistory.view == VK_NULL_HANDLE)
		return;
	VkDescriptorImageInfo colorInfo{};
	colorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	colorInfo.imageView = m_sceneHistory.view;
	colorInfo.sampler = sampler != VK_NULL_HANDLE ? sampler : m_sceneSampler;
	VkDescriptorImageInfo depthInfo = colorInfo;
	depthInfo.imageView = m_depthHistory.view != VK_NULL_HANDLE ? m_depthHistory.view : m_sceneHistory.view;

	VkWriteDescriptorSet writes[2]{};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = set2;
	writes[0].dstBinding = 0;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[0].descriptorCount = 1;
	writes[0].pImageInfo = &colorInfo;
	writes[1] = writes[0];
	writes[1].dstBinding = 1;
	writes[1].pImageInfo = &depthInfo;
	vkUpdateDescriptorSets(m_context->getDevice(), 2, writes, 0, nullptr);
}

void WaterPass::createIndirectBuffers()
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

void WaterPass::destroyIndirectBuffers()
{
	for (IndirectBatch &b : m_indirect)
	{
		if (b.buf.buffer != VK_NULL_HANDLE)
			destroyBuffer(m_context->getAllocator(), b.buf);
		b = {};
	}
}

void WaterPass::record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent, VkDescriptorSet set0, VkDescriptorSet set1,
					   VkDescriptorSet set2, VkPipelineLayout layout, AllocatedImage &hdr,
					   AllocatedImage &liveDepth, const std::vector<Chunk *> &chunks, const glm::vec3 &camPos,
					   const MeshArenas &arenas, VoxelDrawData *drawDataOut, AllocatedBuffer &drawDataBuffer)
{
	const auto beginRendering = beginR();
	const auto endRendering = endR();

	// Copy opaque HDR + depth → history
	VkImageMemoryBarrier before[4] = {
		vkbar::makeBarrier(hdr.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT),
		vkbar::makeBarrier(m_sceneHistory.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
						   VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT),
		vkbar::makeBarrier(liveDepth.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
						   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
						   VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT),
		vkbar::makeBarrier(m_depthHistory.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
						   VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT),
	};
	vkbar::cmdBarriers(cmd, before, 4,
					   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
					   VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkImageCopy colorCopy{};
	colorCopy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	colorCopy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	colorCopy.extent = {extent.width, extent.height, 1};
	vkCmdCopyImage(cmd, hdr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_sceneHistory.image,
				   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &colorCopy);

	VkImageCopy depthCopy{};
	depthCopy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
	depthCopy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
	depthCopy.extent = {extent.width, extent.height, 1};
	vkCmdCopyImage(cmd, liveDepth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_depthHistory.image,
				   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depthCopy);

	VkImageMemoryBarrier after[4] = {
		vkbar::makeBarrier(m_sceneHistory.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
						   VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT),
		vkbar::makeBarrier(m_depthHistory.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
						   VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT),
		vkbar::makeBarrier(hdr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						   VK_ACCESS_TRANSFER_READ_BIT,
						   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
						   VK_IMAGE_ASPECT_COLOR_BIT),
		vkbar::makeBarrier(liveDepth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
						   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
						   VK_IMAGE_ASPECT_DEPTH_BIT),
	};
	vkbar::cmdBarriers(cmd, after, 4, VK_PIPELINE_STAGE_TRANSFER_BIT,
					   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
						   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);

	VkRenderingAttachmentInfo colorAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	colorAtt.imageView = hdr.view;
	colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

	VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	depthAtt.imageView = liveDepth.view;
	depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

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

	VkDescriptorSet sets[3] = {set0, set1, set2};
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 3, sets, 0, nullptr);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
	const float invScreen[4] = {1.f / static_cast<float>(std::max(1u, extent.width)),
								1.f / static_cast<float>(std::max(1u, extent.height)), 0.f, 0.f};
	vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(invScreen), invScreen);

	// Water draws are still emitted back-to-front and must not be reordered
	// by page pair (unlike opaque / shadow): with depth write + no blending
	// the depth test alone decides visibility, but keeping the historical
	// order makes the depth results independent of sorting changes and lets a
	// future transparency mode re-enable strict ordering. Only contiguous
	// same-page-pair runs are split out for binding.
	m_waterChunks.clear();
	const float camOffsetX = camPos.x - CHUNK_SIZE * 0.5f;
	const float camOffsetZ = camPos.z - CHUNK_SIZE * 0.5f;
	const float camOffsetY = camPos.y;
	for (Chunk *chunk : chunks)
	{
		if (chunk && chunk->hasRenderableWaterDraws())
		{
			const glm::vec3 p = chunk->getPosition();
			const glm::vec3 d(p.x - camOffsetX, p.y - camOffsetY, p.z - camOffsetZ);
			m_waterChunks.push_back({chunk, glm::dot(d, d)});
		}
	}
	std::sort(m_waterChunks.begin(), m_waterChunks.end(),
			  [](const WaterEntry &a, const WaterEntry &b) { return a.dist2 > b.dist2; });

	if (m_scratch.size() < kMaxIndirectCommands)
		m_scratch.resize(kMaxIndirectCommands);
	size_t demandedCount = 0;  // commands requested (pre-truncation)
	size_t scratchCount = 0;   // commands actually emitted this frame
	Chunk::IndirectDraw *outPtr = m_scratch.data();
	for (const auto &e : m_waterChunks)
	{
		const uint32_t cCount = e.chunk->getCachedWaterDrawCount();
		demandedCount += cCount;
		if (scratchCount + cCount <= kMaxIndirectCommands)
		{
			std::memcpy(outPtr + scratchCount, e.chunk->cachedWaterDraws(), cCount * sizeof(Chunk::IndirectDraw));
			scratchCount += cCount;
		}
		else
		{
			static bool warned = false;
			if (!warned)
			{
				warned = true;
				std::cerr << "[indirect] water command capacity exceeded: demanded=" << demandedCount
				          << " emitted=" << scratchCount << " capacity=" << kMaxIndirectCommands
				          << " chunkCommands=" << cCount << " - chunk skipped" << std::endl;
			}
		}
	}
	m_lastCommands = static_cast<uint32_t>(std::min<size_t>(demandedCount, UINT32_MAX));
	if (scratchCount > 0)
	{
		const size_t count = scratchCount;
		// Computed once per record: batches obey multiDrawIndirect and
		// maxDrawIndirectCount (see IndirectDrawUtils.hpp).
		const uint32_t batchLimit = indirectBatchLimit(
			m_context->hasMultiDrawIndirect(), m_context->maxDrawIndirectCount());
		const uint32_t baseInstance = voxel_draw::kWaterBase;
		assert(baseInstance + count <= voxel_draw::kEntryCount);
		auto *dst = static_cast<VkDrawIndexedIndirectCommand *>(m_indirect[frameIndex].mapped);
		for (size_t i = 0; i < count; ++i)
		{
			dst[i] = m_scratch[i].cmd;
			dst[i].firstInstance = static_cast<uint32_t>(baseInstance + i);
			if (drawDataOut)
			{
				drawDataOut[baseInstance + i].worldOrigin = m_scratch[i].chunkOrigin;
				drawDataOut[baseInstance + i].flags = 0;
			}
		}
		vmaFlushAllocation(m_context->getAllocator(), m_indirect[frameIndex].buf.allocation, 0,
						   count * sizeof(VkDrawIndexedIndirectCommand));
		if (drawDataOut)
		{
			vmaFlushAllocation(m_context->getAllocator(), drawDataBuffer.allocation,
							   baseInstance * sizeof(VoxelDrawData), count * sizeof(VoxelDrawData));
		}

		size_t i = 0;
		uint32_t first = 0;
		while (i < count)
		{
			const uint64_t key = (uint64_t(m_scratch[i].vertexPage) << 32) | m_scratch[i].indexPage;
			size_t j = i;
			while (j < count &&
				   (uint64_t(m_scratch[j].vertexPage) << 32 | m_scratch[j].indexPage) == key)
				++j;
			VkBuffer vb = arenas.waterVertex.pageBuffer(static_cast<uint32_t>(key >> 32));
			VkBuffer ib = arenas.waterIndex.pageBuffer(static_cast<uint32_t>(key & 0xffffffffu));
			VkDeviceSize voff = 0;
			vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &voff);
			vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
			// One call per batch inside the contiguous run: the back-to-front
			// order is untouched, the run is only split because of the
			// hardware batch ceiling (or per command without
			// multiDrawIndirect).
			issueIndirectRange(cmd, m_indirect[frameIndex].buf.buffer, first, j - i,
							   batchLimit);
			telemetry::registry().add(telemetry::ArenaBinds);
			first += static_cast<uint32_t>(j - i);
			i = j;
		}
		telemetry::registry().add(telemetry::WaterDraws, count);
	}

	// The dynamic rendering scope is ALWAYS closed, even when no water
	// commands were collected: leaving it open corrupts every subsequent
	// pass in the frame graph (missing faces across the world).
	endRendering(cmd);

	// Water -> Sky handoff. Separate dynamic-rendering instances have no
	// implicit ordering (vkCmdEndRendering only ends the scope), and the sky
	// pass re-uses this same depth attachment with LESS_OR_EQUAL + loadOp
	// LOAD — it must observe the depth the water pass just wrote, or distant
	// water gets overwritten by sky again. Same-class hazard on the color
	// attachment the sky keeps rendering into.
	VkImageMemoryBarrier handoff[2] = {
		vkbar::makeBarrier(hdr.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
						   VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
						   VK_IMAGE_ASPECT_COLOR_BIT),
		vkbar::makeBarrier(liveDepth.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
						   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
						   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
						   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT),
	};
	vkbar::cmdBarriers(cmd, handoff, 2,
					   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
						   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
						   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
					   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
						   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
						   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
}
