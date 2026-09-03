#pragma once

#include "Vulkan/VkContext.hpp"

#include <vector>
#include <cstdint>

struct SwapchainSupportDetails
{
	VkSurfaceCapabilitiesKHR capabilities{};
	std::vector<VkSurfaceFormatKHR> formats;
	std::vector<VkPresentModeKHR> presentModes;
};

/// Swapchain + per-image views. Optional depth image for future 3D passes.
class VkSwapchain
{
public:
	VkSwapchain() = default;
	~VkSwapchain();

	VkSwapchain(const VkSwapchain &) = delete;
	VkSwapchain &operator=(const VkSwapchain &) = delete;

	void init(VkContext &context, uint32_t width, uint32_t height, bool vsync = true);
	void recreate(uint32_t width, uint32_t height);
	void recreate(uint32_t width, uint32_t height, bool vsync);
	void shutdown();

	VkSwapchainKHR getSwapchain() const { return m_swapchain; }
	VkFormat getImageFormat() const { return m_imageFormat; }
	VkExtent2D getExtent() const { return m_extent; }
	uint32_t getImageCount() const { return static_cast<uint32_t>(m_images.size()); }
	uint32_t getMinImageCount() const { return m_minImageCount; }
	const std::vector<VkImage> &getImages() const { return m_images; }
	const std::vector<VkImageView> &getImageViews() const { return m_imageViews; }

	bool isVSync() const { return m_vsync; }
	VkPresentModeKHR getPresentMode() const { return m_presentMode; }

	/// Strict policy: FIFO when VSync is enabled, IMMEDIATE when disabled.
	/// Disabling VSync never silently falls back to MAILBOX/FIFO because either
	/// mode can pace presentation to the display refresh.
	static VkPresentModeKHR selectPresentMode(
		const std::vector<VkPresentModeKHR> &modes, bool vsync);
	static const char *presentModeName(VkPresentModeKHR mode);

	static SwapchainSupportDetails querySupport(VkPhysicalDevice device, VkSurfaceKHR surface);

private:
	void createSwapchain(uint32_t width, uint32_t height);
	void createImageViews();
	void cleanupSwapchain();

	VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &formats) const;
	VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR &caps, uint32_t width, uint32_t height) const;

	VkContext *m_context{nullptr};
	VkSwapchainKHR m_swapchain{VK_NULL_HANDLE};
	std::vector<VkImage> m_images;
	std::vector<VkImageView> m_imageViews;
	VkFormat m_imageFormat{VK_FORMAT_UNDEFINED};
	VkExtent2D m_extent{};
	uint32_t m_minImageCount{0};
	bool m_vsync{true};
	VkPresentModeKHR m_presentMode{VK_PRESENT_MODE_FIFO_KHR};
};
