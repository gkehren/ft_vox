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

	// (review §4.8: the dead ImmediateCommands& parameter was removed —
	// the backend never used it.)
	void init(SDL_Window *window, VkContext &context, VkSwapchain &swapchain);
	void shutdown();
	/// Refresh renderer-backend state after Engine recreates the swapchain.
	/// Returns true if backend-owned descriptors were invalidated by reinit.
	bool onSwapchainRecreate(VkSwapchain &swapchain);

	void processEvent(const SDL_Event &event, bool takingTextInput = false);
	void observePointer(float x, float y);
	void observeModifiers(SDL_Keymod mods);
	void cancelInput();
	void yieldKeyboard();
	void beginFrame();
	void endFrame(); // ImGui::Render()

	/// Record ImGui draw data into an active dynamic-rendering command buffer.
	void recordDraw(VkCommandBuffer cmd);

	bool wantCaptureKeyboard() const;
	bool wantCaptureMouse() const;
	bool editingText() const;
	bool hasModal() const;
	bool hasOpenPopup() const;
	bool ownsActiveInput() const;
	/// True when the Developer UI owns the keyboard (PR #316 F01, round 2):
	/// ownership is persistent — a meaningful signal (an active widget, an
	/// editing field, an open popup or a visible keyboard-navigation
	/// highlight) latches this layer as the owner, and only a meaningful
	/// move toward the player UI (a native-screen press, focus loss, a
	/// workspace or session change) releases it. WantCaptureKeyboard retained
	/// by mere passive focus — the window stays focused after the last
	/// interaction was handed away — never owns, so it can neither swallow
	/// the player's contextual keys nor trigger inventory commands.
	bool ownsKeyboard() const;
	bool pendingKeyboard() const;
	/// Latch the Developer UI as the persistent keyboard owner (a Developer
	/// UI press or a navigation highlight did).
	void retainKeyboardOwnership();
	/// Hand persistent keyboard ownership back to the player UI (a native
	/// screen press, focus loss, a workspace or session change).
	void releaseKeyboardOwnership();
	enum class PointerHit { PassThrough, Occludes, Interactive };
	PointerHit pointerHit(float x, float y) const;
	static void markCurrentWindowPassThrough();

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
	void stopTextInput();
	void finishKeyboardYield();

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
	unsigned m_pendingPressEvent{};
	unsigned m_yieldThroughEvent{};
	int m_keyboardReleasedFrame{-1};
	bool m_provisionalTextInput{};
	bool m_devKeyboardOwner{false};
	SDL_Window *m_window{nullptr};
};
