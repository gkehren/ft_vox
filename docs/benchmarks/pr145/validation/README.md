# Vulkan validation runs (vsync on / off)

`FT_VOX_VALIDATION=1`, 10 s benchmark, seed 42. Labels show `26e8626*`
(dirty-tree build marker): the binary contains the final review-fix code
(HEAD 6ba3531, the only uncommitted delta at build time was this report
folder itself).

Both runs exit 0 with only the two known pre-existing VUIDs
(VkImageViewCreateInfo-usage-02275 x12, VkSwapchainCreateInfoKHR-imageFormat-01778 x8)
- no new validation errors from the acquire reordering.

- `bench_20260907_111217_*`: `--vsync on` (FIFO)
- `bench_20260907_111230_*`: `--vsync off` (IMMEDIATE, applied after the
  initial swapchain recreation - see the second present-mode line)
