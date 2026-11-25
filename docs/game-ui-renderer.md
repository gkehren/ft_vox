# Game UI renderer

Phase B (#300) integrates the native renderer selected in the [Game UI ADR](adr/game-ui-backend.md).

## Launch

Run `ft_vox --seed 42 --game-ui --quality low` from the graphical build output. The translucent Game UI strip anchors to the main scene area. Press C to release the cursor and click the scale button to cycle 100%, 125%, 150% and 175%. The icon is a clipped grass-block side from the current resource pack. The existing Graphics panel can load a new pack and the next Game UI frame resolves the replacement atlas. The Developer UI and legacy gameplay controls remain available. Normal launch uses the existing interface. `--ui-spike` opens the standalone preview over a transient scene.

## Rendering and lifetime

- `gameui::DrawList` submits panels, glyph masks and generation-tagged atlas icons in logical window coordinates. Intersected clips become framebuffer scissors. Consecutive batches share a scissor and descriptors, preserving paint order. Submission is bounded to 2,048 quads and 31 batches, reserving the 32nd draw for UNORM encoding. The strip fits in three draws.
- sRGB output blends premultiplied linear RGB directly into the swapchain after postprocessing. UNORM output writes postprocessed SDR into one RGBA16F target per active frame slot, blends there, then encodes to the swapchain with one fullscreen draw. FXAA retains its existing perceptual input. Dev UI loads the final swapchain image after either path. `VkFrameContext` remains the owner of acquire, submit, and present.
- The renderer owns one immutable R8 font atlas, two mapped instance buffers, and four descriptor sets. Scale adjusts logical geometry. Frame-local allocations and descriptor updates occur only after the slot fence. UNORM targets are replaced and retired when that slot is safe. An sRGB swapchain uses no intermediate Game UI target. Format changes and teardown use the host's existing barriers.
- The atlas view and sampler are borrowed from `TextureManager`. Resource-pack replacement already waits for the device in the legacy renderer. Once Engine publishes the new generation, Game UI discards stale handles and rebinds each slot before its next use. It shares the same array image and creates no per-layer views.
- The profiler separates `GameUIBuild`, `GameUIDraw`, `DevUIBuild`, and `DevUIDraw` on CPU and Game UI, Game UI encode, and Dev UI on GPU. The Performance panel exposes draws, vertices, dropped quads, descriptor sets, and memory from the last recorded frame and displays its frame number. Game UI draw cost includes the final encode when active.
- The sample checks SDL event coordinates against the most recently drawn ImGui windows and their dockspace passthrough. Gameplay capture, ImGui dialogs and pointer occlusion cancel an armed press. Game UI still consumes that press's release, so the release cannot reach another interface.

The graphical target reuses the repo-pinned SDL3, Vulkan/VMA and embedded ProggyClean face. The coverage data is baked into a dedicated Game UI atlas. Font and icon attribution are in [third-party notices](../THIRD_PARTY_NOTICES.md). The MoltenVK path uses the same Vulkan 1.2 dynamic-rendering extension, sampled images and graphics pipelines. The UNORM path checks RGBA16F color attachment, blend and sampling features.

## Reproduce the checks

Build `ft_vox` and `test_engine_reload` with `FT_VOX_GRAPHICS=ON`. Run the test executable from the game's output directory so it finds its compiled SPIR-V files. The three modes below run explicitly on hardware. Existing scene regression commands continue to render without UI.

| Test executable arguments | Output |
| --- | --- |
| `--game-ui-screenshot <directory>` | Four offscreen PNGs covering sRGB and UNORM with FXAA on and off. Asserts linear half-white, overlapping translucent quads, glyph coverage, clipped edges, scene preservation, stale handles and Developer UI pass ordering. |
| `--game-ui-present-proof <directory>` | Real swapchain submits at 1920×1080, 1160×680, after SDL minimize/restore, and 1680×720 (21:9), followed by repeated scale, pack reload, and resize. Checks atlas publication, the resource footprint, WSI validation, and pointer routing against an ImGui floating window, capture transitions and a modal. |
| `--game-ui-performance` | Two pairs in baseline → UI and UI → baseline order, each with 600 measured frames per mode after 120 warmup frames. Uses a fixed seed-42 projection, Low quality, 1920×1080, immediate present and Developer UI closed. |

The WSI fixture restores the actual SDL window before submitting the next test frame. It exercises restoration and the first post-restore frames. The minimized branch of the continuous `Engine::run()` loop is outside this fixture. With validation enabled, the test fails on any new error except the two established swapchain STORAGE diagnostics attributed to a loaded RTSS hook. It prints the total error count and the RTSS subset. With validation disabled the capture path reports that validation was unavailable for that run.

The benchmark accepts fresh GPU serials tagged to the current measured range with the expected frame, Game UI and encode pass markers. It drains the last submitted slots through the ordinary fence path. A run fails when GPU profiling is unavailable or a mode has fewer than 570 of 600 valid GPU frames. UI timings are reported as unavailable in baseline mode.

To capture the on-screen proof frames, run the present proof with the Vulkan SDK screenshot layer enabled and `VK_SCREENSHOT_FRAMES=15,31,47,63,79`. Set `VK_SCREENSHOT_DIR` to an existing output directory. Run validation separately with `FT_VOX_VALIDATION=1` and `FT_VOX_SYNC_VALIDATION=1`. Enable `FT_VOX_GPU_PROFILING=1` when measuring. [LunarG screenshot layer](https://vulkan.lunarg.com/doc/view/latest/windows/screenshot_layer.html)

## Windows hardware evidence

Windows 11, MSVC Release and vcpkg, RTX 4070 Ti, driver 32.0.16.2124, Vulkan 1.4.357, SDL 3.4.18. The surface selected `B8G8R8A8_SRGB`; the UNORM route was forced in offscreen tests.

| Presented frame | Capture |
| --- | --- |
| 1920×1080 (16:9), Game UI and Developer UI visible | [Normal frame](images/game-ui-renderer/normal-16x9.webp) |
| 1160×680 after SDL resize | [Resized frame](images/game-ui-renderer/resized.webp) |
| 1160×680 after actual minimize and restore | [Restored frame](images/game-ui-renderer/restored.webp) |
| 1680×720 (21:9) | [Ultrawide frame](images/game-ui-renderer/ultrawide-21x9.webp) |
| 1680×720, 175% scale, after loading a partial pack with a magenta replacement icon | [Reloaded atlas and scale](images/game-ui-renderer/reloaded-pack.webp) |

The offscreen proof passed 25 assertions across both formats and FXAA settings, including the half-white reference, glyph coverage, overlap order, untouched scene pixels, Dev UI ordering, and the return to the original UNORM allocation footprint after resize. The WSI proof passed 19 assertions, including four rounds of resize and pack replacement, scale changes, bounded resources, clicks on an overlapping ImGui window queued before `NewFrame()`, and release ownership across gameplay capture and modal transitions.

With standard and synchronization validation enabled, the WSI test counted 49 diagnostics, all matching the two documented RTSS-injected sRGB/STORAGE swapchain and image-view VUIDs. It recorded no present/acquire hazards. The offscreen proof checks that validation remains clean after startup. [Windows Vulkan validation guide](vulkan-validation.md)

### Release timing

Each mode in both measurement orders collected 600 unique GPU frames out of 600 measured frames after 120 warmup frames. Both modes used the same fixed seed-42 projection, immediate present and Low preset with Dev UI closed. Running with `FT_VOX_GPU_PROFILING=0` failed as expected. All times are milliseconds.

| Measurement order | Frame median, off / on | Frame p95, off / on | GPU frame median, off / on | GPU frame p95, off / on | UI CPU p95 | UI GPU p95 |
| --- | --- | --- | --- | --- | --- | --- |
| Baseline first | 0.560 / 0.560 | 1.509 / 1.500 | 0.605 / 0.606 | 0.855 / 0.847 | 0.0062 | 0.0041 |
| Game UI first | 0.508 / 0.557 | 1.308 / 1.480 | 0.403 / 0.607 | 0.608 / 0.831 | 0.0062 | 0.0041 |

Whole-frame timing varied between measurement orders, while the attributed Game UI cost was stable. The enabled frames used three batches and 204 submitted vertices, four descriptor sets, 208 KiB of requested instance storage across two slots and a 16 KiB VMA font allocation. The sRGB path allocated zero bytes for composition. The extra UNORM encode was verified offscreen and is measured by its own GPU scope when active.

### Build coverage

- Windows Release built the graphical game and affected test targets. All four UI shaders passed `spirv-val --target-env vulkan1.2`.
- `UiShell`, `VisualRegression` (scene-only smoke), `PostResizeFormats`, `--ui-spike-smoke` (10 checks), and the engine reload lifecycle (33 checks) passed.
- The initial Windows graphics-off Release suite passed all 50 tests. The follow-up reran `CoreIncludeGuard`, `HeadlessDeps`, `HeadlessSmoke` and `CpuOnlyBuild` alongside the new `GameUiDrawList` test for clip intersections, paint order, capacity and dropped primitives. The graphical `VulkanValidationErrorReporting` test passed as well.
- Debian WSL compiled and linked the graphical C++ target with its cached shader compiler stub. The actual SPIR-V build and hardware runs used Windows. Native Linux graphics, macOS/MoltenVK runtime, UNORM WSI presentation and cross-monitor DPI changes were unexecuted.
