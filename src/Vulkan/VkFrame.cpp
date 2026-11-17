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
	m_gpuProfiler.init(context, kMaxFramesInFlight);

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
		if (vkCreateSemaphore(device, &semInfo, nullptr,
							  &m_frames[i].imageAvailable) != VK_SUCCESS)
			throw std::runtime_error("Failed to create frame semaphores");

		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		if (vkCreateFence(device, &fenceInfo, nullptr, &m_frames[i].inFlight) != VK_SUCCESS)
			throw std::runtime_error("Failed to create frame fence");
	}

	// Inter-submission chain (review M1), created unsignaled: the first
	// submit only signals it, every later submit waits then re-signals.
	VkSemaphoreCreateInfo chainInfo{};
	chainInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	if (vkCreateSemaphore(device, &chainInfo, nullptr, &m_prevFrameDone) != VK_SUCCESS)
		throw std::runtime_error("Failed to create frame-chain semaphore");
	m_hasSubmitted = false;
}

void VkFrameContext::resetFrameChain()
{
	// Called after a device idle (swapchain recreate): start a fresh chain
	// so the next first-submit never waits on a stale signal state.
	if (!m_context || m_context->getDevice() == VK_NULL_HANDLE)
		return;
	if (m_prevFrameDone != VK_NULL_HANDLE)
		vkDestroySemaphore(m_context->getDevice(), m_prevFrameDone, nullptr);
	VkSemaphoreCreateInfo chainInfo{};
	chainInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	if (vkCreateSemaphore(m_context->getDevice(), &chainInfo, nullptr, &m_prevFrameDone) != VK_SUCCESS)
		throw std::runtime_error("Failed to recreate frame-chain semaphore");
	m_hasSubmitted = false;
}

void VkFrameContext::shutdown()
{
	if (!m_context || m_context->getDevice() == VK_NULL_HANDLE)
	{
		m_context = nullptr;
		return;
	}

	m_context->waitIdle();
	m_gpuProfiler.shutdown();
	VkDevice device = m_context->getDevice();

	for (auto &frame : m_frames)
	{
		if (frame.inFlight != VK_NULL_HANDLE)
			vkDestroyFence(device, frame.inFlight, nullptr);
		if (frame.imageAvailable != VK_NULL_HANDLE)
			vkDestroySemaphore(device, frame.imageAvailable, nullptr);
		if (frame.commandPool != VK_NULL_HANDLE)
			vkDestroyCommandPool(device, frame.commandPool, nullptr);
		frame = {};
	}
	for (VkSemaphore semaphore : m_renderFinishedByImage)
		if (semaphore != VK_NULL_HANDLE)
			vkDestroySemaphore(device, semaphore, nullptr);
	if (m_prevFrameDone != VK_NULL_HANDLE)
		vkDestroySemaphore(device, m_prevFrameDone, nullptr);
	m_prevFrameDone = VK_NULL_HANDLE;
	m_hasSubmitted = false;

	m_imagesInFlight.clear();
	m_renderFinishedByImage.clear();
	m_context = nullptr;
	m_currentFrame = 0;
}

void VkFrameContext::ensureSwapchainImageSync(uint32_t imageCount)
{
	if (m_renderFinishedByImage.size() == imageCount)
		return;

	// A size change means swapchain recreation. Engine recreation waits the
	// device, but waiting here keeps this owner correct for test/legacy paths.
	m_context->waitIdle();
	VkDevice device = m_context->getDevice();
	for (VkSemaphore semaphore : m_renderFinishedByImage)
		if (semaphore != VK_NULL_HANDLE)
			vkDestroySemaphore(device, semaphore, nullptr);
	m_renderFinishedByImage.assign(imageCount, VK_NULL_HANDLE);

	VkSemaphoreCreateInfo semInfo{};
	semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	for (VkSemaphore &semaphore : m_renderFinishedByImage)
	{
		if (vkCreateSemaphore(device, &semInfo, nullptr, &semaphore) !=
			VK_SUCCESS)
			throw std::runtime_error(
				"Failed to create per-swapchain-image semaphore");
	}
}

void VkFrameContext::onSwapchainRecreate(uint32_t imageCount)
{
	// Engine recreation has already waited for the device. Fence handles remain
	// valid, but their association with images from the old swapchain does not.
	m_imagesInFlight.assign(imageCount, VK_NULL_HANDLE);
	ensureSwapchainImageSync(imageCount);
	// Fresh inter-submission chain after the idle (review M1).
	resetFrameChain();
}

bool VkFrameContext::beginFrame(VkSwapchain &swapchain, uint32_t &outImageIndex)
{
	VkDevice device = m_context->getDevice();
	FrameData &frame = m_frames[m_currentFrame];

	if (frame.inFlight == VK_NULL_HANDLE)
		throw std::runtime_error("Frame fence unavailable (previous submit failure was not recoverable)");
	// Checked (review §4.7): on VK_ERROR_DEVICE_LOST the fence is NOT
	// signaled; treating it as ready would reset and reuse corrupt state.
	if (vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
		throw std::runtime_error("Failed to wait for frame fence (device lost?)");
	m_gpuProfiler.onSlotReady(m_currentFrame);

	VkResult result = vkAcquireNextImageKHR(device, swapchain.getSwapchain(), UINT64_MAX, frame.imageAvailable,
											VK_NULL_HANDLE, &outImageIndex);

	if (result == VK_ERROR_OUT_OF_DATE_KHR)
		return false;
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
		throw std::runtime_error("Failed to acquire swapchain image");

	if (m_imagesInFlight.size() != swapchain.getImageCount())
		m_imagesInFlight.assign(swapchain.getImageCount(), VK_NULL_HANDLE);
	ensureSwapchainImageSync(swapchain.getImageCount());

	if (m_imagesInFlight[outImageIndex] != VK_NULL_HANDLE &&
		vkWaitForFences(device, 1, &m_imagesInFlight[outImageIndex], VK_TRUE, UINT64_MAX) != VK_SUCCESS)
		throw std::runtime_error("Failed to wait for swapchain-image fence (device lost?)");
	m_imagesInFlight[outImageIndex] = frame.inFlight;

	vkResetFences(device, 1, &frame.inFlight);
	vkResetCommandBuffer(frame.commandBuffer, 0);
	return true;
}

bool VkFrameContext::submitAndPresent(VkSwapchain &swapchain, uint32_t imageIndex)
{
	FrameData &frame = m_frames[m_currentFrame];
	VkSemaphore renderFinished = m_renderFinishedByImage.at(imageIndex);

	// Inter-submission chain (review M1): wait on the previous submit's
	// completion at TOP_OF_PIPE so no GPU work of this frame (shadow map
	// writes included) can overlap the previous frame's reads of the shared
	// global targets. The acquire semaphore keeps its precise late stage.
	const VkSemaphore waitSemaphores[2] = {frame.imageAvailable, m_prevFrameDone};
	const VkPipelineStageFlags waitStages[2] = {
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
	const VkSemaphore signalSemaphores[2] = {renderFinished, m_prevFrameDone};
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = m_hasSubmitted ? 2u : 1u;
	submit.pWaitSemaphores = waitSemaphores;
	submit.pWaitDstStageMask = waitStages;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &frame.commandBuffer;
	submit.signalSemaphoreCount = 2;
	submit.pSignalSemaphores = signalSemaphores;

	const VkResult submitResult = vkQueueSubmit(m_context->getGraphicsQueue(), 1, &submit, frame.inFlight);
	if (submitResult != VK_SUCCESS)
	{
		// A failed submit never signals the fence (review §4.7): a caller
		// that recovers from the error would block UINT64_MAX in the next
		// beginFrame on this slot. Recreate the fence SIGNALED and drop
		// every stale image association with the old handle first.
		for (VkFence &fence : m_imagesInFlight)
			if (fence == frame.inFlight)
				fence = VK_NULL_HANDLE;
		vkDestroyFence(m_context->getDevice(), frame.inFlight, nullptr);
		frame.inFlight = VK_NULL_HANDLE;
		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		if (vkCreateFence(m_context->getDevice(), &fenceInfo, nullptr, &frame.inFlight) != VK_SUCCESS)
			frame.inFlight = VK_NULL_HANDLE; // device lost: nothing left to salvage
		throw std::runtime_error("Failed to submit frame command buffer");
	}
	m_hasSubmitted = true;

	m_gpuProfiler.markSubmitted(m_currentFrame);
	VkPresentInfoKHR present{};
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &renderFinished;
	VkSwapchainKHR sc = swapchain.getSwapchain();
	present.swapchainCount = 1;
	present.pSwapchains = &sc;
	present.pImageIndices = &imageIndex;

	VkResult result = vkQueuePresentKHR(m_context->getPresentQueue(), &present);
	advanceFrame();

	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
		return false;
	if (result != VK_SUCCESS)
		throw std::runtime_error("Failed to present");
	return true;
}
