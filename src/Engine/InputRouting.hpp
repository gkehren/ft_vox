#pragma once

// Input routing policy for keyboard events while ImGui may own the keyboard
// (issue #76). Pure and header-only so the policy stays unit-testable
// without ImGui or the engine.
//
// Policy summary:
// - F1-F7 (panel toggles) and F10 (VSync) are intentional GLOBAL shortcuts:
//   they are function keys no text field can produce, so they keep working
//   even while ImGui captures the keyboard.
// - Gameplay keys (P = pause, C = mouse capture, B = chunk borders,
//   T = selected block) act on game state and MUST be suppressed while
//   ImGui captures the keyboard: typing 'p' in an InputText would
//   otherwise pause the world, 'c' would steal the cursor, 'b'/'t' would
//   toggle overlays. Classification and execution are separate: P is
//   executed by GameUI, C/B/T by Engine - both only when the UI does not
//   capture the keyboard.
// - Escape is special: it is offered to the active UI/modal first
//   (ImGuiFileDialog cancels itself via IGFD_EXIT_KEY); the app quits only
//   when the UI does not own the keyboard and no application modal is open.
#include <SDL3/SDL_keycode.h>

enum class KeyRoute
{
	GlobalShortcut,	   // F1-F7 panel toggles, F10 VSync
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
	case SDLK_F10: // VSync toggle: explicit global policy (non-text key)
		return KeyRoute::GlobalShortcut;
	case SDLK_P:
	case SDLK_C:
	case SDLK_B:
	case SDLK_T:
	case SDLK_V: // player flight toggle (never while typing in ImGui)
	case SDLK_X: // debug flight speed toggle
		return KeyRoute::GameplayShortcut;
	default:
		return KeyRoute::NotGameUIShortcut;
	}
}

inline bool escapeShouldQuit(bool uiCapturesKeyboard, bool modalDialogOpen)
{
	return !uiCapturesKeyboard && !modalDialogOpen;
}
