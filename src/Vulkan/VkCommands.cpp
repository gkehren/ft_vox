#include "Vulkan/VkCommands.hpp"

#include <stdexcept>

ImmediateCommands::~ImmediateCommands()
{
	shutdown();
}

void ImmediateCommands::init(VkContext &context)
{
	if (m_pool != VK_NULL_HANDLE)
		throw std::runtime_error("ImmediateCommands already initialized");

	m_device = context.getDevice();
	m_queue = context.getGraphicsQueue();

	VkCommandPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = context.getGraphicsQueueFamily();
	if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS)
		throw std::runtime_error("Failed to create immediate command pool");
}

void ImmediateCommands::shutdown()
{
	if (m_device != VK_NULL_HANDLE && m_pool != VK_NULL_HANDLE)
	{
		vkDestroyCommandPool(m_device, m_pool, nullptr);
		m_pool = VK_NULL_HANDLE;
	}
	m_device = VK_NULL_HANDLE;
	m_queue = VK_NULL_HANDLE;
}

void ImmediateCommands::submitAndWait(const std::function<void(VkCommandBuffer)> &record)
{
	if (m_pool == VK_NULL_HANDLE)
		throw std::runtime_error("ImmediateCommands not initialized");

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = m_pool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(m_device, &allocInfo, &cmd) != VK_SUCCESS)
		throw std::runtime_error("Failed to allocate immediate command buffer");

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
		throw std::runtime_error("Failed to begin immediate command buffer");

	record(cmd);

	if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
		throw std::runtime_error("Failed to end immediate command buffer");

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence fence = VK_NULL_HANDLE;
	if (vkCreateFence(m_device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
		throw std::runtime_error("Failed to create immediate fence");

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmd;

	if (vkQueueSubmit(m_queue, 1, &submitInfo, fence) != VK_SUCCESS)
	{
		vkDestroyFence(m_device, fence, nullptr);
		throw std::runtime_error("Immediate queue submit failed");
	}

	vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX);
	vkDestroyFence(m_device, fence, nullptr);
	vkFreeCommandBuffers(m_device, m_pool, 1, &cmd);
}
