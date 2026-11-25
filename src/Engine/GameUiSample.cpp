#include <Engine/GameUiSample.hpp>
#include <utils.hpp>

#include <cstdio>
#include <stdexcept>

namespace
{
// Spike/standalone proof composition. The player-facing surfaces take their
// colors from gameui::style::baseline() instead of these local constants.
constexpr glm::vec4 kInk{0.94f, 0.97f, 1.f, 1.f};
constexpr glm::vec4 kMuted{0.69f, 0.78f, 0.85f, 1.f};
constexpr glm::vec4 kAccent{0.31f, 0.89f, 0.76f, 1.f};
constexpr glm::vec4 kPanel{0.065f, 0.10f, 0.14f, 0.83f};

struct Layout
{
	glm::vec2 windowSize;
	glm::vec2 origin;
	float scale;
	glm::vec4 buttonLocal;

	glm::vec4 rect(glm::vec4 local) const { return local * scale + glm::vec4(origin, origin); }
	glm::vec4 button() const { return rect(buttonLocal); }
};

// The player UI scale is resolved by the ScaleController at the frame
// boundary; the proof composition only shrinks it further when its own panel
// would not fit.
Layout layoutFor(SDL_Window *window, gameui::Clip view, bool standalone, float resolvedScale)
{
	int w = 0, h = 0;
	SDL_GetWindowSize(window, &w, &h);
	const glm::vec2 size{static_cast<float>(w), static_cast<float>(h)};
	if (view.empty())
		view = {{0.f, 0.f, size.x, size.y}};
	const auto &r = view.rect;
	const glm::vec2 available{r.z - r.x, r.w - r.y};
	const glm::vec2 panel = standalone ? glm::vec2(400.f, 240.f) : glm::vec2(384.f, 82.f);
	const float scale = std::min({resolvedScale > 0.f ? resolvedScale : 1.f,
		available.x / (panel.x + 24.f), available.y / (panel.y + 32.f)});
	const glm::vec2 origin{r.x + (available.x - scale * panel.x) * 0.5f,
		standalone ? r.y + (available.y - scale * panel.y) * 0.5f : r.y + 16.f * scale};
	return {size, origin, scale, standalone ? glm::vec4(24, 143, 220, 185) : glm::vec4(244, 20, 365, 61)};
}

/// Logical window size and content scale for the player UI policy: the same two
/// SDL queries PlayerUiScale.hpp documents, read here so the policy stays pure.
gameui::ScaleInputs scaleInputsFor(SDL_Window *window)
{
	int w = 0, h = 0;
	SDL_GetWindowSize(window, &w, &h);
	// R02: pass the raw queries through, so a failed DPI read stays invalid and
	// the controller keeps the last valid Auto scale instead of seeing 100%.
	return gameui::scaleInputs({static_cast<float>(w), static_cast<float>(h)},
		SDL_GetWindowDisplayScale(window), SDL_GetWindowPixelDensity(window));
}

/// Framebuffer pixels per logical point, used to bake the face at device
/// resolution (the player UI equivalent of ImGuiLayer's FontGlobalScale).
/// `nullopt` when the window cannot report both sizes (minimized, mid-resize,
/// query failure): R02, an unknown density must not be mistaken for 1x and
/// trigger a pointless atlas rebuild.
std::optional<float> framebufferScaleFor(SDL_Window *window)
{
	int logicalW = 0, logicalH = 0, pixelW = 0, pixelH = 0;
	SDL_GetWindowSize(window, &logicalW, &logicalH);
	if (!SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH) || logicalW <= 0 || logicalH <= 0 ||
		pixelW <= 0 || pixelH <= 0)
		return std::nullopt;
	return static_cast<float>(pixelW) / static_cast<float>(logicalW);
}

bool contains(glm::vec4 rect, float x, float y)
{
	return x >= rect.x && x < rect.z && y >= rect.y && y < rect.w;
}
} // namespace

void GameUiSample::advancePresentation()
{
	static constexpr float kIndeterminatePeriod = 2.f; // seconds per progress sweep
	const auto now = std::chrono::steady_clock::now();
	const float elapsed = m_lastFrame ? std::chrono::duration<float>(now - *m_lastFrame).count() : 0.f;
	m_lastFrame = now;
	const float step = gameui::presentationStep(elapsed);
	m_toasts.advance(step);
	m_uiPhase += step / kIndeterminatePeriod;
	m_uiPhase -= std::floor(m_uiPhase);
}

bool GameUiSample::refreshFaceForDensity(float pixels)
{
	if (!(pixels > 0.f))
		return false;
	if (std::abs(pixels / m_facePixels - 1.f) < 0.01f)
		return true; // same density: nothing to rebuild
	// Bake and upload into locals; nothing is published until the GPU accepted it.
	gameui::BakedFace next;
	if (!gameui::bakeFace(pixels, next) ||
		(m_immediate &&
			!m_renderer.setFont(*m_immediate, {next.width, next.height}, next.coverage)))
		return false;
	m_face = std::move(next);
	m_facePixels = pixels;
	m_fontScale = m_face.fontScale;
	// Glyph metrics changed with the density: rebuild the geometry, keep state.
	invalidateLayout();
	return true;
}

void GameUiSample::refreshFace(SDL_Window *window)
{
	// S03: an explicit density wins over the queries; the window density is only
	// the default source.
	const auto pixels = m_faceDensityOverride ? m_faceDensityOverride : framebufferScaleFor(window);
	if (!pixels)
		return;
	// R07: a density whose replacement already failed is not retried every frame;
	// a new density (or a restore) clears the tombstone.
	if (m_faceFailedPixels && std::abs(*pixels - *m_faceFailedPixels) < 0.001f)
		return;
	if (refreshFaceForDensity(*pixels))
		m_faceFailedPixels.reset();
	else
		m_faceFailedPixels = pixels;
}

void GameUiSample::init(SDL_Window *window, VkContext &context, ImmediateCommands &immediate,
						VkFormat format, VkColorSpaceKHR colorSpace, bool standalone)
{
	m_windowFlags = SDL_GetWindowFlags(window);
	m_standalone = standalone;
	// One audited face (PlayerUiFonts.hpp), baked once at device resolution.
	const float pixels = framebufferScaleFor(window).value_or(1.f);
	if (!gameui::bakeFace(pixels, m_face))
		throw std::runtime_error("Game UI font atlas bake failed");
	m_fontScale = m_face.fontScale;
	m_facePixels = pixels;
	m_immediate = &immediate;
	m_renderer.init(context, immediate, {m_face.width, m_face.height}, m_face.coverage, format,
		colorSpace);
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
	const SDL_WindowID id = SDL_GetWindowID(window);
	if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST)
	{
		if (event.window.windowID == id)
			handleWindowEvent(event.type);
		return InputResult::Unhandled;
	}
	if (!m_standalone && !canClick)
		cancelPointerPress();

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
			contains(m_standalone ? layoutFor(window, {}, true, m_scale.effective()).button() : m_button,
				event.button.x, event.button.y);
		if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
		{
			m_buttonFocused = m_armed = inside;
			return inside || m_standalone ? InputResult::Consumed : InputResult::Unhandled;
		}
		const bool consumes = m_standalone || m_armed;
		if (m_armed && inside)
			activate("mouse");
		m_armed = false;
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

bool GameUiSample::hitsStrip(float x, float y) const
{
	return m_screens.top() == gameui::Screen::Hud && contains(m_button, x, y);
}

void GameUiSample::prepare(SDL_Window *window, uint32_t slot, VkExtent2D extent, gameui::Clip view,
						   const TextureManager &atlas, uint64_t atlasGeneration, const gameui::HudView *hud)
{
	// Player UI scale: resolved ONCE at the frame boundary, before any screen is
	// built. A changed value cancels the in-flight press and rebuilds the next
	// layout - never mid-paint (issue #302).
	m_atlasLayers = uint32_t(COUNT);
	advancePresentation();
	refreshFace(window);
	if (m_scale.resolve(scaleInputsFor(window)))
		invalidateLayout();
	const float resolvedScale = m_scale.effective();
	const Layout layout = layoutFor(window, view, m_standalone, resolvedScale);
	m_draws.reset(layout.windowSize, atlasGeneration);
	const auto priorButton = m_button;
	m_button = {};
	if (!m_standalone && m_screens.nativeScreen())
	{
		const auto bounds = m_draws.bounds();
		const auto scene = m_screens.suppressDeveloper() || view.empty() ? bounds : view.intersect(bounds);
		m_screenScale = resolvedScale;
		switch (m_screens.top())
		{
		case gameui::Screen::Gallery: buildGallery(scene, resolvedScale); break;
		case gameui::Screen::HudMock: buildHudMock(scene, resolvedScale); break;
		default: buildScreen(scene, resolvedScale); break;
		}
		float x = 0.f, y = 0.f;
		SDL_GetMouseState(&x, &y);
		const auto pointer = SDL_GetMouseFocus() == window ? glm::vec2{x, y} : glm::vec2{-1};
		if (m_screens.top() == gameui::Screen::Gallery || m_screens.top() == gameui::Screen::HudMock)
			paintDesignSystem(pointer);
		else
			paintScreen(pointer);
	}
	else if (layout.scale > 0.f && (m_standalone || (m_previewStrip && m_screens.top() == gameui::Screen::Hud)))
	{
		m_button = layout.button();
		if (priorButton != m_button)
			cancelPointerPress();
		const auto clip = view.empty() ? m_draws.bounds() : view.intersect(m_draws.bounds());
		const auto panel = [&](glm::vec4 rect, glm::vec4 color) { m_draws.panel(layout.rect(rect), color, clip); };
		// Every text path applies the framebuffer compensation, standalone proofs
		// included (issue #302 F11); the role ratio is 1 for these HUD labels.
		const auto label = [&](glm::vec2 pos, std::string_view caption, glm::vec4 color,
							   float roleScale = 1.f) {
			font().paint(m_draws, layout.origin + pos * layout.scale, caption, color,
				layout.scale * roleScale * m_fontScale, clip, clip);
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
			std::snprintf(status, sizeof(status), "Scale %.0f%%", 100.f * resolvedScale);
			label({261, 34}, status, buttonText);
		}
	}
	if (hud && m_hudVisible && !m_standalone && m_screens.top() == gameui::Screen::Hud)
	{
		const auto scene = m_draws.bounds();
		const auto hudGeometry = gameui::hudLayout(scene, view.empty() ? scene : view,
			gameui::style::baseline(), resolvedScale);
		if (hudGeometry.scale > 0.f)
		{
			const gameui::Surface paint(m_draws, font(), gameui::style::baseline(), hudGeometry.scale,
				hudGeometry.view, m_atlasLayers, m_fontScale);
			gameui::paintHud(paint, hudGeometry, scene, *hud);
		}
	}
	m_renderer.prepare(slot, extent, m_draws, atlas, atlasGeneration);
}
