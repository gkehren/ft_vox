# Game UI migration and parity

Epic #298 separates the player-facing Game UI from the Dear ImGui Developer UI. The [architecture decision](adr/game-ui-backend.md) defines the renderer, ownership and input contracts. Phase A adds an isolated preview. A normal launch still follows the existing solo, Join and Host paths and the existing ImGui controls.

## Native player inventory (#304)

Press **E** in a solo or remote world to open a three-by-nine inventory above the nine-slot hotbar. The panel fits the available game view and follows the saved player UI scale. It uses the same slot tokens and generation-tagged atlas icons as the HUD. A missing icon leaves a placeholder and the item count. Tooltips fit inside the game view, including when developer docks are visible.

| Input | Behavior |
| --- | --- |
| Click a stack, then click a destination | Move the whole stack into an empty slot, merge with a matching item, or swap different items. |
| Right-click a stack, then click a destination | Move the larger half into an empty or matching slot. Only the amount that fits moves. |
| Re-click the source, right-click while holding, or click a gap or outside the panel | Cancel the held stack. |
| Arrows, Tab, Shift+Tab | Move focus through the grid and buttons. |
| Enter or Space, with Shift for half | Use the focused slot or button. |
| 1–9 or wheel, Q, Drop selected stack | Select a hotbar slot, drop one item, or drop the whole selected stack. |
| E, Escape, or Close | Close inventory and restore the previous capture state. |

The native screen owns focus and the held stack. The `InventoryUi.hpp` planner stores a typed source slot and whole/half mode. The displayed stack is computed from the current snapshot. A newer private revision or a changed solo snapshot invalidates the gesture, as do focus loss, closing, a workspace toggle and a world or session reset. Joining, resyncing and disconnected states hide the previous inventory and disable its commands. Reconnect uses the existing Multiplayer menu.

One completed gesture emits one Engine intent. The solo adapter returns the simulation result immediately. The remote adapter disables commands while the request is pending, marks the affected slots and keeps showing confirmed counts. The server's reliable channel sends the required inventory revision ahead of the action result. The screen can then show rejection or a partial transfer using that result. Item IDs, saves and wire messages use the existing formats. The old ImGui inventory and its per-layer icon descriptors are retired. Developer inspection, hosting and the dockspace continue through the existing Developer UI.

### Inventory verification

Windows 11, MSVC Release, GeForce RTX 4070 Ti (621.24), Vulkan 1.4.357, SDL 3.4.18, Low quality, seed 42. The native proof submits real SDL mouse and keyboard events and renders through the game swapchain. The screenshot layer captured the presented frames in a separate run from validation.

| Capture | What it shows |
| --- | --- |
| [1920×1080 at 100%](images/game-ui-inventory/normal-16x9.webp) | Full inventory, selected Stone stack and tooltip. |
| [2560×1080 at 100%](images/game-ui-inventory/held-21x9.webp) | Half-stack cursor, source remainder and focused slot. |
| [960×540 at 200% UI scale and 2× font density](images/game-ui-inventory/compact-2x.webp) | Responsive grid and current atlas after resource-pack reload. The proof sets the face density directly. |

Run the proof from the game's Release directory with `FT_VOX_VALIDATION=1`.

```powershell
../tests/Release/test_engine_reload.exe --player-inventory-proof
```

The proof passed 22 checks, including whole and half transfer, merge, swap, full-stack rejection, both drop controls, repeated keys, pointer release over a different target, outside cancellation, focus loss, workspace toggle, atlas reload and world reset. All 21 WSI validation messages matched the known RTSS swapchain diagnostics. The Windows CPU-only build passed all 53 CTests, including inventory policy, Items, GameplayLoop, VoxelActions, EntityReplication and WorldCheckpoint. EntityReplication also delays a private inventory update ahead of its action result and replays an older inventory packet.

The [native Join and hosted closure scripts](multiplayer-client-smoke.md) passed with validation enabled. The Join smoke covered a disabled inventory during synchronization and disconnect, then restored the same private state on reconnect. Two concurrent graphical clients exercised pickup, a pending split, duplicate input during the pending interval, a rejected drop, independent hotbars and checkpoint restart. The guest's inventory stayed private while both clients saw the same terrain edit. The HUD and widget showcase proofs and the existing visual-regression scenes also passed.

The input runs used scripted SDL events on physical hardware. They exercised the font-density path with a 2× override. Reverting the migration restores the ImGui panel against the same saved inventories and wire protocol.

## Live player HUD (#303)

Normal solo and remote launches now use the native Game UI hotbar. It draws the nine shared slots from the solo simulation snapshot or the connected client's confirmed private inventory. The selected stack name and quantity follow that snapshot. Pending remote actions mark the confirmed slot and show feedback until the server responds. The HUD reads the current atlas generation each frame, so reloads and session changes rebuild the icons from the current data.

### Reticle priority (#303 F02)

The world camera projects across the full window, and the reticle is the exact centre of that projection — never the centre of the docked area, so an asymmetric developer dock cannot move the aim point. Geometry and content are separate: `hudLayout` computes every band, then `resolveHud` decides what is painted. An empty band reserves nothing, so an unoccupied status or activity band can never hide the reticle, and an occupied one yields to it. The mandatory hotbar (and a dock covering the aim point) is the only thing allowed to suppress the reticle. Gameplay that does not permit aiming removes it as well: free cursor, pause, inventory, a blocked session, or a non-perspective camera.

The policy is unit-tested headlessly (`tests/test_game_ui_components.cpp`, `liveHud`) at 100%, 150% and 200% over five layouts, including the asymmetric 2560×1080 case where the aim point stays at 1280,540.

### Session feedback (#303 F03/F07)

Every piece of HUD copy sits on one contrast plate built from the `hudScrim` token, so status text stays readable over midday sky and dark caves. Copy has three bounded parts: a one-line headline, a detail wrapped to at most two body lines (`wrapText` → `Surface::wrappedLabel`, elided at the end) and a separate recovery instruction. The detail shrinks to one line before the recovery line is dropped, and the block never reaches the hint band or the hotbar. Detailed packet and motion telemetry stays in the developer workspace; the HUD only carries what a player can act on.

`hudSession` is the one session policy (`src/Engine/PlayerHud.cpp`), mapped by Engine from the transport status and readiness:

| Session state | HUD behavior |
| --- | --- |
| Connected and ready | Normal HUD: hotbar, selection, reticle, hints. |
| Degraded and ready | Normal HUD plus one warning headline/detail line. Pending action feedback is preserved. |
| Degraded (or connected) and not ready | Synchronization headline, detail and recovery line; no hotbar, no reticle. |
| Resyncing / joining / connecting / leaving | Pending headline; no inventory at all, so nothing stale is shown. |
| Disconnected, rejected, timed out | Error headline, the transport detail and a recovery instruction that matches the reachable controls; no hotbar, no reticle. |

### Workspace visibility (#303 F01/F04)

`Ctrl+Shift+D` (Developer ▸ **Hide developer workspace**) shows and hides the complete developer workspace at runtime. `H` keeps toggling the layout tiles, and the existing developer panel shortcuts still bring the workspace back. Hiding is not "close every panel": the dockspace is still submitted, with `ImGuiDockNodeFlags_KeepAliveOnly` and the same dockspace identity (`UiShell::beginFrame(Workspace)`), so the dock node tree, the split sizes and `imgui.ini` associations survive. Hidden surfaces neither paint nor claim pointer input; gameplay keeps the cursor and the keyboard.

Since #304 the native inventory follows the same available game view as the hotbar. The legacy docking figures below document the #303 behavior before this migration.

### Multiplayer recovery (#303 F03)

A blocked session keeps the transport detail and adds one instruction that names only reachable steps. `recoveryInstruction(workspaceVisible, mouseCaptured)` picks between four lines:

| Workspace | Mouse | Instruction |
| --- | --- | --- |
| Hidden | Captured | `Ctrl+Shift+D, C, then Multiplayer > Reconnect.` |
| Hidden | Free | `Ctrl+Shift+D, then Multiplayer > Reconnect.` |
| Visible | Captured | `C to free the mouse, then Multiplayer > Reconnect.` |
| Visible | Free | `Multiplayer > Reconnect.` |

`Multiplayer > Reconnect` is the existing Developer ▸ Multiplayer item (`GameUIFrame::joinServer`), so recovery never requires a restart and never points at a menu that does not exist. Both steps are reachable while the session is blocked: the workspace chord is a global shortcut and `InputContext::choose` keeps `C` with gameplay during a session transition, after every modal, inventory, developer and benchmark guard — movement, gameplay keys, pointer and text stay blocked. The line is painted in the same bounded, contrast-backed feedback block as the detail, so it stays readable at 960×540 and 200% UI scale.

### Creative selection (#303 F06)

Debug flight names its block selection explicitly (`Creative: Stone`) on the selection band. The prefix is independent of the optional F6 hints, and leaving flight restores the authoritative inventory label. The label is bounded and elided by the same text metric as every other HUD string.

### Showcase coexistence (#303 F05)

The opt-in `--game-ui` showcase strip is a demonstration widget and now yields to session copy: while the HUD has a status headline, detail or recovery line, the strip is not painted and its controls receive no pointer input (`hitsStrip` is empty for the frame). Connection errors and pending actions therefore never share the band above the HUD with a demo widget. The standalone `--ui-spike` proof is unchanged.

### Controls

| Control | Behavior |
| --- | --- |
| `1–9` / wheel | Select a hotbar slot or cycle with the mouse captured. |
| LMB / RMB / `Q` | Break and place through the current mode, or drop one from the selected slot. |
| `E` / Escape | Open inventory or dismiss it. Escape in play opens the game menu. `P` also opens the menu in solo. |
| `C` / `F6` | Release or capture the mouse. Toggle the optional HUD hints with F6. |
| Game menu → Hide HUD / Show HUD | Change HUD visibility for the current run. Start hidden with `--no-hud`. |
| `--no-dev-ui` / `F5` | Start with a full-window HUD and open the developer workspace on demand. Other developer panel shortcuts can also open it. The existing `E` inventory works in either layout. |
| `Ctrl+Shift+D` | Show or hide the complete developer workspace without losing the docking layout. `H` stays the layout-tile toggle. |
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

#### Correction pass (F01–F07), executed

Windows 11, MSVC Release (vcpkg), GeForce RTX 4070 Ti, Vulkan 1.3.290 loader with standard validation layers, SDL 3.4.18, seed 42, Low quality. Every row below was executed on that machine, not inferred from the original slice:

| Check | Result |
| --- | --- |
| `ctest --test-dir build-cpu302 -C Release` (`FT_VOX_GRAPHICS=OFF`) | 53/53 passed, including `CoreIncludeGuard`, `GpuWorkflowTrustGuard`, `HeadlessDeps`, `HeadlessRunner`, `HeadlessMultiplayer` and all nine `multiplayer` tests |
| `ctest --test-dir build-g302 -C Release` (graphical) | 75/75 passed: `VisualRegression` (committed goldens, offscreen), `PostResizeFormats`, `EngineReload` (32 checks, real Vulkan lifecycle), `UiShell`, `GameUiComponents` (4977 checks), `GameUiInteraction`, `InputRouting`, `GameUiStyle`, `GameUiDrawList` |
| `test_engine_reload.exe --player-hud-proof` | 31 checks passed. Adds: launch with a hidden workspace; three show/hide cycles whose central dock rectangle is identical before and after (the persisted layout survives); the legacy inventory opening and closing inside the workspace and floating while it is hidden; 960×540 at 200% with a live hotbar; creative flight with F6 hints off, where the creative prefix adds copy to the same selection band; atlas replacement and world epoch reset. Validation: 14 errors, all matching the known RTSS set (`errors == known`) |
| `tests/test_multiplayer_join_native.py` (two loopback servers, graphical client) | 39 checks passed (30 before this pass, plus one readiness and one recovery check per join). Adds the readiness transition on a real transport: an unready remote session presents synchronization with no hotbar and no reticle, the ready session presents the confirmed inventory and drops the session copy, and a session that ended keeps a recovery line |
| `test_engine_reload.exe --game-ui-showcase-proof` | 40 checks passed: the opt-in showcase strip still draws, focuses and activates |
| `test_engine_reload.exe --ui-spike-smoke` | 10 checks passed: the standalone `--ui-spike` behavior is unchanged |

The focused headless suites that carry the new policies ran in both builds: `GameUiComponents` (content-aware reticle collisions at 100/150/200% over five layouts, bounded multiline wrap, the `hudSession` table, the creative label, the strip-yield rule), `GameUiInteraction` (workspace visibility transitions and input ownership), `UiShell` (shortcut metadata with the `Ctrl+Shift+D` chord, workspace policy) and `InputRouting`.

#### Correction pass (F08, F01, F03, F09–F12), executed

Same machine, generator, driver and scene as the pass above. What changed in behavior:

- `creativeLabel()` returns before `memcpy` when nothing is copied, so an empty `string_view` with a null pointer never reaches `memcpy` (F08).
- The hidden-workspace inventory uses a separate, non-persistent identity (`Inventory###inventory-floating`, `NoDocking | NoSavedSettings`). The canonical `Inventory` window is not submitted while the workspace is hidden, so its `DockId` and its `imgui.ini` record are untouched: a hidden-workspace launch, exit or developer-layout reset cannot lose or overwrite a persisted docking association (F01).
- `hudSession()` flags a blocked session instead of publishing a fixed menu path; the host fills the line through `recoveryInstruction(workspaceVisible, mouseCaptured)`. Hidden workspace with a captured mouse: `Ctrl+Shift+D, C, then Multiplayer > Reconnect.` Hidden with a free mouse: `Ctrl+Shift+D, then Multiplayer > Reconnect.` Visible with a captured mouse: `C to free the mouse, then Multiplayer > Reconnect.` Visible and free: `Multiplayer > Reconnect.` Every step names an existing control — the workspace chord, the mouse release and the developer menu bar item (F03). The steps are reachable while the session is blocked: `InputContext::choose` exempts the capture shortcut from the session-transition block (after every modal, inventory, developer and benchmark guard), so `C` still reaches the host with the mouse captured and nothing else becomes actionable — movement, gameplay keys, pointer and text stay blocked.
- `Engine::drawUi()` submits the dockspace alone when neither the developer workspace nor the legacy inventory is on screen, so the full-window path builds no player, save, biome or network snapshot and wires no callbacks (F09). The toggles owned by developer keys still reach the overlay layer on that path: `B` keeps flipping chunk borders with no panel drawn, which the proof asserts.
- The solo terrain wait reads `Waiting for terrain` and describes the held player; the hotbar keeps the authoritative inventory and no longer announces that it will return (F10).
- `setDeveloperWorkspaceVisible()` is gated by `ui::allowsWorkspaceToggle(m_benchmark.isActive())`, so the chord and the Developer menu item are both consumed without changing the layout during Reloading, Warmup and Running benchmarks (F11).

| Check | Result |
| --- | --- |
| `ctest --test-dir build-cpu302 -C Release` (`FT_VOX_GRAPHICS=OFF`) | 53/53 passed |
| `ctest --test-dir build-g302 -C Release` (graphical) | 75/75 passed, `VisualRegression` with the committed goldens; `GameUiComponents` now runs 5464 checks, including the four `recoveryInstruction` states and the blocked-session `hudSession` row |
| `test_engine_reload.exe --player-hud-proof` | 53 checks passed (31 before this pass). New: `Ctrl+Shift+D` routed through real SDL key events in both directions, a key repeat and plain `D`/`Ctrl+D`/`Shift+D`/`Alt+D` that must not toggle the workspace, an in-flight developer drag cancelled by hiding, the benchmark guard for the chord and for the menu action, the dockspace-only frame while the workspace and inventory are both off screen (the main menu bar's last-submitted frame does not advance), a chunk-border toggle still applied on that path, and the canonical inventory `DockId` plus its settings record surviving the hidden period while the temporary window reports `DockId == 0` with `NoDocking`/`NoSavedSettings` and reopens in its original dock |
| `tests/test_multiplayer_join_native.py` (two loopback servers, graphical client) | 55 checks passed (39 before this pass). The readiness transition is deterministic: a genuinely unready session is observed by withholding the camera pose, so the pending row, the absent hotbar and the absent reticle are asserted instead of hoped for, and the reticle invariant follows the host's capture state instead of the window manager's focus. The recovery scenario now runs through the real input path: from a blocked session with the workspace hidden and the mouse captured, `Ctrl+Shift+D` and `C` arrive as SDL events, the workspace has to become visible and the cursor has to be released, the recovery line has to follow the state the player just reached, and the reconnect action then has to restore the same actor, selected slot and authoritative inventory and drop the recovery line. The `Multiplayer > Reconnect` item itself is invoked through the same `GameUIFrame::joinServer` callback the menu calls — the shortcut routing is validated by events, the callback by its contract |
| `test_game_ui_interaction.exe` (`GameUiInteraction`) | The transition matrix: `C` and Escape reach gameplay, movement, gameplay keys, pointer, text and the inventory close key stay blocked, and a benchmark lock, a native screen, a modal, a developer text field, developer keyboard ownership and an open inventory each keep their own policy for the same key |
| `test_engine_reload.exe --game-ui-showcase-proof` | 39 checks passed; the standalone strip behavior is unchanged |
| `test_engine_reload.exe --ui-spike-smoke` | 10 checks passed |
| `test_engine_reload.exe --ui-ini-persistence <ini> dock`, then `--ui-ini-persistence <ini> hidden` | Two real processes sharing one `imgui.ini`: the first docks the inventory into the central node and writes `DockId=0x00000007`; the second starts without the developer workspace, opens and closes the inventory through the temporary identity, and the `[Window][Inventory]` record comes back byte-identical |
| Debian WSL, GCC, `-DFT_VOX_GRAPHICS=OFF -DFT_VOX_SANITIZERS=ON` | The graphics-off suite passes under ASan+UBSan, including `GameUiComponents` (which is where the F08 `memcpy` undefined behaviour showed up) and every dependency/include guard. `TerrainGeneration` — the terrain calibration binary, unrelated to this slice — needs far longer than a Release run under sanitizers on this VM and was still executing when the pass closed; its Release run is green in both configurations |
| Debian WSL, GCC, `-DFT_VOX_GRAPHICS=OFF -DFT_VOX_TSAN=ON` | The nine `multiplayer` tests and the headless, Game UI, dependency and include guards passed under TSan. They were launched through `setarch -R`: this kernel's ASLR entropy trips the known `ThreadSanitizer: unexpected memory mapping` runtime abort, which has nothing to do with the code under test |

#### Frame cost after the correction pass

Re-executed `--game-ui-performance` on the corrected HEAD with the same command, scene and 600-frame measurement window. GPU timestamp coverage was 600/600 in every run and the resource footprint is unchanged: one draw, 936 vertices, four preallocated descriptor sets, 212,992 bytes of instance buffers, 32,768 bytes of font coverage and no composition image.

| Run | CPU frame median / p95 (ms) | GPU frame median / p95 (ms) | HUD CPU median / p95 (ms) | HUD GPU median / p95 (ms) |
| --- | --- | --- | --- | --- |
| Baseline A | 0.559 / 1.287 | 0.417 / 0.813 | — | — |
| HUD A | 0.544 / 1.283 | 0.422 / 0.655 | 0.0051 / 0.0079 | 0.0031 / 0.0041 |
| HUD B | 0.518 / 1.202 | 0.422 / 0.653 | 0.0048 / 0.0078 | 0.0031 / 0.0041 |
| Baseline B | 0.524 / 1.193 | 0.420 / 0.653 | — | — |

A repeat of the same measurement on the final HEAD reproduced the frame-level picture (baseline 0.552 / 1.371 ms, HUD runs 0.546 / 1.250 ms and 0.549 / 1.209 ms, identical resource footprint). The HUD CPU median moved between 0.005 and 0.010 ms across runs on this machine while the GPU median stayed at 0.003–0.004 ms; the draw path itself did not change.

The hidden-workspace path also stops paying for the developer frame: the `DevUIBuild` scope measured in the HUD proof is roughly half of the visible cost — 0.016 vs 0.032 ms on an idle machine and 0.025 vs 0.052 ms on a loaded one — and what remains is ImGui's own frame enumeration plus the `KeepAliveOnly` dockspace submission.

#### Not executed in this correction pass

The graphical build was not run on Linux (GCC/Clang) or macOS/MoltenVK, and UNORM swapchain output and physical cross-monitor DPI transitions were not run. The CPU-only slice was run on Debian WSL with GCC under ASan+UBSan and under TSan. The per-frame screenshot capture of multiplayer feedback over bright terrain and sky was not produced physically: the copy, the `hudScrim` plate contrast and the showcase-strip yield rule are covered by the headless checks, and the transport transitions by the native join smoke. The proof docks the canonical window itself (via the dockspace central node) and then withholds the workspace, so the separated identity is exercised rather than described, and the ini-persistence mode repeats the docking association across two real processes; a player-driven dock drag onto an asymmetric side node was still not reproduced physically. The reconnect workflow was validated against the workspace and capture state the smoke session actually reaches — the two variants it does not reach are asserted in `GameUiComponents` — and a remote server that dies mid-session on physical hardware was not re-run in this pass.

#### Original slice, executed before this pass

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

The measured HUD used one draw, 936 vertices, four preallocated descriptor sets, 212,992 bytes of instance buffers and 32,768 bytes of font coverage. This sRGB path required zero composition-image bytes. Physical HUD runs used Windows. Linux, macOS/MoltenVK and physical cross-monitor DPI transitions remain unverified for #303. The correction pass did not change the draw path: one instanced HUD draw, the same preallocated descriptor sets, no intermediate composition image and no per-frame descriptor allocation.

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
| `Ctrl+Shift+D` | Shows/hides the complete developer workspace at runtime (issue #303 F04). Global, so it works while a developer text field owns the keyboard; the dockspace stays alive, `H` keeps the layout-tile toggle. |
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
