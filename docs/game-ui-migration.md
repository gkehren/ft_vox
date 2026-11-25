# Game UI migration and parity

Epic #298 separates the player-facing Game UI from the Dear ImGui Developer UI. The [architecture decision](adr/game-ui-backend.md) defines the renderer, ownership and input contracts. Phase A adds an isolated preview. A normal launch still follows the existing solo, Join and Host paths and the existing ImGui controls.

## Surface inventory

| Current surface | Behavior and owner today | Planned destination and parity gate |
| --- | --- | --- |
| Hotbar, item count/name, targeting and status | `GameUIInventory` renders the bounded inventory snapshot. Engine maps `1–9`, wheel, `Q`, LMB/RMB and runs voxel targeting and highlight publication. `GameUI` shows F1 status density and F6 hints. | **#303 HUD.** Add a crosshair aligned with the scene viewport and retain hotbar selection, counts, target and interaction feedback in solo and remote walking. Keep the 3D block highlight in the renderer. Move FPS, position, drawn chunks and detailed status/density to Dev UI. |
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

Today Engine handles SDL events and safe-point operations, builds the mixed `GameUIFrame`, starts and ends the ImGui frame, and hands the postprocessing overlay callback to `WorldRenderer`. The renderer loads the post color in its overlay pass. Biome-map uploads publish after the overlay. `VkFrameContext` owns acquire, submit and present. World/image validation uses the offscreen path. #300 keeps that ownership and adds the Game UI draw batches ahead of Dev UI. #301 consolidates input routing and gives the player layer its own state and action interface.

1. **#299** proposes the decision and proves a standalone overlay with text, one atlas sample, scissor, focus and resize.
2. **#300–#302** deliver the renderer, then stack/focus and layout, then tokens and widgets. The first reusable set is a label, button, toggle, text field, list/scroll viewport, modal, tooltip and inventory slot using rows, columns and fixed grids. It includes a keyboard focus ring and enough contrast for the baseline theme.
3. **#303–#305** migrate HUD, inventory and navigation in order. Keep the corresponding legacy entry point until solo and remote behavior for that slice is verified. Keep gameplay actions, results and save/leave semantics on the shared Engine path at every stage.
4. **#306–#307** migrate multiplayer/hosting/admin and player settings using the agreed navigation/actions. Keep CLI and legacy ImGui entry points until verified. Record dual-client and host checkpoint evidence.
5. **#308–#309** make Dev UI opt-in, preserve developer routes and retire superseded player panels only after parity. Verify window and DPI cases, resource reload, input ownership, rendering and budgets with canonical GPU evidence.

The spike can be removed as a unit in #300. During migration the legacy path is the rollback for each unverified player surface. For a replacement, route the active surface through the same Engine adapter and give one surface ownership of each interaction.

## Running the Phase A spike

Run `ft_vox --seed 42 --ui-spike --quality low` in a graphics build. It uses a transient solo scene and closes with Escape. Tab focuses the button. Enter, Space, or a mouse press and release inside the button increments the on-screen counter and writes the input source to stdout. Repeated keydown, release outside the button after pressing inside, and activation after focus loss should leave the counter unchanged. Resize to verify that the panel stays centered, the checker texture stays inside the frame and the button target follows its new position.

The spike draws at most two instanced batches, owns one atlas descriptor, and uses a fixed 9 KiB instance buffer per frame slot. It does not add a readback or a per-frame wait. For standard Vulkan validation, set `FT_VOX_VALIDATION=1`. For extra synchronization checking, also set `FT_VOX_SYNC_VALIDATION=1`. Keep this check targeted to hardware runs.

### Windows GPU evidence

Windows 11, MSVC Release (vcpkg), GeForce RTX 4070 Ti, driver 32.0.16.2124, Vulkan 1.4.357, SDL 3.4.18, `B8G8R8A8_SRGB` swapchain, FIFO, seed 42 and Low quality. A temporary test driver posted events into the real SDL queue and called `SDL_SetWindowSize` between frames. The [LunarG screenshot layer](https://vulkan.lunarg.com/doc/view/latest/windows/screenshot_layer.html) captured the presented images in a separate run from validation. These are hardware captures of the on-screen frame path.

| Check | Observed |
| --- | --- |
| At 1920×1080, press Enter before focus, then Tab and Enter, then repeat Enter | Focus ring appears. Exactly one keyboard activation. [1920×1080 keyboard capture](images/game-ui/spike-keyboard.png) |
| Resize to 1160×680, press inside and release outside, then press and release inside | Panel recenters, checker sample stays within its scissor, and the moved button accepts exactly one mouse activation. [1160×680 mouse capture](images/game-ui/spike-mouse-resized.png) |
| Deliver focus loss, press Space, then Escape | Counter stays at two after focus loss. Escape exits with status 0. |
| CLI | `--help` lists the spike. The four conflicting modes `--connect`, `--world`, `--benchmark` and `--view` each return status 2. |
| Vulkan validation | The spike and the normal launch control each presented 325 frames and resized once with synchronization validation enabled. Both logged 325 present/acquire `SYNC-HAZARD-WRITE-AFTER-READ` reports and the same sRGB/STORAGE swapchain and image-view VUIDs (12 and 9). The standard-validation spike run showed those two VUIDs only. The existing sRGB/STORAGE reports are documented in the [Vulkan validation guide](vulkan-validation.md). |
| CPU-only Release | All 50 configured tests passed, including `CoreIncludeGuard`, `HeadlessDeps` and the nested `CpuOnlyBuild`. |
| Windows graphical Release | `ft_vox` built with both new shaders. Both SPIR-V files passed `spirv-val --target-env vulkan1.2`. `UiShell`, `PostResizeFormats` and the `VisualRegression` smoke suite passed. `test_engine_reload.exe` passed all 33 checks when run from the game's build output directory, where its compiled shader files were available. |
| Linux/WSL compile | Debian GCC compiled and linked the graphical executable with the local build's cached shader compiler stub. The shader compile and GPU evidence above use the actual Windows Vulkan SDK toolchain. |

Linux and macOS/MoltenVK graphics, UNORM swapchain output, high-DPI monitor transitions and minimization were untested for the spike. Production UI timing and resource-pack reload measurements are follow-ups in #300–#309.
