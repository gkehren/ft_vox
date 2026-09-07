#include "VisualHarness.hpp"

#include "Vulkan/VkLoadLibrary.hpp"
#include "Chunk/StreamHelpers.hpp"
#include "utils.hpp"

#include <SDL3/SDL.h>

#include <cstring>
#include <stdexcept>

bool VisualHarness::init(std::ostream &log)
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
	m_window = SDL_CreateWindow("ft_vox visual tests", 640, 360, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
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

	m_swapchain.init(m_context, 640, 360, true);
	m_extent = m_swapchain.getExtent();
	m_imm.init(m_context);
	m_retire.init(m_context.getAllocator(), WorldRenderer::kMaxFramesInFlight);

	// Visual scenes never use async jobs (generateInitialArea is fully
	// synchronous); a small pool keeps ChunkManager's contract satisfied.
	m_threadPool = std::make_unique<ThreadPool>(2);
	m_chunkPool = std::make_unique<ChunkPool>(estimateChunkPoolCapacity(512));
	m_renderer.init(m_context, m_swapchain, m_imm, m_retire, "");

	// Offscreen composite target: same format the swapchain chose so the
	// bytes match what the on-screen path would present. Transfer source for
	// the test-only readback.
	m_targetFormat = m_swapchain.getImageFormat();
	auto device = m_context.getDevice();
	auto allocator = m_context.getAllocator();
	m_target = createImage2D(allocator, device, m_extent.width, m_extent.height, m_targetFormat,
							 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	m_readback = createBuffer(allocator, size_t(m_extent.width) * m_extent.height * 4,
							  VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
							  VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);

	m_ready = true;
	log << "VisualHarness ready: " << m_extent.width << "x" << m_extent.height
		<< " format=" << m_targetFormat << " device=" << m_context.getDeviceProperties().deviceName << "\n";
	return true;
}

void VisualHarness::shutdown()
{
	if (!m_window)
		return;
	if (m_ready)
		m_context.waitIdle();
	m_chunkManager.reset();
	m_terrain.reset();
	if (m_ready)
		m_renderer.shutdown();
	if (m_target.image)
		destroyImage(m_context.getAllocator(), m_context.getDevice(), m_target);
	if (m_readback.buffer)
		destroyBuffer(m_context.getAllocator(), m_readback);
	m_retire.flush();
	m_retire.shutdown();
	m_threadPool.reset();
	m_chunkPool.reset();
	if (m_ready)
	{
		m_imm.shutdown();
		m_swapchain.shutdown();
	}
	SDL_DestroyWindow(m_window);
	SDL_Vulkan_UnloadLibrary();
	SDL_Quit();
	m_window = nullptr;
}

void VisualHarness::beginScene(int seed)
{
	if (m_ready)
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
	if (!m_ready)
		return {};
	m_renderer.setMobs(mobs);
	const float farPlane = m_renderSettings.maxRenderDistance * 1.25f;
	const bool underwater = m_renderer.postSettings().underwater;
	m_renderer.updateFrameUBO(0, m_camera, static_cast<float>(m_extent.width),
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
		m_renderer.recordFrameToImage(cmd, 0, m_target.image, m_target.view, m_extent, m_drawList,
									  m_shadowList, clearColor, {}, nullptr);
		cmdTransitionImageLayout(cmd, m_target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		VkBufferImageCopy region{};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {m_extent.width, m_extent.height, 1};
		vkCmdCopyImageToBuffer(cmd, m_target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_readback.buffer, 1,
							   &region);
		VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
							 nullptr, 0, nullptr);
	});

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
