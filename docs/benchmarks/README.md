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
- [mobs/](mobs/README.md) — passive entities CPU/GPU profile · [issue-130 A/B](mobs/issue-130-instancing.md) — instanced batch submission
- [player-physics/](player-physics/README.md) — physics cost
- [issue-205-pre-refactor.md](issue-205-pre-refactor.md) — Epic #204 Phase A pre-refactor baselines
- [issue-206-phase-b.md](issue-206-phase-b.md) — Epic #204 Phase B authoritative-world verification (A/B + acceptance)
- [issue-207-phase-c.md](issue-207-phase-c.md) — Epic #204 Phase C render-bridge verification (acceptance + review fixes)
- [issue-208-phase-d.md](issue-208-phase-d.md) — Epic #204 Phase D GameSimulation verification (acceptance + host rewiring)
- [issue-209-phase-e.md](issue-209-phase-e.md) — Epic #204 Phase E graphical-host verification (epoch barrier + parity)
- [issue-210-phase-f.md](issue-210-phase-f.md) — Epic #204 Phase F headless runner verification (CPU-only + determinism)
- [issue-211-phase-g.md](issue-211-phase-g.md) — Epic #204 Phase G closure gate (enforcement + parity protocol + cleanup)
- [issue-211-parity.md](issue-211-parity.md) — Phase G interleaved in-game benchmark parity (5 trials, per-trial appendix)
- [pr145/](pr145/validation/README.md) — VSync pacing
- [pr157-water/](pr157-water/README.md) — water rendering
- [worldgen-rework/](worldgen-rework/README.md) — terrain generator rework

Historical raw reports removed by the retention policy remain reachable in
Git history (files deleted under `docs/benchmarks/**`). Syntheses that cited
raw runs link them via permalinks to the pre-retention revision
(`d7a2c9345cb8406efc6022d4d3ff462c4995b231`); the files themselves no longer exist in the working tree.
