# Game UI migration and parity

Epic #298 separates the player-facing Game UI from the Dear ImGui Developer UI. The [architecture decision](adr/game-ui-backend.md) defines the renderer, ownership and input contracts. Phase A adds an isolated preview. A normal launch still follows the existing solo, Join and Host paths and the existing ImGui controls.

## Live player HUD (#303)

Normal solo and remote launches now use the native Game UI hotbar. It draws the nine shared slots from the solo simulation snapshot or the connected client's confirmed private inventory. The selected stack name and quantity follow that snapshot. Pending remote actions mark the confirmed slot and show feedback until the server responds. Joining, leaving, resync and disconnect replace the hotbar with session status until a synchronized snapshot is available. The HUD reads the current atlas generation each frame, so reloads and session changes rebuild the icons from the current data.

The world camera still projects across the full window. The reticle marks that projection's center in perspective play with captured input. With developer docks open, the HUD anchors its hotbar and status inside the central dock's unobstructed rectangle. The reticle keeps the world aim point and disappears if that point is covered by a dock or the space is too tight. A compact dock can shrink the HUD below the chosen player scale to fit all nine slots and omit the optional hint line to leave room for the reticle. The developer layer draws over Game UI. Pause, inventory, and open developer popups or dialogs suppress the gameplay HUD.

| Control | Behavior |
| --- | --- |
| `1–9` / wheel | Select a hotbar slot or cycle with the mouse captured. |
| LMB / RMB / `Q` | Break and place through the current mode, or drop one from the selected slot. |
| `E` / Escape | Open inventory or dismiss it. Escape in play opens the game menu. `P` also opens the menu in solo. |
| `C` / `F6` | Release or capture the mouse. Toggle the optional HUD hints with F6. |
| Game menu → Hide HUD / Show HUD | Change HUD visibility for the current run. Start hidden with `--no-hud`. |
| `--no-dev-ui` / `F5` | Start with a full-window HUD and open the developer workspace on demand. Other developer panel shortcuts can also open it. The existing `E` inventory works in either layout. |
| `F1` | Cycle the developer diagnostics overlay through Off, Minimal and Detailed. It starts Off. |

`--view`, command-line streaming benchmarks, and active in-game benchmarks exclude the player UI from composition. The existing offscreen scene goldens continue to render with UI excluded. `--game-ui` still opens the opt-in widget strip and HUD mock for design-system work. The default developer workspace and Join/Host workflows remain available. The legacy ImGui inventory panel and its command callbacks are the handoff to #304. This slice retires the ImGui hotbar, hint window and always-on multiplayer banner and dot.

### Presented evidence

Windows, MSVC Release, GeForce RTX 4070 Ti, driver 32.0.16.2124, Vulkan 1.4.357, SDL 3.4.18, Low quality, seed 42 and 1920×1080. These captures use the Vulkan SDK screenshot layer in a separate run from validation. The solo comparison starts from the same seeded local inventory and focuses the selected Dirt stack. The two remote clients connect to the same loopback server with separate profiles. Client A selects slot 4 and client B selects slot 7.

| View | Before | After |
| --- | --- | --- |
| Solo with developer docks | [Before](images/game-ui-hud/solo-before.png) | [After](images/game-ui-hud/solo-docked.png) |
| Solo without the workspace | | [Full-window HUD](images/game-ui-hud/solo-player.png) |
| Remote client A | [Before](images/game-ui-hud/client-a-before.png) | [After, full window](images/game-ui-hud/client-a-after.png) |
| Remote client B | [Before](images/game-ui-hud/client-b-before.png) | [After, docked](images/game-ui-hud/client-b-after.png) |

Run the opt-in hardware proof from the game's output directory to exercise live SDL input, menu layering, 960×540 at 200%, atlas replacement and world reload. Set `FT_VOX_VALIDATION=1` for the proof. Run the performance mode with `FT_VOX_GPU_PROFILING=1` and validation disabled.

```powershell
../tests/Release/test_engine_reload.exe --player-hud-proof
../tests/Release/test_engine_reload.exe --game-ui-performance
```

### Verification and frame cost

- Windows Release passed all 53 graphics-off CTests, including the headless dependency and core include guards and all nine tests labeled `multiplayer`. The graphical build passed `UiShell` and `VisualRegression` with the committed goldens.
- The live HUD and native showcase proofs passed on the RTX 4070 Ti. The live HUD proof's seven validation messages matched the known swapchain/image-view diagnostics in the test interval. [Vulkan validation guide](vulkan-validation.md)
- The native Join smoke passed 30 checks, including confirmed selection while an action is pending, endpoint/profile switch, disconnect and reconnect. The native hosted closure ran two graphical clients through mining, pickup, separate hotbars, server stop and checkpoint restart. The HUD's selection and counts matched each client's private inventory before and after restart. The CPU multiplayer restart test covered remote place and drop.
- The performance proof used the same static seed-42 scene at 1920×1080 with immediate present, Low quality and five populated slots. Each run measured 600 frames after 120 warmup frames. GPU timestamp coverage was 600/600 per run.

| Run | CPU frame median / p95 (ms) | GPU frame median / p95 (ms) | HUD CPU median / p95 (ms) | HUD GPU median / p95 (ms) |
| --- | --- | --- | --- | --- |
| Baseline A | 0.555 / 1.467 | 0.578 / 0.782 | — | — |
| HUD A | 0.564 / 1.416 | 0.610 / 0.782 | 0.0046 / 0.0078 | 0.0031 / 0.0041 |
| HUD B | 0.571 / 1.338 | 0.608 / 0.621 | 0.0047 / 0.0080 | 0.0031 / 0.0041 |
| Baseline B | 0.576 / 1.354 | 0.607 / 0.624 | — | — |

The measured HUD used one draw, 936 vertices, four preallocated descriptor sets, 212,992 bytes of instance buffers and 32,768 bytes of font coverage. This sRGB path required zero composition-image bytes. Physical HUD runs used Windows. Linux, macOS/MoltenVK and physical cross-monitor DPI transitions remain unverified for #303.

## Surface inventory

| Current surface | Behavior and owner today | Planned destination and parity gate |
| --- | --- | --- |
| Hotbar, item count/name, targeting and status | `PlayerHud` draws the live bounded snapshot, contextual hints and connection feedback. Engine maps `1–9`, wheel, `Q`, LMB/RMB and publishes the 3D block highlight. | **#303 HUD.** Live in normal solo and remote play. F1 density and detailed performance/diagnostics stay in the developer workspace. |
| Inventory and drag/drop | `E` toggles an Engine flag. `InventoryUi` and `GameUIInventory` handle slots, icons, tooltips and a transient drag cursor. Snapshots and callbacks select, move, swap, split or drop via Engine. Atlas layer descriptors are cached per generation. | **#304 Inventory.** Preserve selected slot, whole/half stack and partial transfers, disabled input during remote pending state, rejected-command feedback, tooltip/icon behavior and drag cancellation on close, focus loss, session or resource change. Compare authoritative inventory before and after solo and remote gestures. |
| Solo world and pause | `--world` and `--seed` enter a saved or transient world through Engine initialization. World panel shows seed, save identity, queue/health and errors. `P` mutates host pause, `C` controls host cursor capture, and Escape or the menu requests exit. Engine owns save, flush, world epoch reset and restoration. | **#305 Main Menu, Loading and Pause.** Create/select an existing world or start transient play through the existing lifecycle. Preserve CLI entry points, save error visibility and session-epoch barriers. Validate open, resume, save-and-leave, exit and retry. Show save health to players and detailed persistence counters in Dev UI. |
| Join, profiles, and client status | `GameUIMultiplayer`, `EngineJoin` and `EngineMultiplayer` own endpoint/expected seed, favorites, named profile creation/selection, private keys, loading/readiness, reconnect, switch, disconnect and return to suspended solo. The public roster and private inventory arrive through the remote client path. | **#306 Multiplayer.** Preserve favorites/profile persistence, unavailable/error states, endpoint validation, two-client roster and local inventory privacy, reconnect identity, and blocked transition on solo save failure. Put actionable status and latency in Game UI and detailed packet, snapshot and motion telemetry in Dev UI. |
| Host, roster and stop | `EngineHosting` and `ServerSupervisor` enumerate server worlds, launch a sibling server and surface readiness, configuration, endpoint copy, auto/manual join, roster and delayed/failing stop. The engine coordinates app exit and force-stop confirmation. | **#306 Host.** Preserve existing/new world and profile selection, private bind guidance, port/player/interest/worker/streaming options, readiness, copy/join, confirmed graceful stop, Wait/Force Stop and exit recovery. Verify stop and restart with checkpoint persistence. |
| Local administration | Hosted panel uses server-verified local permissions. The owner and authorized local delegates have different capabilities. Scope and credentials stay with the local control path. Roster, offline IDs, admission policy, moderation, confirmations and audit are already surfaced. | **#306 Administration.** Preserve permissions, target confirmation, audit and server result/error semantics. Cover grants/revocations, authorize-local-control, kick, ban/unban, allowlist membership/mode and unauthorized guest/remote rejection using the existing [hosting closure](multiplayer-hosting-closure.md). |
| Display, inputs, quality and assets | The Graphics panel owns quality presets, VSync, rendering controls and pack browser/apply/use bundled. The Player panel forwards sensitivity and camera settings. `ImGuiLayer` persists a separate docking/scale configuration in `imgui.ini`. Engine applies VSync at swapchain recreate and handles failure-atomic texture replacement. | **#307 Settings.** Player-facing VSync, quality presets, render distance, sensitivity, player UI scale and resource-pack selection/recovery use typed setters and independent preferences. Preserve deferred apply and current CLI overrides. Keep detailed per-pass knobs, validation/device information, diagnostics, and Dev UI scale/layout under ImGui. |
| Player tools and world inspection | Player/Gameplay exposes flight/creative selection, speed boost, camera/isometric zoom, raycast range, borders, demo players, passive-mob toggle and detailed motion. World includes seed/save diagnostics, interactive biome map and map jobs. | **Dev UI.** Keep inspection and creative controls, authority conditions, map movement/zoom and async publication. Player presentation may share read-only context such as world name, biome and save health. |
| Streaming, rendering and performance | `DebugUI/` has Overview, Graphics, Streaming, Performance, Player Diagnostics, Memory, Chunk Inspector and Render Debug. It samples render settings, timing, GPU data, chunks, queues, arenas and workload. | **Dev UI.** Keep profiling, debug views, full graphics/streaming knobs, resource and player diagnostics, docking layouts and the central scene viewport. Verify the gameplay input path continues when dockspace is visible and no tool owns input. |
| Benchmark, Help and shell | Benchmark panel and CLI capture streaming, timing and biome-map benchmarks. `UiShell` owns docking/status strip, default/reset layout, menu actions and About. `UiShortcuts` and `InputRouting` define current shortcut policy. | **#308 Developer UI** keeps benchmark panel/CLI, reports, docking and detailed About/runtime data. Game UI makes player controls, About and Exit discoverable through normal navigation. Keep benchmark and visual regression paths independent of Game UI. |

## Bindings

Phase A leaves every legacy binding in normal launch. The following destinations become active only once the corresponding replacement is verified. HUD and Help document the bindings that apply to the current context.

| Binding today | Destination |
| --- | --- |
| WASD, mouse, Space, Shift | Gameplay move, look, jump/swim/fly and sprint/swim/fly. Send input only when gameplay owns it. |
| LMB/RMB, wheel, `1–9`, `Q`, `E` | Preserve break/place, hotbar cycle/selection, drop and inventory toggle. Keep flight-only creative editing in Dev UI. `E` may close the focused inventory when no text field or modal owns it. |
| `P`, `C`, Escape | Preserve solo `P` pause and `C` cursor release/capture while gameplay owns input. Escape uses the Game UI stack and pause policy. Explicit Exit uses the existing shutdown path. |
| `B`, `T`, `V`, `X`, `H` | Dev UI keeps chunk borders, creative cycle, flight toggle, speed boost and layout hide/restore. Use existing ownership and flight/solo gates. |
| F1 | Becomes global Dev UI toggle in #308. Status density remains available from View → Status overlay. |
| F2/F3/F4, F7/F8/F9, F11/F12 | Keep Graphics, Streaming, World, Performance, Overview, Chunk Inspector, Memory and Render Debug within Dev UI. Their shortcuts apply with Dev UI enabled. |
| F5/F6 | F5 opens Controls. F6 toggles gameplay hints. Keep them discoverable from Game UI and in the Dev UI menu where appropriate. |
| F10 | Retain intentional global VSync toggle using the same deferred setting action. Modal ownership can veto disruptive view changes. |

## Frame hooks and staged rollout

Today Engine handles SDL events and safe-point operations, builds the mixed `GameUIFrame`, starts and ends the ImGui frame, and hands the postprocessing overlay callback to `WorldRenderer`. The renderer loads the post color in its overlay pass. Biome-map uploads publish after the overlay. `VkFrameContext` owns acquire, submit and present. World/image validation uses the offscreen path. #300 keeps that ownership and validates both composition paths, translucent elements and independent Game UI and Dev UI profiling as specified in the [backend ADR](adr/game-ui-backend.md#production-composition-contract-for-300). #301 consolidates input routing and gives the player layer its own state and action interface.

The Phase C showcase, screen policy and reproducible input checks are documented in [Game UI interaction](game-ui-interaction.md). Phase D adds the player design system: one baseline theme, an audited face with a documented fallback, reusable components, the inventory-slot contract and an independent, persisted player UI scale — see [Player design system](game-ui-design-system.md).

Phase D surfaces and their owners:

| Surface | Owner today | Player-facing path |
| --- | --- | --- |
| Tokens, typography, components, slot contract | `PlayerUiStyle.hpp`, `PlayerUiText.hpp`, `PlayerUiComponents.*`, `PlayerUiFonts.*` | Consumed by every native screen; the gallery renders each component class. |
| Player UI scale | `PlayerUiScale.hpp` + `PlayerUiPreferences.*` (Engine loads/persists) | Auto by default, pause → Design system → **Player UI scale** for the persisted override. ImGui scale/layout untouched. |
| Component gallery | `Screen::Gallery` (`GameUiGallery.cpp`, layout in `PlayerUiGallery.hpp`) | Pause → **Design system**. |
| HUD preview | `Screen::HudMock` | Pause → **HUD mock** in the `--game-ui` showcase. #303 uses the live HUD during play. |

No legacy binding or surface was removed in Phase D: the pause menu only gained two preview entries after Exit to desktop, and the existing ImGui panels, HUD, inventory and multiplayer flows are unchanged.

1. **#299** proposes the decision and proves a standalone overlay with text, one atlas sample, scissor, focus and resize.
2. **#300–#302** deliver the renderer, then stack/focus and layout, then tokens and widgets. The first reusable set is a label, button, toggle, text field, list/scroll viewport, modal, tooltip and inventory slot using rows, columns and fixed grids. It includes a keyboard focus ring and enough contrast for the baseline theme.
3. **#303–#305** migrate HUD, inventory and navigation in order. Keep the corresponding legacy entry point until solo and remote behavior for that slice is verified. Keep gameplay actions, results and save/leave semantics on the shared Engine path at every stage.
4. **#306–#307** migrate multiplayer/hosting/admin and player settings using the agreed navigation/actions. Keep CLI and legacy ImGui entry points until verified. Record dual-client and host checkpoint evidence.
5. **#308–#309** make Dev UI opt-in, preserve developer routes and retire superseded player panels only after parity. Verify window and DPI cases, resource reload, input ownership, rendering and budgets with canonical GPU evidence.

The spike can be removed as a unit in #300. During migration the legacy path is the rollback for each unverified player surface. For a replacement, route the active surface through the same Engine adapter and give one surface ownership of each interaction.

## Running the Phase A spike

The original Phase A captures are retained below. Since #300, `--ui-spike`
uses the production renderer in its standalone preview mode. Use
`--game-ui` to see the interactive strip alongside the existing interface.
See [renderer verification](game-ui-renderer.md) for current captures.

Run `ft_vox --seed 42 --ui-spike --quality low` from the game output directory in a graphics build. It uses a transient solo scene and closes with Escape. The following manual check verifies the rendered output and the controls.

1. Press Enter with no button focus and check that the counter remains at zero. Press Tab, check that the button gains a visible focus ring, then hold Enter long enough for key repeat. The counter should advance to one.
2. Resize to approximately 1160×680. Check that the panel is centered, the checker texture stays inside its frame, and the focus ring follows the button. Press inside the button, drag to the surrounding scene and release. The counter should stay at one. Click the relocated button and check that the counter advances to two.
3. Switch to another application, then return without clicking the button. Press Enter before Tab. The counter should stay at two and the button should have no focus ring. Minimize and restore the window, press Tab then Space, and check that the counter advances to three. Press Escape and check that the process exits successfully.

To replay the input assertions with the real SDL event queue, build the existing graphical test runner and run its opt-in smoke mode. The Windows example assumes a configured `build` directory. Use the game executable's directory as the working directory so the test resolves the compiled shaders.

```powershell
cmake --build build --config Release --target ft_vox test_engine_reload
$uiBuild = (Resolve-Path build).Path
Push-Location "$uiBuild/Release"
try {
    & "$uiBuild/tests/Release/test_engine_reload.exe" --ui-spike-smoke
} finally {
    Pop-Location
}
```

On a single-config build, run `./tests/test_engine_reload --ui-spike-smoke` from the build directory. The scenario checks a real SDL resize and swapchain recreation, outside release, pixel-only resize, synthetic minimize/restore and stale events after focus loss, including `Tab → Enter` and Escape. It also verifies that regaining window focus requires fresh button focus before activation. Run the manual check for the presented pixels. The smoke mode is opt-in on a graphical machine.

The original Phase A renderer drew at most two instanced batches, owned one atlas descriptor, and used a fixed 9 KiB instance buffer per frame slot. For standard Vulkan validation, set `FT_VOX_VALIDATION=1`. For extra synchronization checking, also set `FT_VOX_SYNC_VALIDATION=1`. Keep this check targeted to hardware runs.

### Windows GPU evidence

Windows 11, MSVC Release (vcpkg), GeForce RTX 4070 Ti, driver 32.0.16.2124, Vulkan 1.4.357, SDL 3.4.18, `B8G8R8A8_SRGB` swapchain, FIFO, seed 42 and Low quality. The original capture driver posted SDL events and called `SDL_SetWindowSize` between frames. The smoke mode above replays and extends the input checks. The [LunarG screenshot layer](https://vulkan.lunarg.com/doc/view/latest/windows/screenshot_layer.html) captured the presented hardware frames in a separate run from validation.

| Check | Observed |
| --- | --- |
| At 1920×1080, press Enter before focus, then Tab and Enter, then repeat Enter | Focus ring appears. Exactly one keyboard activation. [1920×1080 keyboard capture](images/game-ui/spike-keyboard.png) |
| Resize to 1160×680, press inside and release outside, then press and release inside | Panel recenters, checker sample stays within its scissor, and the moved button accepts exactly one mouse activation. [1160×680 mouse capture](images/game-ui/spike-mouse-resized.png) |
| Focus loss followed by Space in the original capture sequence | Counter stayed at two. |
| Replay `test_engine_reload --ui-spike-smoke` | All 10 assertions passed on the same Windows GPU. The test also checks delayed Tab, Enter, Escape and clicks while unfocused, and press cancellation on a pixel-only resize or synthetic minimization. |
| CLI | `--help` lists the spike. The four conflicting modes `--connect`, `--world`, `--benchmark` and `--view` each return status 2. |
| Vulkan validation | The spike and the normal launch control each presented 325 frames and resized once with synchronization validation enabled. Both logged 325 present/acquire `SYNC-HAZARD-WRITE-AFTER-READ` reports and the same sRGB/STORAGE swapchain and image-view VUIDs (12 and 9). The standard-validation spike run showed those two VUIDs only. The existing sRGB/STORAGE reports are documented in the [Vulkan validation guide](vulkan-validation.md). |
| CPU-only Release | All 50 configured tests passed, including `CoreIncludeGuard`, `HeadlessDeps` and the nested `CpuOnlyBuild`. |
| Windows graphical Release | `ft_vox` built with both new shaders. Both SPIR-V files passed `spirv-val --target-env vulkan1.2`. `UiShell`, `PostResizeFormats` and the `VisualRegression` smoke suite passed. `test_engine_reload.exe` passed all 33 checks when run from the game's build output directory, where its compiled shader files were available. |
| Linux/WSL compile | Debian GCC compiled and linked the graphical executable with the local build's cached shader compiler stub. The shader compile and GPU evidence above use the actual Windows Vulkan SDK toolchain. |

Linux and macOS/MoltenVK graphics, UNORM swapchain output, high-DPI monitor transitions and OS-driven minimization were untested for the spike. Production UI timing and resource-pack reload measurements are follow-ups in #300–#309.
