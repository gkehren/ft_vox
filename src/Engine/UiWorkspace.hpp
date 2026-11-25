#pragma once

// Developer workspace visibility policy (issue #303 F01/F04).
//
// The developer workspace is the ImGui chrome plus every docked developer/game
// panel. Gameplay (scene, native HUD, legacy inventory) lives outside it, so the
// workspace can be shown and hidden at runtime without restarting the
// application or losing the persisted layout.
//
// Two separate questions, deliberately kept apart:
//   * Is the workspace visible?  -> `Workspace`
//   * May ImGui own input?       -> `developerUiAvailable`
// The dockspace is submitted in BOTH states; hiding it only switches the
// ImGuiDockNodeFlags_KeepAliveOnly maintenance path, which keeps the dock node
// tree (and therefore imgui.ini) alive without rendering or input.
//
// Pure and header-only (no ImGui, no SDL): the visibility transitions and the
// input-ownership rule are unit-tested by tests/test_ui_shell and
// tests/test_game_ui_interaction, matching the UiScale.hpp / LayoutTiles.hpp
// policy-header pattern.

namespace ui
{
enum class Workspace
{
	Hidden = 0,
	Visible,
};

inline constexpr Workspace toggled(Workspace workspace)
{
	return workspace == Workspace::Visible ? Workspace::Hidden : Workspace::Visible;
}

inline constexpr bool visible(Workspace workspace)
{
	return workspace == Workspace::Visible;
}

/// The dockspace is never removed, only submitted in maintenance mode: same
/// dockspace id, same split tree, no rendering and no pointer input.
inline constexpr bool keepDockSpaceAliveOnly(Workspace workspace)
{
	return workspace == Workspace::Hidden;
}

/// ImGui may build a frame. Native player screens, the inspection view, the
/// --ui-spike proof and an active benchmark remove the developer layer
/// entirely; hiding the workspace does not.
inline constexpr bool developerUiPresent(bool imguiReady, bool nativeScreenSuppressed)
{
	return imguiReady && !nativeScreenSuppressed;
}

/// ImGui may own pointer and keyboard input. With the workspace hidden only the
/// legacy inventory is an interactive developer surface (issue #303 F01).
inline constexpr bool developerUiAvailable(bool imguiReady, bool nativeScreenSuppressed,
										   Workspace workspace, bool inventoryOpen)
{
	return developerUiPresent(imguiReady, nativeScreenSuppressed) &&
		(visible(workspace) || inventoryOpen);
}

/// Hiding the workspace must end every developer interaction it owned: focused
/// fields, in-flight pointer presses and the inventory drag cursor.
inline constexpr bool endsDeveloperInput(Workspace before, Workspace after)
{
	return visible(before) && !visible(after);
}

/// Workspace visibility is a UI-only transition, so it is withheld while a
/// benchmark owns the frame (issue #303 F11): Reloading, Warmup and Running must
/// never be perturbed by a new window/chrome composition, and the shortcut is
/// consumed either way. Menu and keyboard transitions share this rule.
inline constexpr bool allowsWorkspaceToggle(bool benchmarkActive)
{
	return !benchmarkActive;
}
} // namespace ui
