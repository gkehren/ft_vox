#pragma once

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "Vulkan/VkAllocator.hpp"

#include <stdexcept>
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <memory>
#include <atomic>

struct QueueFamilyIndices
{
	std::optional<uint32_t> graphicsFamily;
	std::optional<uint32_t> presentFamily;

	bool isComplete() const
	{
		return graphicsFamily.has_value() && presentFamily.has_value();
	}

	bool graphicsEqualsPresent() const
	{
		return isComplete() && graphicsFamily.value() == presentFamily.value();
	}
};

/// Owns Vulkan instance, surface, physical/logical device, and queues.
/// Minimum target: Vulkan 1.2 (+ portability on Apple).
class VkContext
{
public:
	VkContext() = default;
	~VkContext();

	VkContext(const VkContext &) = delete;
	VkContext &operator=(const VkContext &) = delete;

	void init(SDL_Window *window);
	void shutdown();

	VkInstance getInstance() const { return m_instance; }
	VkPhysicalDevice getPhysicalDevice() const { return m_physicalDevice; }
	VkDevice getDevice() const { return m_device; }
	VkSurfaceKHR getSurface() const { return m_surface; }
	VkQueue getGraphicsQueue() const { return m_graphicsQueue; }
	VkQueue getPresentQueue() const { return m_presentQueue; }
	uint32_t getGraphicsQueueFamily() const { return m_queueFamilies.graphicsFamily.value(); }
	uint32_t getPresentQueueFamily() const { return m_queueFamilies.presentFamily.value(); }
	const QueueFamilyIndices &getQueueFamilies() const { return m_queueFamilies; }

	bool hasDynamicRendering() const { return m_dynamicRendering; }
	bool hasTimelineSemaphores() const { return m_timelineSemaphores; }
	bool hasPortabilitySubset() const { return m_portabilitySubset; }
	bool isValidationEnabled() const { return m_validationEnabled; }
	/// Includes initialization and shutdown; reset only by the next init().
	uint64_t validationErrorCount() const { return m_validationErrors.load(std::memory_order_relaxed); }

	const VkPhysicalDeviceProperties &getDeviceProperties() const { return m_deviceProperties; }

	/// VMA allocator (valid after successful init, destroyed on shutdown).
	VmaAllocator getAllocator() const;
	VkAllocator &getAllocatorOwner();

	/// Wait for the device to become idle (shutdown / resize only — not per-frame).
	void waitIdle() const;

private:
	void createAllocator();
	void createInstance(SDL_Window *window);
	void setupDebugMessenger();
	void createSurface(SDL_Window *window);
	void pickPhysicalDevice();
	void createLogicalDevice();

	QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;
	bool isDeviceSuitable(VkPhysicalDevice device) const;
	std::vector<const char *> getRequiredDeviceExtensions() const;

	VkInstance m_instance{VK_NULL_HANDLE};
	VkDebugUtilsMessengerEXT m_debugMessenger{VK_NULL_HANDLE};
	VkSurfaceKHR m_surface{VK_NULL_HANDLE};
	VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
	VkDevice m_device{VK_NULL_HANDLE};
	VkQueue m_graphicsQueue{VK_NULL_HANDLE};
	VkQueue m_presentQueue{VK_NULL_HANDLE};
	QueueFamilyIndices m_queueFamilies{};
	VkPhysicalDeviceProperties m_deviceProperties{};

	bool m_validationEnabled{false};
	std::atomic<uint64_t> m_validationErrors{0};
	bool m_dynamicRendering{false};
	bool m_timelineSemaphores{false};
	bool m_portabilitySubset{false};

	std::unique_ptr<VkAllocator> m_allocator;

	static constexpr uint32_t kApiVersion = VK_API_VERSION_1_2;
};
