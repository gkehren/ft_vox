# Vulkan validation and injected overlays

## SRGB/STORAGE errors from RTSS (issue #99)

On the investigated Windows machine, RivaTuner Statistics Server (RTSS)
7.3.5.28314 hooks Vulkan dispatch and adds STORAGE/TRANSFER_SRC usage to the
swapchain creation request. The engine requests only COLOR_ATTACHMENT |
TRANSFER_DST (decimal 18), with `VK_FORMAT_B8G8R8A8_SRGB`. That SRGB format does
not support storage images on the tested GPU, so validation reports:

- `VUID-VkSwapchainCreateInfoKHR-imageFormat-01778`
- `VUID-VkImageViewCreateInfo-usage-02275`

This is not a GPU timestamp issue. The errors reproduce on the pre-profiler
base, with profiling enabled or disabled. A validation callback stack contains
`test_vulkan_resources.exe → RTSSHooks64.dll → VkLayer_khronos_validation.dll`.
Exiting RTSS removes both VUIDs with the same game binary, format, validation
layer, resource pack and benchmark command. No format fallback or VUID filter
is needed.

`VK_LOADER_LAYERS_DISABLE=~implicit~` does **not** suffice: RTSSHooks64.dll can
remain injected independently of `VK_LAYER_RTSS`. Hiding the OSD also does not
necessarily disable injection. Other installed overlays were not identified
as the cause in this experiment.

## Durable local resolution

In RTSS, add application profiles for **ft_vox.exe** and
**test_vulkan_resources.exe**, set **Application detection level = None** for
each, and restart those executables. This scopes the exclusion to this project;
other applications can retain their overlays and limiter. RTSS's SDK names the
corresponding property `AppDetectionLevel` (0 means None).

Alternatively, exit RTSS while validating. If Windows denies saving its
application profiles in Program Files, configure them from an RTSS instance
with the necessary permissions. The engine does not modify global or per-app
RTSS settings, stop RTSS, or unload injected modules automatically.

On the development machine, automatic profile writes were denied, and the user
exited RTSS for the control runs. **The permanent application exclusions still
need to be saved before RTSS is restarted.** No RTSS configuration was changed.

## Reproduce and diagnose

Run from the repository root, with RTSS excluded or exited:

```powershell
$env:FT_VOX_VALIDATION = '1'
$env:FT_VOX_GPU_PROFILING = '1'
.\build\Release\ft_vox.exe --seed 42 --benchmark 5 --benchmark-warmup 0 --vsync off
$env:FT_VOX_GPU_PROFILING = '0'
.\build\Release\ft_vox.exe --seed 42 --benchmark 5 --benchmark-warmup 0 --vsync off
ctest --test-dir build -C Release --output-on-failure
```

For investigation on Windows, `FT_VOX_TRACE_VALIDATION=1` additionally prints
the actual swapchain request before dispatch and the module names/offsets of
the first validation error's callback stack. It is diagnostic only, not a
symbolized backtrace, and has no per-frame tracing cost when disabled. The
normal error path emits a once-per-process RTSS remediation hint only when one
of the two usage VUIDs occurs and an RTSS hook module is loaded. A loaded module
alone is not taken as proof of an error.

Validation messages remain visible. `VkContext::validationErrorCount()` counts
error-severity callbacks, including initialization and shutdown; a new init
resets the counter. VulkanResourceSmoke fails if any such error occurred, even
when resource operations succeeded. Loader naming warnings do not count as
validation errors.

VulkanValidationErrorReporting deliberately submits a debug-utils ERROR message
without invalid GPU work. CTest expects the executable to fail; this verifies
that error reporting cannot silently pass. It skips when validation is
unavailable. Do not mistake this clearly labelled synthetic diagnostic for a
renderer VUID.

See [the validation evidence](benchmarks/issue99-validation/README.md) for
before/after results. Khronos documents the requested swapchain usage in
[`VkSwapchainCreateInfoKHR`](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainCreateInfoKHR.html).
