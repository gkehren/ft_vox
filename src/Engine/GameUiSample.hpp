#pragma once

#include <Engine/PlayerUiWidgets.hpp>
#include <Renderer/GameUiRenderer.hpp>
#include <SDL3/SDL.h>

#include <optional>
#include <string_view>

// Opt-in host for the native widget showcase and the standalone renderer proof.
class GameUiSample
{
public:
	enum class InputResult { Unhandled, Consumed, Exit };

	void init(SDL_Window *window, VkContext &context, ImmediateCommands &immediate,
			  VkFormat format, VkColorSpaceKHR colorSpace, bool standalone);
	InputResult handleEvent(SDL_Window *window, const SDL_Event &event, bool canClick);
	InputResult handleScreenEvent(const SDL_Event &event);
	void cancelPointerPress() { m_armed = false; m_widgets.cancelPress(); }
	bool hasArmedPress() const { return m_armed || m_widgets.hasPress(); }
	bool hitsStrip(float x, float y) const;
	bool canFocusWidget(float x, float y) const;
	void resetScreens();
	void invalidateLayout();
	void back();
	void openPause();
	void closePause();
	bool syncInventory(bool open);
	void updateTextInput(SDL_Window *window, bool interactive);
	bool yieldTextInput(SDL_Window *window);
	const gameui::ScreenStack &screens() const { return m_screens; }
	void prepare(SDL_Window *window, uint32_t slot, VkExtent2D extent, gameui::Clip view,
				 const TextureManager &atlas, uint64_t atlasGeneration);
	gameui::Renderer &renderer() { return m_renderer; }
	const std::optional<gameui::RecordedStats> &lastRecordedStats() const { return m_lastRecordedStats; }
	void publishRecordedStats(uint64_t frameNumber, uint32_t slot)
	{
		m_lastRecordedStats = gameui::RecordedStats{frameNumber, m_renderer.stats(slot)};
	}

private:
	friend struct UiSpikeProbe;
	friend struct GameUiBackendProbe;

	void handleWindowEvent(Uint32 type);
	bool windowInteractive() const;
	void activate(const char *source);
	gameui::FontView font() const { return {m_glyphs}; }
	void buildScreen(gameui::Clip scene, float scale);
	void paintScreen(glm::vec2 pointer);
	InputResult applyWidgetEvent(gameui::WidgetEvent event);

	gameui::Renderer m_renderer;
	gameui::DrawList m_draws;
	std::optional<gameui::RecordedStats> m_lastRecordedStats;
	std::array<gameui::Glyph, 128> m_glyphs{};
	SDL_WindowFlags m_windowFlags{};
	glm::vec4 m_button{};
	unsigned m_activations{};
	bool m_standalone{}, m_buttonFocused{}, m_armed{};
	gameui::ScreenStack m_screens;
	gameui::WidgetSet m_widgets;
	gameui::TextField m_field, m_secondaryField;
	gameui::TextField *m_sdlEditor{};
	gameui::Toggle m_feature{true}, m_enableSecond{true};
	gameui::Slider m_slider{0.5f}, m_secondarySlider{0.25f};
	gameui::ListBox m_list, m_secondaryList;
	gameui::Clip m_panel{};
	uint64_t m_layoutRevision{UINT64_MAX};
	float m_screenScale{1.f};
};
