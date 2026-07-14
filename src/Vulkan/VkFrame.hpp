#pragma once

#include "Vulkan/VkContext.hpp"
#include "Vulkan/VkSwapchain.hpp"

#include <array>
#include <cstdint>
#include <vector>

/// Per-frame-in-flight resources: command pool/buffer, WSI sync.
/// PR1 uses binary semaphores for acquire/present (WSI requirement).
class VkFrameContext
{
public:
	static constexpr uint32_t kMaxFramesInFlight = 2;

	VkFrameContext() = default;
	~VkFrameContext();

	VkFrameContext(const VkFrameContext &) = delete;
	VkFrameContext &operator=(const VkFrameContext &) = delete;

	void init(VkContext &context);
	void shutdown();

	/// Begin a frame: wait on in-flight fence, acquire swapchain image.
	/// Returns false if the swapchain is out of date (caller should recreate).
	bool beginFrame(VkSwapchain &swapchain, uint32_t &outImageIndex);

	/// Clear the acquired swapchain image to `clearColor` and present.
	/// Returns false if present reports out-of-date / suboptimal (recreate).
	bool endFrameClearAndPresent(VkSwapchain &swapchain, uint32_t imageIndex, const VkClearColorValue &clearColor);

	uint32_t currentFrameIndex() const { return m_currentFrame; }

private:
	struct FrameData
	{
		VkCommandPool commandPool{VK_NULL_HANDLE};
		VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
		VkSemaphore imageAvailable{VK_NULL_HANDLE};
		VkSemaphore renderFinished{VK_NULL_HANDLE};
		VkFence inFlight{VK_NULL_HANDLE};
	};

	void recordClearCommands(VkCommandBuffer cmd, VkSwapchain &swapchain, uint32_t imageIndex,
							 const VkClearColorValue &clearColor);

	VkContext *m_context{nullptr};
	std::array<FrameData, kMaxFramesInFlight> m_frames{};
	/// Per-swapchain-image fence to avoid writing an image still being presented.
	std::vector<VkFence> m_imagesInFlight;
	uint32_t m_currentFrame{0};
};
