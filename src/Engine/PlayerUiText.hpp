#pragma once

// Player UI text service seam — issue #302, Epic #298 Phase D.
//
// One audited face, one glyph table, one documented fallback: the text roles
// of PlayerUiStyle.hpp are measured and painted through this header, and every
// component fits its labels through `fitText` so variable-length strings never
// silently overflow their control (the 16:9 / 21:9 / 100–200% evidence in
// tests/test_game_ui_components.cpp).
//
// The face itself is baked once by PlayerUiFonts.cpp (graphical TU, ImGui's
// embedded ProggyClean — MIT, see the asset table in
// docs/game-ui-design-system.md). Everything that decides *which* glyph is
// used, how wide a string is, and what happens when a glyph or an icon is
// missing is pure and lives here, so the policy is unit-tested without SDL,
// Vulkan or ImGui.
//
// Missing-glyph policy: the baked face covers printable ASCII. A scalar with
// no glyph (any non-ASCII code point today) renders as the fallback glyph '?'
// once per Unicode scalar — never one per UTF-8 byte, and never a dropped
// character. Missing atlased item icons follow the same rule at the slot
// component: the slot keeps its geometry, frame, count and focus ring and
// paints a muted placeholder with the fallback glyph.

#include <Engine/PlayerUiStyle.hpp>
#include <Engine/PlayerUiWidgets.hpp>
#include <Renderer/GameUiDrawList.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace gameui
{
/// Stand-in for a scalar the baked face cannot draw.
inline constexpr char kFallbackGlyph = '?';

inline constexpr size_t kFallbackGlyphIndex = size_t(kFallbackGlyph);

/// Glyph index used for the scalar at `offset` (the fallback when the face has
/// no coverage). Multi-byte scalars resolve to ONE glyph.
inline size_t glyphIndex(FontView font, std::string_view text, size_t offset)
{
	const auto code = static_cast<unsigned char>(offset < text.size() ? text[offset] : 0);
	return code >= 32 && code < font.glyphs.size() ? size_t(code) : kFallbackGlyphIndex;
}

inline bool usesFallbackGlyph(FontView font, std::string_view text, size_t offset)
{
	return glyphIndex(font, text, offset) != size_t(static_cast<unsigned char>(text[offset]));
}

/// Scalars the face can draw directly (printable ASCII).
inline size_t drawableScalars(FontView font, std::string_view text)
{
	size_t count = 0;
	for (size_t i = 0; i < text.size(); i = nextUtf8(text, i))
		if (!usesFallbackGlyph(font, text, i))
			++count;
	return count;
}

// --- Measurement -----------------------------------------------------------

/// ONE text metric for the whole player UI (issue #302 F01). Rendering, caret
/// placement, mouse hit testing, selection, horizontal scrolling and IME
/// candidate placement all multiply the glyph table by the same product:
/// role ratio x resolved player UI scale x framebuffer compensation. Nothing
/// derives it from painting order any more.
struct TextStyle
{
	style::TextRole role{style::TextRole::Body};
	float scale{1.f};
	float fontScale{1.f};

	/// Glyph-space multiplier: pass this to `FontView::measure`/`paint`.
	float glyphScale() const
	{
		const float player = scale > 0.f ? scale : 1.f;
		const float glyphs = fontScale > 0.f ? fontScale : 1.f;
		return style::typeScale(role) * player * glyphs;
	}

	/// One line advance in logical units (the baked face's line height already
	/// accounts for the font's own cell height, so no font scale here).
	float lineHeight() const
	{
		return style::lineHeight(role) * (scale > 0.f ? scale : 1.f);
	}
};

inline TextStyle textStyle(style::TextRole role, float scale, float fontScale = 1.f)
{
	return {role, scale, fontScale};
}

struct TextMetrics
{
	float width{};
	float height{};
};

/// Measure with a design-system role: the baked table scaled by the role ratio,
/// the resolved player UI scale and (when the face is baked at device
/// resolution) the font scale that maps glyph pixels back to logical units.
inline TextMetrics measureRole(FontView font, std::string_view text, style::TextRole role, float scale,
							   float fontScale = 1.f)
{
	const auto style = textStyle(role, scale, fontScale);
	return {font.measure(text, style.glyphScale()), style.lineHeight()};
}

/// Top edge that vertically centers one line of a role inside `rect`.
inline float centeredLineTop(Clip rect, style::TextRole role, float scale)
{
	const float height = style::lineHeight(role) * (scale > 0.f ? scale : 1.f);
	return rect.rect.y + std::max(0.f, (rect.rect.w - rect.rect.y - height) * 0.5f);
}

// --- Overflow --------------------------------------------------------------

/// Result of fitting a label into a pixel budget. `bytes` is the prefix that
/// fits; `elided` asks the painter to append the ASCII ellipsis when the budget
/// left room for it. No allocation: the prefix is a view into the original.
struct FittedText
{
	size_t bytes{};
	bool elided{};
};

/// Fit `text` into `maxWidth` at a role/scale. A string that already fits is
/// returned whole with `elided == false`. When no prefix plus ellipsis fits,
/// the result is empty and the caller paints nothing — a control never draws a
/// partial glyph.
inline FittedText fitText(FontView font, std::string_view text, style::TextRole role, float scale,
						  float maxWidth)
{
	const float roleScale = style::typeScale(role) * (scale > 0.f ? scale : 1.f);
	if (maxWidth <= 0.f)
		return {};
	if (font.measure(text, roleScale) <= maxWidth)
		return {text.size(), false};
	const float ellipsis = font.measure("...", roleScale);
	if (ellipsis > maxWidth)
		return {};
	float width = 0.f;
	size_t bytes = 0;
	for (size_t i = 0; i < text.size();)
	{
		const size_t next = nextUtf8(text, i);
		const float advance = font.measure(text.substr(i, next - i), roleScale);
		if (width + advance + ellipsis > maxWidth)
			break;
		width += advance;
		bytes = next;
		i = next;
	}
	return {bytes, bytes < text.size()};
}

// --- Icons -----------------------------------------------------------------

/// Why an atlased item icon cannot be drawn. Stale generations are the atlas
/// reload case: the handle belongs to a retired atlas and must not be sampled.
enum class IconStatus : uint8_t
{
	Valid = 0,
	MissingLayer,
	StaleGeneration,
};

inline IconStatus iconStatus(AtlasIcon icon, uint64_t generation, uint32_t layers)
{
	if (icon.generation != generation)
		return IconStatus::StaleGeneration;
	if (icon.layer >= layers)
		return IconStatus::MissingLayer;
	return IconStatus::Valid;
}

inline bool drawable(IconStatus status)
{
	return status == IconStatus::Valid;
}
} // namespace gameui
