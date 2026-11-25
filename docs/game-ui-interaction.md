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

Without `--game-ui`, Escape still requests exit when the inventory and Developer UI have no claim. F1–F12 keep the current developer shortcuts and run once per press. The migration to the final F1 Developer UI toggle is tracked in #308. Session changes clear stale focus, gestures and fields before attaching the new world. The screen stack also represents Main Menu and Loading for the future lifecycle screens.

## Input and geometry

Engine translates SDL events to key, pointer and text ownership. The first live pointer press owns motion. Later buttons can join the same surface. A cross-surface press is quarantined through release and cannot mask a new gesture once the old one ends or is cancelled. Wheel input belongs to the surface under the pointer when there is no gesture. During a gesture, only its owner can receive the wheel. Passive ImGui hover does not grant a button edge. A developer click that precedes ImGui's next frame also reserves keyboard ownership during that event batch.

Focus loss and native modal suppression cancel developer ownership, pending ImGui edges, active widgets and SDL capture. Focus notifications survive cancellation. The vendored SDL backend captures on routed mouse edges and exposes a small capture-reset hook. Engine disables SDL auto-capture so a physical button that the router has quarantined cannot keep OS capture. On a failed relative-mode request, Engine reports SDL's error and reads back the actual mode. Gameplay input stays blocked until an explicit capture request or a screen transition succeeds.

Row and column stacks, fixed grids and scroll viewports build logical rectangles. Screens bind each text field, toggle, slider or list to independent state and receive typed actions tagged with their control IDs. The widget layer handles input and paints through the bounded DrawList. The showcase binds two groups to the same implementations. Disabled and fully clipped controls lose editing, focus and any active press. Re-enabling a control requires new input. Tab can reveal an offscreen control. The Vulkan recorder converts the logical clip to an inward-rounded, bounded framebuffer scissor. The showcase anchors to the central scene viewport, and a native modal uses the full window.

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

For a manual pass, type different values into Name and Secondary name and change each slider independently. Disable the secondary field and try to focus it and type again. Start dragging a floating Developer UI window, Alt+Tab away while holding the button, then release it and return. Check that a fresh click or drag works. Repeat in the default launch and with `--game-ui`. While holding an ImGui press, try a second button over the native strip, and release the buttons in reverse order. Dismiss the confirmation while holding a movement key, then resume and check that movement waits for a fresh press.

Run `GameUiInteraction` and `GameUiDrawList` in a graphics-off build. The existing opt-in `--hosting-smoke` scenario also checks that remote snapshots keep arriving while the menu blocks local controls and that hosted shutdown restores the Host panel.

## Verification

On Windows 11 Release with SDL 3.4.18 and an RTX 4070 Ti, the showcase passed 25 checks using the sRGB swapchain at 1920×1080 and after a real resize to 1160×680. The existing presentation proof passed 20 checks, including a same-batch ImGui click and gameplay shortcut. The offscreen format proof passed 25 checks and the standalone proof passed 10. Standard and synchronization validation reported only the documented RTSS diagnostics in the presentation tests.

The native hosting smoke passed 17 checks and the engine reload lifecycle passed 33. The native join driver passed its 25 checks and motion-trace lifecycle scenarios across two loopback servers. All 52 tests in the Windows graphics-off configuration passed, including the nested CPU-only build. The graphical `InputRouting`, `UiShell`, `PostResizeFormats` and `VulkanValidationErrorReporting` tests also passed.

The showcase proof injects SDL composition and keyboard events. An OS-driven IME session, a physical cross-monitor DPI change and macOS/MoltenVK are outside this run. The graphics-free layout tests cover fractional framebuffer scaling.

### PR #312 review checks

On the same Windows hardware, the expanded showcase and presentation proofs each passed 31 checks with Vulkan standard and synchronization validation. Presentation counted 49 diagnostics, all from the known RTSS subset. The proofs cover ordered SDL focus-loss and focus-gain notifications during an ImGui drag in both launch modes, actual SDL OS capture flags, native modal suppression, out-of-order and quarantined button releases, separate widget bindings and the SDL text-input area for a scrolled caret. Presented 1920×1080 and 1160×680 captures were visually checked for independent values and disabled state. The focus transition in the proof is queued through SDL. A physical Alt+Tab pass and an OS-driven IME session remain unverified.
