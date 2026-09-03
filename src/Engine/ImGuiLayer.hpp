#pragma once

#include <Vulkan/VkContext.hpp>
#include <Vulkan/VkSwapchain.hpp>
#include <Vulkan/VkCommands.hpp>

#include <SDL3/SDL.h>

/// Thin Dear ImGui integration (SDL3 platform + Vulkan renderer, dynamic rendering).
class ImGuiLayer
{
public:
	ImGuiLayer() = default;
	~ImGuiLayer();

	void init(SDL_Window *window, VkContext &context, VkSwapchain &swapchain, ImmediateCommands &imm);
	void shutdown();
	/// Refresh renderer-backend state after Engine recreates the swapchain.
	/// Returns true if backend-owned descriptors were invalidated by reinit.
	bool onSwapchainRecreate(VkSwapchain &swapchain);

	void processEvent(const SDL_Event &event);
	void beginFrame();
	void endFrame(); // ImGui::Render()

	/// Record ImGui draw data into an active dynamic-rendering command buffer.
	void recordDraw(VkCommandBuffer cmd);

	bool wantCaptureKeyboard() const;
	bool wantCaptureMouse() const;

private:
	void initVulkanBackend(VkContext &context, VkSwapchain &swapchain);

	bool m_initialized{false};
	bool m_vulkanInitialized{false};
	VkDevice m_device{VK_NULL_HANDLE};
	VkContext *m_context{nullptr};
	VkFormat m_colorFormat{VK_FORMAT_UNDEFINED};
	uint32_t m_swapchainImageCount{0};
	uint32_t m_minImageCount{0};
};
