#pragma once

#include <Engine/PlayerUi.hpp>
#include <imgui/imgui.h>

// Application-level ImGui shell (issue #183): persistent dockspace with a
// passthru central region, main menu / navigation structure, right-side
// status strip and layout actions. Panel contents live in GameUI (Status
// Overlay, Player/Gameplay, World, Help, hints) and in the DebugUI developer
// panels (issue #179); the shell owns only chrome and navigation.
struct GameUIFrame;

namespace ui
{
/// One-shot request for which Help tab to select the next time the Help
/// window is drawn (menu entries open the window AND select their tab).
enum class HelpTabRequest
{
	None,
	Controls,
	About
};

/// Canonical window titles. Docking targets and Begin() calls must use the
/// same constants so default-layout docking cannot drift from the panels
/// (GameUI + DebugUI).
namespace windows
{
inline constexpr const char *kGraphics = "Graphics";
inline constexpr const char *kStreaming = "Streaming";
inline constexpr const char *kPerformance = "Performance";
inline constexpr const char *kOverview = "Overview";
inline constexpr const char *kRenderDebug = "Render Debug";
inline constexpr const char *kPlayerDiagnostics = "Player Diagnostics";
inline constexpr const char *kChunkInspector = "Chunk Inspector";
inline constexpr const char *kMemory = "Memory";
inline constexpr const char *kBenchmark = "Benchmark";
inline constexpr const char *kBenchmarkReport = "Benchmark Report";
inline constexpr const char *kWorld = "World";
inline constexpr const char *kHelp = "Help";
inline constexpr const char *kPlayer = "Player / Gameplay";
} // namespace windows

/// Panel visibility toggles (GameUI + DebugUI PanelState), mirrored into the
/// shell menus. The Status Overlay carries its density instead of a bool:
/// Off is hidden, Minimal/Detailed are the two shown densities (issue #184).
struct ShellToggles
{
	// GameUI-owned surfaces.
	playerui::StatusOverlayDensity *statusOverlay;
	bool *playerPanel; // interactive "Player / Gameplay" panel
	bool *graphics;	  // DebugUI rendering panel ("Graphics", settings)
	bool *streaming;
	bool *world;
	bool *help;
	bool *overlayHints;

	// DebugUI developer surfaces.
	bool *overview;
	bool *performance;
	bool *playerDiagnostics;
	bool *renderDebug;
	bool *chunkInspector;
	bool *memory;
	bool *benchmark;

	HelpTabRequest *helpTabRequest;
};

class UiShell
{
public:
	/// Create the root dockspace and run queued layout operations. Call once
	/// per frame before any window is submitted.
	void beginFrame();

	/// Main menu bar: ft_vox / World / View / Developer / Help + right-side
	/// status strip with graceful degradation at narrow widths.
	void drawMainMenuBar(GameUIFrame &frame, const ShellToggles &toggles);

	/// Undock everything and recreate an empty dockspace (predictable reset
	/// without deleting imgui.ini).
	void queueResetLayout() { m_resetLayoutQueued = true; }
	/// Apply the default developer layout (left: Graphics/Streaming/World,
	/// right: developer tools + Help, bottom: Performance/Benchmark, center:
	/// game passthru).
	void queueDefaultLayout() { m_defaultLayoutQueued = true; }

private:
	void resetLayout(ImGuiID dockspaceId);
	void applyDefaultDeveloperLayout(ImGuiID dockspaceId);

	bool m_defaultLayoutQueued{false};
	bool m_resetLayoutQueued{false};
};
} // namespace ui
