# Issue #99: RTSS interception and validation regression

Windows / RTX 4070 Ti / NVIDIA 616.56 / Khronos validation 1.3.290,
2026-09-04. RTSS executable version: 7.3.5.28314.

## Root-cause evidence

The earlier [PR #98 comparison](../issue98-validation/README.md) reproduces
8 `VUID-VkSwapchainCreateInfoKHR-imageFormat-01778` and
6 `VUID-VkImageViewCreateInfo-usage-02275` occurrences in each five-second
benchmark on clean base and PR, with GPU profiling enabled or disabled.

For #99, `VK_LOADER_DEBUG=layer` identified the active chain: AMD switchable
graphics, NVIDIA Optimus/present, OBS, RTSS and Khronos validation. Repeating
with `VK_LOADER_LAYERS_DISABLE=~implicit~` still produced the usage errors.
The [first error's callback stack](rtss-stack.txt) contains RTSSHooks64.dll
between test_vulkan_resources.exe and the validation DLL. The excerpt retains
module names/offsets only. It was collected while investigating swapchain
creation, not as a successful full resource test.

After the user exited RTSS, the **same previously built game executable**
(`b999bd881a02` plus the #98 review fix, build UTC 11:18:54) produced no VUIDs.
See [enabled control](clean-enabled.txt) and [disabled control](clean-disabled.txt).
These controls precede the new diagnostics build, so they establish that the
fix is removing the RTSS hook, not changing rendering code. The new build was
then checked separately below.

All game runs use seed 42, five seconds, warmup zero, 1920×1080, IMMEDIATE,
view distance 512 and the bundled resource pack. Validation stays enabled;
neither SRGB format nor image usage was changed. The final diagnostics print
`format=50 imageUsage=18` (B8G8R8A8_SRGB, COLOR_ATTACHMENT | TRANSFER_DST).

RTSS application's automatic profile writes were denied by Windows. No global
or application profile was changed. Stopping RTSS is sufficient for the tested
control; durable per-application exclusion must still be configured before
restarting it, as described in [the resolution guide](../../vulkan-validation.md).

## Final branch checks

| Check | Result |
| --- | --- |
| Windows/MSVC Release build | Passed |
| [Full CTest](ctest.txt) | 12/12, 41.93 s |
| [Final targeted Vulkan tests](ctest-vulkan.txt) | 2/2, 1.53 s |
| [Profiler enabled](final-enabled.txt) | 2,425 GPU samples, mean 0.549 ms, zero VUIDs |
| [Profiler disabled](final-disabled.txt) | GPU unavailable, zero VUIDs |
| git diff --check | Passed |

VulkanResourceSmoke now fails on any error-severity callback, including errors
during device shutdown. Its final success message follows validation checking.
VulkanValidationErrorReporting intentionally calls vkSubmitDebugUtilsMessageEXT
with a diagnostic ERROR and expects a failing process; it issues no invalid
Vulkan resource command. CTest inversion verifies the failure path instead of
letting resource-operation success hide validation errors. It skips when the
validation layer is unavailable.

The new runtime hint only appears with a matching usage VUID and an RTSS hook
loaded. Windows-only stack diagnostics are opt-in, once per process, and report
module names rather than full filesystem paths. Normal error reporting is
unchanged; no VUIDs are filtered out. Linux/macOS were not run locally.

Loader warnings about Galaxy layer naming can remain without RTSS. They are
not error-severity validation reports and do not invalidate the zero-VUID result.
