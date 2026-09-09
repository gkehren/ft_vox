# Vulkan graphics architecture

Authoritative description of **how ft_vox draws frames today**: device setup, frame sync, the pass graph, shaders, lighting, materials, and post-processing.

For engine loop, streaming, and world generation, see [`engine-architecture.md`](engine-architecture.md).

---

## 1. Stack overview

| Layer | Role | Primary locations |
|-------|------|-------------------|
| Window / surface | SDL3 `SDL_WINDOW_VULKAN` | `Engine` |
| Loader | **volk** (`VK_NO_PROTOTYPES`) | `Vulkan/VkContext`, `Vulkan/VkLoadLibrary.hpp` |
| Device | Instance, surface, physical/logical device, queues, features | `Vulkan/VkContext` |
| Memory | **VMA** | `Vulkan/VkAllocator` |
| Swapchain / WSI | Formats, present modes, resize | `Vulkan/VkSwapchain` |
| Frame sync | Acquire → record → submit → present (2 FIF) | `Vulkan/VkFrame` → `VkFrameContext` |
| Staging / retire | Async mesh upload, deferred GPU free | `Vulkan/StagingRing`, `Vulkan/GpuResourceRetire` |
| Barriers / pipelines | Shared helpers | `Vulkan/ImageBarrier.hpp` (`vkbar::`), `Vulkan/GraphicsPipelineBuilder.hpp` |
| Frame graph | Pass orchestration | `Renderer/WorldRenderer` |
| Passes | Shadow → opaque (+ mobs, overlays) → water → sky → post | `Renderer/*Pass*`, `PostStack`, `OverlayRenderer` |
| Shaders | GLSL → SPIR-V offline | `ressources/shaders/vulkan/` |

**API target:** Vulkan **1.2+**, with required dynamic rendering provided by
`VK_KHR_dynamic_rendering` on Vulkan 1.2 or by the core API on Vulkan 1.3+.
The selected path is based on the application API target as well as the device
version, and its entry points are validated immediately after loading the
device. On Apple, **MoltenVK** is selected via ICD (`VK_ICD_FILENAMES`). Depth
is **zero-to-one** (`GLM_FORCE_DEPTH_ZERO_TO_ONE`); viewport Y may be flipped
for OpenGL-style world Y without winding flip.

**Viewport / UV convention (issue #158):** the scene passes (`OpaquePass` /
`WaterPass` / `SkyPass`) rasterize with a **negative-height** viewport
(`{0, height, width, -height, 0, 1}`), so NDC `y = +1` lands on framebuffer
row 0. Fullscreen post passes run a positive-height viewport with top-down
`[0, 1]` UVs that `fullscreen.vert` forwards unchanged. CPU-side projections
that target scene-derived images — the god-ray sun position (`sunScreen`) —
must therefore convert NDC with the vertical mirror through the shared helper
`screenspace::ndcToFramebufferUv` (`Renderer/ScreenSpace.hpp`):
`uv = (0.5·ndc.x + 0.5, 0.5 − 0.5·ndc.y)`. The shaders apply the same
convention locally (water refraction `ndc.xy · vec2(0.5, −0.5) + 0.5`;
composite depth reconstruction `1 − 2·uv.y`). The positive-viewport form
`ndc.y·0.5 + 0.5` vertically mirrors the result — it used to aim the god-ray
scattering center at the mirrored sun. The convention is pinned by
`test_render_helpers` (conversion contract) and the visual-regression
`godray_alignment` check (ray energy must converge on the projected sun, not
its mirror).

**Rendering model:** Forward-style world passes into **HDR + depth** (and god-ray source), then fullscreen post to the swapchain. No deferred G-buffer.

---

## 2. Device path (`VkContext`)

`VkContext` (`src/Vulkan/VkContext.hpp`) owns:

1. **Instance** — app info, SDL-required extensions, optional validation (`FT_VOX_VALIDATION` / Debug default).
2. **Surface** — from the SDL window.
3. **Physical device** — graphics + present queues, swapchain support.
4. **Logical device** — queues; feature flags include **dynamic rendering**, timeline semaphores when available, portability subset on Apple.
5. **VMA allocator** — used for buffers/images across the engine.

Loader discovery is centralized in `Vulkan/VkLoadLibrary.hpp` (`loadVulkanLibrary()`): `FT_VOX_VULKAN_LIB` → Homebrew paths → default. Used by the game and `tests` smoke path so macOS does not depend only on `SDL_Vulkan_LoadLibrary(nullptr)`.

Supporting types:

| Type | File | Role |
|------|------|------|
| `VkSwapchain` | `VkSwapchain.hpp` | Images, views, recreate on resize |
| `VkFrameContext` | `VkFrame.hpp` | Per-frame pool/cmd/semaphores/fence; **only** acquire/submit/present owner on the game path |
| `ImmediateCommands` | `VkCommands.hpp` | One-shot uploads (bootstrap, texture init) |
| `AllocatedBuffer` / `AllocatedImage` | `VkBuffer` / `VkImage` | VMA-backed resources |
| `VkShader` | `VkShader.hpp` | Load SPIR-V → `VkShaderModule` |
| `StagingRing` | `StagingRing.hpp` | Ring-buffered host→device copies for mesh upload |
| `GpuResourceRetire` | `GpuResourceRetire.hpp` | Delay destroy until frames-in-flight have finished |

`WorldRenderer` does **not** own WSI sync. The `Engine` calls `VkFrameContext::beginFrame` / `submitAndPresent` and passes a reset command buffer into `WorldRenderer::recordFrame`.

Resize, VSync, and out-of-date requests are deferred to the next pre-acquire
frame boundary. `Engine` recreates the swapchain there, resets
`VkFrameContext`'s per-image associations, refreshes `WorldRenderer` targets,
and notifies ImGui. No UI callback destroys WSI resources for an image already
acquired by the current frame.

### Present mode and VSync contract

`VkSwapchain` applies a strict two-mode policy:

- VSync on selects `VK_PRESENT_MODE_FIFO_KHR`.
- VSync off selects `VK_PRESENT_MODE_IMMEDIATE_KHR`. There is no application
  sleep, frame limiter, `MAILBOX`, `FIFO_RELAXED`, or `FIFO` fallback.

If a surface does not expose `IMMEDIATE`, disabling VSync is rejected explicitly
instead of silently retaining refresh-paced presentation. F10 and the HUD
checkbox recreate the swapchain immediately and display the active Vulkan mode.
The same path is reproducible from the CLI with `--vsync on|off`; combining it
with `--benchmark <seconds>` measures either mode without changing the requested
setting.

---

## 3. Frame graph

### 3.1 Ownership

```text
Engine
  ├── VkContext, VkSwapchain
  ├── VkFrameContext          ← acquire / submit / present
  ├── StagingRing, GpuResourceRetire
  └── WorldRenderer           ← record only
        ├── TextureManager
        ├── MobRenderer        ← passive mobs (opaque + shadow cascades)
        ├── ShadowPass
        ├── OpaquePass  (+ MobRenderer then OverlayRenderer at end of opaque render)
        ├── WaterPass
        ├── SkyPass
        └── PostStack
```

### 3.2 Pass order (one frame)

Recorded in `WorldRenderer::recordFrame` (see `WorldRenderer.cpp`):

| Step | Owner | Writes | Notes |
|------|--------|--------|--------|
| 0 | **preRecord** callback | mesh GPU buffers | `Engine` records `uploadPendingMeshes` + transfer→vertex barrier here, before draws |
| 1 | **ShadowPass** | Cascaded depth array | Directional sun; leaf wind in shadow VS; per cascade, **`MobRenderer::record`** alpha-cuts mobs into the same depth attachment (`GpuPass::MobShadow0-2`) |
| 2 | **OpaquePass** | HDR color + scene depth | Solid chunks (per-section `Chunk::collectOpaqueDraws` commands + indirect draws), then **`MobRenderer::record`** for passive mobs, then **`OverlayRenderer::record`** for highlight / borders / demo players — all inside the same dynamic-rendering scope |
| 3 | **WaterPass** | HDR (opaque write, no blending) | History color/depth for refraction; set2 scene samples |
| 4 | **SkyPass** | HDR + god-ray source MRT, depth test | Procedural sky, sun/moon/stars/clouds |
| 5 | **PostStack** | Swapchain | Exposure metering (auto only) → SSAO (half-res) → AO bilateral upsample → bloom → god rays → composite → spatial AA (FXAA 3.11, only when enabled) |
| 6 | **imguiDraw** callback | Swapchain (load) | ImGui after composite; not a world pass |

**Overlays are not a separate post-sky pass.** They run at the end of **OpaquePass** while HDR/depth are still the color/depth attachments (`OpaquePass.cpp`).

### 3.3 Descriptor sets (world geometry)

Typical world layout:

| Set | Contents |
|-----|----------|
| **set0** | Frame UBO + **MaterialTable** UBO |
| **set1** | Texture array + shadow map array + sampler(s) |
| **set2** (water) | Scene history color + depth for refraction |

Push constants carry cascade index / shadow time for the shadow path where needed.

---

## 4. Pass modules

### ShadowPass (`Renderer/ShadowPass.*`)

- **Cascaded shadow maps (CSM):** `shadow::kCascadeCount` (**3**), practical splits (`ShadowCascades.hpp`).
- Depth array image + one **comparison sampler** (`LESS_OR_EQUAL`, border = lit); light matrices computed in `WorldRenderer::updateFrameUBO`.
- **World-stable texel grid** (issue #137): the light basis is a pure rotation, each cascade's ortho extent is a slice **bounding-sphere radius** (rotation-invariant), and the view anchor is the sphere center snapped to the texel grid **in the absolute light frame** — the grid only moves in whole-texel steps under camera translation/rotation (asserted by `RenderHelpers`).
- Receivers get `FrameUBO::cascadeBiasScales` (normalized depth per world texel = `worldUnitsPerTexel/depthSpan`, multiplied by dimensionless `shadow::kReceiverBiasSlope/Base` in `csm.inc.glsl`) and `cascadeTexelWorldSizes` (plain world-units-per-texel, debug density view).
- **Receiver sampling is shared** by terrain and mobs through `ressources/shaders/vulkan/csm.inc.glsl` (`sampleDirectionalShadow`): footprint-scaled receiver bias + raster depth bias documented together, 12-tap Poisson rotated by a **world-stable absolute light-grid texel hash** (local shadow-map texel + the cascade's absolute grid origin `FrameUBO::cascadeGridOffsets01/2` — invariant across cascade recentering; no fixed banding), radius derived from `textureSize` (no hardcoded map size), cascade blend band preserved.
- **Quality tier** (issue #137): `PostProcessSettings::shadowMapSize` (1024 Low/Medium, 2048 High/Cinematic presets) recreates the shadow array deferred (`WorldRenderer::applyShadowMapSize` → device idle, rebuild, rewrite receiver descriptors incl. mob sets); CLI override `--shadow-size N`. Debug visualization modes (cascade index / blend bands / texel density / receiver depth) via `ShaderParameters::shadowDebug` → `FrameUBO::visualParams.w`, read by `terrain.frag.glsl`.
- Pipeline: `shadow.vert` / `shadow.frag` (depth-only style).
- Caster list comes from `ChunkManager::collectShadowList` (XZ radius = `RenderSettings::shadowDistance`).

### OpaquePass (`Renderer/OpaquePass.*`)

- Dynamic rendering into **HDR** (`R16G16B16A16_SFLOAT`) and **D32** depth owned by `PostStack`.
- Submits opaque chunk geometry through **`vkCmdDrawIndexedIndirect`** from the shared mesh arenas (issue #109): each live section emits exactly one command - section indices are stored section-local and the command's `vertexOffset` rebases them, so commands are never merged. Commands are grouped by arena page pair at submission time, so the arenas are bound once per pair per frame instead of two binds per chunk.
- Then calls **`OverlayRenderer::record`** on the same command buffer before ending the rendering scope (block highlight, chunk borders, demo players).
- Shaders: `terrain.vert` / `terrain.frag` — diffuse, face bias, sky/block light, CSM + PCF (sun **and** moon), cave fill, scotopic night grade, shared aerial perspective (`atmosphere_fog.inc.glsl`), material wind/emissive/ice.

### WaterPass (`Renderer/WaterPass.*`)

- Copies previous opaque HDR/depth into **history** images for refraction, and ends with an explicit **Water → Sky barrier** (depth write → depth read, color write → color read|write): separate dynamic-rendering instances have no implicit ordering, and the sky pass re-uses the same depth attachment with `LESS_OR_EQUAL`.
- Water mesh (`Chunk::collectWaterDraws`; back-to-front order preserved, one command per live section, contiguous same-page-pair runs drawn per bind). **Single composition, no blending:** the fragment outputs the fully composited surface color (refraction + absorption + reflection + foam) with alpha = 1, so blending is disabled and **depth writing is enabled** — the nearest visible surface wins independent of draw order, the sky pass (LESS_OR_EQUAL) no longer overwrites far water, and SSAO / god-ray occlusion / the camera-underwater composite see the surface instead of the geometry behind it.
- Shaders: `water.vert` / `water.frag`: **geometrically flat water** — the vertex stage applies no displacement and no normal spread (greedy rectangles only share geometry along their edges, so vertex animation broke across rectangle boundaries). All motion is fragment-level and world-anchored: two smooth directional sines with analytic derivatives plus the three-octave value-noise detail field (0.30/0.85/2.10), evaluated from the interpolated world position with fixed world-unit derivative epsilons so adjacent rectangles produce identical surface state at the same location. Octaves fade when their feature size approaches the **pixel footprint derived from the projection and resolution** (pixel size at this depth ÷ ray incidence — not `dFdx`, which is constant per primitive and would quantize the normal per greedy triangle), and the far field flattens toward the geometric normal — horizon calm instead of more SSR steps. At wave strength 0 the normal is exactly geometric.
- **Refraction reconstructs camera-space positions** (same `RH_ZO` linearization as the composite; `ndc.y = 1 - 2·uv.y` because the production viewport has negative height — the un-mirrored form mirrors the world and corrupted any world-height consumer). The distortion offset is the **view-space** wave normal, and the optical path is the 3D distance between the reconstructed surface point and the reconstructed floor of the *accepted* background sample (sky history depth maps to the shared `WATER_SKY_COLUMN` cap) — not a screen-axis depth difference.
- Fresnel (F0 = 0.02) keys on a normal flipped toward the observer — unconditionally on the underside, where a per-pixel flip test would shatter the surface into hard Fresnel regions at grazing angles. Sun and moon each use **one roughness-controlled GGX lobe** (`waterSurfaceParams.x`, default 0.12, UI bounds [0.04, 0.35]) with distance-widened roughness, replacing the former pair of fixed-exponent pow lobes; the half-vector is guarded against `V ≈ −L` and the GGX alpha is bounded instead of flooring the denominator (which made the peak non-monotone at low roughness).
- Foam is a thin shoreline band driven by the reconstructed **vertical surface→floor separation** (world Y), lightly modulated by the wave height and **gated on top faces** (a vertical face next to similar-height terrain reconstructs ≈ 0 separation and must not foam); none on the underside. No open-water whitecaps. Foam is lit by the same local-light/sky-reach terms as the water body (no autonomous white in caves or at night).
- **Underside (submerged camera):** detected per fragment (`dot(geoN, V) < 0`); refraction distortion and SSR are disabled and the transmitted boundary background is the **analytic sky** for sky-classified history pixels — the sky renders after this pass and, with depth writing on, could never fill those pixels — with **no in-water attenuation**: the camera→surface medium is the underwater composite's job, so the medium is never counted twice and the boundary does not turn fake teal.
- `sky_radiance.inc.glsl` supplies the normalized day/sunset/night gradient and directional horizon glow to both the sky and water. Clouds/stars/discs are not part of this inexpensive fallback. Downward reflection directions are not mirrored upward.
- High/Cinematic trace current-frame opaque color/depth (no temporal history or extra world render): 24/48 quadratic-distance steps over 40/64 world units, five bisections per candidate crossing and a 0.35 view-space-unit thickness. Hits additionally reject sky depths and silhouette-crossing bilinear neighborhoods; confidence fades at screen edges, grazing horizon directions, long rays and uncertain hits. On SSR pixels a **roughness-scaled 4-tap neighborhood blur** averages depth-validated neighbors only (invalid taps do not join the average), so failures fade to the sky fallback without black bands. Vertical faces use the sky fallback. Missing/offscreen geometry cannot be reflected.
- Top-face classification (SSR eligibility, wave-normal masking) and the CSM receiver normal use the flat geometric face normal passed from the vertex stage — never the wave-perturbed shading normal, which oscillates with wave strength and phase and would spatially/temporally toggle SSR and shadow reception on true horizontal faces.
- Refraction uses unfiltered reconstructed depth and validates all four texels in the bilinear color footprint. It rejects foreground samples and depth jumps, backs off distortion up to four times, and falls back to the original UV. Distortion fades at shores and screen edges.
- Medium and above receive the shared CSM policy on top faces. Visibility modulates direct scatter and sun/moon glitter; the refracted opaque color and ambient scatter remain independent of that multiplier.

#### Shared aerial perspective (`atmosphere_fog.inc.glsl`, issue #159)

- **One air-medium contract** for every world-space surface: terrain, water and dynamic entities all evaluate `evaluateAtmosphereFog` + `applyAtmosphereFog` from the shared include. At an equal world position the fog amount, haze color and aerial composition are identical regardless of the calling shader — far water, far shoreline and a far mob converge toward the same horizon atmosphere.
- **Model** (previously duplicated by `terrain.frag`/`mob.frag`, approximated by `water.frag`): linear `smoothstep` fog over `fogParams.xy`, exponential density fog with height falloff toward `lightingParams.z` (`fogBaseY`), capped at `kAtmosphereFogAmountCap` (0.45, `Lighting.hpp`), gated by the local skylight enclosure term `smoothstep(0.05, 0.45, skyLight)` (sealed caves get no outdoor haze), and a day/sunset/night aerial haze color blended with `frame.fogColor`. Composition desaturates the lit surface toward its own luminance and lifts it toward the haze while retaining bounded surface chroma.
- **Medium responsibilities stay separate**: the Beer–Lambert water column (`water_optics.inc.glsl`) is the *water* medium and is untouched by the air term; the camera-underwater composite (issue #144) remains the sole authority for a submerged camera. The contract **self-gates to zero when the camera is submerged**: `evaluateAtmosphereFog` weights the amount by `1 - frame.lightingParams.w` (the camera-underwater flag packed by `WorldRenderer` from the Engine's voxel-medium sample at the eye), so every caller — terrain, water, entities, and any future surface shader — loses the outdoor air term underwater without needing to remember the rule. The `underside` early-out in `water.frag` is then a pure optimization. `lightingParams.w` is currently boolean; promoting it to a 0→1 submersion blend consistent with the composite's underwater strength would smooth the surface crossing (follow-up).
- **C++ mirrors** (`Lighting.hpp`: `atmosphereFogAmount`, `atmosphereHazeColor`, `applyAerialPerspective`, `ungatedAtmosphereFogAmount`, constants `kAtmosphereFog*`/`kAtmosphereDesatMin`) are exact numeric mirrors of the GLSL on the shader's validated input domain (no extra clamps); `test_render_helpers` asserts monotonicity, height-falloff direction, skylight gating, the submerged-camera zero-gate, the haze palette + composition blend constants, and that all three fragment shaders consume the shared contract with no private fog model (drift guard).
- Low/Medium use sky-only reflection; all tiers use depth-safe refraction. Quality is carried in the std140 `waterQuality` vector (steps, range, thickness, shadow enable); the surface model in `waterSurfaceParams` (roughness, debug view: 0 off / 1 wave normal / 2 optical path / 3 Fresnel / 4 SSR confidence — also exposed in the Graphics > Water panel and via `FT_VOX_WATER_DEBUG` in the audit). No additional images or vertex-format changes. See [issue #139 measurements](benchmarks/issue139-water/README.md).

### SkyPass (`Renderer/SkyPass.*`)

- Standalone sky (not part of post).
- Dual color attachments: HDR scene + **god-ray source**.
- Shaders: `skybox.vert` / `skybox.frag`.

### PostStack (`Renderer/PostStack.*`)

Fullscreen chain on a unit quad (`fullscreen.vert`):

| Stage | Shader | Default behavior when disabled |
|-------|--------|--------------------------------|
| SSAO (half-res RGBA8) | `ssao.frag` | Skip; upsample + composite sample **1×1 white** AO |
| AO bilateral upsample (full-res R8) | `ssaoUpsample.frag` | Skip; composite samples **1×1 white** AO |
| Exposure metering (auto, issue #140) | `luminance_downsample.frag`, `exposure_adapt.frag` | Skip all metering passes; composite uses the exact manual exposure |
| Bloom extract + blur | `bloomExtract.frag`, `bloomBlur.frag` | Skip; composite samples **1×1 black** |
| God rays | `godRays.frag` | Skip unless `lighting::godRaysPassActive`; black default |
| Composite | `composite.frag` | Tonemap, grade, grain, vignette; camera-underwater medium transport skipped. Writes the swapchain when spatial AA is off, or the full-res sRGB-encoded LDR AA-source target (`R8G8B8A8_UNORM`) when it is on |
| Spatial AA (issue #143) | `fxaa.frag` | Skip; composite renders straight to the swapchain (no LDR intermediate) |

True **1×1 defaults** live on `PostStack` (`m_defaultBlack`, `m_defaultWhiteR8`). Selection is pure helper logic in `PostDefaults.hpp` (`postCompositeSources`) so composite never samples half-res targets that were not written this frame.

#### God rays — sun projection convention (issue #158)

`WorldRenderer::recordSceneAndPost` projects the sun (`camPos + sunDir·500`)
through the frame view/projection and converts the NDC result with
`screenspace::ndcToFramebufferUv` (see **Viewport / UV convention** above)
before passing it as `godRays.frag`'s `sunScreenPos` push constant — the
fullscreen UV domain the radial march (`vUV - sunScreenPos`) samples in.
`sunClip.w <= 0` (sun behind the camera) or a `sunScreen` outside `[0, 1]`
forces `sunVisibility = 0`, which both gates the pass
(`lighting::godRaysPassActive`) and scales its output. The visual-regression
`godray_alignment` check pins the scattering center on the projected sun
(off-center sunset, depth occlusion on/off, plus the off-frame gating
contract).

#### SSAO — GTAO-style horizon AO (issue #138)

Normal-aware horizon-based ambient occlusion, replacing the old depth-difference estimator (whose compensating high AO floor and damping are gone):

```text
full-res depth (nearest)
  -> ssao.frag (half-res): raw AO + encoded view-space normal
     target: R8G8B8A8_UNORM (r = AO, gb = normal.xy * 0.5 + 0.5, a = normal.z * 0.5 + 0.5)
  -> ssaoUpsample.frag (full-res): joint bilateral upsample (texel-exact texelFetch on the
     four true half-res texels; depth-nearest fallback)
     target: R8_UNORM (upsampled/denoised AO)
  -> composite.frag: b3 = final AO, b4 = raw half-res RGBA (debug views)
```

- **Estimator:** per pixel, view position and a camera-facing normal are reconstructed from depth (edge-aware central differences, orientation fixed by `dot(N, normalize(P))` so it is UV/y-flip independent). Each slice builds an exact frame: `zeta = cross(dq, V)` is the **slice-plane normal** (never an elevation axis — every sample vector lies in the plane, so `dot(v, zeta) == 0`), the in-plane axes are `xi = cross(V, zeta)` (horizontal) and `eta = -V` (elevation, toward the camera). `steps` samples are marched per slice with mid-bin placement (the outermost sample never sits on the radius boundary where falloff reaches zero). For each side (+xi / −xi, each parameterized on its own axis so angles stay in [−π/2, π/2]), `N` is projected into the slice plane with magnitude `m = length(vec2(dot(N, xi), dot(N, eta)))` — slices with `m ≈ 0` (normal parallel to the slice normal) carry no Lambert weight and are skipped. For a sample horizon `h` above the tangent bound `thetaN − pi/2`, the exact Lambert-weighted occluded arc is

    m · (1 + sin(h − thetaN))

The distance falloff is applied per sample before the side-local maximum, so farther samples cannot reduce the existing occlusion (monotone horizon). Any non-sky forward sample occludes (the signed horizon vs the tangent plane decides; "object on the ground in front of P" and "crease beside P" both register); self-occlusion is suppressed by the tangent-plane clamp. Slice rotation uses per-pixel interleaved-gradient-noise jitter — **deterministic, no time term** — so the visual-regression harness stays reproducible.
- **View-space semantics** (meters):
  - `ssaoRadius` — occluder search radius around the pixel's view-space position at the pixel's view depth, **isotropic**: the UV step per slice direction is derived from both per-axis FOV scales (`invProj[0][0]` horizontal, `invProj[1][1]` vertical).
- **Output/application:** the half-res shader writes raw AO only — no internal floor, no damping, no intensity. Composite applies `mix(1, ao, clamp(ssaoIntensity, 0, 0.85))` (matching `lighting::clampSsaoIntensity`) and then the safety-only floor `max(ao, 0.10)` (= `lighting::kSsaoAoFloor`; must match `composite.frag`).
- **Presets** (`PostProcessSettings::applyPreset`; cost scales with directions × steps):

  | Preset | SSAO | Radius (m) | Intensity | Directions × Steps |
  |--------|------|-----------|-----------|--------------------|
  | Low | off | 0.4 | 0.25 | 4 × 2 (skipped) |
  | Medium (defaults) | on | 0.6 | 0.40 | 4 × 3 |
  | High | on | 0.8 | 0.55 | 6 × 4 |
  | Cinematic | on | 1.0 | 0.62 | 8 × 4 |

  GPU cost per enabled tier (RTX 4070 Ti, 1920×1080, `--seed 42 --quality <tier> --benchmark 30 --vsync off`, clean tree at the final commit; the `GpuPass::Ssao` interval covers SSAO + upsample; High/Cinematic include the #148 2048 shadow maps and #149's anisotropic atlas): **Medium 4×3 = 0.058 ms**, **High 6×4 = 0.088 ms**, **Cinematic 8×4 = 0.098 ms** (Post chain 0.176 / 0.229 / 0.249 ms respectively). Reports (each carries its `Quality:` label): `docs/benchmarks/bench_20260907_200{150,223,257}_*`. Temporal stability: the harness renders `noon_terrain` at two poses 0.125 units apart under SSAO on / SSAO off / isolated final-AO buffer; the isolated AO buffer must satisfy mean ≤ 20/255, p99 ≤ 40/255 and ≤ 1 % of pixels above 20/255 — measured **mean 1.300, p99 11.0, frac>20 0.03 %** — and the composited AO-on delta must stay within the SSAO-off parallax baseline (measured ratio 0.998).
- **Debug views:** Graphics panel → Post-processing → **SSAO debug view** (Off / AO (final) / AO (raw) / Normals (view)); passed to composite as push constant `p4.w`. Debug output bypasses tonemap/grade but still applies the swapchain output-transfer contract (`linearToSrgb` on UNORM + SRGB_NONLINEAR). The selector resets to Off whenever SSAO is disabled; presets also reset it. The encoded normal is `gb = xy`, `a = z` (the z sign is stored, not reconstructed).
HDR RGBA16F -> 64x64 -> 16x16 -> 4x4 R32F log-luminance   luminance_downsample.frag.glsl
4x4 -> 1x1 R32F (debug) + 16-byte state SSBO write         exposure_adapt.frag.glsl
- **Metering — clipped log-average:** each downsample stage averages 16 spread taps of log2 luminance, every sample clipped to [−8, +8] EV so sun-disc/emissive pixels cannot dominate the mean. The HDR stage samples through the linear sampler (RGBA16F is always filterable); the smaller stages use nearest. `exposure_adapt` averages the 4×4 into the meter reading and derives `targetEv = clamp(log2(middleGrey) − meteredLogLum + compensationEv, minEv, maxEv)`, `exposure = 2^targetEv`.
- **State & sync:** `exposure_adapt` writes a 16-byte `autoexposure::ExposureGpuState` `{adaptedExposure, targetExposure, meteredLogLum, clampState}` into a host-visible persistent-mapped SSBO, **one per frame-in-flight**. A buffer barrier (fragment-shader write → read) orders the adapt write before the composite read within the frame; cross-slot hazards are covered by the existing per-slot fence wait in `VkFrameContext::beginFrame`. The CPU debug readout copies the current slot's buffer at the top of `recordExposure` — after that slot's fence was already waited — so there is no readback and no new synchronization on the hot path. This is the engine's only fragment-stage SSBO write, which is why `fragmentStoresAndAtomics` is now a queried + **hard-required** device feature (`VkContext`).

#### Auto exposure — HDR luminance metering + temporal adaptation (issue #140)

Graphics-only reduction (no compute, no readback on the hot path), recorded at the top of `PostStack::recordPost` when auto exposure is enabled — it meters the **raw scene HDR before tone mapping and before bloom/god-rays are added**:

```text
HDR RGBA16F (SHADER_READ)
  -> luminance_downsample.frag: HDR -> 64x64 -> 16x16 -> 4x4, R32F log2 luminance
     (16 spread NEAREST taps per /4 stage; only stage 0 converts luminance to
     log2, later stages average the already-converted .r channel; every sample
     clipped to [-8, +8] EV so sun/emissive outliers cannot dominate the mean)
  -> exposure_adapt.frag (4x4 -> 1x1 R32F): exact 16-texelFetch meter average,
     target = clamp(log2(middleGrey) - meteredLogLum + compensationEv, minEv, maxEv)
     temporal: alpha = 1 - exp(-speed*dt), asymmetric speeds (up 3/s while the
     scene brightens, down 1.25/s while it darkens), dt <= 0 is a strict no-op,
     exact settle only when |target - adapted| < 1e-4 (arrival, never step size)
```

- **State model:** ONE shared 16-byte history SSBO (host-visible, persistent across resize — the metering chain has fixed resolution) holds the temporal state, plus per-frame-in-flight CPU-visible snapshots written by the same pass. The history buffer is bound as composite `set 1, binding 6` and read by `resolveExposure()` when push constant `p5.x` (useAutoExposure) is set; the manual path uses the exact `exposure` push value. Cross-frame ordering relies on the same-queue in-order execution guarantee plus a `SHADER_WRITE -> SHADER_READ` buffer barrier before the adapt pass; the same-frame composite read is ordered by a barrier after it. The CPU debug readout copies the current slot's snapshot after that slot's fence was waited (no added synchronization).
- **Mode transitions:** manual mode skips all metering passes (deterministic output, suspended adaptation). Re-enabling auto re-seeds the history from the current manual exposure (rising-edge detection; also forced on first use), so there is no hidden jump. A swapchain resize keeps the adaptation running seamlessly.
- **Settings** (all reset by `applyPreset`; CPU mirror + unit tests in `Renderer/AutoExposure.hpp` — the GLSL formulas must stay in sync):

  | Setting | Default | Semantics |
  |--------|------|-----------|
  | `autoExposureEnabled` | on | Meters HDR luminance and adapts exposure over time |
  | `exposure` | 1.25 | Exact manual exposure (auto off) |
  | `exposureCompensation` | 0.0 | EV stops on the auto path (0 neutral, +1 = 2x brighter target) |
  | `autoExposureMiddleGrey` | 0.18 | Target mean linear luminance after exposure, before tone mapping |
  | `autoExposureMinEv / MaxEv` | -4 / +1 | Clamp on the target exposure (`exposure = 2^ev`) |
  | `autoExposureSpeedUp / SpeedDown` | 3.0 / 1.25 | Inverse seconds, frame-rate independent |

  The 0.18 key replaces the former 1.0 key, which drove average scene
  luminance into the ACES shoulder and washed out sky/snow. The +1 EV gain
  ceiling (2x, formerly 16x) preserves night/cave darkness instead of lifting
  ambient-only surfaces to daylight. These defaults apply to every quality
  preset; manual exposure and temporal integration are unchanged.

- **Device support:** requires `fragmentStoresAndAtomics` (queried in `VkContext`); when absent the engine stays on the manual path instead of failing.
- **Diagnostics:** Graphics panel controls (auto toggle, compensation, middle grey, EV limits, speeds), profiler readouts (metered EV, current/target exposure, clamp state) fed by an on-demand snapshot copy (`refreshExposureReadout`, ~10 Hz while the profiler panel is visible — the only GPU->CPU traffic of the feature, zero when the panel is closed), and a dedicated `GpuPass::Exposure` timestamp row (nested inside the Post pass timing). Measured cost (RTX 4070 Ti, seed 42 benchmark, Release, base 12acedd vs head f04e564): Post pass 0.317 -> 0.383 ms — delta about +0.07 ms (0.06-0.09 ms across runs; the Post bracket includes the Exposure sub-pass), Exposure sub-pass alone reads ~0.12 ms, score unchanged. Reports: docs/benchmarks/bench_20260907_224059_12acedd27ecb (base) and bench_20260907_232825_f04e564a0975 (head). The `*` in the head report's Revision line (dirty tree at build) flags the untracked benchmark artifact itself, not source drift — the compiled sources were exactly f04e564.


#### Camera-underwater medium transport (issue #144)

When `PostProcessSettings::underwater` is set, `composite.frag` applies underwater light transport **in linear HDR, before exposure/tonemap** — replacing the old flat teal tint + screen-space UV shimmer:

- **Bindings:** composite binds the frame uniform set (**set 0** — `FrameUBO`: view/projection matrices, camera position, sun/moon directions, day/sunset/night factors) plus a **set 1** holding its five texture samplers, **binding 5 = the live scene depth buffer** (D32, full resolution, already in `SHADER_READ_ONLY_OPTIMAL` during post) and **binding 6 = the auto-exposure history SSBO from #140** (the same single shared buffer the adapt pass writes — not a per-frame copy).
- **Depth reconstruction:** per pixel, view distance is linearized from scene depth with the GLM `RH_ZO` projection terms (exactly like the water pass) and world position is rebuilt along the view ray (camera position + unprojected NDC ray rotated to world by the view basis). The NDC y reconstruction is **mirrored** (`1 - 2·uv.y`): the production viewport has negative height, so `gl_FragCoord.y = 0` is `ndc.y = +1` — the earlier un-mirrored form reconstructed a vertically mirrored world that stayed invisible while the transport only consumed path lengths (issue #135). Sky/far-plane depth caps the reconstructed point at ~28 m, and the **optical path** is separated from the scene distance: an upward view ray ends at the local surface plane (`p6.x`), so extinction follows the true in-water path length.
- **Extinction + in-scatter (Beer–Lambert):** `transmittance = exp(-distance * WATER_SIGMA)` with `WATER_SIGMA = vec3(0.42, 0.16, 0.10) * 0.35`, and `in-scatter = WATER_SCATTER_COLOR` (`vec3(0.015, 0.14, 0.24)`) `* (1 - exp(-distance * 0.22)) *` an ambient day/sunset term. These constants live in the shared include `ressources/shaders/vulkan/water_optics.inc.glsl`, used by **both** `water.frag.glsl` (surface water) and `composite.frag.glsl` (camera underwater) — one documented source for the medium's optical constants.
- **Caustics:** a procedural value-noise pattern (shared helper in the same include) from the reconstructed world XZ + time, added as light before tonemap. Gates: sun elevation (smoothstep on `sunDir.y`), depth below the local water surface (exponential fade), an upward-facing factor derived from depth-buffer gradient normals, and the final SSAO term — so occluded/dark cave floors are not brightened as if sunlit. Since the water pass writes its surface into the depth buffer, a hit reconstructed **on the local surface plane** (`worldPos.y > surfaceY - 0.5`) is the boundary itself, not floor: caustics are skipped for those pixels, otherwise the underside of the surface picks up a projected caustic web.
- **Quality tiers:**

  | Preset | Underwater model |
  |--------|------------------|
  | Low | Extinction + in-scatter only, no caustics |
  | Medium | + simple world-space caustics (no normal gating) |
  | High | + depth-gradient normal gating |
  | Cinematic | Same model, finer pattern |

- **Surface blend:** `Engine` still samples the camera voxel each frame (`ChunkCollisionView`, `Medium::Water`) to set the underwater flag — unchanged — and additionally scans up the column to the local water surface. The surface Y reaches composite via push constant `p6`; the submersion factor (camera Y vs surface Y) blends the effect in over roughly the first half-metre below the surface, so crossing the boundary no longer snaps a full-screen filter on/off. The sentinel value `1e9` means "unknown surface → fully submerged" (debug toggles). When the surface is known, upward view rays also intersect that plane optically: the extinction path ends at the surface (scene distance keeps locating the geometry), so looking at the sky from just below the surface is nearly clear instead of suffering the full sky-column extinction.
- **Division of responsibilities with exposure (issue #140):** underwater extinction, in-scatter and caustics are scene-medium light transport applied inside composite **before** exposure; the auto-exposure system (HDR metering + temporal adaptation) meters the raw scene HDR **before** composite and owns how the tonemapped result is mapped to display brightness. The underwater medium therefore never feeds back into its own exposure — no runaway darkening/adaptation loop. There is no separate underwater exposure multiplier — the `underwaterStrength` slider only blends the medium effect in; it does not touch exposure. Composite consumes both: `resolveExposure()` picks the adapted auto value or the exact manual setting (`p5.x`), while the medium terms travel in `p6`.
- **Profiling:** a nested `GpuPass::Composite` interval is recorded around the composite draw inside `GpuPass::Post` (same nested pattern as `GpuPass::Ssao`/`Exposure`), so underwater/composite GPU cost is measurable separately from the metering pass.

#### Spatial AA — FXAA 3.11 (issue #143)

A dedicated fullscreen pass **after composite**, replacing the abbreviated
in-composite approximation that used to run on linear HDR. When spatial AA is
enabled, composite stops writing the swapchain and lands its tone-mapped,
graded LDR (sRGB-encoded) in a full-res intermediate instead:

```text
composite.frag (tonemap + grade + underwater medium, sRGB-encoded)
  -> full-res ldrColor target (R8G8B8A8_UNORM, w·h·4 bytes ≈ 8.3 MB @1080p)
  -> fxaa.frag (FXAA 3.11 quality path) -> swapchain
```

- **Why after composite:** FXAA is designed for the final LDR image — edge
  detection and blending run on **sRGB-encoded tone-mapped** values through
  Rec. 601 perceptual luma, never on linear HDR (see the color-space contract
  below). The extra target is **always allocated** (so the runtime toggle
  needs no reallocation) but only rendered into / sampled while AA is on.
- **Algorithm:** FXAA 3.11 "quality" preset 12 — unrolled span-search steps
  1.0 / 1.5 / 2.0 / 4.0 / 12.0, tuning `subpix = 0.75`, `edgeThreshold =
  0.166`, `edgeThresholdMin = 0.0833`. Ported from NVIDIA's reference
  `FXAA3_11.h` (Fxaa3_11, © 2014 NVIDIA CORPORATION, BSD-3-Clause): the full
  license notice is preserved at the top of `fxaa.frag.glsl` and must also be
  reproduced in documentation accompanying any binary distribution.
- **Debug views:** the SSAO debug views (Graphics panel) bypass the AA pass —
  diagnostics render straight to the swapchain unfiltered.
- **Output transfer:** the pass mirrors composite's contract — when the
  swapchain is a hardware-sRGB attachment it decodes the sRGB-encoded input
  back to display-linear before writing (the attachment re-encodes on write);
  on the UNORM + shader-encode path the already-encoded values pass through
  unchanged. Either way the frame keeps exactly one linear→sRGB encode.
- **Presets/policy:** `PostProcessSettings::fxaaEnabled` (default on) — Low
  drops AA entirely, Medium/High/Cinematic keep the pass on. When disabled the
  pass is skipped and composite renders straight to the swapchain (the legacy
  path, unchanged).
- **Profiling:** a nested `GpuPass::SpatialAA` timestamp ("AA (FXAA)") beside
  `GpuPass::Composite` inside `GpuPass::Post`.

### Color-space contract (`Renderer/ColorSpace.hpp`, issue #135)

One explicit end-to-end contract; helpers, format policy and unit tests live in `ColorSpace.hpp`:

```text
sRGB-authored albedo PNG
  --hardware sRGB decode (sRGB image format)-->
linear RGB
  -> all lighting / fog / HDR post in linear space
  -> tone mapping to display-linear [0, 1]
  --sRGB swapchain conversion-->
display
```

- **Albedo textures** (block atlas + mob textures, bundled or resource-pack) are uploaded as `VK_FORMAT_R8G8B8A8_SRGB` (`colorspace::kAlbedoTextureFormat`) so Vulkan decodes sRGB→linear on sample. Alpha is untouched (sRGB affects RGB only). Non-color data (depth, AO, masks, HDR targets) stays UNORM/float — never sRGB. `test_mob_render` asserts the *actually created* GPU images are sRGB for both the atlas and mob textures.
- **Biome tint colors are sRGB-authored**: `BiomeConfig` grass/foliage colors are display-domain values quantized to RGB8 (like texture pixels); `terrain.vert` decodes them via `srgbToLinear` before `terrain.frag` mixes the tint with the linear-decoded albedo.
- **Output transfer is decided from the {`VkFormat`, `VkColorSpaceKHR`} pair** (`colorspace::classifyOutputTransfer`), not the format alone: the presentation engine interprets pixel values through the color space. Policy is strictly SDR — `VkSwapchain` prefers an sRGB image format + `SRGB_NONLINEAR` (hardware encode on attachment write), accepts the UNORM equivalent + `SRGB_NONLINEAR` (composite encodes via `linearToSrgb`), and *refuses* anything else (HDR10/PQ, Display-P3, extended sRGB…) instead of guessing a transfer. `PostStack::createPipelines` throws on `OutputTransfer::Unsupported` and passes the shader-encode flag to `composite.frag` as push-constant `p4.z`. No double transfer either way; both paths converge on exactly one linear→sRGB encode (asserted by `simulateDisplayOutput` parity tests).
- **Luminance weights are per-domain**: physical luminance on *linear* RGB uses Rec. 709 weights (`kRec709Luma` — terrain saturation, scotopic night, biome tint luminance); film grain keeps the Rec. 601-on-perceptual (sqrt) convention. FXAA (issue #143) runs on the sRGB-encoded post-tonemap LDR intermediate — edge detection and blending happen in that perceptual space through Rec. 601 luma, never on linear HDR. Shared GLSL transfers live in `colorspace.inc.glsl`.
- **`gamma` setting is a creative midtone grade** (default 1.0 = neutral display-linear), applied after tone mapping and before the final sRGB transfer — it is *not* a framebuffer transfer function and must not be used to compensate format semantics. The neutral epsilon (|γ−1| ≤ 0.001 skips the grade) is mirrored exactly between `ColorSpace.hpp` and `composite.frag`.

### OverlayRenderer (`Renderer/OverlayRenderer.*`)

- Block highlight, player markers, optional chunk borders.
- Invoked **from OpaquePass** (not a standalone step after sky).
- Shaders: `overlay.vert` / `overlay.frag`.

### MobRenderer (`Renderer/MobRenderer.*` + `Entities/MobModel.*`)

Draws the passive mobs (cow / pig / sheep / chicken — simulation in [`engine-architecture.md`](engine-architecture.md) §9).

- **Static geometry, instanced transforms:** all four species are baked once into
  one shared vertex buffer (`Entities/MobModel`, Minecraft box-UV unfolding with
  horns / snout / fleece layers / beak / wings). Each frame, `prepare` writes one
  `Instance {mat4 model, vec4 uvScale, vec4 localLight}` per body part into a per-frame-in-flight
  mapped buffer — no per-animal mesh rebuilds or uploads.
- **Local voxel lighting (issue #128):** entities sample the continuous voxel lighting field
  (skylight + propagated RGB4 block light from #141) with bounded trilinear interpolation
  via `ChunkManager::sampleSmoothedLight` / `ChunkMobWorld::sampleLight`. `prepare` writes
  `localLight = vec4(skylight, blockR, blockG, blockB)` into the instance buffer.
  `mob.vert.glsl` forwards `vSkyLight` and `vBlockLightRGB` to `mob.frag.glsl`, which
  faithfully matches terrain lighting semantics:
  - Directional celestial light gated by local `sunReach * (1.0 - shadow)`
  - Cave / outdoor ambient transition via `mix(caveAmbient, outdoorAmbient, sky) * hemisphere`
  - Per-source colored linear RGB block light with quadratic falloff `blockFill * blockPeak * blockLightScale`
  - Shared cascaded shadow map (CSM) sampling via `sampleDirectionalShadow`
  - Consistent contrast / saturation / scotopic night vision and `sunReach`-gated distance fog via the shared atmosphere contract (`atmosphere_fog.inc.glsl`, issue #159)
- **Visibility:** one frustum test per mob against the camera matrix and the three
  cascade matrices; a per-draw visibility mask selects which pass sees which mob
  (`visibleCount` feeds the HUD). Casters behind the camera are still drawn into
  shadow cascades.
- **Draws:** `record` is called once inside OpaquePass (HDR color + depth, before
  overlays) and once per shadow cascade inside ShadowPass. Push constant = the
  view-projection of the target (camera or cascade); descriptor set 0 is the frame
  set, set 1 is the mob albedo. Per-part draw calls reuse the currently bound
  texture descriptor when consecutive parts use the same texture, avoiding
  redundant descriptor binds (draws are not reordered or merged); shadow pipeline
  adds depth bias and an alpha-cut-only fragment shader.
- **Textures (`MobTextures`):** six entity PNGs (`cow_temperate`, `pig_temperate`,
  `sheep`, `sheep_wool`, `sheep_wool_undercoat`, `chicken_temperate`) loaded
  through `ResourcePackReader::readEntityTexture` (`assets/minecraft/textures/entity/…`,
  ZIP or folder packs, wrapped roots for both, classic-name aliases). Both square (64×64)
  and classic (64×32) skins are supported — `uvScale` folds the box UV layout to
  the image aspect. **Mipmapped minification (issue #160):** every skin gets a
  full CPU-generated mip chain (`texture_mips::generateLayerChainRects`, W×H-aware
  and **UV-rect aware** — a mob skin is an atlas of independent box faces whose
  rects are derived from the baked `MobModel` vertices, so the linear-light,
  alpha-coverage-preserving #136 filter never blends neighboring faces across
  odd UV boundaries, each face keeps its own alpha coverage, and unused regions
  get alpha-0 gutters of the neighboring face color). The chain is uploaded
  level-by-level with `uploadRgba8Image2DMipChain` (one copy, all levels resident
  before the descriptor set is written; ~+33% device payload for the chain).
  Sampler: NEAREST magnification (crisp close-range pixel-art texels), LINEAR
  minification with LINEAR mip selection, clamp to edge; anisotropy stays off —
  mostly upright box-model surfaces showed no gain to justify it. The
  `NearestMip0` sampler policy reproduces the pre-#160 behavior for the A/B
  temporal regression in `test_mob_render` (mip0/nearest vs mipmapped over a
  fixed mob ROI).
- **Reload:** part of the failure-atomic resource-pack path. `prepareTextures`
  stages a complete new `Textures` bundle (image, sampler, descriptor pool/sets)
  before `commitTextures` swaps it in; a GPU failure anywhere keeps the previous
  bundle live. `TextureManager::swap` gives the block atlas the same atomicity.
- Profiler passes: `GpuPass::Mobs` (color) and `GpuPass::MobShadow0-2`.

### TextureManager (`Renderer/TextureManager.*`)

- Block **texture array** (`sampler2DArray`) for all solid/water materials (~70 layers: core blocks + wood species, climate surfaces, ice, deepslate, etc.). Uploaded as **sRGB** (`colorspace::kAlbedoTextureFormat`) — see [Color-space contract](#color-space-contract-renderercolorspacehpp-issue-135).
- **Single layer table** `kBlockLayers` in `Renderer/MinecraftTextures.hpp`: Minecraft basename + **bundled fallback** PNG + transparency per `TextureType`. Meshing `TextureManager::isTransparent` and atlas load both use it. Face remap / foliage / ice helpers live here too (`blockTopFace`, `blockIsFoliage`, …).
- Each PNG is decoded **once**; layer size = max frame edge normalized to a
  power of two (min 16); nearest-neighbor into the atlas.
- **Mip chain (CPU-generated):** each decoded RGBA8 layer also gets a full mip
  chain built on the CPU (`Renderer/TextureMips.*`): texels are sRGB-decoded to
  linear light before averaging, color is filtered premultiplied (no fringe),
  and cutout alpha coverage is preserved by a per-level rescale that brings
  each mip's coverage (fraction of texels above the canonical `0.5` threshold —
  shared with the shaders through `ressources/shaders/vulkan/cutout.inc.glsl`
  and `src/Renderer/TextureMips.hpp`) back to the base layer's coverage:
  DirectXTex-style binary search plus quantized tie resolution that
  promotes/demotes individual boundary texels until the covered texel count
  matches the target exactly (Bayer-spread), so isolated same-alpha details
  survive. Fully transparent texels are alpha-bleed dilated with their covered
  neighbors' color on every level including mip 0, so LINEAR filtering never
  darkens cutout edges. Downsampling partitions the source domain in integer
  windows — every source texel contributes exactly once (NPOT edges are never
  dropped), and the canonical layer size is normalized to a power of two
  (min 16). The whole chain is staged and uploaded in one
  `vkCmdCopyBufferToImage` (one copy region per layer × mip,
  layer-major/mip-minor staging layout); a full chain costs ~+33% extra device
  memory for that image.
- **Sampler:** NEAREST magnification (keeps the pixel-art look up close), LINEAR
  minification with LINEAR mip selection, `maxLod = VK_LOD_CLAMP_NONE`. Anisotropy
  is enabled only when the device enabled `samplerAnisotropy`
  (`VkContext::samplerAnisotropyEnabled`), capped at 8x and clamped to the device
  limit; unsupported devices fall back gracefully to 1x.
- Water and shadow passes sample through the same sampler via set1 binding 0.
- Path resolve (explicit pack root only — no getenv in texture code):
  1. `{pack_root}/assets/minecraft/textures/block/<name>.png` when pack is non-empty and file exists
  2. Else `{RES_PATH}textures/<name>.png` when that file exists
  3. Else `{RES_PATH}textures/<bundledFallback>.png` (always one of the shipped core textures)
  - Pack miss (step 1 fails while pack set) is reported; incomplete packs still load via 2/3.
- Pack root resolved at process entry: CLI `--resource-pack` wins over env `FT_VOX_RESOURCE_PACK`, then passed into `Engine` / `WorldRenderer` / `TextureManager`.
- **In-game:** Graphics panel → Resource pack (`GameUIResourcePack.*`) — path field, **Browse…** (ImGuiFileDialog), **Apply pack** / **Use bundled** → `Engine::applyResourcePack` (sole pack-path owner) → device idle → `WorldRenderer::reloadResourcePack` → failure-atomic `TextureManager::initialize` (build temps, then swap) + rewrite set1. Dialog: vendored `src/ImGuiFileDialog/`.
- Animated strips (e.g. `water_still.png`): first frame only (`width × width` view of the strip buffer, no intermediate copy).
- **World gen** uses the expanded palette (birch/spruce/jungle/acacia/dark oak trees, cactus, ice, red sand/terracotta badlands, deepslate below Y≈16, stone variants). Without a pack, new blocks share oak/sand/stone fallbacks but remain distinct voxel IDs.

---

### Shared mesh arenas + indirect drawing (`Vulkan/MeshArena.*`, issue #109)

- Chunk meshes no longer own one VMA vertex/index allocation each. Four
  **device-local arena** instances (opaque/water x vertex/index) back every
  uploaded mesh: 128 MiB vertex pages, 64 MiB index pages, created on demand.
- Each section suballocates an aligned range from its page's first-fit free
  list (vertex ranges align to `sizeof(Vertex)` so `vertexBase` is exact).
  Freeing is **frame-aware**: retired ranges return to the free list after
  `framesInFlight + 1` frames, and fully emptied pages are destroyed through the
  same retire queue - streaming and edits never need `vkDeviceWaitIdle`.
- `allocate()` returns false when VMA cannot back a new page; the upload is
  transactional, so a failed range allocation leaves every slot untouched.
- Published arena ranges are immutable. Every upload plans the touched
  sections, allocates ALL fresh vertex/index ranges for opaque and water
  together (any allocation failure frees the ranges the transaction created
  and leaves every slot, LOD handle and draw count untouched), records the
  copies, atomically swaps in the replacement slot table, and only then
  retires the replaced ranges frame-aware. A rebuilt empty section becomes
  slotless. The synchronous bootstrap surfaces an allocation failure by
  throwing instead of pretending the upload happened; the pending CPU
  result stays attached for a retry.
- Draw submission: each pass collects `Chunk::IndirectDraw` commands - one
  per live section, never merged (section-local indices are rebased by the
  command's `vertexOffset`) - groups them by arena page pair, binds each
  pair once and issues one `vkCmdDrawIndexedIndirect` per group. Commands
  live in host-visible indirect buffers (one per frame in flight per pass),
  written and flushed on the CPU each frame. GPU-driven culling/indirect-count
  remains future work.
- Indices are stored section-local: the indirect command's `vertexOffset`
  performs the rebase on the GPU, so uploads are plain copies (no CPU rebase
  pass). Positions are stored in chunk-local packed 16-byte `Vertex` records;
  per-draw chunk origin is passed via `VoxelDrawData` in a frame-mapped SSBO
  (set 0, binding 2) indexed via `gl_InstanceIndex` (`firstInstance`, issue #110).
- Telemetry: `arena.pages`, `arena.freeBytes`, `arena.highWaterBytes` gauges
  and `arena.binds`, `arena.growEvents` events complement the pre-existing
  `mesh.allocations.*` counters (which now count page creation, not
  per-chunk uploads).

## 5. FrameUBO contract

**Single source of truth:** `src/Renderer/FrameUBO.hpp` (`struct FrameUBO`, `sizeof` **624**, std140).

At CMake configure time, `cmake/GenerateFrameUboGlsl.cmake` parses that header and writes:

```text
ressources/shaders/vulkan/frame_ubo.inc.glsl   # generated — do not hand-edit
```

World shaders `#include "frame_ubo.inc.glsl"` (glslc `-I` includes the generated path). Fields include view/projection, three cascade matrices, fog/light/visual params, sun/moon dirs, sky day factors, cascade splits, moon ambient, lighting params (block/emissive/fogY/underwater), and water params. The post **composite** also binds this set (set 0) for the camera-underwater medium transport (view/projection, camera position, sun/moon directions, day/sunset/night factors); the local water-surface Y used by the submersion blend travels via composite push constant `p6`, not the UBO.

**Lane semantics (issue #161 audit):** every lane must have a production consumer or be commented `reserved` in `FrameUBO.hpp` (the generated GLSL inherits those comments). `lightParams.z` and `visualParams.z` are reserved — the former held the removed no-op "Light levels" slider, the latter a duplicate `colorBoost` copy no shader read. The material-grade knobs (`lightParams.w` colorBoost, `visualParams.x/y` saturation/contrast) are consumed **only** by the terrain and mob lit-material shaders (water and the full-frame post grade are separate — `PostProcessSettings::postSaturation`/`postContrast`); `visualParams.w` is the terrain shadow-debug selector.

CPU fill: `WorldRenderer::updateFrameUBO` from `Camera`, `ShaderParameters`, cascade far (`RenderSettings::shadowCascadeFar`), underwater flag (camera voxel sample; the local water-surface scan feeds the composite push constant instead).

---

## 6. Materials and lighting helpers

### MaterialTable (`Renderer/MaterialTable.hpp`)

Lava has a dedicated `LavaSurface` flag: slow UV advection and bounded thermal
emission preserve texture detail. Self-emission bypasses vertex AO, propagated
block-light feedback and the night desaturation grade. This is an opaque
emissive surface, not a screen-space mirror. Colored block-light irradiance
uses the RGB peak as an additional falloff factor, localizing diffuse bounce
without changing the packed light data or its channel ratios.

Water reads packed sky/RGB block light as well. In enclosed areas its SSR
fallback is a dim local-light approximation rather than outdoor sky; sky
glitter, foam, scattering and fog are gated by local skylight. SSR still
reflects visible opaque geometry (including lava), with the existing
edge/distance confidence fade; offscreen geometry is unavailable.

CPU table → GPU UBO (set0 binding 1). Per `TextureType`:

| Flag / field | Examples |
|--------------|----------|
| Foliage wind | `OAK_LEAVES` |
| Ice specular | `SNOW` |
| Emissive | Ores via `lighting::emissiveIntensityForBlock` |

Shaders read material by texture index instead of hardcoding “tex == 8 means leaves” for emissive/wind policy (shadow VS still keeps wind constants matched to the table for the push-constant-only path).

### Lighting (`Renderer/Lighting.hpp`, `namespace lighting`)

Pure helpers shared with unit tests (`tests/test_render_helpers.cpp`):

- Height fog / terrain fog amount caps  
- Moon ambient color  
- Cave light floor / fill, sun-shadow weight by sky light  
- SSAO intensity clamp + composite AO floor (`kSsaoAoFloor`, mirrored in `composite.frag`)  
- God-ray pass active predicate (`godRaysPassActive`)  
- Block light packing / emissive intensities  

**Colored block light (issue #141).** Propagated block light is RGB, not
scalar. Sources are semantic block data — `lighting::BlockLightSource
{colorLinear, intensity}` from `blockLightSourceForBlock` (LAVA warm
orange/red 15, MAGMA orange 13, REDSTONE_ORE red 14, LAPIS_ORE blue 7,
DIAMOND_ORE cyan 4, EMERALD_ORE green 3, GOLD_ORE yellow 2) — kept
conceptually separate from material self-emission
(`emissiveIntensityForBlock` drives the HDR glow of the surface itself).
The mesher propagates three planar 4-bit channels (`uint8_t` planes;
`Chunk::computeLightField`): same BFS as before, −1 per channel per
6-neighbour step, frontier through air-like cells only, and overlapping
sources combine by **per-channel max** (commutative/associative ⇒ the settled
field is traversal-order independent). Vertex packing uses `packedData` bits
18-21 R / 22-25 G / 26-29 B (sky stays 14-17; 30-31 spare) via
`lighting::packLightBitsRGB4`; `terrain.frag` multiplies albedo by the linear
RGB block light (replacing the old fixed warm tint) while the sky/sun/moon
path stays untouched. Emissive edit invalidation still keys on
`blockLightEmission(type) > 0`.

**Cross-chunk propagation (issue #141 review).** The block-light BFS runs on
a **transient halo domain**: at mesh dispatch the ChunkManager snapshots the
15-voxel ring of neighbor voxels (4 sides + 4 diagonals — the Manhattan BFS
cannot route light around them) into a pooled `ChunkLightHalo` — main-thread
cost measured as the `mesh.haloFill` telemetry stage — and the BFS seeds
center *and* ring sources; only the center (+ its 1-voxel face-sampling
shell) is sampled, so border faces read the neighbor side's real propagated
light and no colored-light seams appear at chunk borders.
Missing or still-generating neighbors (state UNLOADED) contribute AIR (no
sources ⇒ no light); a neighbor in transit for a **mesh** job is read
normally — its voxels are immutable while the mesh worker owns it, so one
batch dispatching two adjacent chunks still gives both correct halos — and
light reaches already-meshed neighbors through two invalidation rules: a
light-relevant **edit** within the halo radius of a border dirties the
reachable neighbors, and a chunk **arrival** dirties the neighbors its border
bands can reach for cells that are emissive **or** non-air-like (arriving
blockers change BFS paths through what the halo assumed to be AIR). The
invalidation records the atomic section mask even while the neighbor is
mid-mesh-job; the mask persists across the in-flight build and
`processFinishedJobs` re-arms `GENERATED` after publish, so a race between
edits/arrivals and meshing can never silently drop an invalidation. The
packed RGB4 helpers (`packBlockLightRGB4` etc.) define the representation
contract issue #128 consumes for entities; actual runtime sampling/storage of
the field for entities remains #128's responsibility.

### Cascades (`Renderer/ShadowCascades.hpp`, `namespace shadow`)

- Split computation, light matrices, cascade blend helpers, bias constants.

Settings knobs: `ShaderParameters` and `PostProcessSettings` in `Engine/EngineDefs.hpp`, driven by **GameUI** Graphics panel and quality presets (`GraphicsQualityPreset`: Low / Medium / High / Cinematic via `PostProcessSettings::applyPreset`; spatial AA is off on Low and FXAA 3.11 on from Medium up — issue #143).

---

## 7. Shader catalog

All under `ressources/shaders/vulkan/` (GLSL compiled to SPIR-V at build):

| File | Stage | Used by |
|------|-------|---------|
| `frame_ubo.inc.glsl` | include | Generated FrameUBO block |
| `water_optics.inc.glsl` | include | Shared water optical constants (`WATER_SIGMA`, `WATER_SCATTER_COLOR`) + value-noise caustics helper — `water.frag.glsl` + `composite.frag.glsl` |
| `atmosphere_fog.inc.glsl` | include | Shared camera-to-surface air/aerial-perspective contract (`evaluateAtmosphereFog` / `applyAtmosphereFog`) — `terrain.frag.glsl` + `water.frag.glsl` + `mob.frag.glsl` |
| `terrain.vert.glsl` / `terrain.frag.glsl` | VS/FS | OpaquePass |
| `shadow.vert.glsl` / `shadow.frag.glsl` | VS/FS | ShadowPass |
| `water.vert.glsl` / `water.frag.glsl` | VS/FS | WaterPass |
| `skybox.vert.glsl` / `skybox.frag.glsl` | VS/FS | SkyPass |
| `overlay.vert.glsl` / `overlay.frag.glsl` | VS/FS | OverlayRenderer |
| `mob.vert.glsl` / `mob.frag.glsl` | VS/FS | MobRenderer (opaque HDR pass) |
| `mob_shadow.frag.glsl` (+ `mob.vert.glsl`) | VS/FS | MobRenderer (shadow cascades, alpha cut) |
| `fullscreen.vert.glsl` | VS | All post passes |
| `ssao.frag.glsl` | FS | PostStack (half-res horizon AO) |
| `ssaoUpsample.frag.glsl` | FS | PostStack (bilateral upsample to full-res AO) |
| `luminance_downsample.frag.glsl` | FS | PostStack (auto-exposure log-luminance metering chain) |
| `exposure_adapt.frag.glsl` | FS | PostStack (auto-exposure adaptation; writes the per-frame state SSBO) |
| `bloomExtract.frag.glsl` / `bloomBlur.frag.glsl` | FS | PostStack |
| `godRays.frag.glsl` | FS | PostStack |
| `composite.frag.glsl` | FS | PostStack (tonemap, grade, grain, vignette, camera-underwater medium transport; set 0 = FrameUBO, set 1 = HDR/bloom/god rays/AO×2 + scene depth + exposure history; writes the swapchain when spatial AA is off, the full-res LDR `ldrColor` target when it is on) |
| `fxaa.frag.glsl` | FS | PostStack (spatial AA, issue #143: FXAA 3.11 quality preset 12 over the sRGB-encoded tone-mapped LDR target — full-res `R8G8B8A8_UNORM`, Rec. 601 perceptual luma; decodes back to display-linear on hardware-sRGB swapchains; output = swapchain) |
| `smoke.vert.glsl` / `smoke.frag.glsl` | VS/FS | Particle / smoke path if enabled |

Conventions:

- Prefer **push constants** for per-draw / per-fullscreen knobs on post.
- Never `vkUpdateDescriptorSets` mid-command-buffer for world sets; use fixed sets + rewrite between frames where needed (composite rebind uses defaults each frame).
- Image layout transitions go through **`vkbar::`** (`ImageBarrier.hpp`) rather than ad-hoc barrier copies.

---

## 8. Visual features (current baseline)

What the pipeline implements **now** (not a roadmap):

| Feature | Implementation notes |
|---------|----------------------|
| Greedy-meshed terrain | CPU mesh; opaque + water streams |
| Texture array + biome tint + vertex AO | Mesh packing in `Chunk` / `Vertex` |
| Directional CSM + PCF | 3 cascades, cool outdoor shadow tint; **moonlight shadows at night** (raw sky-light gate) |
| Cinematic night model | Directional blue moonlight + dark ambient + scotopic desaturation (`terrain.frag`), near-black sky |
| Sky / block light on vertices | Column sky cast + flood; block light BFS; cave fill floor in FS |
| Water: wave normals, sky reflection, depth absorption, glitter, foam | WaterPass + history; analytic per-phase sky reflection; Beer-Lambert teal body |
| Procedural sky, sun/moon, stars, clouds | SkyPass — cratered HDR moon, two-layer tinted stars, moon silver lining on night clouds |
| Height + distance fog, aerial-style haze | `atmosphere_fog.inc.glsl` (terrain/water/mob) + `lighting` helpers |
| SSAO (GTAO-style horizon AO + bilateral upsample), bloom, depth-aware god rays | PostStack half-res AO where applicable |
| ACES/Reinhard, auto/manual exposure, grain, vignette | `composite.frag`; auto exposure meters the raw scene HDR (issue #140) |
| Spatial AA: FXAA 3.11 dedicated pass (Low: off, Medium+: on) | `fxaa.frag` after composite on the sRGB-encoded tone-mapped LDR target (issue #143); output = swapchain, timed as `SpatialAA` |
| Camera-underwater medium transport | Engine voxel sample sets underwater flag + local water-surface scan; composite: scene-depth Beer–Lambert extinction/in-scatter + gated caustics before tonemap (`water_optics.inc.glsl`, issue #144) |
| Quality presets | Low/Med/High/Cinematic — post knobs (incl. spatial AA: off on Low, FXAA 3.11 on from Medium up, issue #143) plus the water path (SSR march budget, water shadows, underwater caustic tier) |

---

## 9. Environment and validation

| Variable | Purpose |
|----------|---------|
| `VK_ICD_FILENAMES` | MoltenVK (or other) ICD JSON |
| `VK_LAYER_PATH` | Validation explicit layers |
| `FT_VOX_VALIDATION` | `0`/`1` override Debug default |
| `FT_VOX_VULKAN_LIB` | Optional explicit loader dylib/so |

macOS example:

```bash
export VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
export VK_LAYER_PATH=/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d
./build-vk/ft_vox
```

---

## 10. Future ideas (non-authoritative)

These are **not** the current baseline. Kept only as short pointers for later work:

- SSR / planar reflections on water  
- Normal maps / PBR-ish materials  
- TAA, better volumetric fog  
- Soft penumbra (PCSS), contact shadows  
- Deferred or clustered lights if block-light density grows  

Do not treat this section as “already shipped.” For historical feature discussions, prefer git history over obsolete markdown.

---

## 11. Related docs

- [`vulkan-validation.md`](vulkan-validation.md) — RTSS SRGB/STORAGE diagnosis, per-app exclusion and validation error reporting

- [`gpu-profiling.md`](gpu-profiling.md) — GPU timestamp lifecycle, UI, benchmark metrics and validation

- [`engine-architecture.md`](engine-architecture.md) — Engine loop, chunks, streaming, terrain generation  
- Root [`README.md`](../README.md) — build, deps, controls  
- [`Agents.md`](../Agents.md) — contributor-oriented project context  
- `docs/benchmarks/` — captured profiling dumps (not architecture)  
