#include "Vulkan/VkSwapchain.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

VkSwapchain::~VkSwapchain()
{
	shutdown();
}

SwapchainSupportDetails VkSwapchain::querySupport(VkPhysicalDevice device, VkSurfaceKHR surface)
{
	SwapchainSupportDetails details;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

	uint32_t formatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
	if (formatCount > 0)
	{
		details.formats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
	}

	uint32_t presentModeCount = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
	if (presentModeCount > 0)
	{
		details.presentModes.resize(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
	}
	return details;
}

void VkSwapchain::init(VkContext &context, uint32_t width, uint32_t height, bool vsync)
{
	m_context = &context;
	m_vsync = vsync;
	createSwapchain(width, height);
	createImageViews();
}

void VkSwapchain::shutdown()
{
	cleanupSwapchain();
	m_context = nullptr;
}

void VkSwapchain::recreate(uint32_t width, uint32_t height)
{
	recreate(width, height, m_vsync);
}

void VkSwapchain::recreate(uint32_t width, uint32_t height, bool vsync)
{
	if (!m_context || width == 0 || height == 0)
		return;

	// Reject an unavailable strict present mode before destroying the working
	// swapchain. The Engine invokes this only at a pre-acquire frame boundary.
	const auto support =
		querySupport(m_context->getPhysicalDevice(), m_context->getSurface());
	selectPresentMode(support.presentModes, vsync);

	m_context->waitIdle();
	cleanupSwapchain();
	m_vsync = vsync;
	createSwapchain(width, height);
	createImageViews();
}

void VkSwapchain::cleanupSwapchain()
{
	if (!m_context || m_context->getDevice() == VK_NULL_HANDLE)
		return;

	for (VkImageView view : m_imageViews)
		vkDestroyImageView(m_context->getDevice(), view, nullptr);
	m_imageViews.clear();
	m_images.clear();

	if (m_swapchain != VK_NULL_HANDLE)
	{
		vkDestroySwapchainKHR(m_context->getDevice(), m_swapchain, nullptr);
		m_swapchain = VK_NULL_HANDLE;
	}
}

VkSurfaceFormatKHR VkSwapchain::chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &formats) const
{
	for (const auto &format : formats)
	{
		if ((format.format == VK_FORMAT_B8G8R8A8_SRGB || format.format == VK_FORMAT_R8G8B8A8_SRGB) &&
			format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			return format;
	}
	return formats.front();
}

VkPresentModeKHR VkSwapchain::selectPresentMode(
	const std::vector<VkPresentModeKHR> &modes, bool vsync)
{
	const VkPresentModeKHR required =
		vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
	if (std::find(modes.begin(), modes.end(), required) != modes.end())
		return required;

	if (!vsync)
	{
		throw std::runtime_error(
			"VSync off requires VK_PRESENT_MODE_IMMEDIATE_KHR, but the "
			"surface does not expose it; refusing a synchronized fallback");
	}
	throw std::runtime_error(
		"VSync on requires VK_PRESENT_MODE_FIFO_KHR, but the surface does not "
		"expose the Vulkan-mandated FIFO mode");
}

const char *VkSwapchain::presentModeName(VkPresentModeKHR mode)
{
	switch (mode)
	{
	case VK_PRESENT_MODE_IMMEDIATE_KHR:
		return "IMMEDIATE";
	case VK_PRESENT_MODE_MAILBOX_KHR:
		return "MAILBOX";
	case VK_PRESENT_MODE_FIFO_KHR:
		return "FIFO";
	case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
		return "FIFO_RELAXED";
	default:
		return "UNKNOWN";
	}
}

VkExtent2D VkSwapchain::chooseExtent(const VkSurfaceCapabilitiesKHR &caps, uint32_t width, uint32_t height) const
{
	if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max())
		return caps.currentExtent;

	VkExtent2D extent{width, height};
	extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
	extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
	return extent;
}

void VkSwapchain::createSwapchain(uint32_t width, uint32_t height)
{
	auto support = querySupport(m_context->getPhysicalDevice(), m_context->getSurface());
	if (support.formats.empty() || support.presentModes.empty())
		throw std::runtime_error("Swapchain support incomplete");

	VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(support.formats);
	VkPresentModeKHR presentMode =
		selectPresentMode(support.presentModes, m_vsync);
	VkExtent2D extent = chooseExtent(support.capabilities, width, height);

	uint32_t imageCount = support.capabilities.minImageCount + 1;
	if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount)
		imageCount = support.capabilities.maxImageCount;

	VkSwapchainCreateInfoKHR createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface = m_context->getSurface();
	createInfo.minImageCount = imageCount;
	createInfo.imageFormat = surfaceFormat.format;
	createInfo.imageColorSpace = surfaceFormat.colorSpace;
	createInfo.imageExtent = extent;
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	const uint32_t graphicsFamily = m_context->getGraphicsQueueFamily();
	const uint32_t presentFamily = m_context->getPresentQueueFamily();
	const uint32_t queueFamilyIndices[] = {graphicsFamily, presentFamily};

	if (graphicsFamily != presentFamily)
	{
		createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		createInfo.queueFamilyIndexCount = 2;
		createInfo.pQueueFamilyIndices = queueFamilyIndices;
	}
	else
	{
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}

	createInfo.preTransform = support.capabilities.currentTransform;
	createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	createInfo.presentMode = presentMode;
	createInfo.clipped = VK_TRUE;
	createInfo.oldSwapchain = VK_NULL_HANDLE;

	if (vkCreateSwapchainKHR(m_context->getDevice(), &createInfo, nullptr, &m_swapchain) != VK_SUCCESS)
		throw std::runtime_error("Failed to create swapchain");

	vkGetSwapchainImagesKHR(m_context->getDevice(), m_swapchain, &imageCount, nullptr);
	m_images.resize(imageCount);
	vkGetSwapchainImagesKHR(m_context->getDevice(), m_swapchain, &imageCount, m_images.data());

	m_imageFormat = surfaceFormat.format;
	m_extent = extent;
	m_minImageCount = createInfo.minImageCount;
	m_presentMode = presentMode;

	std::cout << "Swapchain present mode: " << presentModeName(m_presentMode)
			  << " (VSync " << (m_vsync ? "on" : "off")
			  << (m_vsync ? ")" : ", uncapped)") << '\n';
}

void VkSwapchain::createImageViews()
{
	m_imageViews.resize(m_images.size());
	for (size_t i = 0; i < m_images.size(); ++i)
	{
		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = m_images[i];
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = m_imageFormat;
		viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 1;

		if (vkCreateImageView(m_context->getDevice(), &viewInfo, nullptr, &m_imageViews[i]) != VK_SUCCESS)
			throw std::runtime_error("Failed to create swapchain image view");
	}
}
