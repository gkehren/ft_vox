#include <Engine/Engine.hpp>
#include <Engine/GameUiSample.hpp>

#include "TestMain.hpp"

#include <stdexcept>

namespace
{
void pushEvent(SDL_Event &event)
{
	if (!SDL_PushEvent(&event))
		throw std::runtime_error(std::string("could not queue spike event: ") + SDL_GetError());
}

void pushWindowEvent(SDL_WindowID id, Uint32 type, int width = 0, int height = 0)
{
	SDL_Event event{};
	event.type = type;
	event.window.windowID = id;
	event.window.data1 = width;
	event.window.data2 = height;
	pushEvent(event);
}

void pushKey(SDL_WindowID id, SDL_Keycode key, bool repeat = false)
{
	SDL_Event event{};
	event.type = SDL_EVENT_KEY_DOWN;
	event.key.windowID = id;
	event.key.key = key;
	event.key.repeat = repeat;
	pushEvent(event);
}

void pushButton(SDL_WindowID id, Uint32 type, float x, float y)
{
	SDL_Event event{};
	event.type = type;
	event.button.windowID = id;
	event.button.button = SDL_BUTTON_LEFT;
	event.button.x = x;
	event.button.y = y;
	pushEvent(event);
}
} // namespace

struct UiSpikeProbe
{
	static void run(Engine &engine)
	{
		engine.enableUiSpike();
		engine.running = true;
		SDL_Window *window = engine.window;
		const SDL_WindowID id = SDL_GetWindowID(window);
		const GameUiSample &spike = *engine.m_playerUi;

		engine.handleEvents();
		pushWindowEvent(id, SDL_EVENT_WINDOW_FOCUS_GAINED);
		pushKey(id, SDLK_RETURN);
		engine.handleEvents();
		CHECK(spike.m_activations == 0, "Enter before Tab cannot activate");
		pushKey(id, SDLK_TAB);
		pushKey(id, SDLK_RETURN);
		pushKey(id, SDLK_RETURN, true);
		engine.handleEvents();
		CHECK(spike.m_activations == 1, "Tab and Enter activate once despite key repeat");

		const VkExtent2D previous = engine.swapchain->getExtent();
		if (!SDL_SetWindowSize(window, 1160, 680) || !SDL_SyncWindow(window))
			throw std::runtime_error(std::string("could not resize spike window: ") + SDL_GetError());
		engine.handleEvents();
		int width = 0, height = 0, pixelWidth = 0, pixelHeight = 0;
		if (!SDL_GetWindowSize(window, &width, &height) ||
			!SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight) ||
			pixelWidth <= 0 || pixelHeight <= 0)
			throw std::runtime_error("spike window has no drawable extent");
		engine.recreateSwapchainIfNeeded(static_cast<uint32_t>(pixelWidth), static_cast<uint32_t>(pixelHeight));
		const VkExtent2D resized = engine.swapchain->getExtent();
		CHECK(resized.width == static_cast<uint32_t>(pixelWidth) &&
			resized.height == static_cast<uint32_t>(pixelHeight) &&
			(resized.width != previous.width || resized.height != previous.height),
			"actual window resize reaches the swapchain");

		// An interior point in the button in the resized window, in SDL coordinates.
		const float display = SDL_GetWindowDisplayScale(window);
		const float density = SDL_GetWindowPixelDensity(window);
		const float scale = display > 0.f && density > 0.f ? display / density : 1.f;
		const float x = static_cast<float>(width) * 0.5f;
		const float y = static_cast<float>(height) * 0.5f + 30.f * scale;
		const auto press = [&] { pushButton(id, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y); };
		const auto release = [&] { pushButton(id, SDL_EVENT_MOUSE_BUTTON_UP, x, y); };

		pushWindowEvent(id, SDL_EVENT_WINDOW_FOCUS_GAINED);
		press();
		pushButton(id, SDL_EVENT_MOUSE_BUTTON_UP, 0.f, 0.f);
		engine.handleEvents();
		CHECK(spike.m_activations == 1, "releasing outside cancels the mouse press");
		press();
		release();
		engine.handleEvents();
		CHECK(spike.m_activations == 2, "the resized button accepts a new click");

		press();
		pushWindowEvent(id, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, pixelWidth, pixelHeight);
		release();
		engine.handleEvents();
		CHECK(spike.m_activations == 2, "pixel-only resize cancels an armed press");

		press();
		pushWindowEvent(id, SDL_EVENT_WINDOW_MINIMIZED);
		release();
		pushKey(id, SDLK_TAB);
		pushKey(id, SDLK_RETURN);
		press();
		pushWindowEvent(id, SDL_EVENT_WINDOW_RESTORED);
		release();
		engine.handleEvents();
		CHECK(spike.m_activations == 2, "minimizing cancels presses and ignores later input until restore");

		press();
		pushWindowEvent(id, SDL_EVENT_WINDOW_FOCUS_LOST);
		release();
		pushWindowEvent(0, SDL_EVENT_WINDOW_FOCUS_GAINED);
		pushKey(id, SDLK_TAB);
		pushKey(id, SDLK_RETURN);
		pushKey(id, SDLK_ESCAPE);
		press();
		release();
		engine.handleEvents();
		CHECK(spike.m_activations == 2 && engine.running, "queued keys and clicks after focus loss stay consumed");

		pushWindowEvent(id, SDL_EVENT_WINDOW_FOCUS_GAINED);
		pushKey(id, SDLK_RETURN);
		engine.handleEvents();
		CHECK(spike.m_activations == 2, "window focus gain does not restore button focus");
		pushKey(id, SDLK_TAB);
		pushKey(id, SDLK_SPACE);
		pushKey(id, SDLK_ESCAPE);
		engine.handleEvents();
		CHECK(spike.m_activations == 3 && !engine.running, "fresh focus enables one activation and Escape exits");
	}
};

int runUiSpikeSmoke(Engine &engine)
{
	UiSpikeProbe::run(engine);
	if (testmain::fails == 0)
		std::cout << "PASS: UI spike input and resize (" << testmain::checks << " checks)\n";
	return testmain::exitCode();
}
