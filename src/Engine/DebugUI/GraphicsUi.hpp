#pragma once

// Graphics settings surface (issue #185): stable categories instead of one
// long CollapsingHeader scroll. drawRendering (DebugRendering.cpp) owns the
// window + left navigation; each category renders in the content pane.

#include <Engine/DebugUI/DebugUiCore.hpp>

struct GameUIFrame;

namespace graphics
{

enum class Category : int
{
	General,
	Display,
	Lighting,
	Atmosphere,
	Shadows,
	Water,
	Post,
	Resources
};
constexpr int kCategoryCount = 8;

const char *categoryName(Category c);
void drawCategory(Category c, debugui::UiState &s, GameUIFrame &frame);

// Per-category implementations (GraphicsCategories.cpp):
void drawGeneral(debugui::UiState &s, GameUIFrame &frame);
void drawDisplay(debugui::UiState &s, GameUIFrame &frame);
void drawLighting(debugui::UiState &s, GameUIFrame &frame);
void drawAtmosphere(debugui::UiState &s, GameUIFrame &frame);
void drawShadows(debugui::UiState &s, GameUIFrame &frame);
void drawWater(debugui::UiState &s, GameUIFrame &frame);
void drawPost(debugui::UiState &s, GameUIFrame &frame);
void drawResources(debugui::UiState &s, GameUIFrame &frame);

} // namespace graphics
