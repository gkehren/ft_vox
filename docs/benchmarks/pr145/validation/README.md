# Vulkan validation runs (vsync on / off)

`FT_VOX_VALIDATION=1`, 10 s benchmark, seed 42. Labels show `26e8626*`
(dirty-tree build marker): the binary contains the final review-fix code
(HEAD 6ba3531, the only uncommitted delta at build time was this report
folder itself).

Both runs exit 0 with only the two known pre-existing VUIDs
(VkImageViewCreateInfo-usage-02275 x12, VkSwapchainCreateInfoKHR-imageFormat-01778 x8)
- no new validation errors from the acquire reordering.

The raw reports below were removed from HEAD by the benchmark retention
policy; the links point at the pre-retention revision.

- [bench_20260907_111217_26e86262e45d__s42_sc9978.txt](https://github.com/gkehren/ft_vox/blob/d7a2c9345cb8406efc6022d4d3ff462c4995b231/docs/benchmarks/pr145/validation/bench_20260907_111217_26e86262e45d__s42_sc9978.txt): `--vsync on` (FIFO)
- [bench_20260907_111230_26e86262e45d__s42_sc9951.txt](https://github.com/gkehren/ft_vox/blob/d7a2c9345cb8406efc6022d4d3ff462c4995b231/docs/benchmarks/pr145/validation/bench_20260907_111230_26e86262e45d__s42_sc9951.txt): `--vsync off` (IMMEDIATE, applied after the
  initial swapchain recreation - see the second present-mode line)
