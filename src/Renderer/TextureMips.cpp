#include "Renderer/TextureMips.hpp"

#include "Renderer/ColorSpace.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace texture_mips
{
namespace
{
constexpr uint32_t kBytesPerTexel = 4;
constexpr int kTapCount = 4;
constexpr float kInvTaps = 1.0f / static_cast<float>(kTapCount);
// Alpha is linear data, so the rescale denominator floor only guards the
// un-premultiply divide against degenerate fully transparent inputs.
constexpr float kAlphaEpsilon = 1e-4f;

uint32_t mipDim(uint32_t baseSize, uint32_t level)
{
	return std::max(baseSize >> level, 1u);
}

// Round to nearest 8-bit byte, clamped to the representable range.
uint8_t toByte(float value)
{
	return static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, value * 255.0f + 0.5f)));
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

	// Mip 0 is the verbatim source; every following level is generated from it.
	std::memcpy(outChain, layerPixels, mipLevelBytes(baseSize, 0));

	for (uint32_t level = 1; level < levels; ++level)
	{
		const uint32_t srcW = mipDim(baseSize, level - 1);
		const uint32_t dstW = mipDim(baseSize, level);
		const uint8_t *src = outChain + chainOffset(baseSize, level - 1);
		uint8_t *dst = outChain + chainOffset(baseSize, level);

		for (uint32_t y = 0; y < dstW; ++y)
		{
			// Odd source sizes clamp the {2x, 2x+1} window to the last texel:
			// boundary texels are duplicated, never read out of bounds.
			const uint32_t sy0 = std::min(2u * y, srcW - 1u);
			const uint32_t sy1 = std::min(2u * y + 1u, srcW - 1u);
			for (uint32_t x = 0; x < dstW; ++x)
			{
				const uint32_t sx0 = std::min(2u * x, srcW - 1u);
				const uint32_t sx1 = std::min(2u * x + 1u, srcW - 1u);
				const uint8_t *taps[kTapCount] = {
					src + (sy0 * srcW + sx0) * kBytesPerTexel,
					src + (sy0 * srcW + sx1) * kBytesPerTexel,
					src + (sy1 * srcW + sx0) * kBytesPerTexel,
					src + (sy1 * srcW + sx1) * kBytesPerTexel,
				};

				// Alpha is never sRGB-decoded (it is not color). RGB is decoded
				// to linear light and premultiplied by alpha so fully transparent
				// texels contribute no color; the box average runs on the
				// premultiplied linear values and the color is un-premultiplied
				// again afterwards (no fringe/halo from cut-out texels).
				float premultR = 0.0f;
				float premultG = 0.0f;
				float premultB = 0.0f;
				float alpha = 0.0f;
				float maxAlpha = 0.0f;
				for (const uint8_t *tap : taps)
				{
					const float a = static_cast<float>(tap[3]) / 255.0f;
					premultR += colorspace::srgbToLinear(static_cast<float>(tap[0]) / 255.0f) * a;
					premultG += colorspace::srgbToLinear(static_cast<float>(tap[1]) / 255.0f) * a;
					premultB += colorspace::srgbToLinear(static_cast<float>(tap[2]) / 255.0f) * a;
					alpha += a;
					maxAlpha = std::max(maxAlpha, a);
				}
				premultR *= kInvTaps;
				premultG *= kInvTaps;
				premultB *= kInvTaps;
				alpha *= kInvTaps;

				// Coverage-preserving alpha rescale (issue #136): geometric mean of
				// the window's average and maximum alpha, the Minecraft cutout-mip
				// trick. For the binary alpha used by voxel cutout textures any
				// window holding at least one opaque tap rescales to
				// sqrt(k/4 * 1) >= 0.5, so foliage silhouettes thin gracefully
				// with distance instead of vanishing; opaque textures rescale to
				// exactly 1.0 and stay fully opaque.
				const float rescaled = std::min(1.0f, std::sqrt(alpha * maxAlpha));

				uint8_t *out = dst + (y * dstW + x) * kBytesPerTexel;
				const uint8_t outAlpha = toByte(rescaled);
				if (outAlpha == 0)
				{
					// Transparent black: texels that fell below the cutout
					// threshold must not carry fringe RGB down the chain.
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
