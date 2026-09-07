#include "VisualImage.hpp"

#include <miniz.h>
#include <stb_image/stb_image.h> // declarations only: TextureManager.cpp owns STB_IMAGE_IMPLEMENTATION

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace visual
{
namespace
{
// Append a 32-bit value in big-endian byte order.
void appendBe32(std::vector<uint8_t> &out, uint32_t value)
{
	for (int shift : {24, 16, 8, 0})
		out.push_back(uint8_t(value >> shift));
}

// Append a PNG chunk: 4-byte big-endian payload length, type tag, payload,
// then CRC32 over type + payload.
void appendChunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &payload)
{
	appendBe32(out, uint32_t(payload.size()));
	const size_t crcStart = out.size();
	out.insert(out.end(), type, type + 4);
	out.insert(out.end(), payload.begin(), payload.end());
	appendBe32(out, uint32_t(mz_crc32(0, out.data() + crcStart, out.size() - crcStart)));
}
} // namespace

bool writePng(const std::string &path, const RgbaImage &image)
{
	if (!image.valid() || path.empty())
		return false;

	const std::filesystem::path filePath(path);
	if (!filePath.parent_path().empty())
	{
		std::error_code ec;
		std::filesystem::create_directories(filePath.parent_path(), ec);
		if (ec)
			return false;
	}

	// IHDR: width, height, bit depth 8, color type 6 (RGBA), then reserved zeros.
	std::vector<uint8_t> ihdr;
	appendBe32(ihdr, image.width);
	appendBe32(ihdr, image.height);
	ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});

	// Raw scanline stream: each row prefixed with filter byte 0 (None).
	const size_t stride = size_t(image.width) * 4 + 1;
	std::vector<uint8_t> raw(image.height * stride, 0);
	for (uint32_t y = 0; y < image.height; ++y)
	{
		const size_t rowStart = size_t(y) * stride;
		raw[rowStart] = 0; // filter: None
		const uint8_t *src = image.pixels.data() + size_t(y) * size_t(image.width) * 4;
		std::copy_n(src, size_t(image.width) * 4, raw.data() + rowStart + 1);
	}

	// IDAT: zlib-compressed scanline stream.
	mz_ulong compressedSize = mz_compressBound(mz_ulong(raw.size()));
	std::vector<uint8_t> idat = std::vector<uint8_t>(size_t(compressedSize));
	if (mz_compress(idat.data(), &compressedSize, raw.data(), mz_ulong(raw.size())) != MZ_OK)
		return false;
	idat.resize(size_t(compressedSize));

	std::vector<uint8_t> png{137, 80, 78, 71, 13, 10, 26, 10}; // PNG signature
	appendChunk(png, "IHDR", ihdr);
	appendChunk(png, "IDAT", idat);
	appendChunk(png, "IEND", {});

	std::ofstream file(filePath, std::ios::binary);
	if (!file)
		return false;
	file.write(reinterpret_cast<const char *>(png.data()), std::streamsize(png.size()));
	file.flush();
	return bool(file);
}

RgbaImage readPng(const std::string &path)
{
	RgbaImage image;
	int32_t width = 0;
	int32_t height = 0;
	int32_t channels = 0;
	stbi_uc *data = stbi_load(path.c_str(), &width, &height, &channels, 4);
	if (!data || width <= 0 || height <= 0)
	{
		if (data)
			stbi_image_free(data);
		return image; // invalid: missing or corrupt file
	}
	image.width = uint32_t(width);
	image.height = uint32_t(height);
	image.pixels.assign(data, data + size_t(width) * size_t(height) * 4);
	stbi_image_free(data);
	return image;
}

ImageMetrics compareImages(const RgbaImage &actual, const RgbaImage &expected, int hotPixelThreshold)
{
	ImageMetrics metrics;
	if (!actual.valid() || !expected.valid() || actual.width != expected.width || actual.height != expected.height)
		return metrics; // incomparable: caller checks comparable()

	const size_t pixelCount = actual.pixelCount();
	long long sum = 0;
	double sumSq = 0.0;
	long long maxDelta = 0;
	long long hot = 0;
	for (size_t i = 0; i < pixelCount; ++i)
	{
		const size_t offset = i * 4;
		const int dr = std::abs(int(actual.pixels[offset]) - int(expected.pixels[offset]));
		const int dg = std::abs(int(actual.pixels[offset + 1]) - int(expected.pixels[offset + 1]));
		const int db = std::abs(int(actual.pixels[offset + 2]) - int(expected.pixels[offset + 2]));
		const int delta = std::max({dr, dg, db}); // alpha ignored
		sum += delta;
		sumSq += double(delta) * double(delta);
		maxDelta = std::max<long long>(maxDelta, delta);
		if (delta > hotPixelThreshold)
			++hot;
	}
	metrics.meanAbsError = double(sum) / double(pixelCount);
	metrics.rmsError = std::sqrt(sumSq / double(pixelCount));
	metrics.maxAbsError = double(maxDelta);
	metrics.hotPixels = hot;
	metrics.hotPixelRatio = double(hot) / double(pixelCount);
	metrics.width = actual.width;
	metrics.height = actual.height;
	return metrics;
}

bool withinTolerance(const ImageMetrics &metrics, const CompareThresholds &thresholds)
{
	return metrics.comparable() && metrics.meanAbsError <= thresholds.maxMeanAbsError &&
	       metrics.rmsError <= thresholds.maxRmsError && metrics.hotPixelRatio <= thresholds.maxHotPixelRatio;
}

RgbaImage makeDiffImage(const RgbaImage &actual, const RgbaImage &expected)
{
	RgbaImage diff;
	if (!actual.valid() || !expected.valid() || actual.width != expected.width || actual.height != expected.height)
		return diff; // invalid: inputs not comparable

	diff.width = actual.width;
	diff.height = actual.height;
	diff.pixels.resize(actual.pixels.size());
	const size_t pixelCount = actual.pixelCount();
	for (size_t i = 0; i < pixelCount; ++i)
	{
		const size_t offset = i * 4;
		for (int channel = 0; channel < 3; ++channel)
		{
			const int delta = std::abs(int(actual.pixels[offset + channel]) - int(expected.pixels[offset + channel]));
			diff.pixels[offset + channel] = uint8_t(std::min(255, delta * 8)); // amplified, saturated
		}
		diff.pixels[offset + 3] = 255;
	}
	return diff;
}

void swizzleBgraToRgba(std::vector<uint8_t> &pixels)
{
	for (size_t offset = 0; offset + 3 < pixels.size(); offset += 4)
		std::swap(pixels[offset], pixels[offset + 2]); // B <-> R
}
} // namespace visual
