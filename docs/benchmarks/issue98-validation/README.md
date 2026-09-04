# PR #98 review validation — 2026-09-04

The capability gate now depends on the **selected graphics queue**:
`timestampValidBits` in 1–64 and finite positive `timestampPeriod`.
The device-wide `timestampComputeAndGraphics` guarantee is not required.
The query lifecycle, timestamp stages and nonblocking read flags are unchanged.

## Controlled base comparison

Base: `098ceae278607d3330d1d9e9a797ec94d9a170a9`, clean checkout.
PR: `b999bd881a02aff3d462fb93bc5415e258741908` plus the capability/diagnostics
and regression-test corrections documented here (dirty build identity in logs).
The existing build directory was rebuilt from each source version; the base
checkout has no GPU timestamp code. All runs used the same compiler, packages,
driver, validation configuration, resource pack, viewport and present mode.

```powershell
$env:FT_VOX_VALIDATION = '1'
$env:FT_VOX_GPU_PROFILING = '1' # ignored by base
.\build\Release\ft_vox.exe --seed 42 --benchmark 5 --benchmark-warmup 0 --vsync off
# Repeat PR run with FT_VOX_GPU_PROFILING=0.
```

| Full output | imageFormat-01778 | usage-02275 | Other VUIDs |
| --- | ---: | ---: | ---: |
| [Base](base-validation.txt) | 8 | 6 | 0 |
| [PR enabled](pr-validation.txt) | 8 | 6 | 0 |
| [PR disabled](pr-disabled.txt) | 8 | 6 | 0 |

The IDs are `VUID-VkSwapchainCreateInfoKHR-imageFormat-01778` and
`VUID-VkImageViewCreateInfo-usage-02275`. The same errors occur with profiling
code absent, so they are preexisting and tracked in
[#99](https://github.com/gkehren/ft_vox/issues/99). There are no timestamp/query
VUIDs. This does **not** claim the entire renderer has clean validation.
The engine asks for COLOR_ATTACHMENT | TRANSFER_DST; validation sees extra
STORAGE/TRANSFER_SRC usage. Its origin remains unknown. Matching counts and
messages establish reproduction, not the underlying cause.

## Environment

- Windows; NVIDIA GeForce RTX 4070 Ti, driver 616.56, device API 1.4.351.
- MSVC 19.50.35724.0; Visual Studio 18 2026; Release; same CMake build directory.
- Vulkan SDK shader tools 1.3.290.0 and Khronos validation layer API 1.3.290;
  compile-time Vulkan headers 1.4.350 from vcpkg. `vulkaninfoSDK` reports loader
  instance version 1.4.341 (the SDK tool's process).
- 1920×1080, view distance 512, seed 42, warmup 0, 5 seconds,
  VSync off / IMMEDIATE. Bundled `ressources/default-resource-pack.zip`, SHA256
  `00847414CB6AE5EF97FAE236EDCB9C01449A1865B3D38F907F902278A58477EC`.
- No implicit-layer disable filter for this comparison. Installed implicit
  layers include AMD switchable graphics, NVIDIA Optimus/present and Nsight
  2025.2, OBS, GOG Galaxy, EOS, Steam overlay/fossilize and RTSS. Installation
  does not establish which hooks were active; the active chain is unconfirmed.
  Earlier profiling runs with implicit layers disabled still showed the errors.

## Tests and runtime checks

- Full Release build passed. CTest **11/11**, 41.16 s; [output](ctest.txt).
- `GpuProfile`: support for 36/40/48/56/64 bits, invalid widths and nonfinite,
  zero or negative periods; existing 8/64-bit wrap tests retained. The global
  guarantee is no longer an input to the tested capability helper.
- `VulkanResourceSmoke`: zero-slot graceful failure, two query pools, repeated
  reuse after fence completion, partial availability, clear/reset, disable and
  resume. A completed but unread sample is explicitly left pending across
  OFF → ON, rejected afterwards, and replaced by exactly one fresh sample.
- Smoke diagnostics: graphics family 0, 64 valid bits, period 1 ns,
  `timestampComputeAndGraphics=true`, selected queue supported. A physical
  device exposing false for the global guarantee was not tested.
- Enabled benchmark: **980 GPU samples, mean 0.923 ms**, Shadow 0.049 ms,
  Opaque 0.096 ms, Post 0.278 ms, ImGui 0.010 ms. Disabled benchmark completes
  and reports `GPU timestamp queries: unavailable` with no GPU measurements.
- [Ten-second run with two-second warmup](pr-warmup.txt) also completes with
  GPU timings. Deterministic tests, rather than aggregate timing counts, prove
  exclusion of warmup samples arriving before/after measurement starts and
  old tags across successive benchmarks in one Benchmark instance.
- CPU clear semantics remain covered by ProfilerWorkerConsistency; GPU clear
  and toggles are exercised through the same profiler methods the UI calls.
  Manual F7 button interaction and two full Engine benchmark runs without
  restarting the process were not performed.
- `git diff --check` passed. No query read/wait/reset ordering changes.

Resolution/shadow/overdraw sweeps, RenderDoc/Nsight comparison, Linux and macOS
builds remain unperformed optional checks. These short runs establish
functionality and classify the VUIDs; they are not an overhead benchmark.
