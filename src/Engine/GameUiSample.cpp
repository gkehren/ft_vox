#include <Engine/GameUiSample.hpp>
#include <imgui/imgui.h>
#include <utils.hpp>

#include <cstdio>
#include <stdexcept>

namespace
{
constexpr glm::vec4 kInk{0.94f, 0.97f, 1.f, 1.f};
constexpr glm::vec4 kMuted{0.69f, 0.78f, 0.85f, 1.f};
constexpr glm::vec4 kAccent{0.31f, 0.89f, 0.76f, 1.f};
constexpr glm::vec4 kPanel{0.065f, 0.10f, 0.14f, 0.83f};
constexpr std::array kScales{1.f, 1.25f, 1.5f, 1.75f};

struct Layout
{
	glm::vec2 windowSize;
	glm::vec2 origin;
	float scale;
	glm::vec4 buttonLocal;

	glm::vec4 rect(glm::vec4 local) const { return local * scale + glm::vec4(origin, origin); }
	glm::vec4 button() const { return rect(buttonLocal); }
};

Layout layoutFor(SDL_Window *window, gameui::Clip view, bool standalone, unsigned activations)
{
	int w = 0, h = 0;
	SDL_GetWindowSize(window, &w, &h);
	const glm::vec2 size{static_cast<float>(w), static_cast<float>(h)};
	if (view.empty())
		view = {{0.f, 0.f, size.x, size.y}};
	const auto &r = view.rect;
	const glm::vec2 available{r.z - r.x, r.w - r.y};
	const glm::vec2 panel = standalone ? glm::vec2(400.f, 240.f) : glm::vec2(384.f, 82.f);
	const float density = SDL_GetWindowPixelDensity(window);
	const float display = SDL_GetWindowDisplayScale(window);
	const float logicalScale = density > 0.f && display > 0.f ? display / density : 1.f;
	const float scale = std::min({logicalScale * kScales[activations % kScales.size()],
		available.x / (panel.x + 24.f), available.y / (panel.y + 32.f)});
	const glm::vec2 origin{r.x + (available.x - scale * panel.x) * 0.5f,
		standalone ? r.y + (available.y - scale * panel.y) * 0.5f : r.y + 16.f * scale};
	return {size, origin, scale, standalone ? glm::vec4(24, 143, 220, 185) : glm::vec4(244, 20, 365, 61)};
}

bool contains(glm::vec4 rect, float x, float y)
{
	return x >= rect.x && x < rect.z && y >= rect.y && y < rect.w;
}
} // namespace

void GameUiSample::init(SDL_Window *window, VkContext &context, ImmediateCommands &immediate,
						VkFormat format, VkColorSpaceKHR colorSpace, bool standalone)
{
	m_windowFlags = SDL_GetWindowFlags(window);
	m_standalone = standalone;
	ImFontAtlas atlas;
	atlas.Flags = ImFontAtlasFlags_NoMouseCursors | ImFontAtlasFlags_NoBakedLines;
	ImFont *font = atlas.AddFontDefault();
	unsigned char *pixels = nullptr;
	int width = 0, height = 0;
	atlas.GetTexDataAsAlpha8(&pixels, &width, &height);
	if (!pixels || width <= 0 || height <= 0)
		throw std::runtime_error("Game UI font atlas bake failed");
	for (ImWchar c = 32; c < m_glyphs.size(); ++c)
	{
		const ImFontGlyph &g = *font->FindGlyph(c);
		m_glyphs[c] = {{g.X0, g.Y0, g.X1, g.Y1}, {g.U0, g.V0, g.U1, g.V1}, g.AdvanceX, g.Visible != 0};
	}
	m_renderer.init(context, immediate, {uint32_t(width), uint32_t(height)},
		{pixels, size_t(width) * height}, format, colorSpace);
}

bool GameUiSample::windowInteractive() const
{
	constexpr SDL_WindowFlags mask = SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED;
	return (m_windowFlags & mask) == SDL_WINDOW_INPUT_FOCUS;
}

void GameUiSample::handleWindowEvent(Uint32 type)
{
	switch (type)
	{
	case SDL_EVENT_WINDOW_FOCUS_GAINED:
		m_windowFlags |= SDL_WINDOW_INPUT_FOCUS;
		break;
	case SDL_EVENT_WINDOW_FOCUS_LOST:
		m_windowFlags &= ~SDL_WINDOW_INPUT_FOCUS;
		break;
	case SDL_EVENT_WINDOW_HIDDEN:
		m_windowFlags |= SDL_WINDOW_HIDDEN;
		m_windowFlags &= ~SDL_WINDOW_INPUT_FOCUS;
		break;
	case SDL_EVENT_WINDOW_SHOWN:
		m_windowFlags &= ~SDL_WINDOW_HIDDEN;
		break;
	case SDL_EVENT_WINDOW_MINIMIZED:
		m_windowFlags |= SDL_WINDOW_MINIMIZED;
		break;
	case SDL_EVENT_WINDOW_RESTORED:
	case SDL_EVENT_WINDOW_MAXIMIZED:
		m_windowFlags &= ~SDL_WINDOW_MINIMIZED;
		break;
	case SDL_EVENT_WINDOW_RESIZED:
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
	case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
		break;
	case SDL_EVENT_WINDOW_MOUSE_LEAVE:
		cancelPointerPress();
		return;
	default:
		return;
	}
	m_armed = false;
	m_button = {};
	invalidateLayout();
	if (!windowInteractive())
		m_buttonFocused = false;
}

GameUiSample::InputResult GameUiSample::handleEvent(SDL_Window *window, const SDL_Event &event, bool canClick)
{
	if (!m_standalone && !canClick)
		cancelPointerPress();
	const SDL_WindowID id = SDL_GetWindowID(window);
	if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST)
	{
		if (event.window.windowID == id)
			handleWindowEvent(event.type);
		return InputResult::Unhandled;
	}

	switch (event.type)
	{
	case SDL_EVENT_KEY_DOWN:
		if (!m_standalone)
			return InputResult::Unhandled;
		if (event.key.windowID == id && windowInteractive() && !event.key.repeat)
		{
			if (event.key.key == SDLK_ESCAPE)
				return InputResult::Exit;
			if (event.key.key == SDLK_TAB)
				m_buttonFocused = true;
			else if (m_buttonFocused && (event.key.key == SDLK_RETURN || event.key.key == SDLK_SPACE))
				activate("keyboard");
		}
		return InputResult::Consumed;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
	{
		if (event.button.windowID != id || event.button.button != SDL_BUTTON_LEFT)
			return m_standalone ? InputResult::Consumed : InputResult::Unhandled;
		const bool inside = (m_standalone ? windowInteractive() : canClick) &&
			contains(m_standalone ? layoutFor(window, {}, true, 0).button() : m_button,
				event.button.x, event.button.y);
		if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
		{
			m_buttonFocused = m_armed = m_ownsPress = inside;
			return inside || m_standalone ? InputResult::Consumed : InputResult::Unhandled;
		}
		const bool consumes = m_standalone || m_ownsPress;
		if (m_armed && inside)
			activate("mouse");
		m_ownsPress = m_armed = false;
		return consumes ? InputResult::Consumed : InputResult::Unhandled;
	}
	case SDL_EVENT_KEY_UP:
	case SDL_EVENT_TEXT_INPUT:
	case SDL_EVENT_TEXT_EDITING:
	case SDL_EVENT_MOUSE_MOTION:
	case SDL_EVENT_MOUSE_WHEEL:
		return m_standalone ? InputResult::Consumed : InputResult::Unhandled;
	default:
		return InputResult::Unhandled;
	}
}

void GameUiSample::activate(const char *source)
{
	++m_activations;
	if (m_standalone)
		std::printf("UI spike activated by %s (%u)\n", source, m_activations);
}

void GameUiSample::text(glm::vec2 pos, std::string_view value, glm::vec4 color, float scale, gameui::Clip clip)
{
	for (size_t i = 0; i < value.size(); i = gameui::nextUtf8(value, i))
	{
		const auto c = static_cast<unsigned char>(value[i]);
		const Glyph &g = m_glyphs[c >= 32 && c < m_glyphs.size() ? c : '?'];
		if (g.visible)
			m_draws.glyph(g.rect * scale + glm::vec4(pos, pos), g.uv, color, clip);
		pos.x += g.advance * scale;
	}
}

float GameUiSample::textWidth(std::string_view value, float scale) const
{
	float width = 0.f;
	for (size_t i = 0; i < value.size(); i = gameui::nextUtf8(value, i))
	{
		const auto c = static_cast<unsigned char>(value[i]);
		width += m_glyphs[c >= 32 && c < m_glyphs.size() ? c : '?'].advance;
	}
	return width * scale;
}

bool GameUiSample::hitsStrip(float x, float y) const
{
	return m_screens.top() == gameui::Screen::Hud && contains(m_button, x, y);
}

void GameUiSample::prepare(SDL_Window *window, uint32_t slot, VkExtent2D extent, gameui::Clip view,
						   const TextureManager &atlas, uint64_t atlasGeneration)
{
	const Layout layout = layoutFor(window, view, m_standalone, m_standalone ? 0 : m_activations);
	m_draws.reset(layout.windowSize, atlasGeneration);
	const auto priorButton = m_button;
	m_button = {};
	if (!m_standalone && m_screens.nativeScreen())
	{
		const float density = SDL_GetWindowPixelDensity(window);
		const float display = SDL_GetWindowDisplayScale(window);
		const float scale = density > 0.f && display > 0.f ? display / density : 1.f;
		const auto bounds = m_draws.bounds();
		buildScreen(m_screens.suppressDeveloper() || view.empty() ? bounds : view.intersect(bounds),
			std::clamp(scale * kScales[m_activations % kScales.size()], 0.75f, 2.f));
		float x = 0.f, y = 0.f;
		SDL_GetMouseState(&x, &y);
		paintScreen(SDL_GetMouseFocus() == window ? glm::vec2{x, y} : glm::vec2{-1});
	}
	else if (layout.scale > 0.f && (m_standalone || m_screens.top() == gameui::Screen::Hud))
	{
		m_button = layout.button();
		if (priorButton != m_button)
			cancelPointerPress();
		const auto clip = view.empty() ? m_draws.bounds() : view.intersect(m_draws.bounds());
		const auto panel = [&](glm::vec4 rect, glm::vec4 color) { m_draws.panel(layout.rect(rect), color, clip); };
		const auto label = [&](glm::vec2 pos, std::string_view caption, glm::vec4 color, float fontScale = 1.f) {
			text(layout.origin + pos * layout.scale, caption, color, layout.scale * fontScale, clip);
		};
		float x = 0.f, y = 0.f;
		SDL_GetMouseState(&x, &y);
		const bool hovered = SDL_GetMouseFocus() == window && contains(m_button, x, y) &&
			(!m_standalone || windowInteractive());
		const glm::vec4 fill = m_armed && hovered ? kAccent :
			(hovered ? glm::vec4(0.23f, 0.55f, 0.50f, 0.94f) : glm::vec4(0.15f, 0.38f, 0.37f, 0.9f));
		const auto focusRing = [&] {
			if (m_buttonFocused)
				panel(layout.buttonLocal + glm::vec4(-2, -2, 2, 2), kInk);
			panel(layout.buttonLocal, fill);
		};
		const glm::vec4 buttonText = m_armed && hovered ? kPanel : kInk;
		char status[32];
		if (m_standalone)
		{
			panel({0, 0, 400, 240}, kPanel);
			panel({0, 0, 400, 4}, kAccent);
			label({24, 20}, "Game UI renderer", kInk, 2.f);
			label({24, 53}, "Resource pack atlas preview", kMuted);
			panel({24, 76, 376, 128}, {0.24f, 0.34f, 0.43f, 1.f});
			m_draws.icon(layout.rect({164, 58, 236, 130}), {atlasGeneration, GRASS_SIDE}, glm::vec4(1.f),
				clip.intersect({layout.rect({26, 78, 374, 126})}));
			focusRing();
			label({39, 157}, "Activate button", buttonText);
			std::snprintf(status, sizeof(status), "Activations: %u", m_activations);
			label({239, 157}, status, kInk);
			label({24, 205}, "Tab focus   Enter or click activate   Esc exit", kMuted);
		}
		else
		{
			panel({0, 0, 384, 82}, kPanel);
			panel({0, 0, 384, 3}, kAccent);
			panel({15, 17, 65, 65}, {0.24f, 0.34f, 0.43f, 1.f});
			m_draws.icon(layout.rect({12, 10, 68, 66}), {atlasGeneration, GRASS_SIDE}, glm::vec4(1.f),
				clip.intersect({layout.rect({17, 19, 63, 63})}));
			label({78, 20}, "Game UI", kInk, 1.5f);
			label({78, 46}, "C releases mouse", kMuted);
			focusRing();
			std::snprintf(status, sizeof(status), "Scale %.0f%%", 100.f * kScales[m_activations % kScales.size()]);
			label({261, 34}, status, buttonText);
		}
	}
	m_renderer.prepare(slot, extent, m_draws, atlas, atlasGeneration);
}
