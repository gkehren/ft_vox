#pragma once

#include "Vulkan/VkContext.hpp"

#include <functional>

/// One-shot command recording + submit + fence wait on the graphics queue.
/// Used for staging uploads and PR2 smokes — not for per-frame work.
class ImmediateCommands
{
public:
	ImmediateCommands() = default;
	~ImmediateCommands();

	ImmediateCommands(const ImmediateCommands &) = delete;
	ImmediateCommands &operator=(const ImmediateCommands &) = delete;

	void init(VkContext &context);
	void shutdown();

	void submitAndWait(const std::function<void(VkCommandBuffer)> &record);

	bool isValid() const { return m_pool != VK_NULL_HANDLE; }

private:
	VkDevice m_device{VK_NULL_HANDLE};
	VkQueue m_queue{VK_NULL_HANDLE};
	VkCommandPool m_pool{VK_NULL_HANDLE};
};
