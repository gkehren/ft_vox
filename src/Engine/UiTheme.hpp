#pragma once

#include <Engine/UiScale.hpp>

#include <imgui/imgui.h>

// Centralized ft_vox ImGui presentation (issue #183): application theme,
// coherent UI scaling, and a small set of shared presentation helpers.
// Panels keep raw ImGui access; helpers only remove duplicated decisions.
namespace ui
{
/// Canonical 100% font size all style metrics are scaled from.
inline constexpr float kBaseFontSize = 15.0f;

/// Rebuild the full style from stock ImGui::StyleColorsDark and apply the
/// ft_vox theme at the given scale. Every call starts from the stock base
/// (one ScaleAllSizes pass from 100% values), so repeated calls never
/// accumulate drift. Call outside a widget-submission path.
void applyStyle(float scale);

/// (Re)create the font atlas at the given UI scale. `framebufferScale` is
/// the SDL logical->pixel ratio (1 on Windows, 2 on Retina): glyphs are
/// rasterized at device resolution and io.FontGlobalScale maps them back to
/// logical units, so text stays crisp when the platform framebuffer scale
/// differs from 1. Call before the first frame and whenever the scale or
/// framebuffer scale changes; the caller must recreate the backend font
/// texture afterwards (the Vulkan backend builds/uploads it lazily from
/// this atlas).
void rebuildFontAtlas(float scale, float framebufferScale = 1.0f);

/// Restrained status semantics shared by badges and status text.
enum class StatusKind
{
	Info,
	Ok,
	Warn,
	Error
};
ImVec4 statusColor(StatusKind kind);

// --- Shared presentation helpers -----------------------------------------

/// Consistent section header: breathing room above + themed SeparatorText.
void sectionHeader(const char *label);

/// Classic dim "(?)" with a wrapped tooltip.
void helpMarker(const char *desc);

/// Rounded, tinted status chip (e.g. LIVE / PAUSED) sized to its label.
void statusBadge(const char *label, StatusKind kind);

/// Label left, printf-formatted value right-aligned on the same line.
void metric(const char *label, const char *fmt, ...) IM_FMTARGS(2);

/// Label + capacity bar with "count / capacity" overlay; warn-tinted when full.
void queueBar(const char *label, int count, int capacity);
} // namespace ui
