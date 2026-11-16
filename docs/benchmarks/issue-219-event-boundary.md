# Issue #219: event-boundary CPU streaming-scope cost (Epic #204 follow-up)

Boundary-preserving optimization of the synthetic `bench_persistence` CPU
streaming/edit loop cost discovered by the Phase G closure gate
([#211](issue-211-phase-g.md), §3 "Synthetic streaming/edit loop"). Related:
[#204](https://github.com/gkehren/ft_vox/issues/204) (epic),
[#206](issue-206-phase-b.md) / [#207](issue-207-phase-c.md) (boundary introduction).

The closure gate measured a consistent **~+42%** CPU streaming-scope cost in the
refactored tree (pre-epic `4f931d6` ~3.85 ms → post-refactor ~5.45 ms frame avg,
200 edits/s, view 96, pure CPU). It was explained (the required CPU→render
boundary), bounded (in-game frame time GPU-bound at parity), and tracked here for
reduction. This record documents the investigation and the fix.

## Environment

- **Machine:** AMD Ryzen 7 9800X3D (8c/16t), Windows 11 Pro, NVIDIA RTX 4070 Ti —
  the same reference system as the Phase A–G baselines.
- **Build:** MSVC **Release**, vcpkg toolchain
  `D:/Projects/vcpkg/scripts/buildsystems/vcpkg.cmake`; GLM/FastNoise2 FetchContent.
- **Pre-epic baseline:** `4f931d6`, built in the `ft_vox_baseline_204` worktree with
  the **same** flags/toolchain.
- **Harness:** `tests/bench_persistence.cpp` — **unchanged** (the streaming block is
  byte-identical to the pre-epic harness, so the measurement scope is equivalent).

## 1. Investigation — profiling refined the hypothesis

The issue hypothesized the cost lived in the event-stream boundary (deque→vector
copy + per-event render-lock dispatch) and the per-mesh `ensureShellPopulated`
world lock. Temporary per-call instrumentation of the streaming block (reverted
before commit) showed otherwise:

| Streaming call | avg ms/frame (200 edits/s) | share |
|---|---:|---:|
| `processFinishedJobs` (gen publish + 2 drains + mesh/light publish + edit apply) | 0.17 | ~3% |
| `processDeferredReleases` | 0.001 | ~0% |
| `updateStreaming` (+ drain) | 0.008 | ~0% |
| `processChunkLoading` (+ drain) | 0.003 | ~0% |
| `generatePendingVoxels` (+ drain) | 0.016 | ~0% |
| **`meshPendingChunks`** | **5.2** | **~97%** |
| edit block (`deleteVoxel` + drain) | 0.05 | ~1% |

Drilling into `meshPendingChunks` (counters on both trees, same 12 s run):

| Metric | pre-epic `4f931d6` | post-refactor (pre-fix) |
|---|---:|---:|
| full-quality dispatches | 2340 | 2322 |
| LOD dispatches | 0 | 0 |
| `ensureShellPopulated` lock acquisitions | 0 | 0 |
| **halo fill total ms** | **1278.7** | **1999.8** |
| **halo fill ms/dispatch** | **0.547** | **0.861** |
| enqueue total ms | 16.1 | 14.7 |

**Conclusion:** the dispatch *count* is identical (the mesh budget saturates at
6/frame in both trees), and the shell lock is **never** taken at dispatch (the
`isShellEmpty()` early-out already avoids it — the shell is populated during
generation/bootstrap). The gap is entirely the **per-dispatch halo fill**:
`fillLightHaloFromNeighbors` snapshots the ~476K-cell neighbor ring on the main
thread, and after the CPU/render split each ring cell read
`Chunk::getVoxel() → WorldChunk::getVoxel()` — two null checks, an extra handle
dereference, and a fresh `y*256 + z*16 + x` index per cell. That is the
"render-handle indirection" the issue named, localized to the halo fill. At
6 dispatches/frame the +0.31 ms/dispatch indirection tax is the ~1.9 ms/frame gap.

## 2. The change (boundary-preserving)

1. **Hoist the halo-fill row access** (`src/Chunk/ChunkLighting.cpp`): resolve each
   neighbor's voxel backing **once per region** through the existing render-free
   accessors (`Chunk::worldChunk()` → `WorldChunk::voxelStorage()`), then walk
   contiguous source/halo bytes with one row base per `(y, neighbor row)`. The
   snapshot is **byte-identical**: a readable neighbor contributes its stored
   voxels, a missing/unreadable one (or one with no allocated backing) contributes
   AIR, exactly as the per-cell accessor did; the emissive list and its order are
   unchanged. This removes the per-cell handle indirection *and* the per-cell index
   recompute — it beats even the pre-epic per-cell `getVoxel`.
2. **Reusable event-drain buffer** (issue bullet 1): added
   `VoxelWorld::takeEvents(std::vector<WorldEvent>&)` (append-into-caller-buffer,
   same events/order, render-free) and a persistent `m_eventDrainBuffer` on the
   bridge. The several drains per frame now reuse one buffer's capacity instead of
   allocating a fresh vector each time. The return-by-value `takeEvents()` delegates
   to the overload (headless/tests unchanged).
3. **Audit conclusions** (issue bullets 2–3, no code change needed):
   - *Redundant drains:* the per-call drain points are ordering-critical (issue
     #114 publication ordering; the render map must never observe a recycled
     record) and measured cheap (~3% of the scope). They are kept; the buffer reuse
     removes their allocation cost. No safe consolidation exists in the hot path.
   - *Shell-population locks:* measured **0** acquisitions at mesh dispatch — the
     existing `isShellEmpty()` early-out already avoids the world lock when the shell
     is resident. Nothing to batch.

No simulation, terrain, save-format, renderer/shader, publication-ordering or
gameplay behavior changed. The CPU/render boundary is intact (the halo fill stays
render-side; the new `takeEvents` overload is render-free).

## 3. Results

### Warm quick-check (12 s, unmodified harness, same machine)

| Run | pre-fix frame avg | post-fix frame avg | pre-epic frame avg |
|---|---:|---:|---:|
| A (persistence off, 200 edits/s) | 5.31 ms | 2.31 ms | 3.56 ms |
| B (persistence on, 200 edits/s) | 5.77 ms | 2.48 ms | 3.69 ms |

Streaming scope tracks frame avg (the loop is streaming-dominated). The fix reduces
the streaming-scope cost **~57%** and lands **below** the pre-epic baseline.

### Cool-machine counterbalanced protocol (acceptance)

Interleaved pre-epic / pre-fix / post-fix trials, **2 pairs**, 75 s cool-down
between every run (mirrors the #211 parity protocol; counterbalanced ordering
cancels slow machine-state drift). Unmodified harness, `20 200 42` / `20 0 42`.
Median of the 2 trials per binary; both harness sessions reported (A = persistence
off, B = persistence on).

**200 edits/s — streaming scope avg (ms):**

| Binary | A (off) | B (on) | vs pre-epic (A) |
|---|---:|---:|---:|
| pre-epic `4f931d6` | 3.553 | 3.584 | — |
| pre-fix (post-refactor) | 4.995 | 5.057 | **+40.6%** (reproduces the #211 ~+42%) |
| **post-fix (this PR)** | **2.356** | **2.380** | **−33.7%** (below baseline) |

**200 edits/s — frame avg (ms):**

| Binary | A (off) | B (on) |
|---|---:|---:|
| pre-epic `4f931d6` | 3.614 | 3.643 |
| pre-fix | 5.057 | 5.119 |
| **post-fix** | **2.424** | **2.446** |

Post-fix vs pre-fix: streaming **−52.8%** (A) / **−52.9%** (B). The fix removes the
regression and lands ~34% below the pre-epic baseline (the row-hoisted fill beats
even the pre-epic per-cell `getVoxel`).

**Zero edits (`20 0 42`) — streaming scope avg (ms), 1 interleaved pair:**

| Binary | A (off) | B (on) | vs pre-epic (A) |
|---|---:|---:|---:|
| pre-epic `4f931d6` | 1.390 | 1.418 | — |
| pre-fix | 1.861 | 1.897 | +33.9% (the #211 zero-edit gap) |
| **post-fix** | **0.913** | **0.902** | **−34.3%** (below baseline) |

Confirms the cost lives in the streaming/mesh-dispatch path, not the edit path:
with zero edits the same halo-fill indirection tax appears (fewer dispatches) and
the same fix removes it (post-fix vs pre-fix **−50.9%**).

## 4. Acceptance verification

- **Boundary guards green:** `CoreIncludeGuard`, `HeadlessDeps`, `HeadlessSmoke`,
  `HeadlessRunner` — 4/4 pass; the configure-time link-closure assertion holds.
- **Headless determinism unchanged:** 38-check acceptance driver 0 failures; canonical
  digest `3a068dd4be8ab57e` (chunks=129 edits=4 mobs=15, player
  `(8.301, 63.001, 1.301)`, area `(-16,0,-16)-(15,255,15)`) — bit-identical.
- **Full graphical suite 44/45** — sole failure the pre-existing #213 `cave_emissive`
  golden mismatch, **identical metrics** (`mean_abs=0.041943 rms=0.070555
  hot=0.800885`); every other golden scene passes.
- **Visual goldens:** the halo snapshot is byte-identical, so rendered output is
  unchanged from `main` (all lighting-sensitive scenes pass: mob_lighting,
  auto_exposure_cave, underwater, water/shore, cave).
- **In-game benchmark Grade S** preserved (`--seed 42 --benchmark 20`: Score 9871,
  frames >33.3 ms = 0, `mesh.haloFill` 0.40 ms avg). GPU-bound; the CPU streaming
  scope reduction does not regress frame time.

## Conclusion

The streaming-scope cost is reduced toward (and below) the pre-epic baseline without
violating the CPU/render boundary, changing publication ordering, or touching save
contracts. The dominant cost was the halo-fill per-voxel handle indirection — not the
event copy or the shell lock — and hoisting the row access removes it while producing
a byte-identical light snapshot. The requested drain-buffer reuse is included; the
redundant-drain and shell-lock bullets were audited and found already optimal.
