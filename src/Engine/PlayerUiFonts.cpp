#include <Engine/PlayerUiFonts.hpp>

#include <imgui/imgui.h>
#include <miniz.h>
#include <utils.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace gameui
{
namespace
{
struct Weight
{
	const char *file;
	std::uintmax_t bytes;
	unsigned long crc;
};

constexpr Weight kRegular{"SourceSans3-Regular.ttf", 431196, 0x92ac772d};
constexpr Weight kSemibold{"SourceSans3-Semibold.ttf", 425904, 0x0caeb423};
constexpr int kMaxWidth = 512, kMaxHeight = 1024;
constexpr float kMaxRasterPixels = 72.f;

ImFont *requestWeight(ImFontAtlas &atlas, const std::filesystem::path &directory,
	const Weight &weight, float size, std::string &error)
{
	const auto path = directory / weight.file;
	const auto fail = [&](const char *reason) -> ImFont * {
		error = path.string() + ": " + reason;
		return nullptr;
	};
	std::error_code ec;
	const auto bytes = std::filesystem::file_size(path, ec);
	if (ec == std::errc::no_such_file_or_directory)
		return fail("font file is missing");
	if (ec)
	{
		error = path.string() + ": cannot inspect font file (" + ec.message() + ")";
		return nullptr;
	}
	if (bytes != weight.bytes)
		return fail("invalid font size for the bundled Source Sans 3 release");
	std::ifstream input(path, std::ios::binary);
	std::vector<unsigned char> data(static_cast<size_t>(bytes));
	if (!input.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(bytes)))
		return fail("font read failed");
	// Only the pinned, distributed files are supported. Reject damaged bytes
	// before they can reach the vendored rasterizer.
	if (mz_crc32(0, data.data(), data.size()) != weight.crc)
		return fail("invalid font data for the bundled Source Sans 3 release");

	static constexpr ImWchar ranges[] = {32, 126, 0};
	ImFontConfig config;
	config.FontDataOwnedByAtlas = false;
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

bool finishBake(ImFontAtlas &atlas, ImFont &regular, ImFont &semibold, float pixelsPerEm,
	BakedFace &out, std::string &error)
{
	if (!atlas.Build())
	{
		error = "rasterizer rejected the font atlas";
		return false;
	}
	unsigned char *pixels = nullptr;
	int width = 0, height = 0;
	atlas.GetTexDataAsAlpha8(&pixels, &width, &height);
	if (!pixels || width <= 0 || height <= 0 || width > kMaxWidth || height > kMaxHeight)
	{
		error = "font atlas extent " + std::to_string(width) + "x" + std::to_string(height) +
			" is empty or exceeds 512x1024";
		return false;
	}
	BakedFace next;
	copyGlyphs(regular, next.glyphs);
	copyGlyphs(semibold, next.semibold);
	next.coverage.assign(pixels, pixels + size_t(width) * height);
	next.width = next.sampleWidth = uint32_t(width);
	next.height = next.sampleHeight = uint32_t(height);
	next.fontScale = style::kFontPixels / pixelsPerEm;
	out = std::move(next);
	return true;
}
} // namespace

std::filesystem::path playerFontDirectory()
{
	return std::filesystem::path(resourceRootPath()) / "fonts" / "source-sans-3";
}

bool bakeFace(float framebufferScale, BakedFace &out, std::string &error,
	const std::filesystem::path &directory, float rasterPixels)
{
	error.clear();
	if (!std::isfinite(framebufferScale) || framebufferScale <= 0.f)
	{
		error = "invalid framebuffer density";
		return false;
	}
	const float density = std::clamp(framebufferScale, 0.75f, 4.f);
	// The rasterizer rounds sizes down. Use the same integer size for metric compensation.
	const float pixels = std::floor(std::clamp(rasterPixels * density, 10.f, kMaxRasterPixels));
	ImFontAtlas atlas;
	atlas.Flags = ImFontAtlasFlags_NoMouseCursors | ImFontAtlasFlags_NoBakedLines;
	atlas.TexDesiredWidth = pixels > 24.f ? kMaxWidth : 256;
	auto *regular = requestWeight(atlas, directory, kRegular, pixels, error);
	if (!regular)
		return false;
	auto *semibold = requestWeight(atlas, directory, kSemibold, pixels, error);
	if (!semibold)
		return false;
	if (finishBake(atlas, *regular, *semibold, pixels, out, error))
		return true;
	error = (directory / kRegular.file).string() + " and " + (directory / kSemibold.file).string() + ": " + error;
	return false;
}

bool bakeEmergencyFace(float framebufferScale, BakedFace &out, std::string &error)
{
	error.clear();
	const float density = std::isfinite(framebufferScale) ? std::clamp(framebufferScale, 0.75f, 4.f) : 1.f;
	const float pixels = std::floor(style::kFontPixels * density);
	ImFontAtlas atlas;
	atlas.Flags = ImFontAtlasFlags_NoMouseCursors | ImFontAtlasFlags_NoBakedLines;
	atlas.TexDesiredWidth = pixels > 24.f ? kMaxWidth : 256;
	ImFontConfig config;
	config.SizePixels = pixels;
	auto *fallback = atlas.AddFontDefault(&config);
	if (!fallback)
	{
		error = "embedded ProggyClean could not be loaded";
		return false;
	}
	return finishBake(atlas, *fallback, *fallback, pixels, out, error);
}
} // namespace gameui
