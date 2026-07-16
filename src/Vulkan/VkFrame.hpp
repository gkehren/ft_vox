#pragma once

#include "Vulkan/VkContext.hpp"
#include "Vulkan/VkSwapchain.hpp"

#include <array>
#include <cstdint>
#include <vector>

/// Per-frame-in-flight resources: command pool/buffer, WSI sync.
/// Single game-path owner for acquire / submit / present (no second copy in WorldRenderer).
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

	/// Wait on in-flight fence, acquire swapchain image, reset command buffer.
	/// Returns false if the swapchain is out of date (caller should recreate).
	bool beginFrame(VkSwapchain &swapchain, uint32_t &outImageIndex);

	/// Submit the current frame's command buffer and present.
	/// Returns false if present reports out-of-date / suboptimal.
	bool submitAndPresent(VkSwapchain &swapchain, uint32_t imageIndex);

	/// Legacy clear-only path (tests / smoke). Prefer record + submitAndPresent for the game.
	bool endFrameClearAndPresent(VkSwapchain &swapchain, uint32_t imageIndex, const VkClearColorValue &clearColor);

	uint32_t frameIndex() const { return m_currentFrame; }
	uint32_t currentFrameIndex() const { return m_currentFrame; }
	VkCommandBuffer commandBuffer() const { return m_frames[m_currentFrame].commandBuffer; }
	/// Advance FIF index after a successful present (called by submitAndPresent).
	void advanceFrame() { m_currentFrame = (m_currentFrame + 1) % kMaxFramesInFlight; }

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
	std::vector<VkFence> m_imagesInFlight;
	uint32_t m_currentFrame{0};
};
