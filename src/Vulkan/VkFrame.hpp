#pragma once

#include "Vulkan/VkContext.hpp"
#include "Vulkan/VkSwapchain.hpp"
#include "Vulkan/VkGpuProfiler.hpp"

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
	/// Reset per-image ownership after a swapchain has been recreated.
	void onSwapchainRecreate(uint32_t imageCount);

	/// Wait on in-flight fence, acquire swapchain image, reset command buffer.
	/// Returns false if the swapchain is out of date (caller should recreate).
	bool beginFrame(VkSwapchain &swapchain, uint32_t &outImageIndex);

	/// Submit the current frame's command buffer and present.
	/// Returns false if present reports out-of-date / suboptimal.
	bool submitAndPresent(VkSwapchain &swapchain, uint32_t imageIndex);

	VkGpuProfiler &gpuProfiler() { return m_gpuProfiler; }

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
		VkFence inFlight{VK_NULL_HANDLE};
	};

	void ensureSwapchainImageSync(uint32_t imageCount);
	void resetFrameChain();

	VkGpuProfiler m_gpuProfiler;
	VkContext *m_context{nullptr};
	std::array<FrameData, kMaxFramesInFlight> m_frames{};
	std::vector<VkFence> m_imagesInFlight;
	// Presentation completion is associated with a swapchain image, not a
	// frame slot. Indexing render-finished semaphores by image prevents reuse
	// before the presentation engine has consumed the previous signal.
	std::vector<VkSemaphore> m_renderFinishedByImage;
	// Inter-submission dependency (review M1): the render graph's global
	// targets — HDR color, scene depth, god-ray source, shadow map, exposure
	// and water history — are SINGLE copies shared by every frame slot.
	// Vulkan does NOT guarantee that submissions on one queue execute in
	// order, so frame N+1's GPU work must not start before frame N's
	// finished. This binary semaphore is signaled by every submit and waited
	// by the next at TOP_OF_PIPE: the conservative chain the review allows
	// in place of duplicating every target + descriptor per slot. CPU
	// recording still overlaps (2 frames in flight); GPU execution
	// serializes — what desktop drivers effectively do already. Per-slot
	// targets remain the future perf optimization if overlapping drivers
	// matter.
	VkSemaphore m_prevFrameDone{VK_NULL_HANDLE};
	bool m_hasSubmitted{false};
	uint32_t m_currentFrame{0};
};
