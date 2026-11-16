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
	checkRoute(SDLK_F8, KeyRoute::GlobalShortcut, "F8 global (Overview)");
	checkRoute(SDLK_F9, KeyRoute::GlobalShortcut, "F9 global (Chunk inspector)");
	checkRoute(SDLK_F10, KeyRoute::GlobalShortcut, "F10 global (explicit VSync policy)");
	checkRoute(SDLK_F11, KeyRoute::GlobalShortcut, "F11 global (Memory)");
	checkRoute(SDLK_F12, KeyRoute::GlobalShortcut, "F12 global (Render debug)");

	// Gameplay shortcuts must be routed behind !wantCaptureKeyboard. P/H are
	// executed by GameUI; C/B/T/1-9/Q/E are Engine-owned gameplay keys
	// classified by the same policy (mouse capture, chunk borders, creative
	// block cycle, hotbar selection, item drop, inventory panel — issue #227).
	checkRoute(SDLK_P, KeyRoute::GameplayShortcut, "P gameplay");
	checkRoute(SDLK_C, KeyRoute::GameplayShortcut, "C gameplay");
	checkRoute(SDLK_B, KeyRoute::GameplayShortcut, "B gameplay");
	checkRoute(SDLK_T, KeyRoute::GameplayShortcut, "T gameplay");
	checkRoute(SDLK_V, KeyRoute::GameplayShortcut, "V flight toggle gameplay");
	checkRoute(SDLK_X, KeyRoute::GameplayShortcut, "X flight speed gameplay");
	checkRoute(SDLK_H, KeyRoute::GameplayShortcut, "H toggle layout tiles gameplay");
	checkRoute(SDLK_Q, KeyRoute::GameplayShortcut, "Q drop item gameplay (issue #227)");
	checkRoute(SDLK_E, KeyRoute::GameplayShortcut, "E inventory panel gameplay (issue #227)");
	// Digits 1-9 select hotbar slots (issue #227): they are text input while
	// ImGui captures the keyboard, so they MUST route as gameplay keys.
	checkRoute(SDLK_1, KeyRoute::GameplayShortcut, "1 hotbar gameplay");
	checkRoute(SDLK_2, KeyRoute::GameplayShortcut, "2 hotbar gameplay");
	checkRoute(SDLK_3, KeyRoute::GameplayShortcut, "3 hotbar gameplay");
	checkRoute(SDLK_4, KeyRoute::GameplayShortcut, "4 hotbar gameplay");
	checkRoute(SDLK_5, KeyRoute::GameplayShortcut, "5 hotbar gameplay");
	checkRoute(SDLK_6, KeyRoute::GameplayShortcut, "6 hotbar gameplay");
	checkRoute(SDLK_7, KeyRoute::GameplayShortcut, "7 hotbar gameplay");
	checkRoute(SDLK_8, KeyRoute::GameplayShortcut, "8 hotbar gameplay");
	checkRoute(SDLK_9, KeyRoute::GameplayShortcut, "9 hotbar gameplay");

	// Escape is special (UI/modal routing first, then possibly quit) and
	// must never enter the shortcut routes.
	checkRoute(SDLK_ESCAPE, KeyRoute::NotGameUIShortcut, "Escape not a GameUI shortcut");

	// Anything else (text input, other digits, other letters) stays inert.
	checkRoute(SDLK_A, KeyRoute::NotGameUIShortcut, "A not a shortcut");
	checkRoute(SDLK_0, KeyRoute::NotGameUIShortcut, "digit 0 not a hotbar shortcut");
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
