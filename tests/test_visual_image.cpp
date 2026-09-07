/// Synthetic unit tests for the visual-regression comparison plumbing
/// (issue #142 review): metric units/normalization, hot-pixel counting,
/// PNG round-trip and the diff/swizzle helpers. Pure CPU — no Vulkan.
///
/// These guard against the metric-unit class of bug (thresholds are in
/// [0,1]; a comparison that silently reported 0..255-scale values would
/// make every tolerance effectively bit-exact).

#define STB_IMAGE_IMPLEMENTATION // this target has no other stb translation unit
#include <stb_image/stb_image.h>

#include "VisualImage.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
int failures = 0;

void check(bool condition, const std::string &message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << "\n";
		++failures;
	}
}

visual::RgbaImage solid(uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b)
{
	visual::RgbaImage image;
	image.width = width;
	image.height = height;
	image.pixels.resize(size_t(width) * height * 4);
	for (size_t i = 0; i < image.pixels.size(); i += 4)
	{
		image.pixels[i + 0] = r;
		image.pixels[i + 1] = g;
		image.pixels[i + 2] = b;
		image.pixels[i + 3] = 255;
	}
	return image;
}

void offsetChannel(visual::RgbaImage &image, int channel, int delta)
{
	for (size_t i = channel; i < image.pixels.size(); i += 4)
		image.pixels[i] = uint8_t(std::clamp(int(image.pixels[i]) + delta, 0, 255));
}

constexpr double kEps = 1e-9;
} // namespace

int main()
{
	using namespace visual;
	const CompareThresholds thresholds;

	// Identical images: all metrics zero, within tolerance.
	{
		const RgbaImage a = solid(4, 4, 128, 64, 32);
		const ImageMetrics m = compareImages(a, a, thresholds.hotPixelThreshold);
		check(m.comparable(), "identical: comparable");
		check(m.meanAbsError == 0.0 && m.rmsError == 0.0 && m.maxAbsError == 0.0, "identical: zero error");
		check(m.hotPixels == 0, "identical: no hot pixels");
		check(withinTolerance(m, thresholds), "identical: passes");
	}

	// One LSB on one channel: mean = (1/255)/3, RMS = (1/255)/sqrt(3) —
	// must still pass the 3/255 mean tolerance (this is the regression
	// guard for the metric-unit bug).
	{
		RgbaImage a = solid(4, 4, 128, 64, 32);
		RgbaImage b = a;
		offsetChannel(b, 1, 1); // +1 green everywhere
		const ImageMetrics m = compareImages(a, b, thresholds.hotPixelThreshold);
		check(std::abs(m.meanAbsError - (1.0 / 255.0) / 3.0) < kEps, "+1 green: mean is one LSB / 3");
		check(std::abs(m.rmsError - (1.0 / 255.0) / std::sqrt(3.0)) < kEps, "+1 green: rms is one LSB / sqrt(3)");
		check(m.maxAbsError == 1.0 / 255.0, "+1 green: max is one LSB");
		check(m.hotPixels == 0, "+1 green: no hot pixels");
		check(withinTolerance(m, thresholds), "+1 green: still passes");
	}

	// +3 on every channel everywhere: mean = 3/255 exactly at the
	// threshold; +4 must fail.
	{
		RgbaImage a = solid(2, 2, 100, 100, 100);
		RgbaImage b = a;
		offsetChannel(b, 0, 3);
		offsetChannel(b, 1, 3);
		offsetChannel(b, 2, 3);
		const ImageMetrics m3 = compareImages(a, b, thresholds.hotPixelThreshold);
		check(std::abs(m3.meanAbsError - 3.0 / 255.0) < kEps, "+3 all channels: mean is 3 LSB");
		check(withinTolerance(m3, thresholds), "+3 all channels: at tolerance, passes");
		RgbaImage c = a;
		offsetChannel(c, 0, 4);
		offsetChannel(c, 1, 4);
		offsetChannel(c, 2, 4);
		const ImageMetrics m4 = compareImages(a, c, thresholds.hotPixelThreshold);
		check(!withinTolerance(m4, thresholds), "+4 all channels: fails tolerance");
	}

	// Hot pixels: delta 25 on one pixel of a 2x2 image → ratio 0.25, above
	// the 2% budget; a delta of 20 exactly is not hot (strictly greater).
	{
		RgbaImage a = solid(2, 2, 10, 10, 10);
		RgbaImage hot = a;
		hot.pixels[4] = 35; // pixel 1 red +25
		const ImageMetrics m = compareImages(a, hot, thresholds.hotPixelThreshold);
		check(m.hotPixels == 1, "hot: one hot pixel counted");
		check(std::abs(m.hotPixelRatio - 0.25) < kEps, "hot: ratio is 1/4");
		check(!withinTolerance(m, thresholds), "hot: 25% hot ratio fails");
		RgbaImage edge = a;
		edge.pixels[0] = 30; // exactly +20
		const ImageMetrics me = compareImages(a, edge, thresholds.hotPixelThreshold);
		check(me.hotPixels == 0, "hot: threshold is exclusive");
	}

	// Per-channel means: differing deltas on different channels average out.
	{
		RgbaImage a = solid(1, 3, 0, 0, 0);
		RgbaImage b = a;
		for (size_t i = 0; i < b.pixels.size(); i += 4)
		{
			b.pixels[i + 0] = 9; // R +9 on every pixel
			b.pixels[i + 1] = 6; // G +6 on every pixel
		}
		const ImageMetrics m = compareImages(a, b, thresholds.hotPixelThreshold);
		check(std::abs(m.meanAbsError - (9.0 + 6.0) / 3.0 / 255.0) < kEps,
			  "per-channel: mean averages R and G deltas over 3 channels");
	}

	// Size mismatch and invalid input: incomparable, fails tolerance.
	{
		const RgbaImage a = solid(4, 4, 0, 0, 0);
		const RgbaImage b = solid(2, 8, 0, 0, 0);
		const ImageMetrics m = compareImages(a, b, thresholds.hotPixelThreshold);
		check(!m.comparable(), "size mismatch: incomparable");
		check(!withinTolerance(m, thresholds), "size mismatch: fails");
		const ImageMetrics mi = compareImages(RgbaImage{}, a, thresholds.hotPixelThreshold);
		check(!mi.comparable() && !withinTolerance(mi, thresholds), "invalid input: fails");
	}

	// Diff image: amplified ×8, alpha opaque, invalid/mismatch → invalid.
	{
		RgbaImage a = solid(2, 1, 10, 0, 0);
		RgbaImage b = solid(2, 1, 14, 0, 0);
		const RgbaImage d = makeDiffImage(a, b);
		check(d.valid() && d.pixels[0] == 32 && d.pixels[3] == 255, "diff: delta amplified x8, alpha 255");
		check(!makeDiffImage(a, solid(4, 1, 0, 0, 0)).valid(), "diff: size mismatch invalid");
	}

	// BGRA swizzle round-trip.
	{
		std::vector<uint8_t> pixels{1, 2, 3, 255, 4, 5, 6, 255};
		swizzleBgraToRgba(pixels);
		check(pixels[0] == 3 && pixels[2] == 1 && pixels[4] == 6 && pixels[6] == 4, "swizzle: B<->R swapped");
	}

	// PNG write/read round-trip preserves pixels exactly.
	{
		const RgbaImage image = solid(3, 2, 200, 100, 50);
		const std::string path =
			(std::filesystem::temp_directory_path() / "ft-vox-visual-image-roundtrip.png").string();
		check(writePng(path, image), "png: write succeeded");
		check(!writePng(path, RgbaImage{}), "png: invalid image rejected");
		const RgbaImage decoded = readPng(path);
		check(decoded.valid() && decoded.width == 3 && decoded.height == 2, "png: decoded size");
		const ImageMetrics m = compareImages(image, decoded, thresholds.hotPixelThreshold);
		check(m.comparable() && m.maxAbsError == 0.0, "png: lossless RGBA round-trip");
		check(!readPng(path + ".missing").valid(), "png: missing file returns invalid");
		std::error_code ec;
		std::filesystem::remove(path, ec);
	}

	if (failures == 0)
	{
		std::cout << "test_visual_image: OK\n";
		return 0;
	}
	std::cout << "test_visual_image: FAILED (" << failures << ")\n";
	return 1;
}
