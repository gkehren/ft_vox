#pragma once

// Pure state behind the H "layout tiles" shortcut: capture / hide / restore
// of the developer-panel tile flags. No ImGui and no engine dependencies
// beyond the plain PanelState struct, so the save/restore contract is
// unit-testable headlessly (test_ui_shell). GameUI owns the hidden-mode
// flag and drives these helpers from the H shortcut and the View menu.

#include <Engine/DebugUI/DebugUiCore.hpp>

namespace ui
{
/// Snapshot of every tile flag the H shortcut manages. Deliberately NOT a
/// copy of PanelState: the Status Overlay density and the on-screen hints
/// are gameplay overlays, not layout tiles, and survive H untouched.
struct LayoutTilesSnapshot
{
	bool rendering{false};
	bool streaming{false};
	bool world{false};
	bool playerPanel{false};
	bool help{false};
	bool overview{false};
	bool performance{false};
	bool playerDiagnostics{false};
	bool chunkInspector{false};
	bool memory{false};
	bool renderDebug{false};
	bool benchmark{false};
};

/// True when any H-managed tile flag is set. Diagnostic only — hidden mode
/// is an explicit flag, never derived from this (a layout that is hidden
/// while every panel happens to be closed is still hidden).
inline bool anyLayoutTileVisible(const debugui::PanelState &panels)
{
	return panels.rendering || panels.streaming || panels.world || panels.playerPanel ||
		   panels.help || panels.overview || panels.performance || panels.playerDiagnostics ||
		   panels.chunkInspector || panels.memory || panels.renderDebug || panels.benchmark;
}

/// Capture the current tile flags (entering hidden mode).
inline LayoutTilesSnapshot captureLayoutTiles(const debugui::PanelState &panels)
{
	return {panels.rendering, panels.streaming,	   panels.world,
			panels.playerPanel, panels.help,   panels.overview,
			panels.performance, panels.playerDiagnostics, panels.chunkInspector,
			panels.memory, panels.renderDebug, panels.benchmark};
}

/// Show/hide every H-managed tile flag.
inline void setAllLayoutTiles(debugui::PanelState &panels, bool visible)
{
	panels.rendering = visible;
	panels.streaming = visible;
	panels.world = visible;
	panels.playerPanel = visible;
	panels.help = visible;
	panels.overview = visible;
	panels.performance = visible;
	panels.playerDiagnostics = visible;
	panels.chunkInspector = visible;
	panels.memory = visible;
	panels.renderDebug = visible;
	panels.benchmark = visible;
}

/// Write a snapshot back exactly (leaving hidden mode).
inline void restoreLayoutTiles(debugui::PanelState &panels, const LayoutTilesSnapshot &snapshot)
{
	panels.rendering = snapshot.rendering;
	panels.streaming = snapshot.streaming;
	panels.world = snapshot.world;
	panels.playerPanel = snapshot.playerPanel;
	panels.help = snapshot.help;
	panels.overview = snapshot.overview;
	panels.performance = snapshot.performance;
	panels.playerDiagnostics = snapshot.playerDiagnostics;
	panels.chunkInspector = snapshot.chunkInspector;
	panels.memory = snapshot.memory;
	panels.renderDebug = snapshot.renderDebug;
	panels.benchmark = snapshot.benchmark;
}
} // namespace ui
