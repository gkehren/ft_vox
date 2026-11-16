#pragma once

// Input routing policy for keyboard events while ImGui may own the keyboard
// (issue #76; F8/F9/F11/F12 added with the developer console, issue #179).
// Pure and header-only so the policy stays unit-testable
// without ImGui or the engine.
//
// Policy summary:
// - F1-F12 (panel toggles, F10 = VSync) are intentional GLOBAL shortcuts:
//   they are function keys no text field can produce, so they keep working
//   even while ImGui captures the keyboard.
// - Gameplay keys (P = pause, C = mouse capture, B = chunk borders,
//   T = creative block cycle, 1-9 = hotbar selection, Q = drop item,
//   E = inventory panel) act on game state and MUST be suppressed while
//   ImGui captures the keyboard: typing 'p' in an InputText would
//   otherwise pause the world, 'c' would steal the cursor, 'b'/'t' would
//   toggle overlays, digits would type into the field AND select slots.
//   Classification and execution are separate: P/H are executed by GameUI,
//   C/B/T/1-9/Q/E by Engine - all only when the UI does not capture the
//   keyboard (issue #227: the gameplay keys issue simulation commands).
// - Escape is special: it is offered to the active UI/modal first
//   (ImGuiFileDialog cancels itself via IGFD_EXIT_KEY); the app quits only
//   when the UI does not own the keyboard and no application modal is open.
#include <SDL3/SDL_keycode.h>

enum class KeyRoute
{
		GlobalShortcut,    // F1-F12 application/developer shortcuts
	GameplayShortcut,  // P pause; C/B/T are Engine-owned gameplay keys
	NotGameUIShortcut  // not handled by GameUI shortcut routing
};

inline KeyRoute classifyKeyRoute(int sdlKeycode)
{
	switch (sdlKeycode)
	{
	case SDLK_F1:
	case SDLK_F2:
	case SDLK_F3:
	case SDLK_F4:
	case SDLK_F5:
	case SDLK_F6:
	case SDLK_F7:
	case SDLK_F8:  // Overview dashboard (issue #179)
	case SDLK_F9:  // Chunk inspector (issue #179)
	case SDLK_F10: // VSync toggle: explicit global policy (non-text key)
	case SDLK_F11: // Memory / workload (issue #179)
	case SDLK_F12: // Render debug views (issue #179)
		return KeyRoute::GlobalShortcut;
	case SDLK_P:
	case SDLK_C:
	case SDLK_B:
	case SDLK_T:
	case SDLK_V: // player flight toggle (never while typing in ImGui)
	case SDLK_X: // debug flight speed toggle
	case SDLK_H: // toggle layout tiles
	case SDLK_Q: // drop selected item (issue #227)
	case SDLK_E: // inventory panel toggle (issue #227; panel lands in #229)
	case SDLK_1: // hotbar selection 1-9 (issue #227; digits are text input
	case SDLK_2: // while ImGui captures, so they must route as gameplay)
	case SDLK_3:
	case SDLK_4:
	case SDLK_5:
	case SDLK_6:
	case SDLK_7:
	case SDLK_8:
	case SDLK_9:
		return KeyRoute::GameplayShortcut;
	default:
		return KeyRoute::NotGameUIShortcut;
	}
}

inline bool escapeShouldQuit(bool uiCapturesKeyboard, bool modalDialogOpen)
{
	return !uiCapturesKeyboard && !modalDialogOpen;
}
