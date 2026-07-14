#include "Vulkan/VkAllocator.hpp"

// VMA implementation unit — dynamic function loading via volk.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

VkAllocator::~VkAllocator()
{
	shutdown();
}

void VkAllocator::init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
					   uint32_t vulkanApiVersion)
{
	if (m_allocator != VK_NULL_HANDLE)
		throw std::runtime_error("VkAllocator already initialized");

	VmaAllocatorCreateInfo createInfo{};
	createInfo.flags = 0;
	createInfo.physicalDevice = physicalDevice;
	createInfo.device = device;
	createInfo.instance = instance;
	createInfo.vulkanApiVersion = vulkanApiVersion;

	VmaVulkanFunctions vulkanFunctions{};
	if (vmaImportVulkanFunctionsFromVolk(&createInfo, &vulkanFunctions) != VK_SUCCESS)
		throw std::runtime_error("vmaImportVulkanFunctionsFromVolk failed");
	createInfo.pVulkanFunctions = &vulkanFunctions;

	if (vmaCreateAllocator(&createInfo, &m_allocator) != VK_SUCCESS)
		throw std::runtime_error("vmaCreateAllocator failed");
}

void VkAllocator::shutdown()
{
	if (m_allocator != VK_NULL_HANDLE)
	{
		vmaDestroyAllocator(m_allocator);
		m_allocator = VK_NULL_HANDLE;
	}
}
