#pragma once
#include <cstdint>
#include <vector>

namespace texture_mips
{
	// Canonical alpha-cutout threshold (issue #136). Must stay in sync with
	// ressources/shaders/vulkan/cutout.inc.glsl (kAlphaCutoutThreshold).
	inline constexpr float kAlphaCutoutThreshold = 0.5f;

	// floor(log2(size)) + 1; size >= 1 (square convenience form).
	uint32_t mipLevelCount(uint32_t size);
	// Full chain for a WxH image: floor(log2(max(width, height))) + 1 — the
	// chain stops when BOTH dimensions have collapsed to 1 (64x32 -> 7 levels).
	uint32_t mipLevelCount(uint32_t width, uint32_t height);
	uint32_t mipDim(uint32_t baseSize, uint32_t level);		 // max(baseSize >> level, 1)
	uint32_t mipLevelBytes(uint32_t baseSize, uint32_t level);
	uint32_t mipLevelBytes(uint32_t baseWidth, uint32_t baseHeight, uint32_t level);
	uint32_t chainOffset(uint32_t baseSize, uint32_t level); // byte offset of `level` inside one layer's chain
	uint32_t chainOffset(uint32_t baseWidth, uint32_t baseHeight, uint32_t level);
	uint32_t chainBytes(uint32_t baseSize);					 // total byte size of one layer's chain
	uint32_t chainBytes(uint32_t baseWidth, uint32_t baseHeight);

	// Generate the full mip chain for ONE RGBA8 sRGB image (square or WxH).
	// layerPixels: width*height*4 bytes (mip 0). outChain: chainBytes(w,h) bytes.
	// Mip 0 preserves source alpha and RGB for alpha > 0; the RGB of alpha-0
	// texels may be edge-dilated (alpha bleeding) to avoid dark fringes.
	// Every following level is generated.
	//
	// Filtering policy (issue #136, shared by the terrain atlas and the mob
	// skins — issue #160):
	//  - integer-window partition downsampling per axis: destination texel i
	//    averages the source window [i*src/dst, (i+1)*src/dst) — every source
	//    texel contributes exactly once and NPOT edge rows/columns are never
	//    dropped (a partition of the source domain, not a continuous filter);
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
	void generateLayerChain(uint32_t baseWidth, uint32_t baseHeight, const uint8_t *layerPixels,
							uint8_t *outChain);

	// One used UV region of an atlas image (texel coordinates, half-open
	// [x, x + w) x [y, y + h)). Rectangles must not overlap; regions between
	// rectangles are "don't care" and are never allowed to bleed into them.
	struct MipRect
	{
		uint32_t x, y, w, h;
	};

	// UV-rect-aware variant for atlas images (issue #160 review P1): a mob skin
	// packs the independent faces of several boxes into one image, so the plain
	// whole-image filter above would blend neighboring faces into each other
	// wherever a downsampling window straddles an odd UV boundary, and the
	// whole-image alpha-coverage rescale could trade one face's silhouette away
	// for another's. This variant:
	//  - assigns every destination texel of every level to the rect containing
	//    its source window center and filters ONLY taps inside that rect, so no
	//    destination texel ever mixes two faces (a face's contribution that
	//    falls into a neighbor's destination texel is dropped, not blended);
	//  - runs the alpha-coverage binary search + quantized tie resolution PER
	//    RECT, against that rect's own base-level coverage;
	//  - fills "don't care" texels (outside every rect) with alpha 0 and a
	//    1-texel gutter of their covered owned neighbors' color on every level
	//    including mip 0, so GPU bilinear filtering at a rect border blends
	//    toward the rect's own edge color, never toward an unused region;
	//  - texels inside a rect keep the #136 guarantees (opaque stays 255,
	//    alpha-0 interior texels get same-rect border dilation).
	// With a single full-image rect (or rectCount == 0 it falls back to
	// generateLayerChain) this is equivalent to the whole-image path.
	void generateLayerChainRects(uint32_t baseWidth, uint32_t baseHeight, const uint8_t *layerPixels,
								 const MipRect *rects, size_t rectCount, uint8_t *outChain);

	// Multi-layer upload buffer for the square texture array, layer-major / mip-minor
	// (each layer's full chain is contiguous):
	//   offset(level, layer) = chainOffset(baseSize, level) + layer * chainBytes(baseSize)
	// `atlas` holds `layers` tightly-packed mip-0 layers (baseSize*baseSize*4 bytes each).
	std::vector<uint8_t> buildMipChainAtlas(uint32_t baseSize, uint32_t layers, const std::vector<uint8_t> &atlas);

	struct SamplerAnisotropy { bool enabled; float maxAnisotropy; };
	// Anisotropy policy (issue #136): disabled when unsupported; otherwise enabled with
	// min(requestedMax, deviceMaxAnisotropy); maxAnisotropy at least 1.0f in all cases.
	SamplerAnisotropy resolveAnisotropy(bool deviceSupportsAnisotropy, float deviceMaxAnisotropy, float requestedMax = 8.0f);
} // namespace texture_mips
