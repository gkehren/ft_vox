#include "Engine/UiTheme.hpp"

#include <imgui/imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <initializer_list>

namespace ui
{
namespace
{
// Platform font candidates, most-preferred first. This adds no font
// dependency: when none exists the ImGui embedded default is used.
const char *const kSystemFontCandidates[] = {
	// Windows
	"C:\\Windows\\Fonts\\segoeui.ttf",
	"C:\\Windows\\Fonts\\arial.ttf",
	"C:\\Windows\\Fonts\\consola.ttf",
	// macOS
	"/System/Library/Fonts/Supplemental/Arial.ttf",
	"/Library/Fonts/Arial.ttf",
	"/System/Library/Fonts/Helvetica.ttc",
	// Linux (Debian/Arch/Fedora font layouts)
	"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
	"/usr/share/fonts/TTF/DejaVuSans.ttf",
	"/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
	"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
};

const char *findSystemFont()
{
	for (const char *path : kSystemFontCandidates)
	{
		std::error_code ec;
		if (std::filesystem::exists(path, ec))
			return path;
	}
	return nullptr;
}
} // namespace

void applyStyle(float scale)
{
	ImGuiStyle &style = ImGui::GetStyle();
	ImGui::StyleColorsDark(&style); // canonical base; rebuilt every call -> no drift

	// ft_vox metrics at 100%: moderate rounding, slightly roomier padding
	// than stock, subtle separators.
	style.WindowRounding = 6.f;
	style.ChildRounding = 6.f;
	style.PopupRounding = 6.f;
	style.FrameRounding = 4.f;
	style.GrabRounding = 4.f;
	style.TabRounding = 5.f;
	style.ScrollbarRounding = 9.f;
	style.WindowBorderSize = 1.f;
	style.ChildBorderSize = 1.f;
	style.PopupBorderSize = 1.f;
	style.FrameBorderSize = 0.f;
	style.WindowPadding = ImVec2(9.f, 9.f);
	style.FramePadding = ImVec2(8.f, 4.f);
	style.ItemSpacing = ImVec2(8.f, 6.f);
	style.ItemInnerSpacing = ImVec2(6.f, 4.f);
	style.CellPadding = ImVec2(6.f, 3.f);
	style.IndentSpacing = 18.f;
	style.ScrollbarSize = 13.f;
	style.SeparatorTextBorderSize = 2.f;
	style.DockingSeparatorSize = 2.f;
	style.WindowMenuButtonPosition = ImGuiDir_Left;

	style.ScaleAllSizes(scale); // single pass from the 100% canonical base

	// Palette: neutral cool dark base, slightly differentiated child/frame
	// surfaces, one desaturated blue accent, restrained status colors.
	ImVec4 *c = style.Colors;
	const ImVec4 text(0.92f, 0.93f, 0.95f, 1.f);
	const ImVec4 textDim(0.51f, 0.54f, 0.59f, 1.f);
	const ImVec4 windowBg(0.075f, 0.080f, 0.092f, 0.96f);
	const ImVec4 childBg(0.086f, 0.092f, 0.105f, 1.f);
	const ImVec4 frameBg(0.125f, 0.135f, 0.155f, 1.f);
	const ImVec4 frameBgHovered(0.160f, 0.172f, 0.198f, 1.f);
	const ImVec4 frameBgActive(0.190f, 0.205f, 0.235f, 1.f);
	const ImVec4 accent(0.28f, 0.55f, 0.90f, 1.f);
	const ImVec4 accentDim(0.22f, 0.42f, 0.68f, 1.f);

	c[ImGuiCol_Text] = text;
	c[ImGuiCol_TextDisabled] = textDim;
	c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
	c[ImGuiCol_WindowBg] = windowBg;
	c[ImGuiCol_ChildBg] = childBg;
	c[ImGuiCol_PopupBg] = ImVec4(0.080f, 0.086f, 0.098f, 0.98f);
	c[ImGuiCol_Border] = ImVec4(0.20f, 0.22f, 0.26f, 0.55f);
	c[ImGuiCol_BorderShadow] = ImVec4(0.f, 0.f, 0.f, 0.f);
	c[ImGuiCol_FrameBg] = frameBg;
	c[ImGuiCol_FrameBgHovered] = frameBgHovered;
	c[ImGuiCol_FrameBgActive] = frameBgActive;
	c[ImGuiCol_TitleBg] = ImVec4(0.060f, 0.065f, 0.075f, 1.f);
	c[ImGuiCol_TitleBgActive] = ImVec4(0.095f, 0.125f, 0.170f, 1.f);
	c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.050f, 0.055f, 0.063f, 0.80f);
	c[ImGuiCol_MenuBarBg] = ImVec4(0.090f, 0.095f, 0.110f, 1.f);
	c[ImGuiCol_ScrollbarBg] = ImVec4(0.070f, 0.075f, 0.085f, 1.f);
	c[ImGuiCol_ScrollbarGrab] = frameBgHovered;
	c[ImGuiCol_ScrollbarGrabHovered] = frameBgActive;
	c[ImGuiCol_ScrollbarGrabActive] = accentDim;
	c[ImGuiCol_CheckMark] = accent;
	c[ImGuiCol_SliderGrab] = accentDim;
	c[ImGuiCol_SliderGrabActive] = accent;
	c[ImGuiCol_Button] = frameBg;
	c[ImGuiCol_ButtonHovered] = frameBgHovered;
	c[ImGuiCol_ButtonActive] = frameBgActive;
	c[ImGuiCol_Header] = ImVec4(accentDim.x, accentDim.y, accentDim.z, 0.55f);
	c[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.65f);
	c[ImGuiCol_HeaderActive] = accent;
	c[ImGuiCol_Separator] = ImVec4(0.22f, 0.24f, 0.28f, 0.60f);
	c[ImGuiCol_SeparatorHovered] = accentDim;
	c[ImGuiCol_SeparatorActive] = accent;
	c[ImGuiCol_ResizeGrip] = ImVec4(accent.x, accent.y, accent.z, 0.20f);
	c[ImGuiCol_ResizeGripHovered] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
	c[ImGuiCol_ResizeGripActive] = ImVec4(accent.x, accent.y, accent.z, 0.70f);
	c[ImGuiCol_TabHovered] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
	c[ImGuiCol_TabSelected] = ImVec4(0.115f, 0.155f, 0.215f, 1.f);
	c[ImGuiCol_TabSelectedOverline] = accent;
	c[ImGuiCol_TabDimmed] = ImVec4(0.090f, 0.095f, 0.110f, 1.f);
	c[ImGuiCol_TabDimmedSelected] = ImVec4(0.115f, 0.125f, 0.145f, 1.f);
	c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.35f, 0.38f, 0.44f, 1.f);
	c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
	c[ImGuiCol_DockingEmptyBg] = ImVec4(0.075f, 0.080f, 0.092f, 1.f);
	c[ImGuiCol_PlotLines] = accentDim;
	c[ImGuiCol_PlotLinesHovered] = accent;
	c[ImGuiCol_PlotHistogram] = accentDim;
	c[ImGuiCol_PlotHistogramHovered] = accent;
	c[ImGuiCol_TableHeaderBg] = ImVec4(0.115f, 0.125f, 0.145f, 1.f);
	c[ImGuiCol_TableBorderStrong] = ImVec4(0.20f, 0.22f, 0.26f, 0.60f);
	c[ImGuiCol_TableBorderLight] = ImVec4(0.17f, 0.185f, 0.215f, 0.50f);
	c[ImGuiCol_TableRowBg] = ImVec4(1.f, 1.f, 1.f, 0.02f);
	c[ImGuiCol_TableRowBgAlt] = ImVec4(1.f, 1.f, 1.f, 0.045f);
	c[ImGuiCol_DragDropTarget] = accent;
	c[ImGuiCol_NavCursor] = ImVec4(accent.x, accent.y, accent.z, 0.70f);
	c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.f, 1.f, 1.f, 0.70f);
	c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.f, 0.f, 0.f, 0.55f);
	c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.f, 0.f, 0.f, 0.55f);
}

void rebuildFontAtlas(float scale, float framebufferScale)
{
	ImGuiIO &io = ImGui::GetIO();
	ImFontAtlas *fonts = io.Fonts;
	fonts->Clear();

	// Rasterize at device resolution; io.FontGlobalScale maps the glyphs
	// back to logical units so a framebuffer scale != 1 (Retina, Wayland)
	// stays 1:1 texel-to-pixel instead of stretching the atlas.
	const float fbScale = framebufferScale > 0.f ? framebufferScale : 1.f;
	const float sizePixels = kBaseFontSize * scale * fbScale;
	io.FontGlobalScale = 1.f / fbScale;

	if (const char *fontPath = findSystemFont())
	{
		if (fonts->AddFontFromFileTTF(fontPath, sizePixels))
			return; // Vulkan backend builds/uploads the atlas afterwards
		// Fall through to the embedded default when the file failed to load.
		fonts->Clear();
	}
	ImFontConfig cfg{};
	cfg.SizePixels = sizePixels;
	fonts->AddFontDefault(&cfg);
}

ImVec4 statusColor(StatusKind kind)
{
	switch (kind)
	{
	case StatusKind::Ok:
		return ImVec4(0.42f, 0.75f, 0.45f, 1.f);
	case StatusKind::Warn:
		return ImVec4(0.92f, 0.72f, 0.28f, 1.f);
	case StatusKind::Error:
		return ImVec4(0.90f, 0.40f, 0.36f, 1.f);
	case StatusKind::Info:
	default:
		return ImVec4(0.45f, 0.65f, 0.92f, 1.f);
	}
}

void sectionHeader(const char *label)
{
	ImGui::Dummy(ImVec2(0.f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
	ImGui::SeparatorText(label);
}

void helpMarker(const char *desc)
{
	ImGui::TextDisabled("(?)");
	if (ImGui::BeginItemTooltip())
	{
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.f);
		ImGui::TextUnformatted(desc);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

void statusBadge(const char *label, StatusKind kind)
{
	const ImGuiStyle &style = ImGui::GetStyle();
	const ImVec2 labelSize = ImGui::CalcTextSize(label);
	const ImVec2 pad(style.FramePadding.x * 0.75f, style.FramePadding.y * 0.5f);
	const float rounding = style.FrameRounding;
	const ImVec2 p0 = ImGui::GetCursorScreenPos();
	const ImVec2 p1(p0.x + labelSize.x + pad.x * 2.f, p0.y + labelSize.y + pad.y * 2.f);

	const ImVec4 tint = statusColor(kind);
	ImDrawList *draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(p0, p1,
						ImGui::GetColorU32(ImVec4(tint.x * 0.25f, tint.y * 0.25f, tint.z * 0.25f, 0.85f)),
						rounding);
	draw->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(tint.x, tint.y, tint.z, 0.45f)), rounding);
	draw->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), ImGui::GetColorU32(tint), label);
	ImGui::Dummy(ImVec2(p1.x - p0.x, p1.y - p0.y));
}

void metric(const char *label, const char *fmt, ...)
{
	char value[160];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(value, sizeof(value), fmt, args);
	va_end(args);

	// Right edge of the current content region, in screen coordinates: safe
	// inside children and after indentation, unlike window-relative math.
	const float valueWidth = ImGui::CalcTextSize(value).x;
	const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;

	ImGui::TextUnformatted(label);
	ImGui::SameLine();

	const ImVec2 cursor = ImGui::GetCursorScreenPos();
	if (right - valueWidth > cursor.x)
		ImGui::SetCursorScreenPos(ImVec2(right - valueWidth, cursor.y));
	ImGui::TextUnformatted(value);
}

void queueBar(const char *label, int count, int capacity)
{
	const float fraction = capacity > 0
							   ? std::clamp(static_cast<float>(count) / static_cast<float>(capacity), 0.f, 1.f)
							   : 0.f;
	char overlay[40];
	if (capacity > 0)
		std::snprintf(overlay, sizeof(overlay), "%d / %d", count, capacity);
	else
		std::snprintf(overlay, sizeof(overlay), "%d", count);

	if (label && *label)
	{
		ImGui::TextUnformatted(label);
		ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
	}
	if (fraction >= 1.f)
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, statusColor(StatusKind::Warn));
	ImGui::ProgressBar(fraction, ImVec2(-1.f, 0.f), overlay);
	if (fraction >= 1.f)
		ImGui::PopStyleColor();
}
} // namespace ui
