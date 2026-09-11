#pragma once

// UI scale normalization for the shell-wide scaling policy (issue #183).
// Pure and header-only so the policy stays unit-testable without ImGui.
//
// The five supported steps are the only canonical values: display-derived
// values (SDL_GetWindowDisplayScale) and manual requests are snapped onto
// the nearest step, so the UI-scale menu always has a selected entry and
// internal geometry never runs at an off-menu scale.
#include <cmath>

namespace ui
{
inline constexpr float kSupportedScales[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
inline constexpr float kMinScale = 1.0f;
inline constexpr float kMaxScale = 2.0f;

/// Scale a canonical 100% metric into current UI-scale pixels. All fixed
/// window sizes, offsets and column widths must go through this so the
/// whole UI geometry follows the scale, not just fonts and ImGuiStyle.
inline float scaled(float value, float scale)
{
	return value * scale;
}

/// Fallback for panel code reading the frame's UI scale: treats invalid
/// values as 100% without imposing a canonical step (callers get the exact
/// scale the layer holds, which is already snapped).
inline float effectiveScale(float scale)
{
	return scale > 0.f ? scale : 1.f;
}

/// Non-positive / NaN input falls back to 100%, otherwise clamped to the
/// supported range.
inline float clampScale(float scale)
{
	if (!(scale > 0.f)) // also catches NaN
		return 1.f;
	return scale < kMinScale ? kMinScale : (scale > kMaxScale ? kMaxScale : scale);
}

/// Snap onto the nearest supported step (nearest-neighbour over the five
/// canonical values, after clamping).
inline float snapScale(float scale)
{
	scale = clampScale(scale);
	float best = kSupportedScales[0];
	float bestDist = scale >= best ? scale - best : best - scale;
	for (float s : kSupportedScales)
	{
		const float dist = scale >= s ? scale - s : s - scale;
		if (dist < bestDist)
		{
			best = s;
			bestDist = dist;
		}
	}
	return best;
}

inline bool isSupportedScale(float scale)
{
	for (float s : kSupportedScales)
	{
		const float dist = scale >= s ? scale - s : s - scale;
		if (dist < 0.001f)
			return true;
	}
	return false;
}
} // namespace ui
