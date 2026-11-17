# GPU validation profile (stabilization Phase 6)

Authoritative procedure for executing the Vulkan device tests and the
visual-regression suite on a physical GPU. The CPU-only CI gate
(`.github/workflows/cpu-boundary.yml`) **compiles** the graphical stack but
does not execute device tests — a compilation-only job is not proof of
successful runtime rendering. This profile is the runtime proof, executed on
the canonical reference environment below (or an explicitly documented
equivalent).

## 1. Reference environment (canonical)

| Property | Value |
|---|---|
| OS | Windows 11 (x64) |
| GPU | NVIDIA GeForce RTX 4070 Ti (discrete) |
| Display driver | 32.0.16.2124 (NVIDIA) |
| Vulkan implementation | NVIDIA ICD, instance API 1.4.357, device API 1.4.357 |
| Vulkan driver version | 621.24.0.0 (ICD encoding) |
| CPU / RAM | AMD Ryzen 7 9800X3D (8C/16T), 28 GB |
| Compiler / build | MSVC (Visual Studio 18 2026), `Release`, vcpkg toolchain (`build/`) |
| Validation layers | LunarG `VK_LAYER_KHRONOS_validation` via `FT_VOX_VALIDATION=1` (Debug default; Release opt-in) |
| Golden resolution | 640×360, 8-bit sRGB composite (`R8G8B8A8_SRGB`/`B8G8R8A8_SRGB`) |
| Injected overlays | RTSS / OBS / Galaxy hooks may be present — the harness treats device-init overlay errors as a warned baseline; everything from `WorldRenderer::init` onward must be validation-clean (`docs/vulkan-validation.md`) |

The committed golden references (`tests/visual-references/`) are produced on
this exact stack (`docs/visual-regression.md` §6). A different GPU/driver is
a **different environment**: run smoke mode there, never regenerate goldens
without the explicit review policy.

Record the environment with every run:

```powershell
vulkaninfo --summary   # instance/device API + driver versions
Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion
```

## 2. Build

```powershell
cmake -B build -DCMAKE_TOOLCHAIN_FILE=D:/Projects/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release --parallel `
  --target ft_vox ft_vox_visual_tests test_vulkan_resources test_mesh_upload `
           test_mob_render test_post_resize test_engine_reload test_world_persistence
```

## 3. Test commands (run from the repo root so `./ressources/` resolves)

| Test | Command | Proves |
|---|---|---|
| Vulkan resource smoke | `ctest --test-dir build -C Release -R VulkanResourceSmoke --output-on-failure` | device/swapchain/buffer/image helpers, validation-error reporting probe |
| Mesh upload lifecycle | `ctest --test-dir build -C Release -R MeshUploadLifecycle --output-on-failure` | staging-full deferral, retry, partial rollback, old GPU mesh untouched |
| Mesh arena | `ctest --test-dir build -C Release -R MeshArena --output-on-failure` | suballocation, frame-aware retirement, page growth/exhaustion |
| Mob Vulkan render | `ctest --test-dir build -C Release -R MobVulkanRender --output-on-failure` | mob pipelines, texture rollback, frame slots (validation on) |
| Post resize formats | `ctest --test-dir build -C Release -R PostResizeFormats --output-on-failure` | swapchain format/color-space change keeps pipeline layouts valid |
| Visual regression (smoke) | `ctest --test-dir build -C Release -R VisualRegression --output-on-failure` | all golden scenes + numeric invariants, relaxed cross-vendor tolerances |
| Visual regression (STRICT gate) | `.\build\tests\Release\ft_vox_visual_tests.exe --strict` | canonical-GPU golden gate — mandatory before reference updates |
| Gameplay item rendering checks | `.\build\tests\Release\ft_vox_visual_tests.exe --scene item_rendering_checks` | interpolation, frustum culling, epoch reset, atlas reload, population cap (semantic, no goldens) |
| Gameplay item goldens | `.\build\tests\Release\ft_vox_visual_tests.exe --scene item_population --scene item_max_population` | 128 / 1024 visible dropped items |
| Engine reload lifecycle | `ctest --test-dir build -C Release -R EngineReload --output-on-failure` | persistent world × reloadWorld on a real device (skips 77 without a window) |
| World persistence (device-adjacent) | `ctest --test-dir build -C Release -R WorldPersistence --output-on-failure` | save service + bridge unload/reload integration |

`FT_VOX_VALIDATION=1` is set by the ctest registrations that require it;
for direct runs set it explicitly:

```powershell
$env:FT_VOX_VALIDATION = "1"
.\build\tests\Release\ft_vox_visual_tests.exe --strict
```

## 4. Gameplay rendering coverage (issue #229 / stabilization Phase 6)

Golden scenes (pixel comparison + numeric invariants, entity-free base
frames prove the items contribute pixels):

- `item_entities` — 8 mixed block items on a lit grass pad (per-instance
  atlas layers, derived local light, presentation spin).
- `item_population` — **128** items, mixed types, grid on a large pad;
  hot-pixel count far above the 8-item scene; sunlit item luma bound.
- `item_max_population` — **1024** items (the sim/renderer cap), grid over a
  32×32 pad; the instanced draw stays validation-clean at the cap.

Standalone semantic checks (`item_rendering_checks`, always part of the
suite, no goldens):

- **Interpolation**: the same population rendered at the previous vs current
  snapshot positions (plus spin/bob delta) must change pixels — a renderer
  ignoring interpolation would render identical frames.
- **Frustum culling**: the whole population placed behind the camera must
  contribute **zero** pixels (`visibleItems() == 0`, frame bit-identical to
  the entity-free base).
- **Epoch reset / world reload**: render items → `beginScene` (the
  reload-world ordering) → render empty: bit-identical to a fresh empty
  render — no stale instance slice survives the world transition.
- **Atlas / resource-pack reload**: render items → production
  `WorldRenderer::reloadResourcePack` (failure-atomic, set-1 rebind) →
  render items: bit-identical output, zero new validation errors — items
  sample the shared atlas descriptors, so a reload must rebind them.
- **Population cap**: 1024 items prepare/record cleanly; `visibleItems()`
  never exceeds the cap.

Hotbar/inventory **UI** smoke: the ImGui-free visual harness deliberately
does not render UI. The panel/HUD logic is covered CPU-only by
`test_ui_shell` (InventoryUi layout/cursor/click planning) and
`test_input_routing`; the interactive GPU smoke is a manual checklist
(open `E` panel, drag/split/swap, hotbar selection, resource-pack apply with
items visible) recorded in the run log.

## 5. Vulkan validation checklist

Every device run above executes with validation enabled (except the
device-init overlay baseline). The suites specifically exercise:

- buffer allocation/ownership (VMA) — resource smoke, mesh upload/arena;
- instance-buffer updates — mob/item renderers' per-frame instance slices;
- descriptor lifetime — set-1 atlas rebind on pack reload + shadow resize;
- synchronization — frames-in-flight slots, synchronous readback fences;
- resource retirement — deferred GPU unload, mesh arena frame-aware retire;
- reload/device teardown — `beginScene` world switching, `EngineReload`,
  harness shutdown; repeated world switching is the epoch-reset check;
- item renderer cleanup — harness shutdown after item scenes.

**Zero validation errors** from `WorldRenderer::init` onward is the gate;
any error fails the run (the harness counts them per stage).

## 6. CI strategy

- Normal CPU CI (`cpu-boundary.yml`) never depends on a GPU — unchanged.
- `.github/workflows/gpu-validation.yml` is the **optional** runtime gate:
  `workflow_dispatch` (manual) or a self-hosted GPU runner label. It builds
  the graphical stack, runs the device tests + visual regression, and
  publishes artifacts (test logs, `visual-qa/` screenshots/diffs/metrics,
  validation output, GPU/driver summary). Without a GPU runner it is simply
  not triggered — CPU CI stays green and the GPU tests are reported as
  **not executed**, never as passed.
- A software rasterizer (lavapipe) can run the harness on a GPU-less runner
  in smoke mode once available; strict goldens remain canonical-GPU-only.

## 7. Failure artifacts

On any mismatch the harness writes `build/visual-qa/<scene>/`:
`actual.png`, `expected.png`, `diff.png` (×8 amplified), `metrics.txt`,
`errors.txt` (`docs/visual-regression.md` §8). The GPU workflow uploads the
whole directory plus the ctest logs and the `vulkaninfo --summary` capture.

## 8. Execution log

| Date | Environment | Scope | Result |
|---|---|---|---|
| 2026-11-18 | canonical (RTX 4070 Ti, driver 32.0.16.2124, Vulkan 1.4.357, MSVC Release) | stabilization Phase 6: device tests + visual regression (smoke + strict) + gameplay item checks | **PASSED** — PR #244 / `benchmarks/results/gpu-validation-2026-11-18.md` |
