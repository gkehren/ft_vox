# Game UI migration and parity

Epic #298 separates the player-facing Game UI from the Dear ImGui Developer UI. The [architecture decision](adr/game-ui-backend.md) defines the renderer, ownership and input contracts. Normal play starts in the native Game UI. The Developer UI is available on demand. The sections below record the earlier migration checkpoints and the current ownership split.

## Developer UI separation (#308)

**F1** shows or hides Developer UI. **Ctrl+Shift+D** remains an alias. Pass
`--dev-ui` to request the workspace on launch. Developer panel shortcuts
**F2–F5**, **F7–F9**, **F11** and **F12** open their tool directly from hidden.
**H** hides and restores the current layout tiles within the workspace.
**F6** toggles native HUD hints and **F10** requests a session-only VSync
change at the swapchain boundary.

The Vulkan scene projects across the full output. Native HUD, Inventory
and Pause use the central dock area when the workspace is visible. With
the workspace hidden they use the full window. ImGui submits over the
native layer. **C** releases or recaptures a gameplay cursor. Showing
Developer UI preserves the current cursor state. Hiding it cancels ImGui
pointer presses, text input and open popups. Inventory drops its held
gesture when its layout changes. Full-window native screens and
confirmations keep input and suppress ImGui drawing. F1 updates the
workspace visibility to apply on dismissal. Connected Pause remains
local and the server continues to tick.

Engine builds `DeveloperUiFrame` only for a visible workspace or an explicit
biome-map benchmark. Hidden frames skip ImGui frame construction. The live
context and existing window IDs preserve dock associations, and
`imgui.ini` persists layout and developer scale between launches.
ImGui initialization, swapchain recreation and shutdown stay independent
of visibility. Native presentation receives bounded view models and submits
typed intents. Developer network and server views receive read-only
diagnostics. Native join, hosting and administration keep the existing
Engine adapters and verification checks.

| Surface | Route and parity |
| --- | --- |
| Graphics and Resource Packs | F2, including quality presets, per-pass controls and pack browse/apply/reset. Game Menu → Settings is the editable home for FXAA and VSync. Developer Graphics reports their live state. |
| Streaming and workload | F3, F11 and Developer → Memory. Streaming budgets, memory, queues and live workload remain available. |
| World and biome map | F4. Solo map, overlays and navigation remain available. Remote World continues to display the connected view. |
| Help and About | F5 opens developer Controls. The Game Menu provides its own Settings → Controls and About. |
| Performance, Overview, Chunk Inspector, Render Debug | F7, F8, F9 and F12. Developer also exposes Player Diagnostics. |
| Creative and debugging | Developer → Creative / debug controls. Retains solo flight, camera, creative block, target, demo players and passive mobs, with chunk borders available from Developer or B. Mouse sensitivity and inversion are in native Settings. |
| Status density and layout | View → Status overlay (Off, Minimal, Detailed). View → Reset layout / Apply default developer layout. |
| Benchmark and report | Developer → Benchmark for interactive profiling and reports. CLI benchmark and explicit `--benchmark-map` remain available. |
| Network and server monitoring | Developer → Network / server monitoring. Keeps client prediction and traffic metrics, fresh server counters and the hosted roster. |
| Player HUD, Inventory, Join, Host and administration | Native HUD, E and Escape → Multiplayer. Connection, profiles, hosting, stop and verified administration commands live in that flow. The retired ImGui forms shared those actions and kept separate drafts and confirmation state. |

### Verification on Windows and Vulkan

Captured the same seed and camera with [Developer UI visible](images/developer-ui/developer-workspace.png)
and [Developer UI hidden](images/developer-ui/player-hud.png) at 1280 × 720
on an NVIDIA GeForce RTX 4070 Ti (Vulkan 1.4.357). Both images use the engine
scene and UI composition path. The visible shot places the same native
hotbar inside the center dock area. The HUD proof confirmed that the ImGui
frame counter stayed fixed during the hidden interval and that the dock
tree resumed when reopened.

| Check | Result |
| --- | --- |
| Windows Release graphics-off build and CTest | 55/55 passed, including CoreIncludeGuard and HeadlessDeps. |
| Windows Release graphical build and focused CTest | Game and server built. 10/10 selected UI, graphics and biome-map tests passed. |
| Vulkan HUD/workspace proof | 66 checks passed. Covers default hidden, F1 and its alias, key repeat, drag cancellation, Pause, quit confirmation, Inventory, panel shortcuts, H, reset/default layout, swapchain, atlas and world changes. |
| Vulkan presentation and Settings | Present proof passed 39 checks. Settings passed 46 checks and the fresh-process restart proof passed, including independent developer scale and visibility and display rollback. |
| Graphical join and native hosted flows | Join smoke passed 63 checks, including reconnect with Developer UI hidden and F1 during connected play and Pause. Native Host/Join passed for owner and guest, including verified administration. The two-client hosted closure passed the inventory matrix, checkpoint and restart. |
| Production CLI benchmark | Wrote reports for the standard hidden run and the explicit `--benchmark-map 0.5` run. |
| Vulkan validation | The successful GPU runs reported the documented RTSS swapchain diagnostics. The HUD proof counted 14 such messages and zero additional errors. |

The separate solo inventory proof exercised F1 while a real ImGui text editor
owned the keyboard. Its existing ten pointer and inventory assertions also
failed on the pre-change binary on this desktop. The hosted inventory matrix
passed its ten operations with the original 117-item budget conserved. The
showcase proof reproduced the same two slider assertions on both binaries.
The Vulkan proofs injected SDL events and the resulting captures were reviewed
visually.

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

### Correction pass: ownership and held stack (#316)

- **Keyboard ownership (F01).** The Developer UI and the native inventory share one explicit keyboard owner, carried by `InputContext` over a persistent `ImGuiLayer` ownership latch (round 2 below — a `WantCaptureKeyboard` retained by passive ImGui focus never owns). While an active developer control, ImGui keyboard navigation or a non-modal popup holds the keyboard, every key — including navigation and back keys — goes to the Developer UI and the inventory screen receives nothing, so developer navigation can never trigger inventory gestures. Clicking an inventory slot transfers ownership back to the native screen, and the developer side cancelling input (modal, capture change, transition) also cancels the inventory's held stack and armed gesture. Global shortcuts, the pause menu and the existing modal/text-input priorities are unchanged; `E` and Escape stay contextual. The policy is unit-tested in `GameUiInteraction` and `InputRouting` and regression-tested by the solo inventory proof.
- **Held-stack placement (F03/F05).** `heldStackPlacement` (`src/Engine/PlayerInventory.hpp`) is the single pure geometry helper: it prefers the normal pointer offset, flips it when the icon would cross the right or bottom edge of the available game view, and clamps the final rectangle fully inside — compact, ultrawide and 200%-scale layouts keep the held stack visible, and placement follows the same available game view, so the central developer dock area is respected. The presentation follows the active input modality (`InventoryModality` in `GameUiSample.hpp`): pointer interaction anchors the preview to the pointer; keyboard pickup and arrow navigation anchor it to the focused slot, falling back to the source slot when no valid destination is focused. Passive mouse events do not override keyboard navigation, switching modality neither cancels nor submits an armed gesture and mutates nothing, and the transient modality state resets with the screen (close/reset).
- **Unified pending tracking (F04).** HUD shortcuts (`1–9`, `Q`) and native gestures share one submission adapter, `Engine::submitRemoteInventoryAction` (`src/Engine/EngineMultiplayer.cpp`). The affected slots publish only after a successful submission, so a HUD selection or drop followed by opening the inventory shows the pending state on the relevant slot; the action result, disconnect, resync and session reset clear it, and failed submissions never leave stale metadata. Terrain actions keep their own pending flag. The single-action-in-flight policy and the wire protocol are unchanged.

### Correction verification (#316 F01–F06)

Windows 11, MSVC Release, GeForce RTX 4070 Ti, Vulkan 1.4.357, SDL 3.4.18. Every row was executed on the corrected HEAD of #316. The `--ui-ini-persistence` proof mode and the `GameUIInventory.cpp` ImGui panel are retired and no longer part of the reproducible procedures.

| Check | Result |
| --- | --- |
| `test_engine_reload.exe --player-inventory-proof` | PASS, 40 checks, including the F01 keyboard-ownership scenario (developer navigation/popups own every key, clicking the inventory hands ownership back), the F03 edge/clamp placement, and the F05 modality-following held stack; `errors=21 known_rtss_errors=21` (only the known RTSS swapchain messages) |
| `test_engine_reload.exe --player-hud-proof` | PASS, 48 checks |
| `test_engine_reload.exe --game-ui-showcase-proof` | PASS, 39 checks |
| `ctest -C Release` (graphical build) | 66/66 excluding the multiplayer label; `VisualRegression` passes with the committed goldens (the offscreen scenes contain no player UI) |
| `ctest -C Release -L multiplayer` | 9/9 passed |
| `tests/test_multiplayer_join_native.py` (graphical client) | 60 checks passed: readiness, reconnect, blocked-session recovery, motion tracing across switches |
| Manual hosted two-party session (the real game joined to a dedicated `ft_vox_server` with the F04 submission trace) | 37 accepted native gestures confirmed by authoritative revisions: 23 selects, 4 whole moves (hotbar↔grid both directions), 3 half splits (an odd 3-stack split into 1+1+1), 3 drop-one (exactly one unit each, later re-collected — conservation), 1 whole-stack drop. Two authoritative rejections left the confirmed inventory untouched. Server stop-file shutdown committed checkpoint generation 2 |

A first attempt at the scripted two-client hosted closure (`tests/test_hosted_closure_native.py`) failed with a native `0xC0000005` in both clients; this was resolved in correction round 2 below (the root cause was the F07 harness defect — dangling references to by-value optionals — not a production issue), and the closure now passes repeatedly on the same machine.

### Correction round 2 (#316 F01, F07–F11)

Round 2 replaces the fragile hosted-closure validation with deterministic checks and fixes the reliability findings the first round surfaced. The architecture, server authority, wire protocol and save formats are unchanged.

- **Keyboard ownership latch (F01).** Ownership is now persistent instead of per-event: a meaningful Developer UI signal — an active widget, an editing field, an open popup or a visible keyboard-navigation highlight (`ImGui::GetIO().NavVisible`) — latches `ImGuiLayer::m_devKeyboardOwner` at the frame boundary, and only a meaningful move toward the player UI releases it (clicking the native screen, focus loss, a workspace or session change). A passive `WantCaptureKeyboard` retained by an idle ImGui window never latches, so it can neither swallow the contextual `E`/Escape nor trigger inventory gestures, while a focused button *without* an active `ActiveId` keeps its keys. Global shortcuts, modal and text-input priorities are unchanged.
- **Memory safety in the validation harness (F07).** The root cause of the previously reported `0xC0000005` was in the test harness: `ClientTransport::inventory()` returns `std::optional<wire::Inventory>` **by value**, and the closure bound references to those temporaries (`const auto &inventory = *remote.transport.inventory()`) and ranged over `transport.inventory()->slots` — reading destroyed storage. Every access now takes an owned snapshot, missing authoritative state is a terminal failure with its own diagnostic, and nothing is retained across `pump()` calls.
- **Deterministic multiplayer inventory matrix (F09/F02).** The exhaustive operation matrix no longer mines: after one shared-terrain break (the original closure contract), the owner stops the server, rewrites the committed checkpoint roster into a known fixture (owner: 64/40/5 of material A, 8 of material B, selection 3; guest: 8 of a distinct item, selection 6) with the commit manifest's roster checksum kept consistent, and restarts on the same world. The matrix then drives whole move (+ duplicate-input once-only), odd split (5→2+3), merge, partial merge (destination to capacity, remainder preserved), swap, drop one, drop stack, pickup restoration and a rejected drop from a dynamically discovered empty slot, all through the real native screen and the real client/server action path. World entities are identified by entity id against a baseline snapshot, awaited independently of the action acknowledgment, and the actor is steered toward the tracked drop so the shared 1.5-block auto-pickup radius is actually entered. Guest private-state isolation is polled across the whole matrix and re-verified after the checkpoint restart. See the hosted-closure row in the verification table below.
- **Held-stack bounds (F10).** `heldStackPlacement` constrains the effective cell to the available view (`std::min({cell, available.x, available.y})`) before flipping/clamping, uses that effective size consistently, keeps the square aspect and returns an empty rectangle when nothing fits — a viewport smaller than the requested cell can no longer produce an overflowing preview. `tests/test_game_ui_components.cpp` covers strictly smaller, narrow, short, offset and empty viewports.
- **Input-modality filtering (F11).** The `InventoryModality` (pointer vs keyboard, F05) only changes on a *supported* interaction: unsupported buttons return before touching the preview, modifier-only keys and chords leave it, passive motion and irrelevant releases never reach the branches, and unsupported/unhandled keys do not move it. A modality change never submits or cancels a gesture on its own.

#### Round-2 verification

Windows 11, MSVC Release, GeForce RTX 4070 Ti, Vulkan 1.4.357. CPU-only and graphical ctest suites were re-run after the round-2 changes; the hosted closure ran repeatedly on the same build (see below). The previously reported `0xC0000005` was traced to the F07 harness defect above; with the dangling reads fixed, repeated hosted-closure executions complete without any access violation, on the same machine that reproduced the crash.

| Check | Result |
| --- | --- |
| `test_engine_reload.exe --player-inventory-proof` | PASS, 42 checks — F01 latch (passive focus does not steal keys, clicking restores native nav), F10 bounds and F11 filtered modality; only the known RTSS swapchain validation messages |
| `test_engine_reload.exe --player-hud-proof` | PASS, 48 checks |
| `test_engine_reload.exe --game-ui-showcase-proof` | PASS, 40 checks |
| `ctest -C Release` (graphical) | 75/75 passed (full suite, includes the 9 multiplayer tests) |
| `ctest -C Release -L multiplayer` | 9/9 passed |
| `tests/test_multiplayer_join_native.py` | PASS, 60 checks (readiness, reconnect, blocked-session recovery, motion tracing across switches) |
| `tests/test_hosted_closure_native.py` (two native clients + hosted child) | PASS end to end (deterministic fixture matrix + checkpoint restart phase), repeated consecutive executions without an access violation — see the F07 note above for the resolved crash |

### Correction round 3 (#316 F01, F11–F14)

A small final pass over the ownership handoff, the input-modality filter and the validation harness. The architecture, server authority, wire protocol and save formats are unchanged.

- **Right-click ownership handoff (F01).** A right-click on an inventory control now transfers keyboard ownership to the native screen exactly like the left-click path (owner-checked; gameplay and other native screens keep left-click-only), so a half-stack gesture can start immediately after interacting with the Developer UI. While a deferred handoff drains, stale ImGui signals (a released `ActiveId`, a lingering `NavVisible`) cannot re-latch the developer ownership: the round-2 latch observes them only when no yield boundary is pending. The inventory ownership proof now covers a real non-textual ImGui button: focus and release without an active `ActiveId`, developer-only Tab/arrows/Enter/Space, right-click handoff, deferred frames without re-latching and a completed half-stack split through native input.
- **Modality filtering completion (F11).** Pointer modality changes only on a validated press over a control or the matched release that activates it; keyboard modality updates only when supported navigation actually moves the focus (the previous focus is recorded before navigation, covering Tab, Shift+Tab and the arrows), and the unreachable Tab condition in the arrow branch is gone.
- **Checked harness access (F12).** The hosted closure reaches mandatory remote session state only through a checked helper that fails with its own diagnostic when the authoritative snapshot is missing; polling state (peer visibility, transiently unavailable inventory snapshots) keeps waiting within the deadline instead of dereferencing.
- **Guest synchronization (F13).** The owner waits for two guest-published rendezvous markers: one after the guest observes the live terrain edit before the checkpoint fixture is prepared, one after the guest verifies its exact fixture inventory and hotbar selection before the matrix starts. The guest polls its private inventory and selection until the matrix completes and re-verifies both after the checkpoint restart.
- **Pending-state regressions (F14).** Two new matrix rows: a duplicate native gesture while an action is pending must not replace the pending metadata, mutate the confirmed inventory or submit a second command (complementing the direct adapter rejection check), and HUD shortcuts submitted with the inventory closed (hotbar selection and drop) expose the affected slot as pending when the inventory opens, then clear on the authoritative result.
- **Windows CI `GameUiStyle` concurrency failure.** The R03 reader check (`concurrent readers never report a false corrupt record or a partial value`) failed once in `cpu-windows`. The persistence code is from #302 and untouched by this PR; the failure also reproduces on `main` (run 45719082861) but not locally (40 consecutive passes), and is tracked as issue #317.

#### Round-3 verification

Windows 11, MSVC Release, GeForce RTX 4070 Ti, Vulkan 1.4.357, all proofs with `FT_VOX_VALIDATION=1`.

| Check | Result |
| --- | --- |
| `test_engine_reload.exe --player-inventory-proof` | PASS, 49 checks (round-3 ownership sequence including the non-textual button; only the 21 known RTSS swapchain validation messages) |
| `test_engine_reload.exe --player-hud-proof` | PASS, 48 checks |
| `test_engine_reload.exe --game-ui-showcase-proof` | PASS, 40 checks |
| `ctest -C Release` | 75/75 passed |
| `tests/test_multiplayer_join_native.py` | PASS, 60 checks |
| `tests/test_hosted_closure_native.py` (two native clients + hosted child) | PASS end to end, 15 checks, including both round-3 guest rendezvous points |

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
| Inventory and drag/drop | Native since #304: `GameUiInventoryScreen.cpp` owns the 3×9 grid, hotbar, focus, held-stack presentation and gestures; `InventoryUi.hpp` supplies typed slot identities and pure whole/half click planning. Engine emits one existing command per completed gesture (solo sim result immediately; remote through the shared `submitRemoteInventoryAction` adapter with per-slot pending state). The native renderer samples the same generation-tagged atlas as the HUD. | **#304 Inventory — landed.** Parity record: selected slot, whole/half stack and partial transfers, disabled input during remote pending state, rejected-command feedback, tooltip/icon behavior and drag cancellation on close, focus loss, session or resource change; authoritative inventory compared before and after solo and remote gestures. |
| Solo world and pause | `--world` and `--seed` enter a saved or transient world through Engine initialization. World panel shows seed, save identity, queue/health and errors. `P` mutates host pause, `C` controls host cursor capture, and Escape or the menu requests exit. Engine owns save, flush, world epoch reset and restoration. | **#305 Main Menu, Loading and Pause.** Create/select an existing world or start transient play through the existing lifecycle. Preserve CLI entry points, save error visibility and session-epoch barriers. Validate open, resume, save-and-leave, exit and retry. Show save health to players and detailed persistence counters in Dev UI. |
| Join, profiles, and client status | `GameUIMultiplayer`, `EngineJoin` and `EngineMultiplayer` own endpoint/expected seed, favorites, named profile creation/selection, private keys, loading/readiness, reconnect, switch, disconnect and return to suspended solo. The public roster and private inventory arrive through the remote client path. | **#306 Multiplayer.** Preserve favorites/profile persistence, unavailable/error states, endpoint validation, two-client roster and local inventory privacy, reconnect identity, and blocked transition on solo save failure. Put actionable status and latency in Game UI and detailed packet, snapshot and motion telemetry in Dev UI. |
| Host, roster and stop | `EngineHosting` and `ServerSupervisor` enumerate server worlds, launch a sibling server and surface readiness, configuration, endpoint copy, auto/manual join, roster and delayed/failing stop. The engine coordinates app exit and force-stop confirmation. | **#306 Host.** Preserve existing/new world and profile selection, private bind guidance, port/player/interest/worker/streaming options, readiness, copy/join, confirmed graceful stop, Wait/Force Stop and exit recovery. Verify stop and restart with checkpoint persistence. |
| Local administration | Hosted panel uses server-verified local permissions. The owner and authorized local delegates have different capabilities. Scope and credentials stay with the local control path. Roster, offline IDs, admission policy, moderation, confirmations and audit are already surfaced. | **#306 Administration.** Preserve permissions, target confirmation, audit and server result/error semantics. Cover grants/revocations, authorize-local-control, kick, ban/unban, allowlist membership/mode and unauthorized guest/remote rejection using the existing [hosting closure](multiplayer-hosting-closure.md). |
| Display, inputs, quality and assets | The Graphics panel owns quality presets, VSync, rendering controls and pack browser/apply/use bundled. The Player panel forwards sensitivity and camera settings. `ImGuiLayer` persists a separate docking/scale configuration in `imgui.ini`. Engine applies VSync at swapchain recreate and handles failure-atomic texture replacement. | **#307 Settings.** Window size, player UI scale, VSync, FXAA, and mouse look use typed requests and independent client preferences. Graphics presets, view distance, resource packs, and per-pass controls remain available in Dev UI. |
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
| Player UI scale | `PlayerUiScale.hpp` + `PlayerUiPreferences.*` (Engine loads/persists) | Auto by default. **Settings → Preview display changes** previews the selected scale and **Keep changes** saves it. The design gallery also exercises the scale controller. |
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

## Main menu and local sessions (#305)

A graphical launch without a world, seed, remote endpoint, benchmark, inspection
or UI proof starts at the native main menu. It has no simulation or terrain
projection, and displaying the menu does not create a world. The developer
workspace starts hidden on this launch path. `--world <name>`
still opens or creates that save directly, `--seed <n>` still starts a transient
world, and `--connect`, `--benchmark`, `--view`, `--ui-spike` and `--game-ui`
retain their direct entry points. The server and headless runners keep their
existing command lines.

- **Local worlds** lists up to 64 saves from a scan of at most 512 directory
  entries. Refresh rereads the directory. Open uses the selected world's
  metadata, including its seed. A missing save produces an error.
- **Create world** takes a new name of 1–32 ASCII letters, digits, dashes or
  underscores. Leave the seed blank for a random seed, or enter an integer
  from 1 to 2147483647. Creation reserves a new directory, so reusing a name
  cannot open or overwrite an existing save. A failed creation releases its
  empty reservation so the same name can be retried. A folder containing data
  is preserved and the screen directs the player to refresh or use another name.
- **Escape** opens Pause in play and moves back through menus and confirmations.
  Use **Save world** to checkpoint and stay in Pause, or **Save and return to
  menu** to checkpoint and close. Solo uses the existing suspension policy.
  A remote Pause blocks local controls while the server continues ticking.
  **Leave server** disconnects the client and keeps an independently hosted
  server running.
- **Exit to desktop** and the window close button share the Engine's exit
  path. Exit first saves any local world, then stops a server launched by
  this client and waits for its final checkpoint. A delayed hosted stop
  offers the supervisor's existing force-stop operation behind confirmation.
  An unconfirmed hosted checkpoint remains visible for an explicit exit
  decision. A local checkpoint failure keeps the world open for retry or
  return to play. A close during loading, saving, leaving, or a queued
  connection change is remembered. A failed transition pauses that quit.
  Retry keeps the quit pending through a successful retry, and Back cancels it.
- **Multiplayer** and **Settings** open native screens from the main and pause
  menus. Dev world and player actions require a live world.

The native UI queues one typed request per transition. The loop consumes it
before frame acquisition and presents a loading screen before retiring the
projection when visible. Filesystem scans, world initialization and terrain
generation, and save/checkpoint work run away from SDL input dispatch. During
solo save and close, the worker exclusively owns the simulation. The frame
owner retires or rebuilds the Vulkan projection at the world boundary and
resets the frame clock as control returns. Gameplay stays suppressed through
screen or focus changes, and the epoch barrier clears HUD, inventory and
render caches. Player preferences and the developer layout keep their
existing storage. Engine validates each intent against live session ownership
before starting work and checks that the frame-owner slot is empty before
publishing a worker result.

Projection reuse on explicit save is deferred. A save currently drains and
retires the projection, checkpoints with exclusive CPU world ownership, and
rebuilds the nearby projection on the frame owner. Keeping the projection
requires a separate quiescence contract for retained world references.
Catalogue pagination is also deferred. Refresh repeats the bounded scan and
the screen reports when it stops at either limit. Use direct
`--world <name>` for existing saves outside that list.

The opt-in Windows Vulkan proof writes launch, world selection, loading,
pause and recoverable error captures. It uses a fresh output directory and
stores its fixture under that directory. It drives SDL keyboard, pointer,
window-close and minimize events, and uses injected persistence faults to
verify recovery.

```powershell
$env:FT_VOX_VALIDATION = "1"
.\build\tests\Release\test_engine_reload.exe --session-menu-proof <fresh-output-directory>
```

Captured on Windows with an NVIDIA GeForce RTX 4070 Ti at 960×540.
The proof exercises the production screen renderer and lifecycle with typed
intents and SDL events. Vulkan validation reported the documented RTSS
swapchain errors during initialization and recreation. All validation
callbacks during the corrected session proof matched that known signature.

[Launch](images/session-menu-305/launch.png) ·
[World selection](images/session-menu-305/world-selection.png) ·
[Loading](images/session-menu-305/loading.png) ·
[Pause](images/session-menu-305/pause.png) ·
[Recoverable error](images/session-menu-305/recoverable-error.png)

## Native multiplayer (#306)

Open **Multiplayer** from the main menu or Pause. **Join server** accepts a
private IPv4 endpoint and optional expected seed. Use **Profiles** to create
or select the local identity, and save endpoints as favorites or reuse recent
connections. A reconnect uses the active session's original profile. Use
**Switch server or profile** to submit a different draft. **Play** becomes
available when the confirmed player, inventory and terrain are ready.

**Host server** starts the existing supervised dedicated process. Choose a
server world and local profile, select a port or use 0 for an ephemeral port,
and choose whether to join when ready. The hosting options page controls
loopback versus an explicit trusted LAN or VPN address, player slots, interest
radius, terrain workers and streaming budget. Cancel a connection separately
from stopping a server. A confirmed stop waits for the checkpoint. The
force-stop action appears only after a delayed shutdown and warns about
uncommitted changes.

**Players** shows fresh full monitoring data on the local host and the nearby
players included in a connected client's snapshots. The wire protocol provides
nearby actor IDs to remote clients. Full roles come from the locally verified
administration reply. **Connection details** shows each client's own RTT
estimate, sample age and missing snapshots when available. Transport and
prediction counters remain in Developer UI.

**Administration** uses the launcher's verified local control channel. Refresh
the registered player list and audit before acting on newly registered players.
A confirmation captures the server launch, permission epoch, operation and
selected player. The Engine rechecks those values against the supervisor before
dispatch. Kicking also requires a fresh live roster entry. A role-only remote
profile has no local control channel. Owner recovery remains the documented
offline dedicated-server operation. [Dedicated server guide](dedicated-server.md)

The original Developer UI panel remains available during migration. The native
player flow uses the same Engine adapters for profiles, preferences, join,
hosting and administration. No credentials or child logs enter its view model.

Run the opt-in proof from a Windows Release graphics build. It launches two
graphical clients with isolated profile directories and an ephemeral loopback
server, drives the native controls through SDL and writes PNG evidence.

```powershell
$env:FT_VOX_VALIDATION = '1'
python tests/test_hosted_closure_native.py --menu --server build/Release/ft_vox_server.exe --client build/tests/Release/test_engine_reload.exe --output build/native-multiplayer-evidence
```

The historical hosted-closure phases (persistent identities and inventories
across a checkpoint restart, without the native menu) stay on the same runner:

```powershell
python tests/test_hosted_closure_native.py --server build/Release/ft_vox_server.exe --client build/tests/Release/test_engine_reload.exe --output build/hosted-closure-evidence
```

### Review corrections (#306 round 2)

- **Identity-stable administration targeting.** The registered-player list
  selection is a `sim::PlayerId`, never a row index: roster reorderings keep
  the target, a disappearing target clears the selection into an explicit
  unselected state (no row highlighted, targeted actions hidden until a
  player is chosen again), and a press armed when the target changed is
  cancelled. No destructive operation can implicitly fall back onto another
  player.
- **Kick semantics are explicit.** A kick intentionally targets the persistent
  player identity: the server stores a 60-second `kickedUntil` lockout on the
  persistent access record, actively closes the session and refuses rejoin
  while the lockout runs. The confirmation says so, and both the client
  revalidation and the server refuse to kick a target that is not online with
  a fresh monitor sample. A role revocation also demotes by disconnecting the
  target so it re-enters as a plain player.
- **Refused requests produce typed feedback.** Every drained multiplayer
  request is evaluated before dispatch (`gameui::evaluateMultiplayerRequest`):
  `StaleSession`, `ServerChanged`, `Busy`, `InvalidState`,
  `PermissionChanged` or `ActionUnavailable` surface on the screen that owns
  the action, scoped to the session generation so an old refusal can never
  mask a newer session. Navigating away before the pump runs is an intentional
  cancellation and stays silent. The Engine adapters remain authoritative.
- **Draft lifecycle.** The join endpoint follows the session context (the
  connected endpoint or the most recent saved one) only while the player has
  not edited it; an edited draft survives navigation and session changes.
  Operation-specific errors clear when their screen family is left.
- **Clipboard copy reports success or failure**, roster rows rebuild only when
  the monitor sample or verified roles change, and the multiplayer state in
  `GameUiSample` stays grouped for a future `MultiplayerScreenController`
  extraction.

The `--menu` proof covers: profile creation and selection, host start with
auto-join, invalid endpoint and wrong-seed rejections, favorites, live roster
and independent ping, grant admin with confirmation, an obsolete-epoch
refusal, delegated control refused across the credential boundary, an
end-to-end kick (the guest observes the disconnect and the refused rejoin),
ban and unban of the offline target, allowlist enable, membership and
disable, a cancelled stop with no side effect, stale epoch, stale launch and
vanished-target confirmations refused through the real execution pipeline,
single-execution audit evidence, stop-and-save with checkpoint, and the
occupied-port bind failure. CPU-side, `test_multiplayer_intents` pins the
eligibility and outcome rules for every operation without a GPU.

### Review corrections (#306 rounds 3 and 4)

- **Endpoint editing is stable.** The automatic endpoint is re-published only
  when its value changed and never while a text composition or an active
  text selection is in progress: the caret, selection and IME state survive
  every frame. A synchronization deferred while editing retries once the
  field is left, an untouched draft follows the session context, and an
  edited draft survives navigation and session transitions. Choosing a
  favorite or recent endpoint counts as an explicit edit.
- **Every multiplayer list scrolls independently.** The shared list helper
  reconciles the selected row index and the scroll offset
  (`ListSelection::reconcile`) without revealing anything: wheel browsing
  works with and without a selection across favorites, recent endpoints,
  saved profiles, hosted worlds, the live roster, delegated-control
  profiles and the audit history, the offset survives unchanged views and
  clamps on shrinkage, and only an explicit click or a keyboard step
  reveals a row.
- **The administrative selection is server-scoped.** It belongs to one hosted
  launch, one verified world identity and a live authorization. A relaunch, a
  world change or a lost channel clears the target and the cached reply, so
  identical numeric PlayerIds across worlds or launches can never inherit a
  target.
- **Refusal feedback is generation-scoped.** Server-scoped notices name their
  launch; a `ServerChanged` verdict is reported against the new launch and
  clears on the next launch change, so an old result never masks the current
  server state. Client connection feedback stays scoped to its session
  generation.
- **Transient results never mask state.** A persistent status slot
  (connection or supervisor state) paints beside the transient operation
  feedback: the clipboard result displays independently of a standing
  connection error, the supervisor state stays visible beside a hosting
  operation error, and an administrative refusal stays visible after the
  channel itself disappeared.
- **The gameplay boundary is restored.** `pumpPlayerMultiplayer` returns
  before building any frame or view when no multiplayer screen is active; a
  queued request is consumed as a silent, intentional cancellation. Hosting
  supervision, auto-join and the remote session keep advancing in their own
  ticks, and the first pump after re-entering a screen rebuilds current data.

The final review pass also hardened the proof harness itself. The `--menu`
proof flushes queued real pointer events before each synthetic click
(desktop mouse motion or window leaves cancelled armed activations and
silently dropped navigation clicks), re-issues the idempotent
administration refresh until the registry names the session the monitor
observes (the guest's deliberate seed-mismatch rejection re-registers it a
moment after the roster first shows two players), and drives a UI scale
sweep: the Join screen stays reachable at 100%, 150% and 200% on a
1920x1080 window with keyboard navigation revealing the primary action,
with `join-scale-100.png`, `join-scale-150.png` and `join-scale-200.png` as
evidence. The F17 regression lives in the proof as well: a six-row saved
profiles list keeps its wheel scroll offset across screen reconstructions
and a row picked outside the initial window drives the profile action. CPU
tests pin `ListSelection::reconcile` (selection clamp, offset clamp, `-1`
preserved) and the deferred endpoint synchronization retry.

### Validation record (#306)

Covered by existing suites (verified, not duplicated here):

- Unsupported protocol versions: the `rejectHello` version matrix in
  `test_wire_protocol`.
- Server capacity rejection: the excess-client `Full` rejection in
  `test_udp_transport` and the `Full` status mapping in `test_multiplayer_ui`.
- Lost connections and stale monitoring: the timeout/retry path in
  `EngineJoinSmoke`, `test_server_restart_client` and the hosted-closure
  phases.
- Checkpoint failure: `test_world_checkpoint` and the persistence contracts in
  `test_voxel_world` / `test_world_save`.
- Delayed shutdown and force-stop safeguards: `test_server_supervisor`
  (direct force-stop refused, delayed stop escalation, stale-roster
  freshness).
- CPU intent rules: `test_multiplayer_intents` (eligibility, typed outcomes,
  notice lifetime, cross-world selection scope) and the `ListSelection`
  scrolling and reconcile cases in `test_game_ui_interaction`.

Executed graphical validation (Windows, RTX 4070 Ti, validation layers on):

- `python tests/test_hosted_closure_native.py --menu ...`: two consecutive
  clean runs (25 owner checks, 27 guest checks), including the scale sweep
  screenshots at 1920x1080.
- `python tests/test_hosted_closure_native.py ...` (first and restart
  phases): host and guest across checkpoint restart, 71/29/13/15 checks.
- Full `ctest -C Release`: 76/76. CPU-only configure
  (`FT_VOX_GRAPHICS=OFF`): 54/54.

Remaining manual validation before closing #306 (NOT executed here):

- A positive delegated local-control authorization with a correctly enrolled
  local profile on the launcher machine — the isolated two-process proof can
  only verify the credential-boundary refusal.
- Human inspection of the native screens on the GPU machine: administrative
  roster scrolling with 10+ registered players, scrolling in favorites,
  profiles and hosted worlds, endpoint editing with caret and text
  selection, and a visual check of the three scale screenshots for clipping
  or inaccessible controls.

## Player settings (#307)

Open **Settings** from the main menu or pause menu. Window sizes are logical
units from 960 × 540 to 1920 × 1080. Click or press Enter on the window size
or UI scale to cycle the choices, then select **Preview display changes**.
Engine sends SDL one resize request and uses the acquired framebuffer size
for Vulkan. The confirmation screen shows the requested and current values.
**Keep changes** saves the preview. **Revert changes**, Escape or 15 seconds
without confirmation restore the previous size and scale. The confirmation
deadline also runs while the window is minimized or hidden. Rollback remains
pending until the window can render again, and its verification timer only
accrues while the framebuffer is available. Engine verifies the restored
logical size, framebuffer and scale before reporting success. A recoverable
Vulkan configuration refusal rolls back and refreshes all swapchain consumers
before the next acquisition. If exact rollback times out, Engine tries the
available framebuffer with a supported Vulkan extent and preserves the
previous UI scale and present mode where supported. A successful fallback
reports that the resolution could not be restored exactly and keeps the file
unchanged. Fallback creation and presentation attempts are bounded. Fatal
Vulkan errors and an unusable fallback follow the client error path.
Restore a maximized window to use the size selector.

**Keep changes** is available after a frame successfully presents with the
candidate settings. An out-of-date acquisition or presentation suspends
confirmation until the replacement swapchain presents again. The original
confirmation deadline keeps running.

VSync changes at the next swapchain boundary. Off requires Vulkan's
IMMEDIATE mode and a refused request leaves the previous mode active. The
FXAA switch affects scene rendering. Sensitivity is a percentage of the
default mouse response (25–300% in steps), and vertical inversion changes
local look input. **Controls reference** is browsable with Up and Down.
**Reset to defaults** previews changes to window size or scale. Cancelling the
preview keeps every preference. Resetting a maximized or otherwise non-resizable
window keeps the saved size, explains how to reset it, and resets the other
eligible preferences. The defaults are 1920 × 1080 windowed, VSync and FXAA on, 100%
sensitivity, normal vertical look and Auto UI scale.

Engine writes the versioned record under
`<saves-root>/.client-settings/player-settings.txt` using a unique staging
file and atomic replacement. This is a client-wide setting shared across
solo worlds and remote connections. A missing record uses defaults. Invalid
or unreadable records are reported and kept until the next explicit save.
A failed write leaves the change active for the run and offers **Retry saving
preferences**. The current file requires the complete current schema. Its
absence permits Engine to read the scale-only legacy
`<saves-root>/.client-profiles/player-ui.txt` at startup. The next explicit save
writes the new record, and the legacy file remains available to older builds.
The design gallery previews scale temporarily and restores the previous player
policy when leaving. ImGui scale and docking use `imgui.ini`.

Startup applies a saved window size through the same recoverable display path
without showing a confirmation dialog. It verifies the size with the working
present mode, then applies the saved or command-line VSync choice. If the size
is refused, startup restores the initial size or uses the available framebuffer
and reports the outcome in Settings. The saved record and unrelated preferences
are preserved in either case.

`--vsync` and the FXAA value selected by `--quality` take precedence over the
record for the launch. Those controls show the active value and the override.
Reset preserves the stored value for each overridden setting, so a subsequent
launch without that flag restores the prior choice.
CLI benchmark and inspection runs skip the preference loader and follow their
existing launch policy. Dev UI controls and F10 still make session-only changes
where their shortcuts apply. Opening Settings reads the live values and leaves
developer experiments in place. Changing one player setting persists that
setting without adopting other live developer values. Settings is client-side
presentation and input configuration, independent of world saves, connection
credentials, gameplay authority, and server configuration.

### GPU evidence

The opt-in proof presents through the real Vulkan swapchain and captures the
same UI and scene into readback images after setting the window size. Build
`test_engine_reload`, run it with the built resources available, then use
`--player-settings-proof <fresh-output-dir>` to reproduce the captures.
Invoke `--player-settings-restart-proof <same-output-dir>` in a fresh process
to verify the persisted fixture against the startup loader.
On the Windows RTX 4070 Ti, the Settings proof passed 46 checks and the
fresh-process proof passed 10. The tests inject Vulkan configuration
refusals, fatal device errors and SDL rollback failures. They check recovery
and acquisition, preserve the confirmation deadline across recreation, and
compare the saved record after startup, cancelled previews, CLI overrides,
and a write retry.
Validation reported only the known [RTSS diagnostics](vulkan-validation.md#srgbstorage-errors-from-rtss-issue-99).

- [Settings at 960 × 540](images/player-settings/settings.png)
- [Resize and 125% scale confirmation at 1280 × 720](images/player-settings/resize-preview.png)
- [Settings after applying the display and input changes](images/player-settings/settings-applied.png)
