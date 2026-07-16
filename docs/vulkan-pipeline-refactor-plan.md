# Vulkan Pipeline Refactor Plan

Concrete PR decomposition following the Tier 1 graphics code review. Goal: keep visual behavior, make the frame graph maintainable before Tier 2 (SSR, normals, TAA).

**Non-goals for this plan**

- New visual features (SSR, deferred G-buffer, TAA)
- API version bumps beyond current 1.2 + dynamic rendering
- MoltenVK-specific workarounds beyond what already works

**Success criteria (end state)**

| Metric | Today | Target |
|--------|------:|--------|
| `TerrainRenderer.cpp` lines | ~1225 | ≤450 (orchestrator only) |
| Frame sync implementations | 2 (`VkFrameContext` + Terrain) | 1 |
| Barrier helper dialects | 3 | 1 |
| FrameUBO definitions | 1 C++ + 7 GLSL copies | 1 shared contract |
| Magic texture ordinals in shaders | wind/emissive/ice hardcodes | material flags / table |
| Disabled post effects | still clear+transition | skip or bind 1×1 defaults |

**Hard rules for every PR**

1. Behavior-preserving (screenshot / playtest parity for outdoor + cave + water).
2. No new feature work mixed in.
3. `TerrainRenderer.cpp` must not grow past 1225; prefer shrink each PR.
4. Tests that exist (`test_tier1_graphics`, `test_vulkan_resources`, `test_visual_look`) stay green.
5. Each PR is independently shippable on `vulkan` branch.

---

## Architecture target

```text
Engine
  └── WorldRenderer (rename of TerrainRenderer role)
        ├── FrameSync          // acquire / fences / submit / present
        ├── FrameResources     // per-frame UBO + descriptor set0
        ├── ShadowPass         // CSM targets + record
        ├── OpaquePass         // HDR color+depth, chunk draw, overlays
        ├── WaterPass          // history copy + water mesh
        ├── SkyPass            // sky MRT into god-source (from PostStack)
        ├── PostStack          // SSAO → bloom → god rays → composite
        └── ImGui (callback)   // still Engine-owned draw list

Shared Vulkan utilities
  ├── VkContext / VkSwapchain / VMA wrappers (keep)
  ├── ImageBarrier helpers     // single transition API
  ├── GraphicsPipelineBuilder  // kill createPipelines paste
  └── FrameUBO contract        // single C++/GLSL source of truth
```

**Naming:** keep class name `TerrainRenderer` until PR-F renames, or rename early in PR-F only. Prefer **not** renaming until the split is done (churn).

---

## Dependency DAG

```text
PR-A  Image barriers + pipeline builder          (foundation, no behavior change)
  │
  ├─► PR-B  Unify FrameSync (VkFrameContext)     (Engine + Terrain acquire/present)
  │
  ├─► PR-C  FrameUBO single contract             (shaders + C++ layout)
  │
  └─► PR-D  Split ShadowPass + OpaquePass        (depends A; can parallel B/C after A)
        │
        └─► PR-E  Split WaterPass + move history   (depends D)
              │
              └─► PR-F  Thin WorldRenderer orchestrator + Post defaults
                    │
                    ├─► PR-G  Material flags (wind / emissive / ice)
                    │
                    └─► PR-H  Lighting model cleanup + tests
                          │
                          └─► (optional) PR-I  Quality presets UI only
```

**Parallelism after PR-A**

- PR-B ∥ PR-C (both touch Engine lightly; coordinate FrameUBO size vs frame UBO alloc)
- After B+C+A: PR-D  
- Prefer **linear stack** on Graphite/gh-stack if conflict risk is high: A→B→C→D→E→F→G→H

Recommended linear order for solo work: **A → B → C → D → E → F → G → H**.

---

## PR-A — Foundation: barriers + pipeline builder

### Intent

Delete mechanical duplication before moving code between files.

### Changes

**New files**

| File | Role |
|------|------|
| `src/Vulkan/ImageBarrier.hpp` (+ `.cpp` if needed) | `cmdTransitionColor`, `cmdTransitionDepth`, multi-image batch, aspect helpers |
| `src/Vulkan/GraphicsPipelineBuilder.hpp` (+ `.cpp`) | Defaults for IA/MS/dynamic viewport; `setShaderStages`, `setLayout`, `setRenderingFormats`, `setRaster`, `setDepth`, `setBlend`, `build()` |

**Modify**

| File | What |
|------|------|
| `src/Vulkan/VkImage.hpp/.cpp` | Either wrap existing `cmdTransitionImageLayout` into the new API, or implement new helpers by calling it — **one public path** |
| `src/Renderer/PostStack.cpp` | Replace private `transitionColor`/`transitionDepth` with shared helpers |
| `src/Renderer/TerrainRenderer.cpp` | Replace hand-rolled barriers in shadow/opaque/water-history with helpers; replace triple `createPipelines` blocks with builder |

### Out of scope

- Moving pass logic to new classes
- Changing layouts, formats, or pass order

### Acceptance

- [ ] Bit-identical resource transitions (same old/new layout, stages, access)
- [ ] All three pipelines still create successfully on MoltenVK
- [ ] `TerrainRenderer.cpp` line count **≤** current (expect ~80–150 lines saved)
- [ ] `test_vulkan_resources` + game launch smoke

### Risk

Low. Pure extraction. Revert-friendly.

### Estimated size

~300–500 LOC moved/added; net LOC down slightly.

---

## PR-B — Unify frame sync on `VkFrameContext`

### Intent

One owner for acquire / in-flight fences / submit / present.

### Design

Extend `VkFrameContext` (do not keep a second `FrameData` in TerrainRenderer):

```cpp
// Target API sketch
class VkFrameContext {
  bool beginFrame(VkSwapchain&, uint32_t& imageIndex);  // wait + acquire
  VkCommandBuffer commandBuffer() const;
  uint32_t frameIndex() const;
  // optional: bind per-frame UBO set later in PR-C/F
  bool submitAndPresent(VkSwapchain&, uint32_t imageIndex);
};
```

Remove from `TerrainRenderer`:

- `beginAcquire`, `submitPresent`
- `m_frames[].pool/cmd/imageAvailable/renderFinished/inFlight` **if** fully moved  
  Keep per-frame **UBO buffer + descriptorSet0** either:
  - on `VkFrameContext` as “frame resources”, or  
  - on a small `FrameUniformResources` owned by TerrainRenderer but indexed by `frameContext.frameIndex()`

**Recommended:**  
`VkFrameContext` = sync + command buffer only.  
`TerrainRenderer` keeps `m_frameUbo[i]` + `descriptorSet0[i]` until PR-F.

### Modify

| File | What |
|------|------|
| `src/Vulkan/VkFrame.hpp/.cpp` | Promote from clear-only demo to full game path; drop or gate `endFrameClearAndPresent` behind test-only if unused |
| `src/Renderer/TerrainRenderer.hpp/.cpp` | Use external frame context for cmd recording; remove acquire/present |
| `src/Engine/Engine.cpp` | Own `VkFrameContext`; call begin → updateUBO → record → submit |
| `tests/test_vulkan_resources.cpp` | If it relied on clear path, update |

### Engine loop target

```cpp
if (!frameCtx->beginFrame(*swapchain, imageIndex)) { recreate...; continue; }
terrain->updateFrameUBO(frameCtx->frameIndex(), ...);
terrain->recordFrame(frameCtx->commandBuffer(), frameCtx->frameIndex(), imageIndex, ...);
if (!frameCtx->submitAndPresent(*swapchain, imageIndex)) { recreate...; }
```

### Acceptance

- [ ] Single implementation of image-in-flight + frames-in-flight
- [ ] Resize / out-of-date swapchain path still works
- [ ] No double-wait or missing fence on present
- [ ] FPS parity (±noise)

### Risk

Medium (WSI + sync). Test carefully on macOS resize and alt-tab.

### Estimated size

~200–400 LOC churn; deletes ~100 lines from TerrainRenderer.

---

## PR-C — FrameUBO single contract

### Intent

One layout definition; delete dead fields; stop silent GLSL/C++ drift.

### Design options (pick one in PR)

**Option 1 (pragmatic):**  
`src/Renderer/FrameUBO.hpp` with packed struct + `static_assert(sizeof == N)`.  
Build copies a generated `frame_ubo.glsl` snippet, **or** document that GLSL includes are maintained by a small CMake check that greps field order.

**Option 2 (stronger):**  
CMake generates `frame_ubo.glsl` from a YAML/JSON field list used by both (heavier).

**Recommend Option 1** for speed.

### Changes

| File | What |
|------|------|
| `src/Renderer/FrameUBO.hpp` | **New** — std140 layout, comments for each vec4 packing |
| `TerrainRenderer.hpp` | Remove nested `FrameUBO`; include shared header |
| All GLSL with `uniform FrameUBO` | Match shared layout; **delete `postParams0..3`** (unused) |
| `TerrainRenderer::updateFrameUBO` | Write only live fields |
| CMake | Optional: `static_assert` test target compiling sizeof check |

### Field plan (proposed after trim)

Keep:

- matrices: view, projection, cascade0–2  
- viewPos, lightDirection, fogColor, fogParams  
- lightParams, visualParams  
- sunDir, moonDir, skyParams  
- cascadeSplits, moonAmbient, tier1Params, waterParams  

Drop:

- `postParams0..3` (post uses push constants)

If alignment requires padding, use explicit `vec4 _pad` once, not four dead post vectors.

### Acceptance

- [ ] `sizeof(FrameUBO)` matches shader (document expected size; test asserts)
- [ ] Visual parity (fog/day/water params still update)
- [ ] No shader still references `postParams*`

### Risk

Medium if layout wrong (black screen / garbage lighting). Mitigate with size assert + careful field-by-field port.

### Estimated size

~150–300 LOC; many small GLSL edits.

---

## PR-D — Extract ShadowPass + OpaquePass

### Intent

First real split of `recordFrame`.

### New types

```cpp
// src/Renderer/ShadowPass.hpp
class ShadowPass {
  void init(VkContext&, ...);
  void createResources(); // cascade array + layer views + sampler
  void createPipeline(GraphicsPipelineBuilder&, VkDescriptorSetLayout...);
  void record(VkCommandBuffer, const std::vector<Chunk*>&,
              const std::array<glm::mat4,3>& cascades, float time);
  VkImageView arrayView() const; // for set1
  VkSampler sampler() const;
};

// src/Renderer/OpaquePass.hpp
class OpaquePass {
  void createPipeline(...);
  void record(VkCommandBuffer, VkExtent2D, VkDescriptorSet set0, VkDescriptorSet set1,
              AllocatedImage& hdr, AllocatedImage& depth,
              const std::vector<Chunk*>&, OverlayRenderer&, clearColor);
};
```

### Modify

| File | What |
|------|------|
| `TerrainRenderer.cpp` | `recordFrame` calls `m_shadow.record` then `m_opaque.record`; delete inlined bodies |
| `TerrainRenderer::createPipelines` | Delegate |
| Descriptors set1 | Shadow views/sampler still written by Terrain or ShadowPass::writeDescriptors |

### Acceptance

- [ ] CSM still soft-blends; no acne regression vs baseline screenshot
- [ ] Opaque + overlays still draw
- [ ] `TerrainRenderer.cpp` **≤ 900** lines

### Risk

Low–medium. Pipeline format mismatch is the main footgun (HDR/depth formats from PostStack).

### Estimated size

~400 LOC moved; net complexity down.

---

## PR-E — Extract WaterPass + scene history ownership

### Intent

Isolate water refraction history and transparent pass.

### New type

```cpp
// src/Renderer/WaterPass.hpp
class WaterPass {
  void init(...);
  void resize(uint32_t w, uint32_t h, VkFormat hdrFmt, VkFormat depthFmt);
  void createPipeline(...); // set0+set1+set2
  void writeSceneDescriptors(historyColor, historyDepth, sampler);
  // Records: barrier+copy HDR/depth → history, then water draw sorted back-to-front
  void record(VkCommandBuffer, VkExtent2D, sets, AllocatedImage& hdr, AllocatedImage& depth,
              const std::vector<Chunk*>&, glm::vec3 camPos);
};
```

Move from TerrainRenderer:

- `m_sceneHistory`, `m_depthHistory`, `m_sceneSampler`
- set2 layout + descriptors
- water pipeline + layout
- entire water section of `recordFrame` (~200 lines)

### Acceptance

- [ ] Refraction UV still correct (no Y-flip regression)
- [ ] Foam/depth history still works
- [ ] Sort order preserved (far → near)
- [ ] `TerrainRenderer.cpp` **≤ 700** lines

### Risk

Medium (barriers around history copy). Use PR-A helpers only.

---

## PR-F — Thin orchestrator + PostStack defaults + sky ownership

### Intent

`TerrainRenderer::recordFrame` becomes ~80–120 lines of sequencing. Post “disabled” paths stop doing full clears.

### Orchestrator sketch

```cpp
void TerrainRenderer::recordFrame(VkCommandBuffer cmd, uint32_t frameIndex, ...) {
  preRecord?.(cmd);
  m_shadow.record(...);
  m_opaque.record(...);
  m_water.record(...);
  m_post.recordSky(...);
  // sun screen math → small helper tier1::sunScreenUv(...)
  m_post.recordPost(...);
  if (imguiDraw) recordImGuiOnSwapchain(...);
  transitionSwapchainToPresent(...);
}
```

### PostStack cleanup

- Create once at init: `m_defaultWhite` (R8 or RGBA8 1×1), `m_defaultBlack` HDR 1×1  
- When SSAO/bloom/god rays disabled: **skip** `fsDraw` for that effect; composite binds default image (descriptor rewrite once when toggle changes, or always bind defaults into unused slots)  
- Prefer: update composite descriptor set only when settings flags change (cache last flags)

### Sky

Optional in this PR: move `recordSky` + sky VBO ownership comments so sky is clearly a PostStack “scene attach” or a tiny `SkyPass` called from orchestrator. Do not leave sky half-owned without a comment block.

### Rename (optional)

- Class stays `TerrainRenderer` **or** rename to `WorldRenderer` in this PR only if Engine includes are updated in the same commit.

### Acceptance

- [ ] `TerrainRenderer.cpp` **≤ 450** lines  
- [ ] Toggling SSAO/bloom/god rays off does not spend full clear passes (profile or log draw counts)  
- [ ] ImGui still composites correctly  
- [ ] Present barrier correct  

### Risk

Medium for descriptor updates when toggles change mid-flight — either double-buffer sets or update only when idle/frame boundary.

---

## PR-G — Material flags (wind / emissive / ice)

### Intent

Delete magic texture ordinals from shaders; fix class of bugs like “wind on ground.”

### Design

**CPU material table** (static array indexed by `TextureType`):

```cpp
struct MaterialInfo {
  uint8_t flags; // FOLIAGE_WIND=1, EMISSIVE=2, ICE_SPEC=4, ...
  float emissive;
  float windStrength;
};
const MaterialInfo kMaterials[COUNT];
```

**Mesh packing options**

1. **Minimal change:** keep texture index in vertex; shaders sample a small UBO/SSBO material table bound once per frame.  
2. **Pack flags in unused vertex bits** if any remain (crowded today).

Recommend **(1)** UBO `MaterialTable` (256 × vec4) updated once at init (static).

### Shader changes

| Shader | Change |
|--------|--------|
| `terrain.vert` / `shadow.vert` | `if ((flags & FOLIAGE_WIND) != 0) applyWind(windStrength)` |
| `terrain.frag` | emissive + ice from table, not `if (t==23)` |

### Files

- `src/Renderer/MaterialTable.hpp` (+ init from TextureType)  
- Align with `tier1::emissiveIntensityForBlock` — **one source**, table filled from same functions  
- Tests: table[STONE].wind == 0, table[OAK_LEAVES].wind > 0, table[REDSTONE_ORE].emissive > 0  

### Acceptance

- [ ] Only leaves (and future flagged materials) sway  
- [ ] No hard-coded `8`, `13`, `23` in GLSL for policy  
- [ ] Ore emissive parity  

### Risk

Low if table is static and bound correctly.

---

## PR-H — Lighting model centralization

### Intent

Cave fill, sunReach, light curve, cool-shadow gate live as a named model, not ad-hoc multiplies.

### Changes

| Location | What |
|----------|------|
| `Tier1Graphics.hpp` | Document + implement CPU mirrors: `localLightScale`, `sunShadowWeight(skyL)`, `caveFillRgb`, constants `kCaveLightFloor` |
| `terrain.frag` | Call structure matching helpers (same formulas; comments point to tier1 names) |
| `tests/test_tier1_graphics.cpp` | Expand: sunReach 0 when sky 0; cave fill > 0; outdoor shadow weight ~1 at sky 1 |
| `composite.frag` | SSAO floor constant named; optional tie to tier1 constant via comment + test only |

### Out of scope

- New light propagation quality beyond current BFS  
- Deferred lighting  

### Acceptance

- [ ] Cave readability preserved vs current main  
- [ ] Outdoor not milky (mid fog + SSAO caps still tested)  
- [ ] Helpers and shader constants cannot drift without failing a test (at least floor/curve)  

---

## PR-I (optional) — Quality presets

### Intent

Replace proliferation of independent post toggles with Low / Medium / High / Cinematic.

### Changes

- `PostProcessSettings::applyPreset(Preset)`  
- GameUI combo; still allow advanced collapse for power users  
- No new effects  

Can wait until after G/H.

---

## Suggested PR titles & commit messages

| PR | Title |
|----|--------|
| A | `refactor(vk): shared image barriers and graphics pipeline builder` |
| B | `refactor(vk): single FrameSync path via VkFrameContext` |
| C | `refactor(vk): single FrameUBO contract; drop dead postParams` |
| D | `refactor(render): extract ShadowPass and OpaquePass` |
| E | `refactor(render): extract WaterPass and scene history` |
| F | `refactor(render): thin frame orchestrator; post default images` |
| G | `refactor(render): material table for wind and emissive` |
| H | `refactor(render): centralize cave/outdoor lighting model` |
| I | `feat(ui): graphics quality presets` |

---

## Per-PR test checklist (copy into PR body)

```markdown
- [ ] `cmake --build build-vk -j` succeeds on macOS
- [ ] `ctest --output-on-failure` (or `make test`)
- [ ] Launch game, seed fixed if possible
- [ ] Outdoor noon: shadows + grass color OK
- [ ] Cave: readable rock, not pure black; no full-bright wash
- [ ] Water shore: refraction not mirrored; foam present
- [ ] Toggle SSAO / bloom / god rays
- [ ] Resize window / fullscreen once
- [ ] Line count TerrainRenderer.cpp: ____ (must not increase vs parent)
```

---

## File ownership map (end state)

| Module | Owns |
|--------|------|
| `VkContext` | instance, device, queues, VMA |
| `VkSwapchain` | images, views, recreate |
| `VkFrameContext` | FIF sync, cmd buffers, acquire/present |
| `ImageBarrier` / `VkImage` | layout transitions |
| `GraphicsPipelineBuilder` | pipeline create boilerplate |
| `FrameUBO.hpp` | per-frame camera/light contract |
| `MaterialTable` | wind/emissive/ice policy |
| `ShadowPass` | CSM map + record |
| `OpaquePass` | terrain+overlay into HDR |
| `WaterPass` | history + water |
| `PostStack` | HDR targets, sky attach, SSAO/bloom/shafts/composite |
| `TerrainRenderer` / WorldRenderer | init order, set layouts, **sequence only** |
| `Engine` | loop, camera, chunk lists, ImGui, settings |

---

## What not to do

- Do not add SSR/TAA in the middle of A–F.  
- Do not “just extract a 200-line lambda” without a type boundary.  
- Do not keep `VkFrameContext` and Terrain acquire in parallel “for later.”  
- Do not regenerate SPIR-V in PR description without CI/build step — CMake already compiles shaders.  
- Do not grow `recordFrame` with more inline barriers after PR-A.

---

## Effort estimate (solo, part-time)

| PR | Effort |
|----|--------|
| A | 0.5–1 day |
| B | 1 day |
| C | 0.5–1 day |
| D | 1 day |
| E | 1 day |
| F | 1 day |
| G | 0.5–1 day |
| H | 0.5 day |
| I | 0.5 day |
| **Total** | **~6–9 days** |

---

## Immediate next step

Implement **PR-A** first (barriers + pipeline builder). It unblocks every later extraction and reduces merge conflict surface when moving `recordFrame` chunks.

If using Graphite/stacked PRs: open A as base of stack; land before starting D.
