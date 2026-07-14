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

	void processEvent(const SDL_Event &event);
	void beginFrame();
	void endFrame(); // ImGui::Render()

	/// Record ImGui draw data into an active dynamic-rendering command buffer.
	void recordDraw(VkCommandBuffer cmd);

	bool wantCaptureKeyboard() const;
	bool wantCaptureMouse() const;

private:
	bool m_initialized{false};
	VkDevice m_device{VK_NULL_HANDLE};
};
