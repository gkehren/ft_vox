#include <Engine/PlayerUiFonts.hpp>

#include <imgui/imgui.h>

#include <algorithm>
#include <cmath>

namespace gameui
{
bool bakeFace(float framebufferScale, BakedFace &out)
{
	out = {};
	const float device = framebufferScale > 0.f ? framebufferScale : 1.f;
	out.fontScale = 1.f / device;

	ImFontAtlas atlas;
	atlas.Flags = ImFontAtlasFlags_NoMouseCursors | ImFontAtlasFlags_NoBakedLines;
	ImFontConfig config{};
	config.SizePixels = style::kFontPixels * device;
	ImFont *font = atlas.AddFontDefault(&config);
	unsigned char *pixels = nullptr;
	int width = 0, height = 0;
	atlas.GetTexDataAsAlpha8(&pixels, &width, &height);
	if (!font || !pixels || width <= 0 || height <= 0)
		return false;

	for (ImWchar c = 32; c < kGlyphCount; ++c)
	{
		const ImFontGlyph &glyph = *font->FindGlyph(c);
		out.glyphs[size_t(c)] = {{glyph.X0, glyph.Y0, glyph.X1, glyph.Y1},
			{glyph.U0, glyph.V0, glyph.U1, glyph.V1}, glyph.AdvanceX, glyph.Visible != 0};
	}
	out.coverage.assign(pixels, pixels + size_t(width) * size_t(height));
	out.width = out.sampleWidth = uint32_t(width);
	out.height = out.sampleHeight = uint32_t(height);
	return true;
}
} // namespace gameui
