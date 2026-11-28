#include <Engine/PlayerUiFonts.hpp>

#include <imgui/imgui.h>
#include <utils.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace gameui
{
namespace
{
ImFont *requestWeight(ImFontAtlas &atlas, const char *file, float size)
{
	const auto path = std::filesystem::path(resourceRootPath()) / "fonts" / "source-sans-3" / file;
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	const auto bytes = input.tellg();
	if (bytes <= 0 || bytes > 1024 * 1024)
		return nullptr;
	std::vector<char> data(static_cast<size_t>(bytes));
	if (!input.seekg(0).read(data.data(), static_cast<std::streamsize>(bytes)))
		return nullptr;
	static constexpr ImWchar ranges[] = {32, 126, 0};
	ImFontConfig config;
	config.FontDataOwnedByAtlas = false; // ImGui takes its own copy on AddFont.
	config.OversampleH = config.OversampleV = 1;
	return atlas.AddFontFromMemoryTTF(data.data(), static_cast<int>(bytes), size, &config, ranges);
}

void copyGlyphs(ImFont &font, GlyphTable &out)
{
	for (ImWchar c = 32; c < kGlyphCount; ++c)
	{
		const ImFontGlyph &glyph = *font.FindGlyph(c);
		out[size_t(c)] = {{glyph.X0, glyph.Y0, glyph.X1, glyph.Y1},
			{glyph.U0, glyph.V0, glyph.U1, glyph.V1}, glyph.AdvanceX, glyph.Visible != 0};
	}
}
} // namespace

bool bakeFace(float framebufferScale, BakedFace &out)
{
	if (!std::isfinite(framebufferScale) || framebufferScale <= 0.f)
		return false;
	const float device = std::clamp(framebufferScale, 0.75f, 4.f);

	ImFontAtlas atlas;
	atlas.Flags = ImFontAtlasFlags_NoMouseCursors | ImFontAtlasFlags_NoBakedLines;
	atlas.TexDesiredWidth = device > 1.5f ? 512 : 256;
	const float pixelsPerEm = style::kFontPixels * device;
	auto *regular = requestWeight(atlas, "SourceSans3-Regular.ttf", pixelsPerEm);
	auto *semibold = requestWeight(atlas, "SourceSans3-Semibold.ttf", pixelsPerEm);
	if (!regular || !semibold)
		return false;

	unsigned char *pixels = nullptr;
	int width = 0, height = 0;
	atlas.GetTexDataAsAlpha8(&pixels, &width, &height);
	if (!pixels || width <= 0 || height <= 0 || width > 512 || height > 1024)
		return false;

	BakedFace next;
	copyGlyphs(*regular, next.glyphs);
	copyGlyphs(*semibold, next.semibold);
	next.coverage.assign(pixels, pixels + size_t(width) * height);
	next.width = next.sampleWidth = uint32_t(width);
	next.height = next.sampleHeight = uint32_t(height);
	next.fontScale = 1.f / device;
	out = std::move(next);
	return true;
}
} // namespace gameui
