#pragma once

// Engine-wide function keys can run while Developer UI owns the keyboard.
// Only the workspace toggle runs through a full-window native modal.
// The router sends gameplay and navigation keys to their current input owner.
#include <SDL3/SDL_keycode.h>

enum class KeyRoute
{
	GlobalShortcut,    // Function keys and the workspace chord
	GameplayShortcut,  // P pause; C/B/T are Engine-owned gameplay keys
	NotGameUIShortcut  // No registered shortcut
};

/// F1 and the legacy Ctrl+Shift+D chord show and hide the developer workspace.
/// Plain D is untouched, so text fields keep typing normally.
inline bool isWorkspaceToggle(int sdlKeycode, SDL_Keymod mods)
{
	const bool chord = (mods & SDL_KMOD_CTRL) != 0 && (mods & SDL_KMOD_SHIFT) != 0;
	return sdlKeycode == SDLK_F1 || (chord && sdlKeycode == SDLK_D);
}

inline KeyRoute classifyKeyRoute(int sdlKeycode, SDL_Keymod mods = SDL_KMOD_NONE)
{
	if (isWorkspaceToggle(sdlKeycode, mods))
		return KeyRoute::GlobalShortcut;
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
