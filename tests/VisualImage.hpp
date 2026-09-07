#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// Shared image plumbing for the deterministic visual-regression harness
/// (issue #142): RGBA8 pixel buffers, PNG IO and tolerant comparison
/// metrics. Final-frame comparison is deliberately NOT bit-exact —
/// floating-point rasterization varies slightly across Vulkan drivers.
namespace visual
{
struct RgbaImage
{
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> pixels; // RGBA8, row-major, top row first
	bool valid() const { return width != 0 && height != 0 && pixels.size() == size_t(width) * height * 4; }
	size_t pixelCount() const { return size_t(width) * height; }
};

struct ImageMetrics
{
	/// Mean per-channel absolute error, normalized to [0,1] (3/255 = a
	/// 3-LSB average delta per channel).
	double meanAbsError = 0.0;
	double rmsError = 0.0;
	double maxAbsError = 0.0;
	/// Pixels whose max channel delta exceeds the hot-pixel threshold.
	long long hotPixels = 0;
	double hotPixelRatio = 0.0;
	/// 0 width/height = comparison was impossible (size mismatch / invalid input).
	uint32_t width = 0;
	uint32_t height = 0;
	bool comparable() const { return width != 0 && height != 0; }
};

struct CompareThresholds
{
	/// Tolerant cross-vendor defaults: tight enough to catch a real grade /
	/// lighting / shadow regression, loose enough for driver rounding.
	double maxMeanAbsError = 3.0 / 255.0;
	double maxRmsError = 5.0 / 255.0;
	/// Fraction of pixels allowed to differ strongly (hot pixels).
	double maxHotPixelRatio = 0.02;
	/// Max channel delta (0..255) below which a pixel difference is "quiet".
	int hotPixelThreshold = 20;
};

/// Encode RGBA8 as PNG (8-bit, color type 6) and write to disk.
/// Returns false on encoding or I/O failure (also when image is invalid).
bool writePng(const std::string &path, const RgbaImage &image);

/// Decode a PNG file to RGBA8 (any bit depth stb supports). Returns an
/// invalid RgbaImage (valid() == false) when the file is missing/corrupt.
RgbaImage readPng(const std::string &path);

/// Compare two images and return normalized metrics:
/// - meanAbsError / rmsError are computed per channel (R, G, B — alpha
///   ignored) and expressed in [0,1] (1.0 = full-scale 255 delta);
/// - maxAbsError is the largest single channel delta, also in [0,1];
/// - hotPixels counts pixels whose max channel delta exceeds the threshold.
/// Size mismatch or invalid input returns incomparable metrics.
ImageMetrics compareImages(const RgbaImage &actual, const RgbaImage &expected, int hotPixelThreshold);

/// True when every metric sits inside the tolerance envelope.
bool withinTolerance(const ImageMetrics &metrics, const CompareThresholds &thresholds);

/// Error-amplified visualization: RGB = |delta| * 8 saturated, A = 255.
/// Invalid inputs return an invalid image.
RgbaImage makeDiffImage(const RgbaImage &actual, const RgbaImage &expected);

/// Reorder a BGRA8 buffer (e.g. VkFormat B8G8R8A8 readback) to RGBA8 in place.
void swizzleBgraToRgba(std::vector<uint8_t> &pixels);
} // namespace visual
