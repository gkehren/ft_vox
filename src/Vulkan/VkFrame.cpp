#include "Vulkan/VkFrame.hpp"

#include <stdexcept>

VkFrameContext::~VkFrameContext()
{
	shutdown();
}

void VkFrameContext::init(VkContext &context)
{
	m_context = &context;
	VkDevice device = m_context->getDevice();

	for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
	{
		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = m_context->getGraphicsQueueFamily();
		if (vkCreateCommandPool(device, &poolInfo, nullptr, &m_frames[i].commandPool) != VK_SUCCESS)
			throw std::runtime_error("Failed to create command pool");

		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = m_frames[i].commandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(device, &allocInfo, &m_frames[i].commandBuffer) != VK_SUCCESS)
			throw std::runtime_error("Failed to allocate command buffer");

		VkSemaphoreCreateInfo semInfo{};
		semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		if (vkCreateSemaphore(device, &semInfo, nullptr, &m_frames[i].imageAvailable) != VK_SUCCESS ||
			vkCreateSemaphore(device, &semInfo, nullptr, &m_frames[i].renderFinished) != VK_SUCCESS)
			throw std::runtime_error("Failed to create frame semaphores");

		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		if (vkCreateFence(device, &fenceInfo, nullptr, &m_frames[i].inFlight) != VK_SUCCESS)
			throw std::runtime_error("Failed to create frame fence");
	}
}

void VkFrameContext::shutdown()
{
	if (!m_context || m_context->getDevice() == VK_NULL_HANDLE)
	{
		m_context = nullptr;
		return;
	}

	m_context->waitIdle();
	VkDevice device = m_context->getDevice();

	for (auto &frame : m_frames)
	{
		if (frame.inFlight != VK_NULL_HANDLE)
			vkDestroyFence(device, frame.inFlight, nullptr);
		if (frame.imageAvailable != VK_NULL_HANDLE)
			vkDestroySemaphore(device, frame.imageAvailable, nullptr);
		if (frame.renderFinished != VK_NULL_HANDLE)
			vkDestroySemaphore(device, frame.renderFinished, nullptr);
		if (frame.commandPool != VK_NULL_HANDLE)
			vkDestroyCommandPool(device, frame.commandPool, nullptr);
		frame = {};
	}

	m_imagesInFlight.clear();
	m_context = nullptr;
	m_currentFrame = 0;
}

bool VkFrameContext::beginFrame(VkSwapchain &swapchain, uint32_t &outImageIndex)
{
	VkDevice device = m_context->getDevice();
	FrameData &frame = m_frames[m_currentFrame];

	vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);

	VkResult result = vkAcquireNextImageKHR(
		device,
		swapchain.getSwapchain(),
		UINT64_MAX,
		frame.imageAvailable,
		VK_NULL_HANDLE,
		&outImageIndex);

	if (result == VK_ERROR_OUT_OF_DATE_KHR)
		return false;
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
		throw std::runtime_error("Failed to acquire swapchain image");

	// Ensure we do not start writing an image that is still in flight
	if (m_imagesInFlight.size() != swapchain.getImageCount())
		m_imagesInFlight.assign(swapchain.getImageCount(), VK_NULL_HANDLE);

	if (m_imagesInFlight[outImageIndex] != VK_NULL_HANDLE)
		vkWaitForFences(device, 1, &m_imagesInFlight[outImageIndex], VK_TRUE, UINT64_MAX);
	m_imagesInFlight[outImageIndex] = frame.inFlight;

	vkResetFences(device, 1, &frame.inFlight);
	vkResetCommandBuffer(frame.commandBuffer, 0);
	return true;
}

void VkFrameContext::recordClearCommands(VkCommandBuffer cmd, VkSwapchain &swapchain, uint32_t imageIndex,
										 const VkClearColorValue &clearColor)
{
	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
		throw std::runtime_error("Failed to begin command buffer");

	VkImage image = swapchain.getImages()[imageIndex];
	const VkExtent2D extent = swapchain.getExtent();

	// Undefined → transfer dst
	VkImageMemoryBarrier toTransfer{};
	toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransfer.image = image;
	toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	toTransfer.subresourceRange.baseMipLevel = 0;
	toTransfer.subresourceRange.levelCount = 1;
	toTransfer.subresourceRange.baseArrayLayer = 0;
	toTransfer.subresourceRange.layerCount = 1;
	toTransfer.srcAccessMask = 0;
	toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

	vkCmdPipelineBarrier(
		cmd,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &toTransfer);

	VkImageSubresourceRange range{};
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.baseMipLevel = 0;
	range.levelCount = 1;
	range.baseArrayLayer = 0;
	range.layerCount = 1;
	vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

	// Transfer dst → present
	VkImageMemoryBarrier toPresent = toTransfer;
	toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toPresent.dstAccessMask = 0;

	vkCmdPipelineBarrier(
		cmd,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &toPresent);

	(void)extent;
	if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
		throw std::runtime_error("Failed to end command buffer");
}

bool VkFrameContext::endFrameClearAndPresent(VkSwapchain &swapchain, uint32_t imageIndex,
											 const VkClearColorValue &clearColor)
{
	FrameData &frame = m_frames[m_currentFrame];
	recordClearCommands(frame.commandBuffer, swapchain, imageIndex, clearColor);

	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &frame.imageAvailable;
	submitInfo.pWaitDstStageMask = &waitStage;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &frame.commandBuffer;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &frame.renderFinished;

	if (vkQueueSubmit(m_context->getGraphicsQueue(), 1, &submitInfo, frame.inFlight) != VK_SUCCESS)
		throw std::runtime_error("Failed to submit draw command buffer");

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &frame.renderFinished;
	VkSwapchainKHR sc = swapchain.getSwapchain();
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &sc;
	presentInfo.pImageIndices = &imageIndex;

	VkResult result = vkQueuePresentKHR(m_context->getPresentQueue(), &presentInfo);

	m_currentFrame = (m_currentFrame + 1) % kMaxFramesInFlight;

	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
		return false;
	if (result != VK_SUCCESS)
		throw std::runtime_error("Failed to present swapchain image");
	return true;
}
