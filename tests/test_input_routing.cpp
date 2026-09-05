// Input routing policy (issue #76): classifies which keyboard shortcuts are
// intentionally global while ImGui captures the keyboard, which are gameplay
// keys that must be suppressed, and when Escape is allowed to quit.
#include <Engine/InputRouting.hpp>

#include <iostream>

static int g_fails = 0;

#define CHECK(cond, msg)                                                       \
	do                                                                         \
	{                                                                          \
		if (!(cond))                                                           \
		{                                                                      \
			std::cerr << "FAIL: " << msg << " (" << __LINE__ << ")\n";         \
			++g_fails;                                                         \
		}                                                                      \
	} while (0)

static void checkRoute(int key, KeyRoute expected, const char *name)
{
	CHECK(classifyKeyRoute(key) == expected, name);
}

int main()
{
	// Global panel / application shortcuts: F1-F7 panel toggles, F10 VSync.
	// These are intentionally global (non-text function keys).
	checkRoute(SDLK_F1, KeyRoute::GlobalShortcut, "F1 global");
	checkRoute(SDLK_F2, KeyRoute::GlobalShortcut, "F2 global");
	checkRoute(SDLK_F3, KeyRoute::GlobalShortcut, "F3 global");
	checkRoute(SDLK_F4, KeyRoute::GlobalShortcut, "F4 global");
	checkRoute(SDLK_F5, KeyRoute::GlobalShortcut, "F5 global");
	checkRoute(SDLK_F6, KeyRoute::GlobalShortcut, "F6 global");
	checkRoute(SDLK_F7, KeyRoute::GlobalShortcut, "F7 global");
	checkRoute(SDLK_F10, KeyRoute::GlobalShortcut, "F10 global (explicit VSync policy)");

	// Gameplay shortcuts must be routed behind !wantCaptureKeyboard. P is
	// executed by GameUI; C/B/T are Engine-owned gameplay keys classified
	// by the same policy (mouse capture, chunk borders, selected block).
	checkRoute(SDLK_P, KeyRoute::GameplayShortcut, "P gameplay");
	checkRoute(SDLK_C, KeyRoute::GameplayShortcut, "C gameplay");
	checkRoute(SDLK_B, KeyRoute::GameplayShortcut, "B gameplay");
	checkRoute(SDLK_T, KeyRoute::GameplayShortcut, "T gameplay");
	checkRoute(SDLK_V, KeyRoute::GameplayShortcut, "V flight toggle gameplay");
	checkRoute(SDLK_X, KeyRoute::GameplayShortcut, "X flight speed gameplay");

	// Escape is special (UI/modal routing first, then possibly quit) and
	// must never enter the shortcut routes.
	checkRoute(SDLK_ESCAPE, KeyRoute::NotGameUIShortcut, "Escape not a GameUI shortcut");

	// Anything else (text input, digits, other letters) stays inert.
	checkRoute(SDLK_A, KeyRoute::NotGameUIShortcut, "A not a shortcut");
	checkRoute(SDLK_1, KeyRoute::NotGameUIShortcut, "digit not a shortcut");
	checkRoute(SDLK_SPACE, KeyRoute::NotGameUIShortcut, "space not a shortcut");
	checkRoute(SDLK_RETURN, KeyRoute::NotGameUIShortcut, "return not a shortcut");

	// This helper only decides whether the application may quit.
	// Modal dismissal itself is handled by ImGuiFileDialog.
	CHECK(escapeShouldQuit(false, false), "escape quits with free UI");
	CHECK(!escapeShouldQuit(true, false),
		  "escape does not quit while ImGui captures keyboard");
	CHECK(!escapeShouldQuit(false, true),
		  "escape does not quit while a modal dialog is open");
	CHECK(!escapeShouldQuit(true, true),
		  "escape does not quit when UI captures and a modal is open");

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "PASS: input routing policy - global F-keys, gated gameplay "
			  << "keys, Escape quit gating\n";
	return 0;
}
