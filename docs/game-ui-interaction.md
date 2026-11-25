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

Engine translates SDL events to key, pointer and text ownership. The routing policy handles explicit developer shortcuts, native modal ownership, active developer input and visible targets, then gameplay. A developer click that precedes ImGui's next frame reserves keyboard ownership during that event batch. The router remembers the destination of a press until release. Focus, capture and screen changes cancel the original gesture and clear pending jumps and held movement. Polling requires a fresh gameplay-owned key. Pointer coordinates maintain ImGui hover without granting it a native-owned button edge.

Row and column stacks, fixed grids and scroll viewports build logical rectangles. The controls use those same rectangles and clips for input and painting. The Vulkan recorder converts the logical clip to an inward-rounded, bounded framebuffer scissor. The showcase anchors to the central scene viewport, and a native modal uses the full window. Fully clipped controls are unavailable to pointer and activation events. Tab can reveal an offscreen control. Layout, screen state, router and text editing have graphics-free tests.

The showcase editor holds up to 96 bytes on one line. It accepts SDL committed UTF-8 and previews SDL IME composition, moves the caret at scalar boundaries, and supports selection, deletion and clipboard shortcuts. The current baked face covers ASCII and displays a fallback glyph for other scalars while preserving their bytes. Full coverage, shaping and advanced Unicode behavior belong to the production text service.

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

For a manual pass, open the showcase with Developer UI visible. Type into Name, then try selection and clipboard shortcuts with UTF-8 on a system with an IME enabled. Scroll the list and shrink the window. Check that clipped rows cannot be clicked, and that Tab scrolls the next control into view. Move a floating Developer UI window over the native control and click it before returning to the showcase. Dismiss the confirmation while holding a movement key, then resume. The player should wait for a fresh press.

Run `GameUiInteraction` and `GameUiDrawList` in a graphics-off build. The existing opt-in `--hosting-smoke` scenario also checks that remote snapshots keep arriving while the menu blocks local controls and that hosted shutdown restores the Host panel.

## Verification

On Windows 11 Release with SDL 3.4.18 and an RTX 4070 Ti, the showcase passed 25 checks using the sRGB swapchain at 1920×1080 and after a real resize to 1160×680. The existing presentation proof passed 20 checks, including a same-batch ImGui click and gameplay shortcut. The offscreen format proof passed 25 checks and the standalone proof passed 10. Standard and synchronization validation reported only the documented RTSS diagnostics in the presentation tests.

The native hosting smoke passed 17 checks and the engine reload lifecycle passed 33. The native join driver passed its 25 checks and motion-trace lifecycle scenarios across two loopback servers. All 52 tests in the Windows graphics-off configuration passed, including the nested CPU-only build. The graphical `InputRouting`, `UiShell`, `PostResizeFormats` and `VulkanValidationErrorReporting` tests also passed.

The showcase proof injects SDL composition and keyboard events. An OS-driven IME session, a physical cross-monitor DPI change and macOS/MoltenVK are outside this run. The graphics-free layout tests cover fractional framebuffer scaling.
