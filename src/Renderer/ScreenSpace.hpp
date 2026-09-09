#pragma once

/// CPU-side screen-space conventions shared by the renderer and tooling
/// (issue #158). Any projection that targets a scene-derived image (god-ray
/// sun position, future screen-space effects) must go through this helper so
/// the viewport convention lives in exactly one place.

#include <glm/glm.hpp>

namespace screenspace
{

/// NDC [-1, +1] (GLM_FORCE_DEPTH_ZERO_TO_ONE projection, after the /w divide)
/// to top-down framebuffer / post-process UV [0, 1].
///
/// The production scene passes (OpaquePass / WaterPass / SkyPass) rasterize
/// with a NEGATIVE-height Vulkan viewport:
///
///     VkViewport{0, extent.height, extent.width, -extent.height, 0, 1}
///
/// so NDC y = +1 lands on framebuffer row 0 (top). Fullscreen post passes
/// run a positive-height viewport and fullscreen.vert forwards top-down
/// [0, 1] UVs (v = 0 at the top) unchanged. The matching post UV therefore
/// needs the vertical mirror (u = 0.5·ndc.x + 0.5, v = 0.5 − 0.5·ndc.y);
/// the positive-viewport form `ndc.y * 0.5 + 0.5` vertically mirrors the
/// result (it mapped the god-ray scattering center to the mirrored sun —
/// issue #158). The shaders handle the same convention locally:
/// water.frag.glsl (`ndc.xy * vec2(0.5, -0.5) + 0.5`) and composite.frag
/// (`1 - 2·uv.y` depth reconstruction).
inline glm::vec2 ndcToFramebufferUv(const glm::vec2 &ndc)
{
	return {ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f};
}

} // namespace screenspace
