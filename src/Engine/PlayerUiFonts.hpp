#pragma once

// Player UI face and glyph table — issue #302, Epic #298 Phase D.
//
// ONE audited face for the whole player interface. The repository already
// vendors Dear ImGui (1.91.9 WIP) including its embedded ProggyClean bitmap
// face, which is MIT-licensed; the player UI bakes an independent copy of those
// glyphs into its own atlas (selected in the Game UI backend ADR, #299), so
// this adds no asset download, no new dependency and no new license obligation
// beyond the existing notice. Attribution and the license texts stay with the
// vendored source; docs/game-ui-design-system.md carries the asset table.
//
// Graphics-scoped translation unit (it includes ImGui only to run the CPU-side
// bake). Everything a caller can assert — the table layout, the fallback glyph,
// the role ratios — lives in PlayerUiText.hpp and is tested CPU-only.

#include <Engine/PlayerUiStyle.hpp>
#include <Engine/PlayerUiWidgets.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace gameui
{
/// The audited face. Injected into the glyph table and reported in the
/// design-system documentation and the gallery footer.
struct FontAsset
{
	std::string_view family;
	std::string_view license;
	std::string_view source;
};

inline constexpr FontAsset kPlayerUiFace{
	.family = "ProggyClean (Dear ImGui 1.91.9 WIP embedded face)",
	.license = "MIT",
	.source = "src/imgui/imgui_draw.cpp, src/imgui/LICENSE.txt",
};

/// Printable ASCII plus the indexed fallback path used by PlayerUiText.hpp.
inline constexpr size_t kGlyphCount = 128;

using GlyphTable = std::array<Glyph, kGlyphCount>;

/// The bake result the renderer uploads (one alpha8 coverage bitmap) and the
/// text service samples. `coverage` is `width * height` bytes.
struct BakedFace
{
	GlyphTable glyphs{};
	std::vector<uint8_t> coverage;
	uint32_t width{};
	uint32_t height{};
	uint32_t sampleWidth{};
	uint32_t sampleHeight{};
	/// Glyph-pixel -> logical-unit factor (1 / framebufferScale), because the
	/// face is rasterized at device resolution on high-DPI displays.
	float fontScale{1.f};
};

/// Bake the audited face at `kFontPixels * framebufferScale`. Returns false when
/// the bake produced no pixels; the caller then keeps the fallback glyph table
/// so text still renders as the documented '?' placeholder instead of nothing.
bool bakeFace(float framebufferScale, BakedFace &out);
} // namespace gameui
