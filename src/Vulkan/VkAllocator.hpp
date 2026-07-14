#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <stdexcept>
#include <cstdint>

/// Thin VMA owner: create after the logical device, destroy before the device.
class VkAllocator
{
public:
	VkAllocator() = default;
	~VkAllocator();

	VkAllocator(const VkAllocator &) = delete;
	VkAllocator &operator=(const VkAllocator &) = delete;

	void init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
			  uint32_t vulkanApiVersion = VK_API_VERSION_1_2);
	void shutdown();

	VmaAllocator handle() const { return m_allocator; }
	bool isValid() const { return m_allocator != VK_NULL_HANDLE; }

private:
	VmaAllocator m_allocator{VK_NULL_HANDLE};
};
