#pragma once

#include <Engine/PlayerHud.hpp>
#include <Engine/PlayerInventory.hpp>
#include <Engine/PlayerUiComponents.hpp>
#include <Engine/PlayerUiFonts.hpp>
#include <Engine/PlayerUiGallery.hpp>
#include <Engine/PlayerUiScale.hpp>
#include <Engine/PlayerUiWidgets.hpp>
#include <Renderer/GameUiRenderer.hpp>
#include <SDL3/SDL.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

// Host for the native player HUD and screen stack, including the design
// gallery and standalone rendering proof.
class GameUiSample
{
public:
	enum class InputResult { Unhandled, Consumed, Exit };

	void init(SDL_Window *window, VkContext &context, ImmediateCommands &immediate,
			  VkFormat format, VkColorSpaceKHR colorSpace, bool standalone);
	InputResult handleEvent(SDL_Window *window, const SDL_Event &event, bool canClick);
	InputResult handleScreenEvent(const SDL_Event &event);
	void cancelPointerPress()
	{
		m_armed = false;
		m_widgets.cancelPress();
		m_inventoryPress.reset();
	}
	bool hasArmedPress() const { return m_armed || m_widgets.hasPress() || m_inventoryPress.has_value(); }
	bool hitsStrip(float x, float y) const;
	bool canFocusWidget(float x, float y) const;
	void resetScreens();
	void invalidateLayout();
	void back();
	void openPause();
	void closePause();
	void openGallery();
	/// Push a screen on the real stack (the pause menu and the proofs use it).
	void openScreen(gameui::Screen screen);
	void setInventoryOpen(bool open);
	void setInventoryView(gameui::InventoryView view);
	void cancelInventoryGesture();
	inventoryui::Action handleInventoryEvent(const SDL_Event &event);
	void updateTextInput(SDL_Window *window, bool interactive);
	bool yieldTextInput(SDL_Window *window);
	const gameui::ScreenStack &screens() const { return m_screens; }
	void prepare(SDL_Window *window, uint32_t slot, VkExtent2D extent, gameui::Clip view,
				 const TextureManager &atlas, uint64_t atlasGeneration, const gameui::HudView *hud = nullptr);
	void setPreviewStripVisible(bool visible) { m_previewStrip = visible; }
	bool hudVisible() const { return m_hudVisible; }
	void setHudVisible(bool visible) { m_hudVisible = visible; }
	gameui::Renderer &renderer() { return m_renderer; }
	const std::optional<gameui::RecordedStats> &lastRecordedStats() const { return m_lastRecordedStats; }
	void publishRecordedStats(uint64_t frameNumber, uint32_t slot)
	{
		m_lastRecordedStats = gameui::RecordedStats{frameNumber, m_renderer.stats(slot)};
	}

	// --- Player UI scale (issue #302) --------------------------------------
	/// Apply a policy (from the persisted preferences or a player request).
	/// Never changes the scale mid-frame: the next prepare() resolves it.
	void setScalePolicy(gameui::ScalePolicy policy) { m_scale.setPolicy(policy); }
	gameui::ScalePolicy scalePolicy() const { return m_scale.policy(); }
	float playerUiScale() const { return m_scale.effective(); }
	/// A scale request raised by a player surface (the gallery selector today,
	/// the settings screen in #307). The host persists it and pushes it back
	/// through setScalePolicy, so the surface never touches the settings store.
	std::optional<gameui::ScalePolicy> takeScaleRequest()
	{
		auto request = m_scaleRequest;
		m_scaleRequest.reset();
		return request;
	}
	/// A player-facing message about the persisted settings store (empty when it
	/// read and wrote cleanly). Presentation only: the host owns the text and
	/// the file, the surface just prints it next to the scale selector.
	void setSettingsNotice(std::string_view notice)
	{
		m_settingsNotice.assign(notice.substr(0, 160));
	}

private:
	friend struct UiSpikeProbe;
	friend struct GameUiBackendProbe;
	friend struct EngineJoinProbe;

	void handleWindowEvent(Uint32 type);
	/// Re-bakes the face when the framebuffer density changed (issue #302 F11)
	/// and hands the new atlas to the renderer at a safe synchronization point.
	void refreshFace(SDL_Window *window);
	/// Re-bake the face for `pixels` framebuffer points per logical point and
	/// publish it only if the GPU accepted the replacement (issue #302 R07/S03).
	/// Returns false when the bake or the replacement failed — the previous face,
	/// `m_facePixels` and `m_fontScale` then stay untouched. An unchanged density
	/// returns true without rebuilding. `refreshFace` is this applied to the
	/// window; the GPU proof drives it directly to exercise a real transition.
	bool refreshFaceForDensity(float pixels);
	/// Bake the face for `pixels` instead of the SDL-reported density (issue #302
	/// S03). `refreshFace` prefers the override, so a host that knows the target
	/// density better than SDL — and the GPU proof exercising a real transition —
	/// can drive it without moving the window between displays. `nullopt` returns
	/// to the queries.
	void setFaceDensityOverride(std::optional<float> pixels) { m_faceDensityOverride = pixels; }
	/// Ages notifications and the indeterminate phase from a monotonic clock
	/// (issue #302 F10): the timers are frame-rate independent.
	void advancePresentation();
	/// Monotonic notification id, independent of any animation phase.
	uint32_t nextToastId() { return ++m_toastSerial; }
	bool windowInteractive() const;
	void activate(const char *source);
	gameui::FontView font() const { return {m_face.glyphs}; }
	gameui::Surface surface(gameui::Clip clip);
	void buildScreen(gameui::Clip scene, float scale);
	void buildGallery(gameui::Clip scene, float scale);
	void buildHudMock(gameui::Clip scene, float scale);
	void paintInventory(gameui::Clip scene, glm::vec2 pointer, std::optional<uint64_t> atlasGeneration);
	gameui::ControlId inventoryControlAt(glm::vec2 point) const;
	void paintScreen(glm::vec2 pointer);
	void paintDesignSystem(glm::vec2 pointer);
	InputResult applyWidgetEvent(gameui::WidgetEvent event);
	InputResult applyGalleryActivate(gameui::ControlId id);

	gameui::Renderer m_renderer;
	gameui::DrawList m_draws;
	std::optional<gameui::RecordedStats> m_lastRecordedStats;
	gameui::BakedFace m_face;
	float m_fontScale{1.f};
	/// Framebuffer pixels per logical point the current face was baked at.
	float m_facePixels{1.f};
	/// Density whose atlas replacement failed; not retried while unchanged (R07).
	std::optional<float> m_faceFailedPixels;
	/// Explicit density for the face bake, over the SDL queries (S03).
	std::optional<float> m_faceDensityOverride;
	ImmediateCommands *m_immediate{};
	SDL_WindowFlags m_windowFlags{};
	glm::vec4 m_button{};
	unsigned m_activations{};
	bool m_standalone{}, m_buttonFocused{}, m_armed{};
	bool m_previewStrip{true}, m_hudVisible{true};
	gameui::ScreenStack m_screens;
	gameui::WidgetSet m_widgets;
	gameui::InventoryView m_inventoryView;
	gameui::InventoryLayout m_inventoryLayout;
	/// Last meaningful interaction device on the inventory screen (F05): the
	/// held stack follows it; reset by every gesture cancellation.
	gameui::InventoryModality m_inventoryModality{gameui::InventoryModality::Pointer};
	inventoryui::Interaction m_inventoryInteraction;
	struct InventoryPress { gameui::ControlId control{}; Uint8 button{}; };
	std::optional<InventoryPress> m_inventoryPress;
	gameui::TextField m_field, m_secondaryField;
	gameui::TextField *m_sdlEditor{};
	gameui::Toggle m_feature{true}, m_enableSecond{true};
	gameui::Slider m_slider{0.5f}, m_secondarySlider{0.25f};
	gameui::ListBox m_list, m_secondaryList;
	gameui::Clip m_panel{};
	/// Screen stack revision whose presentation state is initialized. Layout
	/// invalidation and screen entry are separate events (issue #302 F16).
	uint64_t m_screenRevision{UINT64_MAX};
	uint64_t m_layoutRevision{UINT64_MAX};
	float m_screenScale{1.f};
	uint32_t m_atlasLayers{};

	// Player UI scale + player-surface state (issue #302).
	gameui::ScaleController m_scale;
	std::optional<gameui::ScalePolicy> m_scaleRequest;
	std::string m_settingsNotice;
	float m_uiPhase{};
	uint32_t m_toastSerial{};
	std::optional<std::chrono::steady_clock::time_point> m_lastFrame;
	gameui::TextField m_galleryName;
	gameui::Toggle m_galleryToggle{true};
	gameui::Slider m_gallerySlider{0.4f};
	gameui::ListBox m_galleryList;
	gameui::ToastQueue m_toasts;
	gameui::GalleryLayout m_gallery{};
	gameui::HudMockLayout m_hudMock{};
	gameui::ConfirmationLayout m_confirmation{};
	int m_gallerySlot{2};
};
