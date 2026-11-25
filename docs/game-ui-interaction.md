# Game UI interaction

Phase C (#301) adds the screen stack, event ownership and reusable widget behavior. Start the graphical game from its build output and open the showcase with Escape → **Widget showcase**.

```sh
ft_vox --seed 42 --game-ui --quality low
```

The default launch keeps the current interface during migration. In the opt-in mode, the existing inventory still uses the same ImGui presentation, inventory snapshots and Engine commands. The native stack manages its input and lifetime. The ordinary HUD is passive. The opt-in renderer strip has one scale control when the cursor is free.

## Navigation

| Surface | Input and session behavior |
| --- | --- |
| HUD | Gameplay owns available controls. Escape opens the game menu. Solo P opens the same menu. |
| Inventory | E opens it and releases the cursor. E or Escape closes it when a developer text field or dialog has no claim. Local controls are suppressed. Solo simulation and the server continue. |
| Game menu | Resume, Escape or solo P returns to the HUD and restores cursor capture. Solo simulation uses the existing host suspension policy. In a connected world, Engine sends neutral controls and continues receiving state and presenting the remote world. |
| Widget showcase | Tab and Shift+Tab traverse available controls. Enter and Space activate buttons and toggles. Arrows adjust the slider or select a list row. Wheel scrolls the visible list or the panel. Back restores focus to the opener. Changes are local to the demonstration. |
| Exit confirmation | Escape or Cancel dismisses the confirmation and restores focus. The full-window modal temporarily hides Developer UI and vetoes view shortcuts. Confirm uses the existing exit and save path. A hosted shutdown returns to the Host panel for progress and recovery. |

Since #303, Escape opens the game menu in normal play. `--game-ui` enables the optional widget strip. Function keys still run once per press, with F6 controlling player HUD hints and F1 cycling developer diagnostics. Ctrl+Shift+D shows or hides the complete developer workspace while keeping its dockspace and layout alive, so gameplay and the developer surfaces swap without a restart; the legacy inventory keeps floating in either state. The F1 Developer UI toggle is tracked in #308. Session changes clear stale focus, gestures and fields before attaching the new world. The screen stack also represents Main Menu and Loading for the future lifecycle screens. [Live HUD and controls](game-ui-migration.md#live-player-hud-303)

## Input and geometry

Engine translates SDL events to key, pointer and text ownership. The first live pointer press owns motion. Later buttons can join the same surface. A cross-surface press is quarantined through release and cannot mask a new gesture once the old one ends or is cancelled. Wheel input belongs to the surface under the pointer when there is no gesture. During a gesture, only its owner can receive the wheel. Passive ImGui hover does not grant a button edge. A developer click that precedes ImGui's next frame also reserves keyboard ownership during that event batch.

Keyboard and text ownership transfers in both directions. A click that a native screen accepts as a widget press — or, in the default launch, a click that returns to gameplay — releases the developer reservation in the same SDL batch: `yieldKeyboard()` stops the developer IME session and provisional SDL text input, drops the pending press, and records the last developer-bound event ID as the handoff boundary. Events at or before that boundary still reach the editor that was active when the click arrived, so an earlier commit is never lost. Later events stay outside ImGui until it builds again, so a click, key or preedit never lands on the outgoing editor. The reservation ends at `EndFrame`, which clears the active editor and keyboard state. The reverse transition already existed: a developer press over a native editor calls `yieldTextInput()`, which clears the native composition, stops its SDL session and clears native focus, while ImGui keeps SDL text input provisionally active until its new `InputText` becomes the active ID. Neither surface keeps stale editing state, and no synthetic click or release is generated.

Pointer policy distinguishes three overlay behaviors:

| Surface | Pointer | Keyboard and text |
| --- | --- | --- |
| Interactive Developer UI window | Owns the press, drag and wheel | Owns keys while it has an active editor or control |
| Passive occluding overlay | Blocks the press so an obscured native control cannot be activated | Claims nothing; gameplay continues to receive keys and text |
| Pass-through overlay | Ignored; the surface below stays reachable | Claims nothing |

A window that sets `ImGuiWindowFlags_NoMouseInputs` occludes by default; the read-only status overlay and the multiplayer status line opt out by calling `ImGuiLayer::markCurrentWindowPassThrough()`. Overlays that intentionally shield a visually obscured control therefore keep that behavior, and new passive surfaces must state their intent. The remaining Developer UI toggle and panel-layout work stays in #308.

Focus loss and native modal suppression cancel developer ownership, pending ImGui edges, active widgets and SDL capture. Focus notifications survive cancellation. The vendored SDL backend captures on routed mouse edges and exposes a small capture-reset hook. Engine disables SDL auto-capture so a physical button that the router has quarantined cannot keep OS capture. On a failed relative-mode request, Engine reports SDL's error and reads back the actual mode. Gameplay input stays blocked until an explicit capture request or a screen transition succeeds.

Row and column stacks, fixed grids and scroll viewports build logical rectangles. Screens bind each text field, toggle, slider or list to independent state and receive typed actions tagged with their control IDs. The widget layer handles input and paints through the bounded DrawList. The showcase binds two groups to the same implementations. Every effective text modification — committed text, deletion, cut or a completed paste — reports exactly one `Change` action for the focused binding, while an unchanged value, preedit, caret movement and selection report nothing and the SDL adapter owns the clipboard. Disabled and fully clipped controls lose editing, focus and any active press. Re-enabling a control requires new input. Tab can reveal an offscreen control. The Vulkan recorder converts the logical clip to an inward-rounded, bounded framebuffer scissor. The showcase anchors to the central scene viewport, and a native modal uses the full window.

The editor holds up to 96 bytes on one line. It keeps committed UTF-8 and preedit separate, validates the SDL cursor and selection as UTF-8 scalar ranges, and uses the rendered caret's horizontal offset for SDL candidate positioning. Releasing editing ownership clears preedit and keeps the committed value. The baked face uses a fallback glyph for non-ASCII scalars. SDL 3.4.18 on Windows can report UTF-16 offsets after supplementary characters. Platform IME verification and SDL version handling are tracked in #302. [SDL editing event contract](https://wiki.libsdl.org/SDL3/SDL_TextEditingEvent), [SDL 3.4.18 Windows backend](https://github.com/libsdl-org/SDL/blob/release-3.4.18/src/video/windows/SDL_windowskeyboard.c#L900-L937)

## Reproduce

Build `ft_vox` and `test_engine_reload` with graphics enabled. Run from the game's output directory so the test finds SPIR-V.

```powershell
cmake --build build --config Release --target ft_vox test_engine_reload
$uiBuild = (Resolve-Path build).Path
$env:FT_VOX_VALIDATION = '1'
$env:FT_VOX_SYNC_VALIDATION = '1'
Push-Location "$uiBuild/Release"
try {
    & "$uiBuild/tests/Release/test_engine_reload.exe" --game-ui-showcase-proof
} finally {
    Pop-Location
}
```

On a single-config build run `./tests/test_engine_reload --game-ui-showcase-proof` from the build directory. The proof uses the SDL event queue and real swapchain submissions. It covers inventory and pause, focus restoration, UTF-8 commit and composition events, slider and list behavior, disabled controls, pointer cancellation during focus loss and a real resize, modal priority and movement quarantine. The WSI validation delta allows the two known RTSS diagnostics when its hook is loaded. [Renderer validation instructions](game-ui-renderer.md#reproduce-the-checks)

For a manual pass, type different values into Name and Secondary name and change each slider independently. Disable the secondary field and try to focus it and type again. Open a floating Developer UI window while the native Name field is active, click into an ImGui `InputText`, keep typing, then click back into Name: each surface must receive only the characters typed while it held the caret, and an unfinished composition must not survive the transfer. Start dragging a floating Developer UI window, Alt+Tab away while holding the button, then release it and return. Check that a fresh click or drag works. Repeat in the default launch and with `--game-ui`. While holding an ImGui press, try a second button over the native strip, and release the buttons in reverse order. Dismiss the confirmation while holding a movement key, then resume and check that movement waits for a fresh press.

Run `GameUiInteraction` and `GameUiDrawList` in a graphics-off build. The existing opt-in `--hosting-smoke` scenario also checks that remote snapshots keep arriving while the menu blocks local controls and that hosted shutdown restores the Host panel.

## Verification

On Windows 11 Release with SDL 3.4.18 and an RTX 4070 Ti, the showcase passed 25 checks using the sRGB swapchain at 1920×1080 and after a real resize to 1160×680. The existing presentation proof passed 20 checks, including a same-batch ImGui click and gameplay shortcut. The offscreen format proof passed 25 checks and the standalone proof passed 10. Standard and synchronization validation reported only the documented RTSS diagnostics in the presentation tests.

The native hosting smoke passed 17 checks and the engine reload lifecycle passed 33. The native join driver passed its 25 checks and motion-trace lifecycle scenarios across two loopback servers. All 52 tests in the Windows graphics-off configuration passed, including the nested CPU-only build. The graphical `InputRouting`, `UiShell`, `PostResizeFormats` and `VulkanValidationErrorReporting` tests also passed.

The showcase proof injects SDL composition and keyboard events. An OS-driven IME session, a physical cross-monitor DPI change and macOS/MoltenVK are outside this run. The graphics-free layout tests cover fractional framebuffer scaling.

### PR #312 correction checks

Same hardware, `FT_VOX_VALIDATION=1` and `FT_VOX_SYNC_VALIDATION=1`. The showcase proof passed 38 checks and the presentation proof 39. Presentation reported 56 diagnostics, all in the two known RTSS classes (`VUID-VkSwapchainCreateInfoKHR-imageFormat-01778` and `VUID-VkImageViewCreateInfo-usage-02275`).

The showcase proof now drives one real `ImGui::InputText` against the native `Name` field. It checks the native-to-developer press in the middle of an active native editing session, and that the accepted press, `End` key and committed text reach only the new `InputText` in the same SDL batch. It then checks the reverse press, where the queued `End` key, commit and composition event reach only the native `TextField` while the outgoing editor's earlier commit still lands. A third pass switches back before ImGui builds, so the handoff boundary must keep the newer developer events out of the outgoing native session and then deliver them to the editor the click activates. The final pass checks that retiring ImGui at `EndFrame` does not stop the SDL text session the new native field started.

The presentation proof builds a passive `NoInputs` overlay over a native control. It reports `Occludes`, receives the click with no native activation and takes no keyboard reservation; the same overlay marked pass-through reports `PassThrough` and the click reaches the native control. A legacy-mode pass clicks a real ImGui `InputText`, then clicks the game view and checks that the editor is released, the pre-click commit survives, later text is dropped and gameplay owns the following key press.

`GameUiInteraction` covers the `Change` contract without graphics: one notification per effective commit, deletion, cut and paste; no notification for an unchanged value, an empty deletion, caret movement, selection change, preedit or a disabled binding; and `Copy`/`Paste`/`Cut` naming the selection without touching a clipboard the widget layer cannot see.

### PR #312 review checks

On the same Windows hardware, the expanded showcase and presentation proofs each passed 31 checks with Vulkan standard and synchronization validation. Presentation counted 49 diagnostics, all from the known RTSS subset. The proofs cover ordered SDL focus-loss and focus-gain notifications during an ImGui drag in both launch modes, actual SDL OS capture flags, native modal suppression, out-of-order and quarantined button releases, separate widget bindings and the SDL text-input area for a scrolled caret. Presented 1920×1080 and 1160×680 captures were visually checked for independent values and disabled state. The focus transition in the proof is queued through SDL. A physical Alt+Tab pass and an OS-driven IME session remain unverified.
