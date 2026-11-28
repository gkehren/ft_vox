# Slate player UI verification

[Phase B #327](https://github.com/gkehren/ft_vox/issues/327) · [Epic #325](https://github.com/gkehren/ft_vox/issues/325) · [Approved direction](game-ui-visual-direction.md) · [Live design system](game-ui-design-system.md)

Phase B applies Slate through the shared native components. Inventory
continues to use the existing slot identities, drag planning and session
command path. The independently labeled focus detail and drop action show
which stack the action targets. The crafting gallery groups input and output
using those same slots and the existing tooltip.

## Native captures

The paired captures use the same native fixtures, Low graphics preset,
seed 42, viewport, scale and input checkpoints. The inventory proof captures
the presented swapchain frames. HUD and gallery captures use the native
renderer readback from the proof's camera and composition. Day and night
use the same camera and fixed fixture times. Streaming and live scene
sampling can vary between runs. The original PNG and PPM output was
converted to WebP for this visual comparison.

| Specimen | Before | Slate |
| --- | --- | --- |
| Day HUD · 1280×720 · 100% | [Before](images/game-ui-slate/hud-day-before.webp) | [Slate](images/game-ui-slate/hud-day-after.webp) |
| Night HUD · 1280×720 · 100% | [Before](images/game-ui-slate/hud-night-before.webp) | [Slate](images/game-ui-slate/hud-night-after.webp) |
| Inventory and tooltip · 1920×1080 · 100% | [Before](images/game-ui-slate/inventory-16x9-before.webp) | [Slate](images/game-ui-slate/inventory-16x9-after.webp) |
| Ultrawide held stack · 2560×1080 · 100% | [Before](images/game-ui-slate/inventory-21x9-held-before.webp) | [Slate](images/game-ui-slate/inventory-21x9-held-after.webp) |
| Compact inventory after atlas reload · 960×540 · 200%, 2× density override | [Before](images/game-ui-slate/inventory-compact-200-before.webp) | [Slate](images/game-ui-slate/inventory-compact-200-after.webp) |
| Crafting preview and tooltip · 1280×720 · 100% | [Before](images/game-ui-slate/crafting-standard-before.webp) | [Slate](images/game-ui-slate/crafting-standard-after.webp) |
| Compact crafting preview · 960×540 · 100% | [Before](images/game-ui-slate/crafting-compact-before.webp) | [Slate](images/game-ui-slate/crafting-compact-after.webp) |
| High-scale crafting preview · 1920×1080 · 200% | [Before](images/game-ui-slate/crafting-200-before.webp) | [Slate](images/game-ui-slate/crafting-200-after.webp) |

The compact inventory witness focuses Sand x10 and names the selected
Cobblestone x40 explicitly in the drop action. Compact feedback uses a
dedicated two-line region alongside the pinned action, with a separate
recovery line and a hover tooltip for longer feedback. It fits both without
adding an inventory scroll interaction. The hold preview still follows the
active pointer or keyboard anchor and clamps to the existing game view.

## Executed checks

| Environment | Result |
| --- | --- |
| Windows MSVC Release, graphics off | `GameUiStyle`, `GameUiComponents`, `GameUiInteraction`, `GameUiDrawList`, `MultiplayerUi`, `CoreIncludeGuard`, and `HeadlessDeps` passed. The layout test checks all twelve extent/scale combinations and all 36 slot identities in each. |
| Debian WSL2, GCC Debug, ASan/UBSan | `GameUiStyle`, `GameUiComponents`, `GameUiInteraction`, and `GameUiDrawList` passed. |
| Windows MSVC Release, RTX 4070 Ti/621.24, Vulkan 1.4.357, SDK validation layer 1.3.290 | Game, server and engine proof compiled. Inventory (63 checks), HUD (87), gallery (104), inline IME showcase (115), Settings (58), WSI (39) and offscreen compositing (25) passed. The native inventory proof checks all twelve combinations, actual held-stack and drop gestures, and atlas reload. Gallery checks both face weights in one image plus failed and repeated density replacements. |
| Two native clients and dedicated server | `test_hosted_closure_native.py --full-ui` passed menus, actor-scoped inventory, pending and stale results, and checkpoint restart on loopback. |
| Validation and synchronization validation | The native logs only reported the known RTSS swapchain/image-view VUIDs `01778` and `02275`. [Environment notes](vulkan-validation.md) |

The visual inspection used fixture output from the physical GPU. A fresh
full CPU-suite run, graphical Linux and macOS device runs, interactive
OS IME and different-DPI monitor transitions were outside the local pass.

## Renderer budgets

The comparison uses the [#322 baseline](game-ui-closure.md#release-timing-on-the-rtx-4070-ti)
and the same Release timing fixture at 1920×1080 with seed 42, Low graphics
and VSync off. Each measured mode has 600 tagged GPU samples after warmup.
Each active player mode supplied 300 samples per renderer slot. p95 values
are in milliseconds. The Slate columns show the range across two runs.
Each HUD value uses the highest attributed p95 across HUD with and without
the developer workspace in that run.

| Mode | #322 p95 CPU / GPU | Slate p95 CPU / GPU | Peak draws / vertices / dropped | ADR ceiling CPU / GPU / draws |
| --- | --- | --- | --- | --- |
| HUD | 0.0130 / 0.0041 | 0.0096–0.0186 / 0.0041–0.0061 | 1 / 618 / 0 | 0.25 / 0.20 / 8 |
| Inventory | 0.0234 / 0.0236 | 0.0156–0.0284 / 0.0236 | 1 / 2,142 / 0 | 0.50 / 0.35 / 32 |

HUD and inventory p95 CPU varied across the two runs, with the higher
values 5–6 microseconds above #322. Both runs were within the budgets.
Peak vertices fell from 936 to 618
in the HUD and from 3,072 to 2,142 in inventory. Four descriptor sets and
208 KiB of instance storage matched the baseline. The 1× font image used
16 KiB compared with 32 KiB in #322. Native density replacement returned to
the initial allocation. sRGB composition used zero intermediate bytes.
Whole-frame p95 varied between the two scene-only samples.

## Reproduce

Build the Release game and `test_engine_reload`. From the game's output
directory set `FT_VOX_VALIDATION=1` and `FT_VOX_SYNC_VALIDATION=1`.
Use the [native fixture setup](game-ui-closure.md#reproduce).

```powershell
../tests/Release/test_engine_reload.exe --player-hud-proof <out>/hud
../tests/Release/test_engine_reload.exe --game-ui-design-system <out>/gallery
../tests/Release/test_engine_reload.exe --player-inventory-proof
```

For inventory screenshots rerun with validation disabled and the SDK
screenshot layer on `VK_INSTANCE_LAYERS`. Set `VK_SCREENSHOT_DIR` to an
existing directory and `VK_SCREENSHOT_FRAMES=3,23,74`. Check the
`INVENTORY capture=` markers when using a different revision or driver.
The three marks correspond to standard, ultrawide held and compact after
reload. For timing unset the capture layer, set `FT_VOX_VALIDATION=0`,
`FT_VOX_SYNC_VALIDATION=0` and `FT_VOX_GPU_PROFILING=1`, then run
`test_engine_reload --game-ui-performance`.
