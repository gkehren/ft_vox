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
	void resetScreens();
	void invalidateLayout();
	void back();
	void openPause();
	void closePause();
	bool syncInventory(bool open);
	void updateTextInput(SDL_Window *window, bool interactive);
	void yieldTextInput(SDL_Window *window);
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

	struct Glyph
	{
		glm::vec4 rect{}, uv{};
		float advance{};
		bool visible{};
	};

	void handleWindowEvent(Uint32 type);
	bool windowInteractive() const;
	void activate(const char *source);
	void text(glm::vec2 pos, std::string_view text, glm::vec4 color, float scale, gameui::Clip clip);
	float textWidth(std::string_view value, float scale) const;
	void buildScreen(gameui::Clip scene, float scale);
	void paintScreen(glm::vec2 pointer);
	void focusControl(const gameui::Widget *widget);
	InputResult useControl(const gameui::Widget &widget, glm::vec2 point = {});
	void editText(gameui::NavKey key, bool shift, bool shortcut);
	void setSlider(const gameui::Widget &widget, float x);

	gameui::Renderer m_renderer;
	gameui::DrawList m_draws;
	std::optional<gameui::RecordedStats> m_lastRecordedStats;
	std::array<Glyph, 128> m_glyphs{};
	SDL_WindowFlags m_windowFlags{};
	glm::vec4 m_button{};
	unsigned m_activations{};
	bool m_standalone{}, m_buttonFocused{}, m_armed{}, m_ownsPress{};
	gameui::ScreenStack m_screens;
	gameui::WidgetSet m_widgets;
	gameui::TextField m_field;
	gameui::ListSelection m_list;
	gameui::Clip m_panel{};
	uint64_t m_layoutRevision{UINT64_MAX};
	float m_screenScale{1.f}, m_slider{0.5f};
	bool m_feature{true}, m_ownsTextInput{};
};
