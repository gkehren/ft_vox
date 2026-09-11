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
};

// Globals first (F1-F12), then gameplay keys. Gameplay entries stay gated
// behind !wantCaptureKeyboard at their call sites (InputRouting policy).
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
	{SDLK_P, "P", "Pause world tick", false},
	{SDLK_C, "C", "Capture / free mouse", false},
	{SDLK_B, "B", "Toggle chunk borders", false},
	{SDLK_T, "T", "Cycle selected block", false},
	{SDLK_V, "V", "Toggle walk / debug flight", false},
	{SDLK_X, "X", "Toggle flight speed boost", false},
};

inline const ShortcutRef *findShortcut(int keycode)
{
	for (const ShortcutRef &s : kShortcuts)
		if (s.keycode == keycode)
			return &s;
	return nullptr;
}

inline const char *shortcutKeyName(int keycode)
{
	const ShortcutRef *ref = findShortcut(keycode);
	return ref ? ref->key : nullptr;
}
} // namespace ui
