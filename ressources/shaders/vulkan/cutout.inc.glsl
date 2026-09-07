// Canonical alpha-cutout threshold (issue #136): the mip generator
// (src/Renderer/TextureMips.hpp, texture_mips::kAlphaCutoutThreshold = 0.5)
// rescales per-mip alpha with sqrt(avg * max) so binary-cutout windows keep
// their alpha at/above this threshold, and the terrain + shadow passes discard
// below it, so cutout silhouettes (leaves, grass, flowers, kelp) stay stable
// across mip levels in color AND shadows.
// Must stay in sync with src/Renderer/TextureMips.hpp.
#ifndef FT_VOX_CUTOUT_INC
#define FT_VOX_CUTOUT_INC
const float kAlphaCutoutThreshold = 0.5;
#endif
