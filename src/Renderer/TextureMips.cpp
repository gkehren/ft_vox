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

float cutoutCoverage(const uint8_t *level, uint32_t w, uint32_t h)
{
	const uint64_t texels = static_cast<uint64_t>(w) * h;
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
float scaledCutoutCoverage(const uint8_t *level, uint32_t w, uint32_t h, float scale)
{
	const uint64_t texels = static_cast<uint64_t>(w) * h;
	uint64_t covered = 0;
	for (uint64_t i = 0; i < texels; ++i)
	{
		const float a = static_cast<float>(level[i * kBytesPerTexel + 3]) / 255.0f * scale;
		if (a >= kAlphaCutoutThreshold)
			++covered;
	}
	return static_cast<float>(covered) / static_cast<float>(texels);
}

// Deterministic 4x4 Bayer rank used to spread tie resolutions across a level
// instead of clustering them.
uint32_t bayer4Rank(uint32_t x, uint32_t y)
{
	static constexpr uint32_t kBayer4[16] = {
		0,  8,  2, 10,
		12,  4, 14,  6,
		3,  11,  1,  9,
		15,  7, 13,  5,
	};
	return kBayer4[(y % 4) * 4 + (x % 4)];
}

// Binary search the alpha scale that brings a level's cutout coverage closest
// to `targetCoverage` (equal-error ties resolve toward the higher coverage).
// `probe(scale)` reports the level's coverage if every alpha were scaled.
template <typename Probe>
float findCoverageScale(float coverage, float targetCoverage, Probe &&probe)
{
	float bestScale = 1.0f;
	float bestError = std::abs(coverage - targetCoverage);
	float bestCoverage = coverage;
	float lo = 0.0f;
	float hi = kMaxCoverageScale;
	for (uint32_t iteration = 0; iteration < kCoverageSearchIterations; ++iteration)
	{
		const float mid = 0.5f * (lo + hi);
		const float midCoverage = probe(mid);
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
	return bestScale;
}

// Alpha values assigned to boundary texels when the coverage count must be
// quantized: one on each side of the canonical threshold.
constexpr float kPromotedCutoutAlpha = 128.0f / 255.0f; // >= kAlphaCutoutThreshold
constexpr float kDemotedCutoutAlpha = 127.0f / 255.0f;  // <  kAlphaCutoutThreshold

// Coverage-preserving rescale, quantized (issue #136 review): the global
// `scale` from the binary search is only the first approximation — a uniform
// multiplier cannot break alpha ties (four isolated 0.25 texels can only all
// pass or all fail together, erasing representable details). After scaling,
// texels are promoted/demoted individually until the level's cutout texel
// count matches round(targetCoverage * texelCount): candidates closest to the
// threshold first, ties spread by Bayer rank then resolved by index.
void applyCoverageRescale(uint8_t *level, uint32_t w, uint32_t h, float scale, float targetCoverage)
{
	const uint64_t texels = static_cast<uint64_t>(w) * h;
	std::vector<float> scaled(texels);
	uint64_t covered = 0;
	for (uint64_t i = 0; i < texels; ++i)
	{
		scaled[i] = std::min(1.0f, static_cast<float>(level[i * kBytesPerTexel + 3]) / 255.0f * scale);
		if (scaled[i] >= kAlphaCutoutThreshold)
			++covered;
	}
	const uint64_t targetCount =
		static_cast<uint64_t>(std::lround(targetCoverage * static_cast<double>(texels)));
	if (covered != targetCount)
	{
		const bool promote = covered < targetCount;
		std::vector<uint64_t> candidates;
		candidates.reserve(texels);
		for (uint64_t i = 0; i < texels; ++i)
		{
			const bool isCovered = scaled[i] >= kAlphaCutoutThreshold;
			if (promote ? !isCovered : isCovered)
				candidates.push_back(i);
		}
		std::sort(candidates.begin(), candidates.end(), [&](uint64_t a, uint64_t b) {
			const float fa = scaled[a];
			const float fb = scaled[b];
			if (fa != fb)
				return promote ? fa > fb : fa < fb; // closest to the threshold first
			const uint32_t ra = bayer4Rank(static_cast<uint32_t>(a % w), static_cast<uint32_t>(a / w));
			const uint32_t rb = bayer4Rank(static_cast<uint32_t>(b % w), static_cast<uint32_t>(b / w));
			if (ra != rb)
				return ra < rb;
			return a < b;
		});
		const uint64_t moves = std::min<uint64_t>(
			promote ? targetCount - covered : covered - targetCount, candidates.size());
		for (uint64_t k = 0; k < moves; ++k)
			scaled[candidates[static_cast<size_t>(k)]] = promote ? kPromotedCutoutAlpha : kDemotedCutoutAlpha;
	}
	for (uint64_t i = 0; i < texels; ++i)
	{
		uint8_t *texel = level + i * kBytesPerTexel;
		texel[3] = toByte(scaled[i]);
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
void dilateBorderColors(uint8_t *level, uint32_t w, uint32_t h)
{
	std::vector<uint8_t> snapshot(level, level + static_cast<size_t>(w) * h * kBytesPerTexel);
	for (uint32_t y = 0; y < h; ++y)
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
			const uint32_t ny1 = std::min(y + 1, h - 1);
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

// ---- span-based variants used by the UV-rect path (issue #160 review P1) ----
// A span is the list of level-buffer texel indices owned by one UV rect; the
// quantized rescale then only ever promotes/demotes texels of that rect.

uint64_t spanCoveredCount(const uint8_t *level, const std::vector<uint32_t> &span)
{
	uint64_t covered = 0;
	for (uint32_t i : span)
	{
		if (isCutoutAlpha(level[size_t(i) * kBytesPerTexel + 3]))
			++covered;
	}
	return covered;
}

float spanScaledCoverage(const uint8_t *level, const std::vector<uint32_t> &span, float scale)
{
	if (span.empty())
		return 0.0f;
	uint64_t covered = 0;
	for (uint32_t i : span)
	{
		if (static_cast<float>(level[size_t(i) * kBytesPerTexel + 3]) / 255.0f * scale >= kAlphaCutoutThreshold)
			++covered;
	}
	return static_cast<float>(covered) / static_cast<float>(span.size());
}

// Same quantized rescale as applyCoverageRescale, restricted to one rect's
// texels: binary-search scale (caller-provided), then boundary promotions /
// demotions until the span's covered COUNT equals targetCount. Bayer ranks use
// the level-grid position so ties spread identically to the contiguous path.
void spanCoverageRescale(uint8_t *level, uint32_t lw, const std::vector<uint32_t> &span, float scale,
						 uint64_t targetCount)
{
	std::vector<float> scaled(span.size());
	uint64_t covered = 0;
	for (size_t k = 0; k < span.size(); ++k)
	{
		scaled[k] = std::min(1.0f, static_cast<float>(level[size_t(span[k]) * kBytesPerTexel + 3]) / 255.0f * scale);
		if (scaled[k] >= kAlphaCutoutThreshold)
			++covered;
	}
	if (covered != targetCount)
	{
		const bool promote = covered < targetCount;
		std::vector<size_t> candidates;
		candidates.reserve(span.size());
		for (size_t k = 0; k < span.size(); ++k)
		{
			const bool isCovered = scaled[k] >= kAlphaCutoutThreshold;
			if (promote ? !isCovered : isCovered)
				candidates.push_back(k);
		}
		std::sort(candidates.begin(), candidates.end(), [&](size_t a, size_t b) {
			const float fa = scaled[a];
			const float fb = scaled[b];
			if (fa != fb)
				return promote ? fa > fb : fa < fb; // closest to the threshold first
			const uint32_t ia = span[a], ib = span[b];
			const uint32_t ra = bayer4Rank(ia % lw, ia / lw);
			const uint32_t rb = bayer4Rank(ib % lw, ib / lw);
			if (ra != rb)
				return ra < rb;
			return ia < ib;
		});
		const uint64_t moves = std::min<uint64_t>(
			promote ? targetCount - covered : covered - targetCount, candidates.size());
		for (uint64_t k = 0; k < moves; ++k)
			scaled[candidates[static_cast<size_t>(k)]] = promote ? kPromotedCutoutAlpha : kDemotedCutoutAlpha;
	}
	for (size_t k = 0; k < span.size(); ++k)
	{
		uint8_t *texel = level + size_t(span[k]) * kBytesPerTexel;
		texel[3] = toByte(scaled[k]);
		if (texel[3] == 0)
		{
			texel[0] = 0;
			texel[1] = 0;
			texel[2] = 0;
		}
	}
}

// Ownership-aware border serving for one level. `owner` holds one byte per
// texel: the owning rect index, or kNoOwner for "don't care" (unused atlas
// space). Owned texels with alpha 0 take the linear-light average color of
// covered SAME-owner neighbors (#136 dark-fringe fix, confined to their face);
// don't-care texels get alpha 0 and the color of covered neighbors from ANY
// rect (a gutter, so GPU bilinear filtering at a rect border blends toward the
// bordering rect's own color instead of toward unused-region content).
constexpr uint8_t kNoOwner = 0xff;

void dilateLevelBorders(uint8_t *level, uint32_t w, uint32_t h, const std::vector<uint8_t> &owner)
{
	std::vector<uint8_t> snapshot(level, level + static_cast<size_t>(w) * h * kBytesPerTexel);
	for (uint32_t y = 0; y < h; ++y)
	{
		for (uint32_t x = 0; x < w; ++x)
		{
			const size_t texelIndex = static_cast<size_t>(y) * w + x;
			uint8_t *texel = level + texelIndex * kBytesPerTexel;
			const uint8_t own = owner[texelIndex];
			const bool dontCare = own == kNoOwner;
			if (!dontCare && texel[3] != 0)
				continue;
			float r = 0.0f;
			float g = 0.0f;
			float b = 0.0f;
			uint32_t contributors = 0;
			const uint32_t ny0 = y > 0 ? y - 1 : 0;
			const uint32_t ny1 = std::min(y + 1, h - 1);
			const uint32_t nx0 = x > 0 ? x - 1 : 0;
			const uint32_t nx1 = std::min(x + 1, w - 1);
			for (uint32_t ny = ny0; ny <= ny1; ++ny)
			{
				for (uint32_t nx = nx0; nx <= nx1; ++nx)
				{
					const size_t neighborIndex = static_cast<size_t>(ny) * w + nx;
					const uint8_t *neighbor = snapshot.data() + neighborIndex * kBytesPerTexel;
					if (!isCutoutAlpha(neighbor[3]))
						continue;
					// Owned texels only take from their own rect; don't-care
					// texels take from any covered texel.
					if (!dontCare && owner[neighborIndex] != own)
						continue;
					r += colorspace::srgbToLinear(static_cast<float>(neighbor[0]) / 255.0f);
					g += colorspace::srgbToLinear(static_cast<float>(neighbor[1]) / 255.0f);
					b += colorspace::srgbToLinear(static_cast<float>(neighbor[2]) / 255.0f);
					++contributors;
				}
			}
			if (contributors == 0)
			{
				if (dontCare)
				{
					texel[0] = 0;
					texel[1] = 0;
					texel[2] = 0;
					texel[3] = 0;
				}
				continue;
			}
			texel[0] = toByte(colorspace::linearToSrgb(r / static_cast<float>(contributors)));
			texel[1] = toByte(colorspace::linearToSrgb(g / static_cast<float>(contributors)));
			texel[2] = toByte(colorspace::linearToSrgb(b / static_cast<float>(contributors)));
			if (dontCare)
				texel[3] = 0;
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

uint32_t mipLevelCount(uint32_t width, uint32_t height)
{
	// Both dimensions halve per level, so the chain length is governed by the
	// larger axis; it ends only when BOTH sides have reached 1 texel.
	return mipLevelCount(std::max(width, height));
}

uint32_t mipDim(uint32_t baseSize, uint32_t level)
{
	return std::max(baseSize >> level, 1u);
}

uint32_t mipLevelBytes(uint32_t baseSize, uint32_t level)
{
	return mipLevelBytes(baseSize, baseSize, level);
}

uint32_t mipLevelBytes(uint32_t baseWidth, uint32_t baseHeight, uint32_t level)
{
	return mipDim(baseWidth, level) * mipDim(baseHeight, level) * kBytesPerTexel;
}

uint32_t chainOffset(uint32_t baseSize, uint32_t level)
{
	return chainOffset(baseSize, baseSize, level);
}

uint32_t chainOffset(uint32_t baseWidth, uint32_t baseHeight, uint32_t level)
{
	uint32_t offset = 0;
	for (uint32_t l = 0; l < level; ++l)
		offset += mipLevelBytes(baseWidth, baseHeight, l);
	return offset;
}

uint32_t chainBytes(uint32_t baseSize)
{
	return chainOffset(baseSize, baseSize, mipLevelCount(baseSize));
}

uint32_t chainBytes(uint32_t baseWidth, uint32_t baseHeight)
{
	return chainOffset(baseWidth, baseHeight, mipLevelCount(baseWidth, baseHeight));
}

void generateLayerChain(uint32_t baseSize, const uint8_t *layerPixels, uint8_t *outChain)
{
	generateLayerChain(baseSize, baseSize, layerPixels, outChain);
}

void generateLayerChain(uint32_t baseWidth, uint32_t baseHeight, const uint8_t *layerPixels,
						uint8_t *outChain)
{
	const uint32_t levels = mipLevelCount(baseWidth, baseHeight);

	// Mip 0 keeps the source alpha and the RGB of alpha > 0 texels; only
	// alpha-0 RGB may be edge-dilated below. Its cutout coverage is the
	// target every generated level must preserve.
	std::memcpy(outChain, layerPixels, mipLevelBytes(baseWidth, baseHeight, 0));
	const float targetCoverage = cutoutCoverage(outChain, baseWidth, baseHeight);
	// The base level is served too: slight minification already filters mip 0
	// with LINEAR, so alpha-0 texels next to cutout texels need the border
	// color here as well (dark fringe near LOD 0 — issue #136 review).
	if (targetCoverage > 0.0f)
		dilateBorderColors(outChain, baseWidth, baseHeight);

	for (uint32_t level = 1; level < levels; ++level)
	{
		const uint32_t srcW = mipDim(baseWidth, level - 1);
		const uint32_t srcH = mipDim(baseHeight, level - 1);
		const uint32_t dstW = mipDim(baseWidth, level);
		const uint32_t dstH = mipDim(baseHeight, level);
		const uint8_t *src = outChain + chainOffset(baseWidth, baseHeight, level - 1);
		uint8_t *dst = outChain + chainOffset(baseWidth, baseHeight, level);

		for (uint32_t y = 0; y < dstH; ++y)
		{
			uint32_t sy0 = 0;
			uint32_t sy1 = 0;
			sourceWindow(y, srcH, dstH, sy0, sy1);
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

		// Coverage preservation (DirectXTex-style alpha rescale + quantized tie
		// resolution, issue #136): plain box averaging drifts cutout coverage
		// upward as levels densify (a 50% mask measured 100% at mip 1). A
		// per-level binary search finds the alpha scale that brings this
		// level's cutout coverage back toward the base level's, then boundary
		// texels are promoted/demoted so the covered texel COUNT matches the
		// target exactly. Fully transparent (nothing to preserve) and fully
		// opaque (alpha must stay exactly 255) layers skip the search.
		if (targetCoverage > 0.0f && targetCoverage < 1.0f)
		{
			const float coverage = cutoutCoverage(dst, dstW, dstH);
			const float target = targetCoverage;
			if (coverage != target)
			{
				const float bestScale = findCoverageScale(coverage, target, [&](float mid) {
					return scaledCutoutCoverage(dst, dstW, dstH, mid);
				});
				applyCoverageRescale(dst, dstW, dstH, bestScale, target);
			}
		}

		if (targetCoverage > 0.0f)
			dilateBorderColors(dst, dstW, dstH);
	}
}

void generateLayerChainRects(uint32_t baseWidth, uint32_t baseHeight, const uint8_t *layerPixels,
							 const MipRect *rects, size_t rectCount, uint8_t *outChain)
{
	if (rectCount == 0 || !rects)
	{
		generateLayerChain(baseWidth, baseHeight, layerPixels, outChain);
		return;
	}
	const uint32_t levels = mipLevelCount(baseWidth, baseHeight);

	// Level 0 keeps the source texels inside every rect; don't-care texels get
	// alpha 0 plus a gutter color right away. Per-rect base coverage is the
	// target that rect's levels must preserve.
	std::memcpy(outChain, layerPixels, mipLevelBytes(baseWidth, baseHeight, 0));
	std::vector<uint8_t> owner0(static_cast<size_t>(baseWidth) * baseHeight, kNoOwner);
	std::vector<float> coverage(rectCount, 0.0f);
	for (size_t r = 0; r < rectCount; ++r)
	{
		const MipRect &rect = rects[r];
		uint64_t covered = 0;
		for (uint32_t y = rect.y; y < rect.y + rect.h; ++y)
		{
			for (uint32_t x = rect.x; x < rect.x + rect.w; ++x)
			{
				owner0[static_cast<size_t>(y) * baseWidth + x] = static_cast<uint8_t>(r);
				if (isCutoutAlpha(outChain[(static_cast<size_t>(y) * baseWidth + x) * kBytesPerTexel + 3]))
					++covered;
			}
		}
		coverage[r] = static_cast<float>(covered) / static_cast<float>(uint64_t(rect.w) * rect.h);
	}
	dilateLevelBorders(outChain, baseWidth, baseHeight, owner0);
	std::vector<uint8_t> ownerPrev = std::move(owner0);

	for (uint32_t level = 1; level < levels; ++level)
	{
		const uint32_t srcW = mipDim(baseWidth, level - 1);
		const uint32_t srcH = mipDim(baseHeight, level - 1);
		const uint32_t dstW = mipDim(baseWidth, level);
		const uint32_t dstH = mipDim(baseHeight, level);
		const uint8_t *src = outChain + chainOffset(baseWidth, baseHeight, level - 1);
		uint8_t *dst = outChain + chainOffset(baseWidth, baseHeight, level);

		// Destination-texel ownership: the window center is itself a source
		// texel of level (l-1), so its PREVIOUS-LEVEL owner is the owning rect
		// — no per-level rect remapping needed, and the same map doubles as
		// the tap mask (a source texel only ever contributes to the rect that
		// owned it, so neighboring faces can never blend).
		std::vector<uint8_t> owner(static_cast<size_t>(dstW) * dstH, kNoOwner);
		std::vector<std::vector<uint32_t>> owned(rectCount);
		for (uint32_t y = 0; y < dstH; ++y)
		{
			uint32_t sy0 = 0, sy1 = 0;
			sourceWindow(y, srcH, dstH, sy0, sy1);
			const uint32_t cy = (sy0 + sy1) / 2;
			for (uint32_t x = 0; x < dstW; ++x)
			{
				uint32_t sx0 = 0, sx1 = 0;
				sourceWindow(x, srcW, dstW, sx0, sx1);
				const uint32_t cx = (sx0 + sx1) / 2;
				const uint8_t own = ownerPrev[static_cast<size_t>(cy) * srcW + cx];
				owner[static_cast<size_t>(y) * dstW + x] = own;
				if (own != kNoOwner)
					owned[own].push_back(y * dstW + x);
			}
		}

		// Premultiplied linear-light filtering, taps restricted to source
		// texels owned by the same rect. The center texel is owned, so the
		// intersection is never empty.
		for (size_t r = 0; r < rectCount; ++r)
		{
			for (uint32_t idx : owned[r])
			{
				const uint32_t x = idx % dstW;
				const uint32_t y = idx / dstW;
				uint32_t sx0 = 0, sx1 = 0, sy0 = 0, sy1 = 0;
				sourceWindow(x, srcW, dstW, sx0, sx1);
				sourceWindow(y, srcH, dstH, sy0, sy1);

				float premultR = 0.0f, premultG = 0.0f, premultB = 0.0f, alpha = 0.0f;
				uint32_t taps = 0;
				for (uint32_t sy = sy0; sy < sy1; ++sy)
				{
					for (uint32_t sx = sx0; sx < sx1; ++sx)
					{
						if (ownerPrev[static_cast<size_t>(sy) * srcW + sx] != static_cast<uint8_t>(r))
							continue; // a neighboring face's texel: never blended
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

				uint8_t *out = dst + static_cast<size_t>(idx) * kBytesPerTexel;
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

		// Per-rect alpha-coverage preservation: each rect's cutout coverage is
		// pinned to the rect's OWN base coverage (a global rescale could spend
		// one face's silhouette to pay for another's). Opaque and fully
		// transparent rects need no search.
		for (size_t r = 0; r < rectCount; ++r)
		{
			if (coverage[r] <= 0.0f || coverage[r] >= 1.0f || owned[r].empty())
				continue;
			const std::vector<uint32_t> &span = owned[r];
			const uint64_t targetCount =
				static_cast<uint64_t>(std::lround(coverage[r] * static_cast<double>(span.size())));
			const float coverageNow =
				static_cast<float>(spanCoveredCount(dst, span)) / static_cast<float>(span.size());
			const float targetCoverage =
				static_cast<float>(targetCount) / static_cast<float>(span.size());
			if (coverageNow != targetCoverage)
			{
				const float scale = findCoverageScale(coverageNow, targetCoverage,
													  [&](float mid) { return spanScaledCoverage(dst, span, mid); });
				spanCoverageRescale(dst, dstW, span, scale, targetCount);
			}
		}

		// Same-owner interior dilation + don't-care gutters on every level.
		dilateLevelBorders(dst, dstW, dstH, owner);
		ownerPrev = std::move(owner);
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
