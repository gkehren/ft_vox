#pragma once

#include <string>

struct GameUIFrame;

/// State for the Graphics → Resource pack panel (owned by GameUI).
struct ResourcePackUiState
{
	char pathBuf[512]{};
	/// Last known engine path used to resync the edit buffer (empty = bundled).
	std::string lastActiveRoot;
	std::string status;
	bool statusError{false};
	bool statusWarning{false};
};

/// Draw path field, Browse, Apply, Use bundled inside an already-open Graphics window.
void drawResourcePackSection(GameUIFrame &frame, ResourcePackUiState &state, bool &showGraphics);

/// Process ImGuiFileDialog for pack directory selection (call every frame).
void displayResourcePackFileDialog(ResourcePackUiState &state, bool &showGraphics);
