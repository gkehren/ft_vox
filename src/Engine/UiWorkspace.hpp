#pragma once

// Visibility and input policy shared by Engine and the UI tests.

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

/// Hidden and suppressed workspaces skip both frame submission and input.
inline constexpr bool developerUiAvailable(bool imguiReady, bool nativeScreenSuppressed, Workspace workspace)
{
	return imguiReady && !nativeScreenSuppressed && visible(workspace);
}

/// Hiding the workspace must end every developer interaction it owned: focused
/// fields and in-flight pointer presses.
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
