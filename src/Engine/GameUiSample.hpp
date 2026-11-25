#pragma once

#include <Renderer/GameUiRenderer.hpp>
#include <SDL3/SDL.h>

#include <optional>
#include <string_view>

// First consumer of the Game UI renderer. Input and styling stay on the host.
class GameUiSample
{
public:
	enum class InputResult { Unhandled, Consumed, Exit };

	void init(SDL_Window *window, VkContext &context, ImmediateCommands &immediate,
			  VkFormat format, VkColorSpaceKHR colorSpace, bool standalone);
	InputResult handleEvent(SDL_Window *window, const SDL_Event &event, bool canClick);
	void cancelPointerPress() { m_armed = false; }
	bool hasArmedPress() const { return m_armed; }
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

	gameui::Renderer m_renderer;
	gameui::DrawList m_draws;
	std::optional<gameui::RecordedStats> m_lastRecordedStats;
	std::array<Glyph, 128> m_glyphs{};
	SDL_WindowFlags m_windowFlags{};
	glm::vec4 m_button{};
	unsigned m_activations{};
	bool m_standalone{}, m_buttonFocused{}, m_armed{}, m_ownsPress{};
};
