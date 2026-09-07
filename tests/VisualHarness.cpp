#include "VisualHarness.hpp"

#include "Vulkan/ImageBarrier.hpp"
#include "Vulkan/VkLoadLibrary.hpp"
#include "Chunk/StreamHelpers.hpp"
#include "utils.hpp"

#include <SDL3/SDL.h>

#include <cstring>
#include <stdexcept>

bool VisualHarness::initDevice(std::ostream &log, uint32_t width, uint32_t height)
{
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		log << "SKIP: SDL video init failed: " << SDL_GetError() << "\n";
		return false;
	}
	if (!loadVulkanLibrary(&log))
	{
		log << "SKIP: no Vulkan loader available\n";
		return false;
	}
	m_window = SDL_CreateWindow("ft_vox visual tests", width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
	if (!m_window)
	{
		log << "SKIP: window creation failed: " << SDL_GetError() << "\n";
		return false;
	}
	try
	{
		m_context.init(m_window);
	}
	catch (const std::exception &e)
	{
		log << "SKIP: Vulkan device unavailable (" << e.what() << ")\n";
		return false;
	}

	m_swapchain.init(m_context, width, height, true);
	m_extent = m_swapchain.getExtent();
	m_targetFormat = m_swapchain.getImageFormat();

	// Golden-image contract (review P2): fixed resolution + 8-bit sRGB
	// composite. Anything else would compare against references produced
	// under a different resolution/encoding.
	if (m_extent.width != width || m_extent.height != height)
	{
		log << "SKIP: surface caps force extent " << m_extent.width << "x" << m_extent.height
			<< ", the golden contract requires " << width << "x" << height << "\n";
		return false;
	}
	switch (m_targetFormat)
	{
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_B8G8R8A8_SRGB:
		break;
	default:
		log << "SKIP: swapchain chose non-sRGB8 format " << m_targetFormat
			<< ", the golden contract requires R8G8B8A8_SRGB/B8G8R8A8_SRGB\n";
		return false;
	}

	m_imm.init(m_context);
	m_retire.init(m_context.getAllocator(), WorldRenderer::kMaxFramesInFlight);
	m_deviceReady = true;
	log << "VisualHarness device ready: " << m_extent.width << "x" << m_extent.height
		<< " format=" << m_targetFormat << " device=" << m_context.getDeviceProperties().deviceName << "\n";
	return true;
}

void VisualHarness::initRenderer(std::ostream &log)
{
	// Visual scenes never use async jobs (generateInitialArea is fully
	// synchronous); a small pool keeps ChunkManager's contract satisfied.
	m_threadPool = std::make_unique<ThreadPool>(2);
	m_chunkPool = std::make_unique<ChunkPool>(estimateChunkPoolCapacity(512));
	m_renderer.init(m_context, m_swapchain, m_imm, m_retire, "");

	// Offscreen composite target: same format the swapchain chose so the
	// bytes match what the on-screen path would present. Transfer source for
	// the test-only readback. The HDR target is copied in the same submit
	// for the pre-tonemap NaN/Inf scan (fp16, 8 bytes per pixel).
	auto device = m_context.getDevice();
	auto allocator = m_context.getAllocator();
	m_target = createImage2D(allocator, device, m_extent.width, m_extent.height, m_targetFormat,
							 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	m_readback = createBuffer(allocator, size_t(m_extent.width) * m_extent.height * 4,
							  VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
							  VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
	m_hdrReadback =
		createBuffer(allocator, size_t(m_extent.width) * m_extent.height * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
					 VMA_MEMORY_USAGE_AUTO,
					 VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
	m_gpu.init(m_context, WorldRenderer::kMaxFramesInFlight); // both FIF slots may be driven
	m_rendererReady = true;
	log << "VisualHarness renderer ready (Shadow/Opaque/Water/Sky + post)\n";
}

void VisualHarness::shutdown()
{
	if (!m_window)
		return;
	if (m_deviceReady)
		m_context.waitIdle();
	m_gpu.shutdown();
	m_chunkManager.reset();
	m_terrain.reset();
	if (m_rendererReady)
		m_renderer.shutdown();
	if (m_target.image)
		destroyImage(m_context.getAllocator(), m_context.getDevice(), m_target);
	if (m_readback.buffer)
		destroyBuffer(m_context.getAllocator(), m_readback);
	if (m_hdrReadback.buffer)
		destroyBuffer(m_context.getAllocator(), m_hdrReadback);
	m_retire.flush();
	m_retire.shutdown();
	m_threadPool.reset();
	m_chunkPool.reset();
	if (m_deviceReady)
	{
		m_imm.shutdown();
		m_swapchain.shutdown();
	}
	// Destroy device/instance before SDL unloads vulkan-1.dll: VkContext's
	// destructor would otherwise run after SDL_Vulkan_UnloadLibrary and call
	// volk entry points into an unloaded module (access violation at exit).
	m_context.shutdown();
	SDL_DestroyWindow(m_window);
	SDL_Vulkan_UnloadLibrary();
	SDL_Quit();
	m_window = nullptr;
}

autoexposure::ExposureGpuState VisualHarness::exposureMeterProbe(
	const VkClearColorValue &full, const VkClearColorValue *leftQuarter,
	const PostProcessSettings &settings)
{
	assert(m_rendererReady);
	AllocatedImage &hdr = m_renderer.hdrColor();
	VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

	// Paint pass: SHADER_READ (post-frame invariant) -> COLOR_ATTACHMENT,
	// full-frame clear + optional left-quarter overlay via clear attachments
	// (the HDR target has no TRANSFER_DST usage, so no transfer clears).
	m_imm.submitAndWait([&](VkCommandBuffer cmd) {
		cmdTransitionImageLayout(cmd, hdr.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
									 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		ca.imageView = hdr.view;
		ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
		ri.renderArea = {{0, 0}, m_extent};
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &ca;
		// volk loads core-1.3 entry points only for a 1.3 instance; the
		// engine targets 1.2 + VK_KHR_dynamic_rendering (same fallback
		// pattern as PostStack::beginR/endR).
		auto beginRendering = vkCmdBeginRendering ? vkCmdBeginRendering : vkCmdBeginRenderingKHR;
		auto endRendering = vkCmdEndRendering ? vkCmdEndRendering : vkCmdEndRenderingKHR;
		beginRendering(cmd, &ri);
		VkClearRect rects[2] = {
			{{{0, 0}, {m_extent.width, m_extent.height}}, 0, 1},
			{{{0, 0}, {m_extent.width / 4, m_extent.height}}, 0, 1},
		};
		VkClearAttachment att{};
		att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		att.colorAttachment = 0;
		att.clearValue.color = full;
		vkCmdClearAttachments(cmd, 1, &att, 1, rects);
		if (leftQuarter)
		{
			att.clearValue.color = *leftQuarter;
			vkCmdClearAttachments(cmd, 1, &att, 1, &rects[1]);
		}
		endRendering(cmd);
		cmdTransitionImageLayout(cmd, hdr.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
									 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});

	// Probe passes (own submits, dt = 0 leaves the history untouched): the
	// record-time readout copy of pass B observes the state pass A wrote
	// (pass A's submit has completed by the time B is recorded).
	m_imm.submitAndWait([&](VkCommandBuffer cmd) {
		m_renderer.recordExposureProbe(cmd, m_frameSlot, settings);
	});
	m_imm.submitAndWait([&](VkCommandBuffer cmd) {
		m_renderer.recordExposureProbe(cmd, m_frameSlot, settings);
	});
	return m_renderer.exposureReadout();
}

void VisualHarness::beginScene(int seed)
{
	if (m_deviceReady)
		m_context.waitIdle();
	m_retire.flush();
	m_drawList.clear();
	m_shadowList.clear();
	m_chunkManager.reset();
	m_terrain = std::make_unique<TerrainGenerator>(seed);
	m_chunkManager = std::make_unique<ChunkManager>(m_terrain.get(), m_threadPool.get(), m_chunkPool.get());
}

void VisualHarness::buildArea(const glm::vec3 &center, int radiusChunks)
{
	m_chunkManager->generateInitialArea(center, radiusChunks, m_context.getAllocator(), m_imm,
										m_renderer.arenas());
}

void VisualHarness::remeshEditedChunks()
{
	for (Chunk *chunk : m_chunkManager->getActiveChunks())
	{
		if (chunk->getState() != ChunkState::GENERATED)
			continue;
		if (!chunk->generateMesh())
			throw std::runtime_error("visual harness: generateMesh failed during remesh");
		chunk->uploadToGPU(m_context.getAllocator(), m_imm, m_renderer.arenas());
	}
}

visual::RgbaImage VisualHarness::renderFrame(float time, const std::vector<entities::MobRenderState> &mobs)
{
	if (!m_rendererReady)
		return {};
	m_lastNonFinite = 0;
	m_renderer.setMobs(mobs);
	const float farPlane = m_renderSettings.maxRenderDistance * 1.25f;
	const bool underwater = m_renderer.postSettings().underwater;
	m_renderer.updateFrameUBO(m_frameSlot, m_camera, static_cast<float>(m_extent.width),
							  static_cast<float>(m_extent.height), farPlane, time, m_shader,
							  m_renderSettings.shadowCascadeFar, underwater);

	m_chunkManager->updateVisibility(m_camera, static_cast<int>(m_extent.width),
									 static_cast<int>(m_extent.height), m_renderSettings);
	m_drawList.clear();
	m_shadowList.clear();
	m_chunkManager->collectDrawList(m_drawList);
	m_chunkManager->collectShadowList(m_shadowList, m_camera, m_renderSettings.shadowDistance);

	const uint64_t frame = m_frameCounter++;
	m_renderer.arenas().beginFrame(frame);
	m_retire.beginFrame(frame);

	const VkClearColorValue clearColor{{0.38f, 0.58f, 0.92f, 1.0f}};
	m_imm.submitAndWait([&](VkCommandBuffer cmd) {
		m_renderer.recordFrameToImage(cmd, m_frameSlot, m_target.image, m_target.view, m_extent, m_drawList,
									  m_shadowList, clearColor, {}, &m_gpu);
		// LDR composite readback.
		cmdTransitionImageLayout(cmd, m_target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		VkBufferImageCopy region{};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {m_extent.width, m_extent.height, 1};
		vkCmdCopyImageToBuffer(cmd, m_target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_readback.buffer, 1,
							   &region);
		// HDR readback for the pre-tonemap NaN/Inf scan: recordPost leaves
		// the HDR target SHADER_READ_ONLY_OPTIMAL (PostStack composite).
		vkbar::cmdTransitionColor(cmd, m_renderer.hdrColor().image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
								  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
								  VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
								  VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy hdrRegion{};
		hdrRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		hdrRegion.imageExtent = {m_extent.width, m_extent.height, 1};
		vkCmdCopyImageToBuffer(cmd, m_renderer.hdrColor().image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
							   m_hdrReadback.buffer, 1, &hdrRegion);
		vkbar::cmdTransitionColor(cmd, m_renderer.hdrColor().image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
								  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
								  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
								  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
							 nullptr, 0, nullptr);
	});

	m_gpu.markSubmitted(m_frameSlot);
	m_gpu.onSlotReady(m_frameSlot);
	auto scanNonFinite = [this](const AllocatedBuffer &buffer, size_t halfCount) {
		vmaInvalidateAllocation(m_context.getAllocator(), buffer.allocation, 0, VK_WHOLE_SIZE);
		auto *data = static_cast<const uint16_t *>(buffer.info.pMappedData);
		long long bad = 0;
		// fp16 NaN/Inf: exponent bits all set (sign ignored).
		for (size_t i = 0; i < halfCount; ++i)
			if ((data[i] & 0x7C00u) == 0x7C00u)
				++bad;
		return bad;
	};
	m_lastNonFinite = scanNonFinite(m_hdrReadback, size_t(m_extent.width) * m_extent.height * 4);
	vmaInvalidateAllocation(m_context.getAllocator(), m_readback.allocation, 0, VK_WHOLE_SIZE);
	auto *data = static_cast<uint8_t *>(m_readback.info.pMappedData);

	visual::RgbaImage image;
	image.width = m_extent.width;
	image.height = m_extent.height;
	image.pixels.assign(data, data + size_t(m_extent.width) * m_extent.height * 4);
	if (m_targetFormat == VK_FORMAT_B8G8R8A8_SRGB || m_targetFormat == VK_FORMAT_B8G8R8A8_UNORM)
		visual::swizzleBgraToRgba(image.pixels);
	return image;
}
