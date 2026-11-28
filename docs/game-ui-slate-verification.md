# Slate player UI verification

[Phase B #327](https://github.com/gkehren/ft_vox/issues/327) · [Epic #325](https://github.com/gkehren/ft_vox/issues/325) · [Approved direction](game-ui-visual-direction.md) · [Live design system](game-ui-design-system.md)

Phase B applies Slate through the shared native components. Inventory
continues to use the existing slot identities, drag planning and session
command path. The independently labeled focus detail and drop action show
which stack the action targets. The crafting gallery groups input and output
using those same slots and the existing tooltip.

## Native captures

The paired captures use the same native fixtures, Low graphics preset,
seed 42, viewport, scale and input checkpoints. The pre-Slate inventory images captured
the presented frames. The corrected inventory, HUD and gallery images use
the renderer readback from the same fixtures. The readback excludes desktop
overlays. Day and night
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
Cobblestone x40 explicitly in the drop action. Its
[pre-correction PNG](images/game-ui-slate/inventory-compact-before-correction.png)
and the [corrected PNG](images/game-ui-slate/inventory-compact-corrected.png)
show the change in item size at the same checkpoint. Ordinary icons now
draw at 26 units, with at least 22 for the standard selected and focused
slot. A count may cover the lower-right corner. Semantic and interaction
cues use their own rail, and any progress track has its own band.
[Native compound-state detail](images/game-ui-slate/compound-gallery.png)

When a compact or standard summary elides feedback or recovery, **View
feedback** opens a paged reader in place of the grids. Recovery comes
first. Tab and Enter reach the same controls as the mouse. Page Up,
Page Down and the wheel move between pages without submitting an inventory
action. **Back to items** restores the slots. **Close** and an authorized
**Drop** remain reachable. The reader keeps an owned copy for its open
session, and screen closure or an inventory epoch change clears it.
Pending slots keep their semantic status and selected items while all
blocked slots suppress hover, press and focus cues. Keyboard navigation
resumes from remembered focus after the result. Pointer and keyboard
choose one tooltip source at a time, and a held stack suppresses ordinary
tooltips.

The trailing-value regression reproduced a measured width of 9.030666
logical units as 9.030640 after subtracting the edges of a clip ending at
700. The 0.001-unit fitting tolerance absorbs roundoff across the shared
text path. The row reserves two scaled logical units of slack and caps the
value lane at half the row. The actual clip still bounds every glyph.
HUD and tooltips already allocate scaled padding around measurements.
Buttons fit into a padded rectangle.

## Lossless typography and startup recovery

The native typography proof renders the same roles, text and viewport for
both bakes at 1× framebuffer density. Comparing the native 15 px PNGs
with the original WebPs confirmed the visible softness was present before
WebP conversion. The 36 px fixed bake sharpens edges across the three
scales and preserves one atlas and two weight tables. UI zoom can reuse it,
so scale-dependent tiers and separate per-role rasters were unnecessary
for these captures.

| UI zoom | Original 15 px bake | Corrected 36 px bake |
| --- | --- | --- |
| 100% | [PNG](images/game-ui-slate/typography-15-100.png) | [PNG](images/game-ui-slate/typography-36-100.png) |
| 150% | [PNG](images/game-ui-slate/typography-15-150.png) | [PNG](images/game-ui-slate/typography-36-150.png) |
| 200% | [PNG](images/game-ui-slate/typography-15-200.png) | [PNG](images/game-ui-slate/typography-36-200.png) |

The 1× allocation measured 16 KiB at 256×64 and 64 KiB at 512×128.
Higher framebuffer densities use up to 72 raster pixels per em with the
existing 512×1024 limit. The graphical executable and native proof target
both deploy the pinned fonts and license. The loader reports the exact
failed path and distinguishes absent files, invalid lengths, read failures,
damaged data and rasterization failure. Startup can explicitly fall back
to the embedded MIT ProggyClean face. A subsequent failed density
replacement keeps both working glyph tables and GPU image.

## Executed checks

| Environment | Result |
| --- | --- |
| Windows MSVC Release, graphics off | `GameUiStyle`, `GameUiComponents`, `GameUiInteraction`, `GameUiDrawList`, `MultiplayerUi`, `CoreIncludeGuard`, and `HeadlessDeps` passed. The layout test checks all twelve extent/scale combinations and all 36 slot identities in each. |
| Debian WSL2, GCC Debug, ASan/UBSan | `GameUiStyle`, `GameUiComponents`, `GameUiInteraction`, and `GameUiDrawList` passed. |
| Windows MSVC Release, RTX 4070 Ti/621.24, Vulkan 1.4.357, SDK validation layer 1.3.290 | Game, server and engine proof compiled. Inventory (74 checks), HUD (87), gallery (104), typography (5), inline IME showcase (115), Settings (58), WSI (39) and offscreen compositing (25) passed. The native inventory proof checks all twelve combinations, actual held-stack and drop gestures, atlas reload, compact feedback paging and tooltip precedence. Gallery checks both face weights in one image plus failed and repeated density replacements. |
| Two native clients and dedicated server | The initial Slate revision passed `test_hosted_closure_native.py --full-ui` through checkpoint restart. The first correction run timed out at the owner ban step while the GPU was saturated. A quiet rerun is pending. |
| Font startup and deployment | The game and native fixture outputs contained the two matching TTFs and the OFL notice. The typography probe rejected missing, truncated and same-size damaged assets. A separate no-font package logged the missing Regular path, started with embedded ProggyClean and passed the 114-check showcase. |
| Vulkan validation | The passing native runs reported only known RTSS swapchain/image-view VUIDs `01778` and `02275`. With synchronization validation enabled, the injected overlay's `vkCmdEndRenderPass` produced present-after-write reports on both the pre-correction and corrected binaries. The local pass therefore used standard validation with synchronization validation off. [Environment notes](vulkan-validation.md) |

The visual inspection used fixture output from the physical GPU. A fresh
full CPU-suite run, graphical Linux and macOS device runs, interactive
OS IME and different-DPI monitor transitions were outside the local pass.

## Renderer budgets

The comparison uses the [#322 baseline](game-ui-closure.md#release-timing-on-the-rtx-4070-ti)
and the same Release timing fixture at 1920×1080 with seed 42, Low graphics
and VSync off. Each measured mode has 600 tagged GPU samples after warmup.
Each active player mode supplied 300 samples per renderer slot. p95 values
are in milliseconds. The initial Slate column shows the range across two runs before the review corrections.
Each HUD value uses the highest attributed p95 across HUD with and without
the developer workspace in that run.

| Mode | #322 p95 CPU / GPU | Initial Slate p95 CPU / GPU | Initial Slate draws / vertices / dropped | ADR ceiling CPU / GPU / draws |
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
Whole-frame p95 varied between the two scene-only samples. The corrected
1× atlas measurement above comes from the isolated typography test.
The first corrected frame-timing run had 68.5 ms scene-only GPU p95 under
external contention and was stopped after the budget check failed. It
needs a quiet rerun for a comparable correction measurement.

## Reproduce

Build the Release game and `test_engine_reload`. From the game's output
directory set `FT_VOX_VALIDATION=1`. The local run used
`FT_VOX_SYNC_VALIDATION=0` for the overlay environment above.
Use the [native fixture setup](game-ui-closure.md#reproduce).

```powershell
../tests/Release/test_engine_reload.exe --player-hud-proof <out>/hud
../tests/Release/test_engine_reload.exe --game-ui-design-system <out>/gallery
../tests/Release/test_engine_reload.exe --game-ui-typography <out>/typography
../tests/Release/test_engine_reload.exe --player-inventory-proof <out>/inventory
```

The inventory readback files correspond to standard, ultrawide held,
and compact after reload. The fixture also renders the compact reader.
Use an isolated output for the typography probe, which writes malformed
font fixtures alongside its PNGs. For timing unset the capture layer, set `FT_VOX_VALIDATION=0`,
`FT_VOX_SYNC_VALIDATION=0` and `FT_VOX_GPU_PROFILING=1`, then run
`test_engine_reload --game-ui-performance`.
