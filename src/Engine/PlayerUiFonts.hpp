#pragma once

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
	.family = "Source Sans 3 Regular and Semibold",
	.license = "SIL OFL 1.1",
	.source = "ressources/fonts/source-sans-3/OFL.txt",
};

/// Printable ASCII plus the indexed fallback path used by PlayerUiText.hpp.
inline constexpr size_t kGlyphCount = 128;

using GlyphTable = std::array<Glyph, kGlyphCount>;

/// The bake result the renderer uploads (one alpha8 coverage bitmap) and the
/// text service samples. `coverage` is `width * height` bytes.
struct BakedFace
{
	GlyphTable glyphs{};
	GlyphTable semibold{};
	std::vector<uint8_t> coverage;
	uint32_t width{};
	uint32_t height{};
	uint32_t sampleWidth{};
	uint32_t sampleHeight{};
	/// Glyph-pixel -> logical-unit factor (1 / framebufferScale), because the
	/// face is rasterized at device resolution on high-DPI displays.
	float fontScale{1.f};
};

/// Bake both weights in one bounded coverage atlas. Failed replacements keep
/// the working atlas and both existing metric tables.
bool bakeFace(float framebufferScale, BakedFace &out);
} // namespace gameui
