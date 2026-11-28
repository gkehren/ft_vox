#pragma once

#include <Engine/PlayerUiStyle.hpp>
#include <Engine/PlayerUiWidgets.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
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
	/// Glyph pixels back to the authored type size before the UI-role multiplier.
	float fontScale{1.f};
};

inline constexpr float kSlateRasterPixels = 36.f;
std::filesystem::path playerFontDirectory();
bool bakeFace(float framebufferScale, BakedFace &out, std::string &error,
	const std::filesystem::path &directory = playerFontDirectory(), float rasterPixels = kSlateRasterPixels);
bool bakeEmergencyFace(float framebufferScale, BakedFace &out, std::string &error);
} // namespace gameui
