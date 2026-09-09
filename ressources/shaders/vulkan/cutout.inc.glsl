// Canonical alpha-cutout threshold (issue #136): the mip generator
// (src/Renderer/TextureMips.hpp, texture_mips::kAlphaCutoutThreshold = 0.5)
// rescales per-mip alpha (DirectXTex-style coverage preservation) so
// binary-cutout windows keep their alpha at/above this threshold, and the
// terrain, shadow AND mob passes (mob.frag.glsl / mob_shadow.frag.glsl)
// discard below it, so cutout silhouettes stay stable
// across mip levels in color AND shadows.
// Must stay in sync with src/Renderer/TextureMips.hpp.
#ifndef FT_VOX_CUTOUT_INC
#define FT_VOX_CUTOUT_INC
const float kAlphaCutoutThreshold = 0.5;
#endif
