#pragma once

// Component gallery and HUD mock layout — issue #302, Epic #298 Phase D.
//
// Two authored compositions that prove the design system end to end:
//   * the component gallery (tokens, typography, buttons, rows, fields,
//     status/progress, notifications, confirmation, inventory slots), and
//   * a minimal HUD mock built from the same real components, which is the
//     first #303 preview (crosshair, hotbar, status stack, interaction prompt,
//     toast slot) without touching gameplay authority.
//
// The layout is pure data so the acceptance evidence — usable at small
// supported windows, 16:9 and 21:9, 100–200% scale, no clipping outside the
// viewport — is asserted headlessly (tests/test_game_ui_components.cpp) instead
// of depending on a GPU frame.

#include <Engine/PlayerUiComponents.hpp>
#include <Renderer/GameUiGeometry.hpp>

#include <span>
#include <string_view>

namespace gameui
{
struct GallerySection
{
	std::string_view title;
	/// Authored height at 100%, INCLUDING its own header line, so a section can
	/// never paint over its own title (the gallery shows the same component
	/// stack a real screen would).
	float height;
};

/// Fixed section order and authored heights. One table shared by the screen and
/// the tests so "the gallery shows every component class" is checkable.
std::span<const GallerySection> gallerySections();

struct GalleryLayout
{
	Clip view{};
	Clip panel{};  // centred, clamped card
	Clip body{};   // content region the sections scroll inside
	float scale{1.f};
	float contentHeight{};
	float sectionSpacing{};
	float columnWidth{};
	int columns{1};
	size_t perColumn{};
};

/// Section header band: the heading line plus its gap. Content starts below it.
float galleryHeaderHeight(const style::Theme &theme, float scale);

GalleryLayout galleryLayout(Clip view, const style::Theme &theme, float scale);

/// Rect of `index` inside the gallery body at its authored height. The caller
/// intersects with the gallery clip to scroll.
Clip gallerySectionRect(const GalleryLayout &layout, const style::Theme &theme, size_t index);

inline constexpr size_t kCraftingGallerySection = 10;

struct CraftingGalleryLayout
{
	ConfirmationLayout modal;
	SlotGrid inputs;
	Clip inputLabel, separator, output, outputLabel;
};

CraftingGalleryLayout craftingGalleryLayout(Clip section, const style::Theme &theme, float scale);

// --- HUD mock --------------------------------------------------------------

struct HudMockLayout
{
	Clip view{};
	Clip status{};    // top-left status stack
	Clip toast{};     // top-right notification slot
	Clip target{};    // interaction prompt above the hotbar
	Clip hotbar{};    // nine-slot strip
	Clip crosshair{}; // centre box
	float hotbarCell{};
	float hotbarGap{};
	int hotbarSlots{9};
	bool fits{};
};

/// HUD mock geometry. Every rect is clamped inside `view` and the horizontal
/// stacks are capped to half the viewport each, so the composition stays
/// non-overlapping from a small window up to 21:9 at any supported scale.
HudMockLayout hudMockLayout(Clip view, const style::Theme &theme, float scale, int hotbarSlots = 9);
} // namespace gameui
