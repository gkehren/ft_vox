#pragma once

// Centralized shortcut display metadata (issue #183). Pure and header-only so
// menu labels and the Help / Controls listing stay consistent with the
// binding policy in InputRouting.hpp without introducing a command system.
//
// tests/test_ui_shell cross-checks the `global` flag of every entry against
// classifyKeyRoute(): adding a binding without metadata (or with the wrong
// classification) fails the test.
#include <SDL3/SDL_keycode.h>

namespace ui
{
struct ShortcutRef
{
	int keycode;
	const char *key;	// display name, e.g. "F1"
	const char *action; // shared by menus and Help / Controls
	bool global;		// intentional global: works while ImGui owns the keyboard
	/// Modifiers the binding requires. Plain bindings leave this at
	/// SDL_KMOD_NONE, so (keycode, mods) is the unique binding identity
	/// (issue #303 F04: Ctrl+Shift+D vs the unbound plain D).
	SDL_Keymod mods{SDL_KMOD_NONE};
};

// Globals first (F1-F12 plus the workspace chord), then gameplay keys. Gameplay
// entries stay gated behind !wantCaptureKeyboard at their call sites
// (InputRouting policy).
inline constexpr ShortcutRef kShortcuts[] = {
	{SDLK_F1, "F1", "Status overlay density", true},
	{SDLK_F2, "F2", "Graphics", true},
	{SDLK_F3, "F3", "Streaming", true},
	{SDLK_F4, "F4", "World / biome map", true},
	{SDLK_F5, "F5", "Help / controls", true},
	{SDLK_F6, "F6", "On-screen hints", true},
	{SDLK_F7, "F7", "Performance", true},
	{SDLK_F8, "F8", "Overview", true},
	{SDLK_F9, "F9", "Chunk inspector", true},
	{SDLK_F10, "F10", "Toggle VSync", true},
	{SDLK_F11, "F11", "Memory / workload", true},
	{SDLK_F12, "F12", "Render debug", true},
	{SDLK_D, "Ctrl+Shift+D", "Show / hide developer workspace", true, SDL_KMOD_CTRL | SDL_KMOD_SHIFT},
	{SDLK_P, "P", "Pause world tick", false},
	{SDLK_C, "C", "Capture / free mouse", false},
	{SDLK_B, "B", "Toggle chunk borders", false},
	{SDLK_T, "T", "Cycle creative block (flight)", false},
	{SDLK_V, "V", "Toggle walk / debug flight", false},
	{SDLK_X, "X", "Toggle flight speed boost", false},
	{SDLK_H, "H", "Toggle layout tiles", false},
	// Issue #227 (Epic #223 Phase D): gameplay interaction keys. The digit
	// entry represents the 1-9 hotbar range (all classified GameplayShortcut).
	{SDLK_1, "1-9", "Select hotbar slot", false},
	{SDLK_Q, "Q", "Drop selected item", false},
	{SDLK_E, "E", "Toggle inventory panel", false},
};

/// Binding lookup. `mods` defaults to none, so existing call sites keep asking
/// for the plain binding; a chord is found only with its exact modifiers.
inline const ShortcutRef *findShortcut(int keycode, SDL_Keymod mods = SDL_KMOD_NONE)
{
	for (const ShortcutRef &s : kShortcuts)
		if (s.keycode == keycode && s.mods == mods)
			return &s;
	return nullptr;
}

inline const char *shortcutKeyName(int keycode, SDL_Keymod mods = SDL_KMOD_NONE)
{
	const ShortcutRef *ref = findShortcut(keycode, mods);
	return ref ? ref->key : nullptr;
}
} // namespace ui
