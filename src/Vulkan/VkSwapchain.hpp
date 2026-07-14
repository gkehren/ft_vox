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
	void shutdown();

	VkSwapchainKHR getSwapchain() const { return m_swapchain; }
	VkFormat getImageFormat() const { return m_imageFormat; }
	VkExtent2D getExtent() const { return m_extent; }
	uint32_t getImageCount() const { return static_cast<uint32_t>(m_images.size()); }
	const std::vector<VkImage> &getImages() const { return m_images; }
	const std::vector<VkImageView> &getImageViews() const { return m_imageViews; }

	void setVSync(bool enabled);
	bool isVSync() const { return m_vsync; }

	static SwapchainSupportDetails querySupport(VkPhysicalDevice device, VkSurfaceKHR surface);

private:
	void createSwapchain(uint32_t width, uint32_t height);
	void createImageViews();
	void cleanupSwapchain();

	VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &formats) const;
	VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR> &modes) const;
	VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR &caps, uint32_t width, uint32_t height) const;

	VkContext *m_context{nullptr};
	VkSwapchainKHR m_swapchain{VK_NULL_HANDLE};
	std::vector<VkImage> m_images;
	std::vector<VkImageView> m_imageViews;
	VkFormat m_imageFormat{VK_FORMAT_UNDEFINED};
	VkExtent2D m_extent{};
	bool m_vsync{true};
};
