# Game UI and Developer UI verification

[Epic #298](https://github.com/gkehren/ft_vox/issues/298) · [Phase K #309](https://github.com/gkehren/ft_vox/issues/309)

Game UI is the default on a regular launch. F1 opens the independent
Developer UI workspace. Both use Engine's input and Vulkan frame lifecycle.
The player-facing routes read bounded snapshots and submit typed requests
through Engine. The solo simulation or the dedicated server applies gameplay
rules, and the hosted local control channel verifies administrative authority.

## Migration matrix

| Scope | Merged implementation | Verification route |
| --- | --- | --- |
| Architecture, output and resource lifecycle | [#310](https://github.com/gkehren/ft_vox/pull/310), [#311](https://github.com/gkehren/ft_vox/pull/311) | `GameUiDrawList`, offscreen format and WSI proofs. See the [backend ADR](adr/game-ui-backend.md) and [renderer documentation](game-ui-renderer.md). |
| Navigation, input and component library | [#312](https://github.com/gkehren/ft_vox/pull/312), [#313](https://github.com/gkehren/ft_vox/pull/313) | `GameUiInteraction`, `GameUiComponents`, `GameUiStyle`, showcase and gallery proofs. See the [controls](game-ui-interaction.md) and [design system](game-ui-design-system.md). |
| Live HUD and inventory | [#315](https://github.com/gkehren/ft_vox/pull/315), [#316](https://github.com/gkehren/ft_vox/pull/316) | `GameUiComponents`, native HUD and inventory proofs, confirmed inventory in both hosted clients. |
| Local launch, worlds, pause and save recovery | [#318](https://github.com/gkehren/ft_vox/pull/318) | `GameUiInteraction`, `EngineReload`, native session menu proof with save and bootstrap fault injection. |
| Hosted and remote player flows | [#319](https://github.com/gkehren/ft_vox/pull/319) | `MultiplayerUi`, `MultiplayerJoin`, `MultiplayerIntents`, `ServerAdministration`, two-client native menu and checkpoint restart. |
| Player settings | [#320](https://github.com/gkehren/ft_vox/pull/320) | `GameUiStyle`, native Settings and Settings restart proofs. |
| Developer tools, docking and benchmark | [#321](https://github.com/gkehren/ft_vox/pull/321) | HUD/workspace proof exercises F1, panel shortcuts, dock reset and the Performance capture controls. See the [migration table](game-ui-migration.md#developer-ui-separation-308). |

The native routes replaced the ImGui player panels in #308. The widget strip,
design gallery, HUD mock and spike remain explicit demonstration and
regression entry points. Phase K removes the superseded Escape-to-quit
helper. The active screen stack and input router now supply its coverage.
Future crafting and container flows can compose the gallery's slot grid,
tooltip and confirmation with a screen-specific bounded snapshot and
authority-checked intents. Controller navigation, localization, crafting and
public networking remain outside this delivery. Real OS IME composition
coverage is tracked in [#314](https://github.com/gkehren/ft_vox/issues/314).

## Executed evidence

| Environment | Result |
| --- | --- |
| Windows, MSVC 14.52 Release, graphics off | 55 CTest entries passed, including the nested clean configure/build/test and core/headless guards. A targeted rerun covered the final preferences matrix. The optional Boost prototype was absent. |
| Debian 13 WSL2, GCC 14.2 Release, graphics off | 56 CTest entries passed, including the nested clean check, guards and the optional Boost prototype. The affected UI and preferences tests also passed in targeted reruns. |
| Debian 13 WSL2, GCC 14.2 Debug, ASan/UBSan | All seven affected Game UI and multiplayer presentation/intent tests passed. The build used the standard warning policy after an inherited warnings-as-errors cache rejected existing aggregate initializers. |
| Windows 11 build 26300, MSVC Release, Vulkan 1.4.357, RTX 4070 Ti/621.24 | Game, server and native fixtures compiled. Engine reload (33 checks), solo menus (35), HUD and developer workspace (87), inventory (51), Settings and restart (49 and 10), gallery (46), showcase (40), WSI (39) and offscreen composition (25) passed. |
| Two Windows clients and a dedicated child on loopback | Native menus passed 25 owner and 27 guest checks. The checkpoint fixture passed 74 owner and 32 guest checks on first launch and 15 owner and 17 guest checks after restart. Both clients checked private inventories, owner/delegate scope, shutdown and stale presentation cleanup. |

The initial solo inventory run lost synthetic clicks because the fixture
was flushing its own queued events. The fixture now filters desktop
pointer events while preserving queued test gestures. The two-client menu
proof also drives focus explicitly in both processes to avoid the window
manager cancelling one process's synthetic input. These tests exercise SDL
routing and native presentation. They use injected events.

Validation and synchronization validation were enabled for functional Vulkan
runs. RTSS was loaded and emitted the two known sRGB/STORAGE swapchain and
image-view VUIDs, `01778` and `02275`. The loader also reported Galaxy layer
naming messages. The logs contained no other VUIDs or synchronization hazards.
The offscreen UI probe checks exact scissor and overlay witness pixels.
The sRGB transfer witnesses and the untouched scene sample tolerate one
8-bit channel code of difference.
[Vulkan validation notes](vulkan-validation.md)

### Captures

| Capture | What to inspect |
| --- | --- |
| [Daylight HUD](images/game-ui-closure/player-hud.png) and [night HUD](images/game-ui-closure/player-hud-night.png) | Reticle and hotbar readability at the same camera pose |
| [Developer workspace](images/game-ui-closure/developer-workspace.png) | The live scene, dock layout and player HUD in the central view |
| [Crafting component preview](images/game-ui-closure/crafting-gallery.png) | Shared confirmation card, input slots and output state |
| [Owner confirmation](images/game-ui-closure/owner-kick-confirmation.png) and [guest roster](images/game-ui-closure/guest-roster.png) | The local owner's confirmation and the guest's read-only view |

The captures come from the native fixtures. Use them as visual evidence.
The isolated sRGB/UNORM probe supplies the automated pixel comparison.

### Release timing on the RTX 4070 Ti

The run collected 600 tagged GPU frames for each mode. Times in milliseconds
are median / p95. The scene-only baseline skips both UI record paths. The
visible Developer UI sample uses the default dock workspace reached by F1.

| Mode | CPU frame | GPU frame | Game UI CPU | Game UI GPU | Dev UI CPU | Dev UI GPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Scene only, first | 0.562 / 1.286 | 0.419 / 0.817 | — | — | — | — |
| HUD | 0.561 / 1.253 | 0.419 / 0.652 | 0.0081 / 0.0094 | 0.0031 / 0.0041 | — | — |
| HUD and Developer UI | 0.570 / 1.178 | 0.424 / 0.652 | 0.0077 / 0.0083 | 0.0031 / 0.0041 | 0.0299 / 0.0323 | 0.0041 / 0.0051 |
| HUD, after hiding Developer UI | 0.558 / 1.171 | 0.419 / 0.649 | 0.0088 / 0.0093 | 0.0031 / 0.0041 | — | — |
| Inventory | 0.585 / 1.228 | 0.439 / 0.661 | 0.0142 / 0.0151 | 0.0236 / 0.0236 | — | — |
| Scene only, last | 0.563 / 1.190 | 0.417 / 0.647 | — | — | — | — |

The HUD and inventory used one Game UI draw each, with 936 and 3,072
vertices respectively. All player modes used four descriptor sets,
208 KiB of instance buffers and a 32 KiB font allocation. The sRGB
swapchain needed zero composition bytes. The Developer UI sample peaked
at two indexed draw commands and 6,372 bytes of used vertex/index upload
data. Both player modes met their attributed budgets. The first baseline's
GPU frame p95 was 0.170 ms higher than the last baseline, so the attributed
scopes provide a more stable estimate of UI cost than whole-frame p95
subtraction. Repeated resize and atlas reload checks verified that the UI
resource footprint returned to its baseline.

## Reproduce

For both Windows and Linux, build the graphics-off configuration and run
CTest. The full suite includes the core and headless guards. Add
`-DFT_VOX_CPU_ONLY_BUILD_CHECK=ON` for the nested clean build check.

```sh
cmake -S . -B build-cpu -DFT_VOX_GRAPHICS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpu --config Release --parallel
ctest --test-dir build-cpu -C Release --output-on-failure
```

Use a separate GCC or Clang Debug build with `-DFT_VOX_SANITIZERS=ON`
for the affected sanitizer checks. Run CTest with
`-R 'GameUi|Multiplayer(Intents|Join|Ui)'`. The targets compile without
SDL, Vulkan or ImGui.

On Windows build `ft_vox`, `ft_vox_server` and `test_engine_reload`
in Release. From the game's output directory, run `test_engine_reload.exe`
with the arguments below. Use isolated output paths under the build
directory for evidence and fixture saves. On single-config builds the
test executable is in `tests/` under the build directory.

| Arguments | Purpose |
| --- | --- |
| No arguments | Persistent world reload, projection retirement and distant saved spawn |
| `--session-menu-proof <out>` | Native create/open/save/retry and return-to-menu |
| `--player-hud-proof <out>` | HUD, Developer UI, dock/cursor handoff, panel shortcuts, day and night captures |
| `--player-inventory-proof` | Native input, stale gestures, atlas and world epochs |
| `--player-settings-proof <out>`, then `--player-settings-restart-proof <out>` | Display confirmation and recovery across settings persistence |
| `--game-ui-design-system <out>` | Gallery and HUD mock at 16:9, 21:9 and compact 100/150/200% combinations |
| `--game-ui-present-proof <out>` and `--game-ui-screenshot <out>` | WSI resize, minimize and restore, resource reload, sRGB and offscreen UNORM composition |

Enable `FT_VOX_VALIDATION=1` and use the SDK validation layer for the
functional runs. From the repository root on Windows, run the two-client
sequence against a fresh destination.

```powershell
python tests/test_hosted_closure_native.py --full-ui `
  --server build/Release/ft_vox_server.exe `
  --client build/tests/Release/test_engine_reload.exe `
  --output build/ui-closure-hosted
```

For Release timing use `FT_VOX_VALIDATION=0`, `FT_VOX_SYNC_VALIDATION=0`,
`FT_VOX_GPU_PROFILING=1` and `--game-ui-performance`. The fixed seed-42 Low scene uses the same camera,
1920×1080 size and VSync off in each mode. The sequence brackets the UI
modes with a scene-only baseline. Each mode warms up for 120 frames and
samples 600 frames. The report includes frame medians and p95, attributed
CPU and GPU scopes, player draw count and instance/font/composition bytes,
and the Developer UI's peak indexed draw commands and used upload bytes.
The ImGui descriptor pool capacity is 256 sets. Used upload bytes describe
the geometry submitted, while the backend can retain a larger allocated
buffer. The player budget is the [backend ADR](adr/game-ui-backend.md#verification-and-budgets)
p95 limit of 0.25 ms CPU / 0.20 ms GPU and eight draws for the HUD, and
0.50 ms CPU / 0.35 ms GPU and 32 draws for the inventory. No mandatory GPU
job is added. The existing scene-only goldens remain separate from the UI
captures.

## Hands-on acceptance

The maintainer reported that the hands-on checklist passed during review
of [PR #322](https://github.com/gkehren/ft_vox/pull/322). This covers solo
play, settings, two-client hosting and restart, Developer UI, and window
and input handling. The SDL-driven results and captures above come from
the automated fixture runs.

The manual sign-off did not specify which optional configurations were
available. OS-driven DPI transitions and IME, overlay-free validation,
native Linux graphics and macOS/MoltenVK build and smoke have no separate
environment-specific result recorded here.

| Check | Operator actions |
| --- | --- |
| Solo | Launch normally, create and reopen a local world, walk, mine, pick up and place a block, use the hotbar and move, split and drop stacks. Pause, resume, save, then reopen and verify the saved state. |
| Settings and recovery | Keep, revert and time out a display preview. Change sensitivity and inversion. Exercise failed save and retry, disconnected status and degraded terrain sync. Restart and verify stored preferences. |
| Host and Join | Launch two regular clients with separate profiles. Host, join, check owner and guest permissions, cancel then confirm a moderation action, reconnect and compare confirmed private inventories. Stop, reopen the hosted world and return each client to solo. |
| Developer UI | Toggle F1 while playing and over native menus. Open Graphics, Streaming, World, Help, Performance, Overview, Chunk Inspector, Memory, Render Debug, the console, Network and Hosted Server diagnostics. Check dock restore and benchmark start/cancel. |
| Display and input | Check 16:9, 21:9 and 960×540 at representative 100/150/200% scale. Alt+Tab during a held input, minimize/restore and resize. Reload a partial pack and try missing resources. Capture daylight, night and both multiplayer roles. |
