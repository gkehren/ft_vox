#pragma once

// Right-side main-menu status strip layout (issue #183). Pure and header-only
// so field selection / degradation is unit-testable without ImGui.
//
// The strip is right-aligned inside the menu bar as [fps][sep][ms][sep][badge]
// within the region left of the menus' end. When horizontal space runs out it
// degrades ms first, then fps; the state badge survives last. This replaces
// the fragile fixed-offset SetCursorPosX path that could overlap menus as
// they grow.
namespace ui
{
/// Window-local x-range the status strip may occupy in the menu bar:
/// [start, start + width]. Pure so the coordinate arithmetic (menu end, gap,
/// right padding, empty-region clamp) is unit-testable without ImGui.
struct StatusRegion
{
	float start{0.f};
	float width{0.f};
};

inline StatusRegion statusRegion(float cursorX, float windowWidth,
								 float rightPadding, float gap)
{
	const float start = cursorX + gap;
	const float right = windowWidth - rightPadding;
	return {start, right > start ? right - start : 0.f};
}

struct StatusStripPlan
{
	bool showFps{false};
	bool showMs{false};
	bool showBadge{false};
	float fpsX{0.f};   // field positions relative to the strip region start
	float msX{0.f};
	float badgeX{0.f};
};

/// @param contentRight  width of the region the strip may occupy
/// @param padRight      breathing room kept from the right edge
/// @param fpsW/msW      measured text widths of the FPS / frame-time fields
/// @param badgeW        measured width of the state badge (incl. padding)
/// @param separatorW    width of one "field | field" separator block
inline StatusStripPlan planStatusStrip(float contentRight, float padRight,
									   float fpsW, float msW, float badgeW,
									   float separatorW)
{
	StatusStripPlan plan;
	const float avail = contentRight - padRight;

	float total = fpsW + separatorW + msW + separatorW + badgeW;
	plan.showFps = true;
	plan.showMs = true;

	if (total > avail)
	{
		plan.showMs = false;
		total -= separatorW + msW;
	}
	if (total > avail)
	{
		plan.showFps = false;
		total -= fpsW + separatorW;
	}
	plan.showBadge = badgeW <= avail;

	float x = plan.showBadge ? avail - total : 0.f;
	plan.fpsX = x;
	if (plan.showFps)
		x += fpsW + separatorW;
	plan.msX = x;
	if (plan.showMs)
		x += msW + separatorW;
	plan.badgeX = x;
	return plan;
}
} // namespace ui
