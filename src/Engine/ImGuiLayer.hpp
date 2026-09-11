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

	/// Current UI scale (1.0 = 100%). Derived from the display content scale
	/// at startup, overridden by the persisted value in imgui.ini.
	float uiScale() const { return m_uiScale; }
	/// Queue a UI-scale change; applied safely at the next beginFrame
	/// (style rebuild + font atlas rebuild + font texture recreation).
	void requestUiScale(float scale);
	/// True when an imgui.ini existed at init (i.e. this is not a first run):
	/// the shell uses it to decide whether to apply the default layout.
	bool hadExistingIni() const { return m_hadIniAtStartup; }

private:
	void initVulkanBackend(VkContext &context, VkSwapchain &swapchain);
	void registerScaleSettingsHandler();

	bool m_initialized{false};
	bool m_vulkanInitialized{false};
	VkDevice m_device{VK_NULL_HANDLE};
	VkContext *m_context{nullptr};
	VkFormat m_colorFormat{VK_FORMAT_UNDEFINED};
	uint32_t m_swapchainImageCount{0};
	uint32_t m_minImageCount{0};
	float m_uiScale{1.f};
	float m_pendingUiScale{0.f};
	float m_framebufferScale{1.f};
	bool m_hadIniAtStartup{false};
	SDL_Window *m_window{nullptr};
};
