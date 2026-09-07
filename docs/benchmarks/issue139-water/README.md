# Issue #139: scene-aware water

Release / MSVC, NVIDIA GeForce RTX 4070 Ti, Vulkan 1.4.351, 2026-09-07.

## Reproduction

Run from the repository root after `cmake --build build --config Release`:

```powershell
$env:FT_VOX_VALIDATION = '1'
./build/tests/Release/ft_vox_visual_tests.exe --water-audit --out build/issue-139-qa/1080
./build/tests/Release/ft_vox_visual_tests.exe --water-audit --audit-1440 --out build/issue-139-qa/1440
```

The edited lake fixture contains a cliff, tree, bridge, shallow beach and
water-filled kelp/seagrass. Nine camera/time variants each save twelve slow-motion
samples. Individual captures and an SSR difference image remain in the local
artifact directories above; no golden references were changed.

## GPU measurements

The adjacent CSVs contain means of 20 GPU timestamp samples per tier, following
eight warmup frames. Tier 0/1/2/3 means Low/Medium/High/Cinematic. The frame column
covers Shadow through Post; synchronous CPU readback, PNG encoding, presentation,
streaming and ImGui are excluded. Water includes the existing history copies.
These fixed-fixture results are not a general gameplay frame-rate claim. Full
presets are applied, including their actual shadow-map sizes. Final measurements
are made with the RTSS overlay closed; the explicit Khronos validation layer
remains enabled and both runs exit with code 0. The harness now destroys its
VkContext before `SDL_Vulkan_UnloadLibrary`, fixing an exit-time access
violation that RTSS's loader retention had been masking on this machine.

SSR has 0/0/24/48 march steps, with five bisections per candidate crossing.
Thickness is 0.35 view-space units; ranges are 40/64 units on High/Cinematic.
No additional GPU images are allocated (0 image bytes); FrameUBO grows by 16
bytes per frame slot. No hit-rate counter or normal-noise optimization was added:
Medium's measured water cost is already small and the original wave function is
preserved.

## Validation

- Release build and GLSL-to-SPIR-V compilation pass.
- All 29 CTest cases pass (25 non-Vulkan plus four Vulkan/visual cases).
- Existing VisualRegression smoke comparisons pass without reference updates.
- The explicit audit checks SSR changes scene pixels with other post knobs held
  fixed, repeat-frame determinism, water shadow contribution, finite HDR output,
  valid captures and renderer validation errors. Frozen-time strafe pairs bound
  SSR motion against the no-SSR parallax baseline (default wave and 0.25), and a
  wave-strength 0.45 probe keeps SSR and shadow reception alive where gating on
  the wave-animated shading normal would collapse them (regression for
  geometric-normal gating).
- The final audit runs pass those checks at both resolutions with zero renderer
  validation errors, in the default environment (RTSS closed, implicit layers
  untouched). Earlier runs with the RTSS overlay active injected seven known
  swapchain-init errors (`01778` / `02275`), tolerated only by the existing
  device-init baseline policy; no other renderer errors appeared. Clean
  validation with overlays enabled is not established.

The scene-aware path can only reflect visible opaque geometry; hidden/back-facing
geometry and clouds are not reconstructed. Edge, distance, horizon and thickness
confidence fades return to the shared analytic sky. The original procedural wave
normal function and vertex animation are preserved. No water retessellation or
transparent ordering changes are introduced.
