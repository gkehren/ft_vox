# GPU validation execution log — 2026-11-18 (stabilization Phase 6)

Executed on the **canonical reference environment** (`docs/gpu-validation.md`):

| Property | Value |
|---|---|
| OS | Windows 11 x64 |
| GPU | NVIDIA GeForce RTX 4070 Ti (discrete) |
| Display driver | 32.0.16.2124 |
| Vulkan | NVIDIA ICD, instance + device API 1.4.357, driver 621.24.0.0 |
| CPU / RAM | AMD Ryzen 7 9800X3D (8C/16T), 28 GB |
| Build | MSVC (VS 18 2026) Release, vcpkg toolchain (`build/`) |
| Validation | `FT_VOX_VALIDATION=1` (LunarG Khronos layer) |
| Injected overlays | RTSS / OBS / Galaxy present — 7 device-init validation errors tolerated as the documented baseline; **zero** errors from `WorldRenderer::init` onward |
| Code under test | stabilization stack PR #238–#243 + Phase 6 (branch `test/gpu-validation-profile`) |

## Results

| Suite | Command | Result |
|---|---|---|
| Vulkan resource smoke + error-reporting probe | `ctest -R VulkanResourceSmoke\|VulkanValidationErrorReporting` | **PASSED** |
| Mesh upload lifecycle | `ctest -R MeshUploadLifecycle` | **PASSED** (2.45 s) |
| Mesh arena | `ctest -R MeshArena` | **PASSED** (0.98 s) |
| Mob Vulkan render (validation on) | `ctest -R MobVulkanRender` | **PASSED** (2.52 s) |
| Post resize formats (validation on) | `ctest -R PostResizeFormats` | **PASSED** (1.28 s) |
| World persistence (bridge integration, checkpoints active) | `ctest -R WorldPersistence` | **PASSED** (5.34 s) |
| Engine reload lifecycle (real window + device) | `ctest -R EngineReload` | **PASSED** (5.17 s) |
| Gameplay item rendering checks | `ft_vox_visual_tests --scene item_rendering_checks` | **PASSED** — interpolation moves pixels; culling bit-identical to entity-free base with `visibleItems()==0`; resource-pack reload bit-identical; 1024-instance cap clean; epoch reset leaves no stale instances; 0 new validation errors, 0 non-finite HDR samples |
| Visual regression — smoke (ctest) | `ctest -R VisualRegression` | **PASSED** (74.3 s, all scenes incl. `item_population`, `item_max_population`) |
| Visual regression — **STRICT canonical gate** | `ft_vox_visual_tests --strict` | **PASSED — 24 scenes within tolerance**, all standalone checks green (auto-exposure meter/adaptation, underwater optics/exposure, water-surface terms, godray alignment, AO motion, resize, item checks) |

## Golden references

- **No existing reference was modified.** The strict gate passes on driver
  32.0.16.2124 with the committed goldens (the `cave_emissive` driver refresh
  of issue #213 remains stable at this driver).
- **Two NEW references added** for the new Phase 6 scenes, generated explicitly
  on the canonical GPU per `docs/visual-regression.md` §6/§10:
  `tests/visual-references/item_population.png` (128 items) and
  `tests/visual-references/item_max_population.png` (1024 items, the cap).
  The `item_max_population` hot-pixel invariant was calibrated from the
  measured overview-distance pixel budget (~4k hot pixels; floor 3000 = 20×
  the 8-item scene's floor) — a new-scene calibration, not a relaxation of an
  existing gate.

## Gameplay UI smoke

The ImGui-free visual harness does not render UI (by design). CPU-only UI
logic gates (`UiShell` incl. InventoryUi layout/cursor/click planning,
`InputRouting` hotbar/E-panel shortcut policy) ran in the graphical suite.
Interactive GPU UI smoke (open `E`, drag/split/swap, hotbar selection,
resource-pack apply with items visible) is a manual checklist
(`docs/gpu-validation.md` §4); the atlas-reload-with-items path is covered
automatically by `item_rendering_checks`.

## Notes

- Device tests were **executed on a physical GPU**, not skipped: this machine
  is the canonical golden-reference stack.
- The optional CI workflow (`.github/workflows/gpu-validation.yml`) runs this
  profile on a self-hosted `gpu` runner or manual dispatch; GitHub-hosted
  CPU CI never depends on it (device tests there are *not executed*, never
  reported as passed).
