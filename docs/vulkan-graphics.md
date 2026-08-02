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
| Passes | Shadow → opaque (+ overlays) → water → sky → post | `Renderer/*Pass*`, `PostStack`, `OverlayRenderer` |
| Shaders | GLSL → SPIR-V offline | `ressources/shaders/vulkan/` |

**API target:** Vulkan **1.2+**, dynamic rendering preferred. On Apple, **MoltenVK** via ICD (`VK_ICD_FILENAMES`). Depth is **zero-to-one** (`GLM_FORCE_DEPTH_ZERO_TO_ONE`); viewport Y may be flipped for OpenGL-style world Y without winding flip.

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
        ├── ShadowPass
        ├── OpaquePass  (+ OverlayRenderer at end of opaque render)
        ├── WaterPass
        ├── SkyPass
        └── PostStack
```

### 3.2 Pass order (one frame)

Recorded in `WorldRenderer::recordFrame` (see `WorldRenderer.cpp`):

| Step | Owner | Writes | Notes |
|------|--------|--------|--------|
| 0 | **preRecord** callback | mesh GPU buffers | `Engine` records `uploadPendingMeshes` + transfer→vertex barrier here, before draws |
| 1 | **ShadowPass** | Cascaded depth array | Directional sun; leaf wind in shadow VS |
| 2 | **OpaquePass** | HDR color + scene depth | Solid chunks (`Chunk::draw`), then **`OverlayRenderer::record` inside the same dynamic rendering** (highlight / borders / demo players) |
| 3 | **WaterPass** | HDR (transparent) | History color/depth for refraction; set2 scene samples |
| 4 | **SkyPass** | HDR + god-ray source MRT, depth test | Procedural sky, sun/moon/stars/clouds |
| 5 | **PostStack** | Swapchain | SSAO → bloom → god rays → composite |
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

- **Cascaded shadow maps (CSM):** `shadow::kCascadeCount` (**3**), map size `shadow::kShadowMapSize` (**1024**), practical splits (`ShadowCascades.hpp`).
- Depth array image + one sampler; light matrices computed in `WorldRenderer::updateFrameUBO` using camera frustum slices.
- Pipeline: `shadow.vert` / `shadow.frag` (depth-only style).
- Caster list comes from `ChunkManager::collectShadowList` (XZ radius = `RenderSettings::shadowDistance`).

### OpaquePass (`Renderer/OpaquePass.*`)

- Dynamic rendering into **HDR** (`R16G16B16A16_SFLOAT`) and **D32** depth owned by `PostStack`.
- Draws opaque chunk meshes (`Chunk::draw`).
- Then calls **`OverlayRenderer::record`** on the same command buffer before ending the rendering scope (block highlight, chunk borders, demo players).
- Shaders: `terrain.vert` / `terrain.frag` — diffuse, face bias, sky/block light, CSM + PCF (sun **and** moon), cave fill, scotopic night grade, height/distance fog, material wind/emissive/ice.

### WaterPass (`Renderer/WaterPass.*`)

- Copies previous opaque HDR/depth into **history** images for refraction.
- Transparent water mesh (`Chunk::drawWater`).
- Shaders: `water.vert` / `water.frag` — vertex wave displacement + fragment-level procedural wave normals (3-octave value noise, top-face masked), Fresnel F0=0.02, **analytic sky reflection** (gradient identical to `skybox.frag` per phase: day blue / sunset orange / night near-black), **Beer-Lambert depth absorption** (linearized history depth vs view depth → teal body, red dies first), sun/moon glitter (pow 700 + sheen) on wave normals, shore foam from real water column + whitecaps, history refraction, near-opaque alpha (refraction composited in-color).

### SkyPass (`Renderer/SkyPass.*`)

- Standalone sky (not part of post).
- Dual color attachments: HDR scene + **god-ray source**.
- Shaders: `skybox.vert` / `skybox.frag`.

### PostStack (`Renderer/PostStack.*`)

Fullscreen chain on a unit quad (`fullscreen.vert`):

| Stage | Shader | Default behavior when disabled |
|-------|--------|--------------------------------|
| SSAO (half-res) | `ssao.frag` | Skip; composite samples **1×1 white** AO |
| Bloom extract + blur | `bloomExtract.frag`, `bloomBlur.frag` | Skip; composite samples **1×1 black** |
| God rays | `godRays.frag` | Skip unless `lighting::godRaysPassActive`; black default |
| Composite | `composite.frag` | Tonemap, grade, FXAA, grain, vignette, underwater |

True **1×1 defaults** live on `PostStack` (`m_defaultBlack`, `m_defaultWhiteR8`). Selection is pure helper logic in `PostDefaults.hpp` (`postCompositeSources`) so composite never samples half-res targets that were not written this frame.

### OverlayRenderer (`Renderer/OverlayRenderer.*`)

- Block highlight, player markers, optional chunk borders.
- Invoked **from OpaquePass** (not a standalone step after sky).
- Shaders: `overlay.vert` / `overlay.frag`.

### TextureManager (`Renderer/TextureManager.*`)

- Block **texture array** (`sampler2DArray`) for all solid/water materials (~70 layers: core blocks + wood species, climate surfaces, ice, deepslate, etc.).
- **Single layer table** `kBlockLayers` in `Renderer/MinecraftTextures.hpp`: Minecraft basename + **bundled fallback** PNG + transparency per `TextureType`. Meshing `TextureManager::isTransparent` and atlas load both use it. Face remap / foliage / ice helpers live here too (`blockTopFace`, `blockIsFoliage`, …).
- Each PNG is decoded **once**; layer size = max frame edge; nearest-neighbor into the atlas.
- Path resolve (explicit pack root only — no getenv in texture code):
  1. `{pack_root}/assets/minecraft/textures/block/<name>.png` when pack is non-empty and file exists
  2. Else `{RES_PATH}textures/<name>.png` when that file exists
  3. Else `{RES_PATH}textures/<bundledFallback>.png` (always one of the shipped core textures)
  - Pack miss (step 1 fails while pack set) is reported; incomplete packs still load via 2/3.
- Pack root resolved at process entry: CLI `--resource-pack` wins over env `FT_VOX_RESOURCE_PACK`, then passed into `Engine` / `WorldRenderer` / `TextureManager`.
- **In-game:** Graphics panel → Resource pack (`GameUIResourcePack.*`) — path field, **Browse** (ImGuiFileDialog), **Apply pack** / **Use bundled** → `Engine::applyResourcePack` (sole pack-path owner) → device idle → `WorldRenderer::reloadResourcePack` → failure-atomic `TextureManager::initialize` (build temps, then swap) + rewrite set1. Dialog: vendored `src/ImGuiFileDialog/`.
- Animated strips (e.g. `water_still.png`): first frame only (`width × width` view of the strip buffer, no intermediate copy).
- **World gen** uses the expanded palette (birch/spruce/jungle/acacia/dark oak trees, cactus, ice, red sand/terracotta badlands, deepslate below Y≈16, stone variants). Without a pack, new blocks share oak/sand/stone fallbacks but remain distinct voxel IDs.

---

## 5. FrameUBO contract

**Single source of truth:** `src/Renderer/FrameUBO.hpp` (`struct FrameUBO`, `sizeof` **528**, std140).

At CMake configure time, `cmake/GenerateFrameUboGlsl.cmake` parses that header and writes:

```text
ressources/shaders/vulkan/frame_ubo.inc.glsl   # generated — do not hand-edit
```

World shaders `#include "frame_ubo.inc.glsl"` (glslc `-I` includes the generated path). Fields include view/projection, three cascade matrices, fog/light/visual params, sun/moon dirs, sky day factors, cascade splits, moon ambient, lighting params (block/emissive/fogY/underwater), and water params.

CPU fill: `WorldRenderer::updateFrameUBO` from `Camera`, `ShaderParameters`, cascade far (`RenderSettings::shadowCascadeFar`), underwater flag.

---

## 6. Materials and lighting helpers

### MaterialTable (`Renderer/MaterialTable.hpp`)

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
- SSAO intensity clamp
- God-ray pass active predicate (`godRaysPassActive`)
- Block light packing / emissive intensities

### Cascades (`Renderer/ShadowCascades.hpp`, `namespace shadow`)

- Split computation, light matrices, cascade blend helpers, bias constants.

Settings knobs: `ShaderParameters` and `PostProcessSettings` in `Engine/EngineDefs.hpp`, driven by **GameUI** Graphics panel and quality presets (`GraphicsQualityPreset`: Low / Medium / High / Cinematic via `PostProcessSettings::applyPreset`).

---

## 7. Shader catalog

All under `ressources/shaders/vulkan/` (GLSL compiled to SPIR-V at build):

| File | Stage | Used by |
|------|-------|---------|
| `frame_ubo.inc.glsl` | include | Generated FrameUBO block |
| `terrain.vert.glsl` / `terrain.frag.glsl` | VS/FS | OpaquePass |
| `shadow.vert.glsl` / `shadow.frag.glsl` | VS/FS | ShadowPass |
| `water.vert.glsl` / `water.frag.glsl` | VS/FS | WaterPass |
| `skybox.vert.glsl` / `skybox.frag.glsl` | VS/FS | SkyPass |
| `overlay.vert.glsl` / `overlay.frag.glsl` | VS/FS | OverlayRenderer |
| `fullscreen.vert.glsl` | VS | All post passes |
| `ssao.frag.glsl` | FS | PostStack |
| `bloomExtract.frag.glsl` / `bloomBlur.frag.glsl` | FS | PostStack |
| `godRays.frag.glsl` | FS | PostStack |
| `composite.frag.glsl` | FS | PostStack (tonemap, grade, FXAA, grain, vignette, underwater) |
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
| Height + distance fog, aerial-style haze | `terrain.frag` + `lighting` helpers |
| SSAO, bloom, depth-aware god rays | PostStack half-res where applicable |
| ACES/Reinhard, exposure, FXAA, grain, vignette | `composite.frag` |
| Underwater grade | Engine sets flag from voxel sample; composite + UBO |
| Quality presets | Low/Med/High/Cinematic on existing post knobs only |

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

- [`engine-architecture.md`](engine-architecture.md) — Engine loop, chunks, streaming, terrain generation
- Root [`README.md`](../README.md) — build, deps, controls
- [`Agents.md`](../Agents.md) — contributor-oriented project context
- `docs/benchmarks/` — captured profiling dumps (not architecture)
