#pragma once
#include <cstdint>
#include <vector>

namespace texture_mips
{
	// Canonical alpha-cutout threshold (issue #136). Must stay in sync with
	// ressources/shaders/vulkan/cutout.inc.glsl (kAlphaCutoutThreshold).
	inline constexpr float kAlphaCutoutThreshold = 0.5f;

	uint32_t mipLevelCount(uint32_t size);					 // floor(log2(size)) + 1; size >= 1
	uint32_t mipLevelBytes(uint32_t baseSize, uint32_t level);
	uint32_t chainOffset(uint32_t baseSize, uint32_t level); // byte offset of `level` inside one layer's chain
	uint32_t chainBytes(uint32_t baseSize);					 // total byte size of one layer's chain

	// Generate the full mip chain for ONE RGBA8 sRGB layer.
	// layerPixels: baseSize*baseSize*4 bytes (mip 0). outChain: chainBytes(baseSize) bytes;
	// mip 0 is copied verbatim, every following level is generated.
	//
	// Filtering policy (issue #136):
	//  - linear light: each texel's RGB is sRGB-decoded (colorspace::srgbToLinear), box-averaged,
	//    re-encoded (colorspace::linearToSrgb) — never average gamma-encoded values;
	//  - color is premultiplied by alpha before averaging and un-premultiplied after, so fully
	//    transparent texels cannot bleed their RGB into the result (no fringe/halo);
	//  - alpha is averaged in linear alpha space, then rescaled per texel with the geometric mean
	//    of the window's average and maximum alpha (a' = min(1, sqrt(avg * max)), the Minecraft
	//    cutout-mip trick): for the binary cutout textures used by voxel games any window holding
	//    an opaque tap stays >= kAlphaCutoutThreshold, so foliage silhouettes thin gracefully with
	//    distance instead of vanishing, while fully transparent windows still collapse to 0;
	//  - texels whose rescaled alpha is ~0 get RGB 0 (transparent black) to avoid halo pollution;
	//  - opaque textures stay fully opaque (alpha 255 through every level);
	//  - odd source sizes: downsample w -> max(w/2, 1); the source window of texel x is {2x, 2x+1}
	//    clamped to the last texel (boundary texels are duplicated, never read out of bounds).
	void generateLayerChain(uint32_t baseSize, const uint8_t *layerPixels, uint8_t *outChain);

	// Multi-layer upload buffer for the texture array, mip-major / layer-minor:
	//   offset(level, layer) = chainOffset(baseSize, level) + layer * chainBytes(baseSize)
	// `atlas` holds `layers` tightly-packed mip-0 layers (baseSize*baseSize*4 bytes each).
	std::vector<uint8_t> buildMipChainAtlas(uint32_t baseSize, uint32_t layers, const std::vector<uint8_t> &atlas);

	struct SamplerAnisotropy { bool enabled; float maxAnisotropy; };
	// Anisotropy policy (issue #136): disabled when unsupported; otherwise enabled with
	// min(requestedMax, deviceMaxAnisotropy); maxAnisotropy at least 1.0f in all cases.
	SamplerAnisotropy resolveAnisotropy(bool deviceSupportsAnisotropy, float deviceMaxAnisotropy, float requestedMax = 8.0f);
} // namespace texture_mips
