# Benchmarks

This directory holds **methodology syntheses only** — one `README.md` (or
`issue-*.md`) per effort, containing hardware, commit, settings, mean/p95,
before/after numbers and the conclusion.

## Retention policy

- **Commit:** synthesis documents (`.md`) like the ones linked below.
- **Do not commit:** raw console dumps (`bench_*.txt`, `ctest.txt`,
  `benchmark.txt`, profiler `.csv` traces, validation logs). They are
  development artifacts: keep them local (gitignored `benchmark-results/`) or
  attach them to the GitHub issue/PR as evidence.

The in-game benchmark (`ft_vox --benchmark <seconds>`, or *Save summary* in
the Graphics panel) writes `bench_<timestamp>_<rev>_s<seed>_sc<score>.txt`
into `benchmark-results/` (gitignored), not here.

## Index

- [issue-80-profiler-reset.md](issue-80-profiler-reset.md)
- [issue-88-biome-map-scheduling.md](issue-88-biome-map-scheduling.md)
- [issue-93-pooled-reset.md](issue-93-pooled-reset.md)
- [issue101/](issue101/README.md) — telemetry vs reporting
- [issue108/](issue108/README.md) — MeshArena allocator
- [issue110/](issue110/README.md) — StagingRing
- [issue112-ab/](issue112-ab/README.md) — VoxelPool A/B
- [issue122/](issue122/README.md) · [issue122b/](issue122b/README.md) — water meshing
- [issue139-water/](issue139-water/README.md) — water optics
- [issue144-underwater/](issue144-underwater/README.md) — underwater post
- [issue98-validation/](issue98-validation/README.md) · [issue99-validation/](issue99-validation/README.md) — validation overhead
- [mobs/](mobs/README.md) — passive entities CPU/GPU profile
- [player-physics/](player-physics/README.md) — physics cost
- [pr145/](pr145/validation/README.md) — VSync pacing
- [pr157-water/](pr157-water/README.md) — water rendering
- [worldgen-rework/](worldgen-rework/README.md) — terrain generator rework

Historical raw reports removed by the retention policy remain reachable in
Git history (files deleted under `docs/benchmarks/**`). Syntheses that cited
raw runs link them via permalinks to the pre-retention revision
(`d7a2c9345cb8406efc6022d4d3ff462c4995b231`); the files themselves no longer exist in the working tree.
