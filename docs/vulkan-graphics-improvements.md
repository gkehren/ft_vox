# Vulkan Graphics Improvements

Possibilities to improve ft_vox graphics toward Minecraft-style shader packs (Complementary, BSL, Seus, etc.), based on the current Vulkan pipeline.

## Current baseline

ft_vox already has a solid Minecraft-adjacent foundation:

| System | Status |
|--------|--------|
| Terrain | Greedy-meshed chunks, texture array, biome tint, vertex AO |
| Lighting | Quantized diffuse + ambient + face bias |
| Shadows | Single orthographic shadow map (2048²), 3×3 PCF |
| Water | Separate transparent pass |
| Sky | Procedural sky, sun/moon, stars, DDA “block” clouds |
| Particles | Smoke |
| Post | HDR (`R16G16B16A16`), bloom extract/blur, god rays, ACES/Reinhard, FXAA, exposure/gamma/contrast/saturation |

Key code locations:

- `src/Renderer/TerrainRenderer` — opaque, shadow, water
- `src/Renderer/PostStack` — HDR targets, sky MRT, bloom, god rays, composite
- `ressources/shaders/vulkan/` — GLSL → SPIR-V
- `EngineDefs.hpp` — `RenderSettings` / `PostProcessSettings` toggles (Graphics ImGui panel)

Vulkan does not block advanced effects; it mainly unlocks cleaner multi-pass targets, compute, and (later, where available) ray queries.

---

## What you have vs. what shader packs do

| Area | ft_vox today | Typical Minecraft shaders |
|------|--------------|---------------------------|
| Lighting | Quantized diffuse + ambient + face bias | Soft shadows, multi-bounce ambient, block light |
| Shadows | Single ortho map, 3×3 PCF | Cascaded shadows, soft penumbra, contact shadows |
| Water | Transparent pass | Waves, refraction, SSR, foam, caustics |
| Atmosphere | Height/distance fog + sky god rays | Volumetric fog, aerial perspective, weather |
| Materials | Diffuse atlas only | Normals, specular, emissive, wetness |
| Post | Bloom, god rays, tonemap, FXAA | TAA, SSAO, DoF, color grade, under-water, film grain |

---

## Tier 1 — High impact, fits the current stack

These mostly extend `terrain.frag`, water, and `PostStack` without redesigning the world.

### 1. Better water (biggest “wow” for voxels)

- **Vertex/fragment waves** (scroll UV + height offset from time + noise)
- **Refraction**: sample HDR/scene color with a distorted UV (HDR + depth already exist in `PostStack`)
- **Fresnel + specular sun highlight** on water normals
- **Foam** at shore (depth difference or edge blocks)
- **Underwater**: blue/green grade, blur, bubble particles, reduced god rays

This is the single most recognizable “shader pack” look.

### 2. Soft / cascaded shadows

Today: one 2048² ortho map. Upgrade path:

- **CSM** (2–4 cascades) for near detail + far coverage
- **PCSS** or larger Poisson PCF for soft penumbra
- Optional **contact shadows** (short ray-march in screen space) for thin cracks

Looks much more modern without changing materials.

### 3. Ambient occlusion beyond vertex AO

Vertex AO is already good for Minecraft style. Add:

- **SSAO / GTAO** after the opaque pass (needs depth + normals G-buffer or reconstructed normals)
- Or **HBAO** at half-res for cost control

Corners and caves stop looking flat under soft ambient light.

### 4. Atmosphere / volumetric fog

Distance + height fog and screen-space god rays already exist. Next step:

- **Exponential height fog** tinted by sun/sky color
- **Volumetric light shafts** that use depth (not only sky MRT) so trees cast light shafts
- Optional cheap **ray-marched fog** along the view ray (8–16 samples)

This is the core of “cinematic” packs at dusk.

### 5. Day/night material response

- Stronger **moon-colored ambient** at night
- **Emissive blocks** (torches, lava, glowstone) as a second light term or bloom seed
- **Block light** baked per-vertex or from a coarse lightmap (classic Minecraft approach)

Emissives + bloom already play well with the extract/blur passes.

---

## Tier 2 — Material and “PBR-lite” (shader-pack DNA)

Minecraft packs rarely do full Disney PBR; they fake it.

### 6. Normal + specular atlas layers

Extend `sampler2DArray` (or add layers):

- Normal maps for stone/wood/leaves
- Roughness/specular mask
- Optional height for **parallax** on close blocks

In `terrain.frag`: replace flat face normals with TBN + normal map; add Blinn-Phong or GGX specular from the sun.

### 7. Wetness / rain

- Global wetness factor from weather
- Darken albedo, raise specular, puddle normals on top faces
- Rain streaks in post or particle quads

Huge mood change with little geometry work.

### 8. Leaves / foliage enhancement

- Subsurface-ish backlighting (`dot(-L, N)` through leaves)
- Wind sway in the vertex shader (time + hash of chunk/block pos)
- Alpha-to-coverage or better dithered transparency for distant leaves

### 9. Color grading / LUTs

After tonemap in `composite.frag`:

- 3D LUT texture for “cinematic”, “vibrant”, “noir” presets
- Or simple split-toning (cool shadows, warm highlights at sunset)

Cheap and very pack-like for user presets in the Graphics ImGui panel.

---

## Tier 3 — Deferred / G-buffer

Today lighting is forward in the terrain pass. A lightweight deferred path:

**G-buffer (MRT):** albedo, normal, roughness/metal or material ID, emissive, depth

Then screen-space:

- SSAO / GTAO
- SSR (water + wet stone)
- Deferred sun + shadow
- Local lights (torches) as screen-space or tiled lights

Vulkan dynamic rendering (already in use) is ideal: extra color attachments without classic render-pass rewrite pain.

**SSR** especially: reflect sky/sun/terrain on water and wet blocks — classic Complementary look.

---

## Tier 4 — Temporal & quality polish

| Effect | Notes |
|--------|--------|
| **TAA** | Replaces/supplements FXAA; needs motion vectors (chunk static + camera only is OK) |
| **SMAA** | Quality AA without history if TAA ghosting is annoying |
| **DoF** | Blur far/near from depth; half-res |
| **Motion blur** | Camera-only first |
| **Chromatic aberration / vignette / grain** | Style knobs in composite |
| **Sharpen** after TAA | CAS-style |

Keep these **toggleable** in `PostProcessSettings` — same pattern as bloom / god rays / FXAA.

---

## Tier 5 — Advanced (optional demos)

- **Compute bloom** (downsample pyramid / dual Kawase) — faster and prettier than simple blur
- **Volumetric clouds** as 3D noise (soft noise clouds beyond current DDA block clouds)
- **Ray queries / RT shadows** (where supported; not reliably available on all MoltenVK setups)
- **Path-traced offline / hybrid** for screenshots only
- **Compute meshing or virtual texturing** — more engine than “shaders,” enables distant detail

For macOS/MoltenVK, prefer **core Vulkan 1.2 + fragment/compute**; avoid depending on hardware ray tracing early.

---

## Architecture that scales

Recommended pass graph:

```text
Shadow pass(es)
    ↓
G-buffer or forward HDR scene (opaque)
    ↓
Water (forward, reads scene color + depth)
    ↓
Sky / clouds (sky MRT for shafts)
    ↓
SSAO → (optional SSR) → lighting composite if deferred
    ↓
Bloom pyramid → volumetric / god rays
    ↓
Tonemap + grade + AA → swapchain
    ↓
ImGui
```

Practical rules for this codebase:

1. **Keep `FrameUBO`** as the time-of-day / sun / fog / post knob hub.
2. **Add effects as optional passes in `PostStack`**, with booleans in `PostProcessSettings` (like god rays today).
3. **Never update descriptors mid-pass**; pre-bind sets per technique.
4. **Half-res** for SSAO, SSR, volumetrics, DoF — full-res only for final composite.
5. **Quality presets** (Low / Medium / High / Cinematic) instead of dozens of independent toggles for players.

---

## Suggested implementation order (“Complementary-lite”)

1. Water waves + refraction + Fresnel  
2. CSM + softer PCF  
3. SSAO (half-res)  
4. Depth-aware volumetric fog / better shafts  
5. Emissive blocks + stronger bloom coupling  
6. Normal maps + simple specular  
7. SSR on water  
8. TAA + color grading LUT  
9. Weather / wetness  

Each step stays shippable and visible in screenshots.

---

## What not to chase first

- Full path tracing in-engine  
- Perfect PBR with hundreds of materials  
- Iris/OptiFine-compatible shader loading (packs assume Minecraft’s pipeline and those APIs)  
- Heavy particle fog everywhere before half-res volumetrics  

Iris/OptiFine packs are **not drop-in** on a custom engine; reimplement the *look*, not the pack format.

---

## Quick wins (mostly shader-only)

| Change | Where |
|--------|--------|
| Sunset-tinted ambient / cool shadow tint | `terrain.frag` |
| Stronger specular on water/ice | water fragment |
| Subtle wind on leaves/tall grass | `terrain.vert` |
| Underwater color grade when camera in water | `composite.frag` |
| Film grain + vignette | `composite.frag` |
| Sharper bloom threshold for lava/sun only | `bloomExtract.frag.glsl` |
| Better sky aerial perspective on terrain fog | fog term in terrain |

---

## Bottom line

Vulkan already positions ft_vox for a full shader-pack pipeline. Highest payoff for a voxel sandbox:

**water → soft/cascaded shadows → SSAO → volumetric atmosphere → emissives → SSR**

Layer these on the existing HDR / `PostStack` path, with quality presets in the Graphics UI.

---

## Next steps (optional follow-ups)

- Concrete PR plan (files, passes, resource layouts) for Tier 1 only  
- Deep dive on one effect (e.g. water refraction or CSM)
