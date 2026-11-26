#include "Vulkan/VkSwapchain.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <cstdlib>
#include <cstring>

VkSwapchain::~VkSwapchain()
{
	shutdown();
}

SwapchainSupportDetails VkSwapchain::querySupport(VkPhysicalDevice device, VkSurfaceKHR surface)
{
	// Every query result is checked (review §4.7): a surface error must not
	// be read as "zero formats/present modes" and surface later as an
	// unrelated creation failure.
	SwapchainSupportDetails details;
	if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities) != VK_SUCCESS)
		throw std::runtime_error("Failed to query surface capabilities");

	uint32_t formatCount = 0;
	if (vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr) != VK_SUCCESS)
		throw std::runtime_error("Failed to query surface format count");
	if (formatCount > 0)
	{
		details.formats.resize(formatCount);
		if (vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data()) != VK_SUCCESS)
			throw std::runtime_error("Failed to query surface formats");
	}

	uint32_t presentModeCount = 0;
	if (vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr) != VK_SUCCESS)
		throw std::runtime_error("Failed to query surface present mode count");
	if (presentModeCount > 0)
	{
		details.presentModes.resize(presentModeCount);
		if (vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data()) != VK_SUCCESS)
			throw std::runtime_error("Failed to query surface present modes");
	}
	return details;
}

void VkSwapchain::init(VkContext &context, uint32_t width, uint32_t height, bool vsync)
{
	m_context = &context;
	createSwapchain(width, height, vsync, false);
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

void VkSwapchain::recreate(uint32_t width, uint32_t height, bool vsync, bool requireExactExtent)
{
	if (!m_context || width == 0 || height == 0)
		return;

	createSwapchain(width, height, vsync, requireExactExtent);
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
	// Strict SDR/sRGB policy (issue #135): the output transfer is decided from the
	// {format, colorSpace} pair, so only pairs the pipeline can transfer correctly
	// are accepted. Anything else (HDR10/PQ, Display-P3, linear BT.709, …) is
	// refused instead of silently guessing an sRGB encode.
	// 1. Preferred: sRGB image format — the attachment write performs linear->sRGB.
	for (const auto &format : formats)
	{
		if ((format.format == VK_FORMAT_B8G8R8A8_SRGB || format.format == VK_FORMAT_R8G8B8A8_SRGB) &&
			format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			return format;
	}
	// 2. Fallback: UNORM equivalent — composite.frag encodes linear->sRGB itself.
	for (const auto &format : formats)
	{
		if ((format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) &&
			format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			return format;
	}
	throw SwapchainConfigurationError(
		"Surface exposes no SDR sRGB-compatible format {B8G8R8A8/R8G8B8A8 + SRGB_NONLINEAR}; "
		"HDR/wide-gamut swapchains are not supported by the current color pipeline");
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
		throw SwapchainConfigurationError(
			"VSync off requires VK_PRESENT_MODE_IMMEDIATE_KHR, but the "
			"surface does not expose it; refusing a synchronized fallback");
	}
	throw SwapchainConfigurationError(
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

void VkSwapchain::createSwapchain(uint32_t width, uint32_t height, bool vsync, bool requireExactExtent)
{
	auto support = querySupport(m_context->getPhysicalDevice(), m_context->getSurface());
	if (support.formats.empty() || support.presentModes.empty())
		throw SwapchainConfigurationError("Swapchain support incomplete");

	VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(support.formats);
	VkPresentModeKHR presentMode =
		selectPresentMode(support.presentModes, vsync);
	VkExtent2D extent = chooseExtent(support.capabilities, width, height);
	if (requireExactExtent && (extent.width != width || extent.height != height))
		throw SwapchainConfigurationError("The Vulkan surface refused the requested framebuffer extent");

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
	// Usage validated against the surface (review §4.7): COLOR_ATTACHMENT is
	// the only usage the composite path needs; TRANSFER_DST is requested for
	// capture-style paths but only when the surface actually supports it.
	createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if ((support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0)
		createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if ((support.capabilities.supportedUsageFlags & createInfo.imageUsage) != createInfo.imageUsage)
		throw SwapchainConfigurationError("Surface does not support the required swapchain image usage");

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
	if (const char *trace = std::getenv("FT_VOX_TRACE_VALIDATION"); trace && std::strcmp(trace, "1") == 0)
		std::cerr << "[Vulkan] Swapchain request before dispatch: format=" << createInfo.imageFormat
			<< " imageUsage=" << createInfo.imageUsage << " (COLOR_ATTACHMENT | TRANSFER_DST)\n";

	// Validate the proposed configuration before retiring the working images.
	const auto device = m_context->getDevice();
	if (m_swapchain)
	{
		const VkResult idle = vkDeviceWaitIdle(device);
		if (idle != VK_SUCCESS)
			throw std::runtime_error("Failed to wait for swapchain retirement (VkResult " + std::to_string(idle) + ')');
		cleanupSwapchain();
	}
	VkSwapchainKHR replacement = VK_NULL_HANDLE;
	const VkResult created = vkCreateSwapchainKHR(device, &createInfo, nullptr, &replacement);
	if (created == VK_ERROR_INITIALIZATION_FAILED || created == VK_ERROR_OUT_OF_DATE_KHR)
		throw SwapchainConfigurationError("The Vulkan surface rejected swapchain creation (VkResult " +
			std::to_string(created) + ')');
	if (created != VK_SUCCESS)
		throw std::runtime_error("Failed to create swapchain (VkResult " + std::to_string(created) + ')');
	m_swapchain = replacement;

	if (vkGetSwapchainImagesKHR(m_context->getDevice(), m_swapchain, &imageCount, nullptr) != VK_SUCCESS)
		throw std::runtime_error("Failed to query swapchain image count");
	m_images.resize(imageCount);
	if (vkGetSwapchainImagesKHR(m_context->getDevice(), m_swapchain, &imageCount, m_images.data()) != VK_SUCCESS)
		throw std::runtime_error("Failed to retrieve swapchain images");

	m_imageFormat = surfaceFormat.format;
	m_colorSpace = surfaceFormat.colorSpace;
	m_extent = extent;
	m_minImageCount = createInfo.minImageCount;
	m_presentMode = presentMode;
	createImageViews();
	m_vsync = vsync;

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
