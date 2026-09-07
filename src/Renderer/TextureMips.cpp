#include "Renderer/TextureMips.hpp"

#include "Renderer/ColorSpace.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace texture_mips
{
namespace
{
constexpr uint32_t kBytesPerTexel = 4;
// Alpha is linear data; the floor only guards the un-premultiply divide
// against degenerate fully transparent inputs.
constexpr float kAlphaEpsilon = 1e-4f;
// Binary-search ceiling for the coverage rescale: 8 lets a mip whose average
// alpha collapsed to ~1/8 of the threshold climb back toward its target.
constexpr float kMaxCoverageScale = 8.0f;
constexpr uint32_t kCoverageSearchIterations = 30;

uint32_t mipDim(uint32_t baseSize, uint32_t level)
{
	return std::max(baseSize >> level, 1u);
}

// Round to nearest 8-bit byte, clamped to the representable range.
uint8_t toByte(float value)
{
	return static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, value * 255.0f + 0.5f)));
}

bool isCutoutAlpha(uint8_t alphaByte)
{
	return static_cast<float>(alphaByte) / 255.0f >= kAlphaCutoutThreshold;
}

// Half-open source window [x0, x1) that destination texel `index` averages: an
// area mapping in which every source texel contributes to exactly one
// destination texel. Power-of-two sizes (the canonical atlas layer size is
// normalized to POT) yield the classic {2x, 2x+1} window; odd sizes spread the
// remainder across the edge texels instead of dropping the last row/column.
void sourceWindow(uint32_t index, uint32_t srcSize, uint32_t dstSize, uint32_t &x0, uint32_t &x1)
{
	x0 = index * srcSize / dstSize;
	x1 = (index + 1) * srcSize / dstSize;
	if (x1 <= x0)
		x1 = x0 + 1;
}

float cutoutCoverage(const uint8_t *level, uint32_t w)
{
	const uint64_t texels = static_cast<uint64_t>(w) * w;
	uint64_t covered = 0;
	for (uint64_t i = 0; i < texels; ++i)
	{
		if (isCutoutAlpha(level[i * kBytesPerTexel + 3]))
			++covered;
	}
	return static_cast<float>(covered) / static_cast<float>(texels);
}

// Coverage the level would have if every alpha were scaled by `scale`, without
// writing anything (binary-search probe).
float scaledCutoutCoverage(const uint8_t *level, uint32_t w, float scale)
{
	const uint64_t texels = static_cast<uint64_t>(w) * w;
	uint64_t covered = 0;
	for (uint64_t i = 0; i < texels; ++i)
	{
		const float a = static_cast<float>(level[i * kBytesPerTexel + 3]) / 255.0f * scale;
		if (a >= kAlphaCutoutThreshold)
			++covered;
	}
	return static_cast<float>(covered) / static_cast<float>(texels);
}

// Rescale the level's alpha around the cutout threshold; texels that end up
// fully transparent collapse to transparent black.
void applyAlphaScale(uint8_t *level, uint32_t w, float scale)
{
	const uint64_t texels = static_cast<uint64_t>(w) * w;
	for (uint64_t i = 0; i < texels; ++i)
	{
		uint8_t *texel = level + i * kBytesPerTexel;
		const float a = static_cast<float>(texel[3]) / 255.0f * scale;
		texel[3] = toByte(a);
		if (texel[3] == 0)
		{
			texel[0] = 0;
			texel[1] = 0;
			texel[2] = 0;
		}
	}
}

// Alpha bleeding (issue #136): give fully transparent texels the color of
// their covered neighbors so GPU LINEAR minification interpolating across a
// cutout edge blends toward the real border color instead of toward black
// (dark fringe on leaves/grass). Alpha stays 0, so the premultiplied CPU
// filter of deeper levels is unaffected.
void dilateBorderColors(uint8_t *level, uint32_t w)
{
	std::vector<uint8_t> snapshot(level, level + static_cast<size_t>(w) * w * kBytesPerTexel);
	for (uint32_t y = 0; y < w; ++y)
	{
		for (uint32_t x = 0; x < w; ++x)
		{
			uint8_t *texel = level + (static_cast<size_t>(y) * w + x) * kBytesPerTexel;
			if (texel[3] != 0)
				continue;
			float r = 0.0f;
			float g = 0.0f;
			float b = 0.0f;
			uint32_t contributors = 0;
			const uint32_t ny0 = y > 0 ? y - 1 : 0;
			const uint32_t ny1 = std::min(y + 1, w - 1);
			const uint32_t nx0 = x > 0 ? x - 1 : 0;
			const uint32_t nx1 = std::min(x + 1, w - 1);
			for (uint32_t ny = ny0; ny <= ny1; ++ny)
			{
				for (uint32_t nx = nx0; nx <= nx1; ++nx)
				{
					const uint8_t *neighbor =
						snapshot.data() + (static_cast<size_t>(ny) * w + nx) * kBytesPerTexel;
					if (!isCutoutAlpha(neighbor[3]))
						continue;
					r += colorspace::srgbToLinear(static_cast<float>(neighbor[0]) / 255.0f);
					g += colorspace::srgbToLinear(static_cast<float>(neighbor[1]) / 255.0f);
					b += colorspace::srgbToLinear(static_cast<float>(neighbor[2]) / 255.0f);
					++contributors;
				}
			}
			if (contributors == 0)
				continue;
			texel[0] = toByte(colorspace::linearToSrgb(r / static_cast<float>(contributors)));
			texel[1] = toByte(colorspace::linearToSrgb(g / static_cast<float>(contributors)));
			texel[2] = toByte(colorspace::linearToSrgb(b / static_cast<float>(contributors)));
		}
	}
}
} // namespace

uint32_t mipLevelCount(uint32_t size)
{
	uint32_t levels = 1;
	while ((size >> levels) != 0)
		++levels;
	return levels;
}

uint32_t mipLevelBytes(uint32_t baseSize, uint32_t level)
{
	const uint32_t dim = mipDim(baseSize, level);
	return dim * dim * kBytesPerTexel;
}

uint32_t chainOffset(uint32_t baseSize, uint32_t level)
{
	uint32_t offset = 0;
	for (uint32_t l = 0; l < level; ++l)
		offset += mipLevelBytes(baseSize, l);
	return offset;
}

uint32_t chainBytes(uint32_t baseSize)
{
	return chainOffset(baseSize, mipLevelCount(baseSize));
}

void generateLayerChain(uint32_t baseSize, const uint8_t *layerPixels, uint8_t *outChain)
{
	const uint32_t levels = mipLevelCount(baseSize);

	// Mip 0 is the verbatim source; its cutout coverage is the target every
	// generated level must preserve.
	std::memcpy(outChain, layerPixels, mipLevelBytes(baseSize, 0));
	const float targetCoverage = cutoutCoverage(outChain, baseSize);

	for (uint32_t level = 1; level < levels; ++level)
	{
		const uint32_t srcW = mipDim(baseSize, level - 1);
		const uint32_t dstW = mipDim(baseSize, level);
		const uint8_t *src = outChain + chainOffset(baseSize, level - 1);
		uint8_t *dst = outChain + chainOffset(baseSize, level);

		for (uint32_t y = 0; y < dstW; ++y)
		{
			uint32_t sy0 = 0;
			uint32_t sy1 = 0;
			sourceWindow(y, srcW, dstW, sy0, sy1);
			for (uint32_t x = 0; x < dstW; ++x)
			{
				uint32_t sx0 = 0;
				uint32_t sx1 = 0;
				sourceWindow(x, srcW, dstW, sx0, sx1);

				// RGB is sRGB-decoded to linear light and premultiplied by
				// alpha so fully transparent texels contribute no color (no
				// fringe); alpha is never sRGB-decoded and is averaged plainly
				// — the coverage rescale below runs on the finished level.
				float premultR = 0.0f;
				float premultG = 0.0f;
				float premultB = 0.0f;
				float alpha = 0.0f;
				uint32_t taps = 0;
				for (uint32_t sy = sy0; sy < sy1; ++sy)
				{
					for (uint32_t sx = sx0; sx < sx1; ++sx)
					{
						const uint8_t *tap = src + (static_cast<size_t>(sy) * srcW + sx) * kBytesPerTexel;
						const float a = static_cast<float>(tap[3]) / 255.0f;
						premultR += colorspace::srgbToLinear(static_cast<float>(tap[0]) / 255.0f) * a;
						premultG += colorspace::srgbToLinear(static_cast<float>(tap[1]) / 255.0f) * a;
						premultB += colorspace::srgbToLinear(static_cast<float>(tap[2]) / 255.0f) * a;
						alpha += a;
						++taps;
					}
				}
				const float invTaps = 1.0f / static_cast<float>(taps);
				premultR *= invTaps;
				premultG *= invTaps;
				premultB *= invTaps;
				alpha *= invTaps;

				uint8_t *out = dst + (static_cast<size_t>(y) * dstW + x) * kBytesPerTexel;
				const uint8_t outAlpha = toByte(alpha);
				if (outAlpha == 0)
				{
					out[0] = 0;
					out[1] = 0;
					out[2] = 0;
					out[3] = 0;
					continue;
				}
				const float invAlpha = 1.0f / std::max(alpha, kAlphaEpsilon);
				out[0] = toByte(colorspace::linearToSrgb(premultR * invAlpha));
				out[1] = toByte(colorspace::linearToSrgb(premultG * invAlpha));
				out[2] = toByte(colorspace::linearToSrgb(premultB * invAlpha));
				out[3] = outAlpha;
			}
		}

		// Coverage preservation (DirectXTex-style alpha rescale, issue #136):
		// plain box averaging drifts cutout coverage upward as levels densify
		// (a 50% mask measured 100% at mip 1). A per-level binary search finds
		// the alpha scale that brings this level's cutout coverage back to the
		// base level's; ties resolve toward the higher coverage so silhouettes
		// never vanish. Fully transparent (nothing to preserve) and fully
		// opaque (alpha must stay exactly 255) layers skip the search.
		if (targetCoverage > 0.0f && targetCoverage < 1.0f)
		{
			const float coverage = cutoutCoverage(dst, dstW);
			if (coverage != targetCoverage)
			{
				float bestScale = 1.0f;
				float bestError = std::abs(coverage - targetCoverage);
				float bestCoverage = coverage;
				float lo = 0.0f;
				float hi = kMaxCoverageScale;
				for (uint32_t iteration = 0; iteration < kCoverageSearchIterations; ++iteration)
				{
					const float mid = 0.5f * (lo + hi);
					const float midCoverage = scaledCutoutCoverage(dst, dstW, mid);
					const float error = std::abs(midCoverage - targetCoverage);
					if (error < bestError || (error == bestError && midCoverage > bestCoverage))
					{
						bestError = error;
						bestCoverage = midCoverage;
						bestScale = mid;
					}
					if (midCoverage > targetCoverage)
						hi = mid;
					else if (midCoverage < targetCoverage)
						lo = mid;
					else
						break;
				}
				applyAlphaScale(dst, dstW, bestScale);
			}
		}

		if (targetCoverage > 0.0f)
			dilateBorderColors(dst, dstW);
	}
}

std::vector<uint8_t> buildMipChainAtlas(uint32_t baseSize, uint32_t layers, const std::vector<uint8_t> &atlas)
{
	const uint32_t layerStride = chainBytes(baseSize);
	std::vector<uint8_t> out(static_cast<size_t>(layers) * layerStride, 0u);
	const size_t mip0Bytes = mipLevelBytes(baseSize, 0);
	if (atlas.size() < static_cast<size_t>(layers) * mip0Bytes)
		return out; // short input: deterministic zero-filled chains
	for (uint32_t layer = 0; layer < layers; ++layer)
	{
		const uint8_t *src = atlas.data() + static_cast<size_t>(layer) * mip0Bytes;
		uint8_t *dst = out.data() + static_cast<size_t>(layer) * layerStride;
		generateLayerChain(baseSize, src, dst);
	}
	return out;
}

SamplerAnisotropy resolveAnisotropy(bool deviceSupportsAnisotropy, float deviceMaxAnisotropy, float requestedMax)
{
	if (!deviceSupportsAnisotropy)
		return {false, 1.0f};
	// Clamped to the device limit; never below 1.0 in any case.
	return {true, std::max(std::min(requestedMax, deviceMaxAnisotropy), 1.0f)};
}

} // namespace texture_mips
