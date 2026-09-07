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
	//  - integer-window partition downsampling: destination texel i averages the
	//    source window [i*src/dst, (i+1)*src/dst) — every source texel contributes
	//    exactly once and NPOT edge rows/columns are never dropped (a partition of
	//    the source domain, not a continuous area filter);
	//  - linear light: each texel's RGB is sRGB-decoded (colorspace::srgbToLinear),
	//    box-averaged premultiplied by alpha, re-encoded (colorspace::linearToSrgb)
	//    — never average gamma-encoded values, and fully transparent texels
	//    cannot bleed their RGB into the result (no fringe/halo);
	//  - alpha coverage preservation (DirectXTex-style rescale + quantized tie
	//    resolution): alpha is averaged plainly, then each level's cutout coverage
	//    (fraction of texels with alpha >= kAlphaCutoutThreshold) is brought back
	//    to the base level's coverage by a per-level binary search over an alpha
	//    scale, then individual boundary texels are promoted/demoted until the
	//    covered texel COUNT equals round(targetCoverage * texelCount) — a uniform
	//    scale alone cannot break alpha ties (isolated same-alpha details would
	//    vanish). Ties spread by 4x4 Bayer rank. Fully transparent layers skip the
	//    rescale; fully opaque layers keep alpha exactly 255;
	//  - texels whose final alpha is 0 store RGB 0, then receive a 1-texel edge
	//    dilation (alpha bleeding) on EVERY level including mip 0: they take the
	//    color of their covered neighbors so GPU LINEAR filtering across a cutout
	//    edge blends toward the real border color instead of black (dark fringe),
	//    even during slight minification of the base level;
	//  - odd source sizes: windows are partition-mapped (see above), boundary
	//    texels are never read out of bounds.
	void generateLayerChain(uint32_t baseSize, const uint8_t *layerPixels, uint8_t *outChain);

	// Multi-layer upload buffer for the texture array, layer-major / mip-minor
	// (each layer's full chain is contiguous):
	//   offset(level, layer) = chainOffset(baseSize, level) + layer * chainBytes(baseSize)
	// `atlas` holds `layers` tightly-packed mip-0 layers (baseSize*baseSize*4 bytes each).
	std::vector<uint8_t> buildMipChainAtlas(uint32_t baseSize, uint32_t layers, const std::vector<uint8_t> &atlas);

	struct SamplerAnisotropy { bool enabled; float maxAnisotropy; };
	// Anisotropy policy (issue #136): disabled when unsupported; otherwise enabled with
	// min(requestedMax, deviceMaxAnisotropy); maxAnisotropy at least 1.0f in all cases.
	SamplerAnisotropy resolveAnisotropy(bool deviceSupportsAnisotropy, float deviceMaxAnisotropy, float requestedMax = 8.0f);
} // namespace texture_mips
