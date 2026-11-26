#pragma once

#include <Engine/PlayerHud.hpp>
#include <Engine/PlayerSessionUi.hpp>
#include <Engine/PlayerMultiplayer.hpp>
#include <Engine/PlayerInventory.hpp>
#include <Engine/PlayerUiComponents.hpp>
#include <Engine/PlayerUiFonts.hpp>
#include <Engine/PlayerUiGallery.hpp>
#include <Engine/PlayerUiScale.hpp>
#include <Engine/PlayerUiWidgets.hpp>
#include <Renderer/GameUiRenderer.hpp>
#include <SDL3/SDL.h>

#include <chrono>
#include <deque>
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
	void resetScreens(gameui::Screen root = gameui::Screen::Hud);
	void setWorldCatalog(gameui::WorldCatalog catalog);
	void setSessionStatus(std::string message, bool retry = false, bool force = false,
		bool failedQuit = false);
	void setSessionContext(bool remote, bool persistent, bool hosted);
	std::optional<gameui::SessionRequest> takeSessionRequest();
	bool holdingForSession() const;
	void finishSessionScreen();
	void setMultiplayerView(gameui::MultiplayerView view);
	std::optional<gameui::MultiplayerRequest> takeMultiplayerRequest();
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

	bool buildSessionScreen(gameui::Screen screen, gameui::StackLayout &column);
	// --- Native multiplayer state (issue #306) ------------------------------
	// Kept grouped here (correction F09): a future PR may extract a dedicated
	// MultiplayerScreenController owning this block plus
	// GameUiMultiplayerScreen.cpp; no Engine or networking responsibility may
	// be added to GameUiSample beyond the view/intent pair.
	void buildMultiplayerScreen(gameui::Screen screen, gameui::StackLayout &column);
	bool activateMultiplayerControl(gameui::ControlId id);
	void changeMultiplayerControl(gameui::ControlId id);
	gameui::MultiplayerView m_multiplayer;
	std::optional<gameui::MultiplayerRequest> m_multiplayerRequest, m_multiplayerConfirm;
	gameui::TextField m_joinEndpoint, m_joinSeed, m_profileName, m_hostWorld, m_hostPort, m_hostBind,
		m_hostSlots, m_hostRadius;
	/// Last AUTOMATIC join endpoint (correction F06): while the player has
	/// not edited the draft, a session context change re-syncs the field to
	/// the connected endpoint or the most recent saved one; an edited draft
	/// survives navigation and session changes untouched.
	std::string m_joinEndpointAuto;
	gameui::Toggle m_hostLan, m_autoJoin{true};
	gameui::ListBox m_mpList, m_mpSecondList, m_mpThirdList;
	std::vector<std::string> m_mpRows, m_mpSecondRows;
	std::vector<std::string_view> m_mpLabels, m_mpSecondLabels;
	std::deque<std::string> m_mpText;
	std::shared_ptr<const localcontrol::Reply> m_mpAdminDisplayed;
	/// Roster row cache (correction F07): the Players list rebuilds only when
	/// the supervisor publishes a new monitor sample or the verified roles
	/// change — both tracked by pointer identity, not by content comparison.
	std::shared_ptr<const monitoring::Status> m_mpMonitorDisplayed;
	std::shared_ptr<const localcontrol::Reply> m_mpRosterAdminDisplayed;
	std::vector<std::string> m_mpRosterRows;
	std::vector<std::string_view> m_mpRosterLabels;
	/// Explicit administration target (correction F01): a stable identity,
	/// never a row index — a roster refresh moves rows without retargeting
	/// actions, and a disappearing target clears the selection instead of
	/// falling back onto another player.
	sim::PlayerId m_mpAdminTarget{};
	/// Administration context the selection belongs to (correction F12):
	/// server launch, verified world identity and live authorization. Any
	/// change clears the target and the cached reply — the same numeric
	/// PlayerId in another world or launch is a different player.
	uint64_t m_mpAdminContextLaunch{}, m_mpAdminContextWorld{};
	bool m_mpAdminContextAuthorized{};
	std::vector<std::string> m_mpAdminRows;
	std::vector<std::string_view> m_mpAdminLabels;
	int m_mpWorkerChoice{1}, m_mpStreamingChoice{1};
	std::string m_mpError, m_mpDetail;
	/// Persistent status slot (correction F19): the live connection or
	/// supervisor state, painted beside the transient operation feedback so
	/// neither can mask the other.
	std::string m_mpStatus;
	gameui::Clip m_mpStatusMessage{};
	/// Short-lived clipboard feedback (correction F08); cleared by the next
	/// join-screen operation.
	std::string m_mpClipboard;
	gameui::Clip m_mpMessage{};
	uint64_t m_mpListRevision{UINT64_MAX};
	/// m_mpError was raised by the Start server validation: it is cleared when
	/// the player leaves the host screens (correction F06).
	bool m_mpErrorOnHost{}, m_mpInitialized{};
	bool activateSessionControl(gameui::ControlId id);
	gameui::WorldCatalog m_worldCatalog;
	std::vector<std::string_view> m_worldNames;
	gameui::ListBox m_worldList;
	gameui::TextField m_worldName, m_worldSeed;
	std::string m_sessionStatus;
	gameui::Clip m_sessionMessage{};
	std::optional<gameui::SessionRequest> m_sessionRequest;
	bool m_retrySession{}, m_forceStop{}, m_failedQuit{}, m_remoteSession{}, m_persistentSession{}, m_hostedSession{};

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
