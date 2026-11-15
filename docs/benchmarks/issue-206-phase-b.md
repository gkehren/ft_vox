# Issue #206: Phase B verification (Epic #204 — authoritative world)

Post-refactor verification that moving the authoritative voxel world
(`VoxelWorld` + `WorldChunk`, CPU-only `ft_vox_core`) out of the render-side
`ChunkManager`/`Chunk` introduced **no behavior or performance regression**.
Compare against [`issue-205-pre-refactor.md`](issue-205-pre-refactor.md).

See [`../simulation-render-contracts.md`](../simulation-render-contracts.md)
§10 for the boundary, the world event stream and the adapter removal plan.

## Environment

- **Machine:** AMD Ryzen 7 9800X3D (8 cores / 16 threads), 31.2 GB RAM,
  Windows 11 Pro 10.0.26300 (same reference system as the Phase A baselines).
- **GPU:** NVIDIA GeForce RTX 4070 Ti, driver 32.0.16.2114, Vulkan 1.4.357.
- **Base commit:** `2426f9bb` (main, Phase A landed) vs the Phase B branch.
- **Build:** Visual Studio 18 2026, MSVC **Release**, vcpkg toolchain
  `D:/Projects/vcpkg/scripts/buildsystems/vcpkg.cmake`.
- **Date measured:** 2026-11-14.

## Test suites

```powershell
ctest --test-dir build -C Release --output-on-failure
```

**39 / 40 PASS.** The only failure is the pre-existing `cave_emissive`
golden mismatch tracked in [#213](https://github.com/gkehren/ft_vox/issues/213),
with metrics **identical** to the Phase A baseline:

```
mean_abs=0.041943 rms=0.070555 hot=0.800885
```

All other visual scenes pass; the swapchain-format validation probe message
in the offscreen harness is also pre-existing. New/changed suites:

| Suite | Result | Notes |
|---|---|---|
| **VoxelWorld** (new, CPU-only) | PASS, 211 checks, 0.54 s | Links only `ft_vox_core`; issue #206 acceptance list (below) |
| CoreContracts | PASS | unchanged |
| CoreIncludeGuard | PASS | new roots + `VoxelPool` lifted into the allowlist |
| ChunkLifecycle | PASS | probes re-pointed at `WorldChunk`/`VoxelWorld`; assertions unchanged |
| WorldPersistence | PASS | deferred-edit probe re-pointed; assertions unchanged |
| MeshUploadLifecycle | PASS | identity stamps via `sim::ChunkIdentity` |
| MeshFaceKey / MeshResultPool | PASS | `beginBuild` identity contract |
| EngineReload | PASS | world reload through the adapter |
| TerrainGeneration | PASS | per-seed determinism fixtures byte-identical |

### Issue #206 acceptance coverage (`test_voxel_world`)

- Headless construct → generate → sample → edit → unload → reload → persist,
  no graphics runtime (links only `ft_vox_core`).
- Per-seed generated voxel fixtures **byte-identical** across two independent
  worlds (voxels, borders, biomes, heights, tints).
- Border edits + corner mirrors (both adjacent neighbors, never the diagonal),
  deferred + coalesced edits, negative coordinates (floor division), unknown
  terrain, safe chunk recycling (new incarnation, no stale edit resurrection),
  out-of-order generation completion and stale-revision rejection.
- Save contracts: restart, sparse edit normalization (revert deletes the chunk
  file), seed-mismatch guard, close failure/retry (persistence stays open and
  usable), concurrent edits during pending async I/O (newest value wins),
  stranded-edit close refusal then resolved close.
- Exact `ChunkCollisionView` collision semantics via `WorldCollisionView`
  (solid floor / open sky / SHRT envelope, unknown cells, pending-edit
  overlay, water medium + surface scan).

## Persistence / streaming A/B (same-session, alternating)

```powershell
./build/tests/Release/bench_persistence.exe 15
```

Seed 42, 200 edits/s, view distance 96, forward 24 blocks/s. To cancel
machine-state drift, the Phase B build and a **fresh build of main
(`2426f9bb`)** were run alternately in the same session (3 pairs, 2 runs
each, 15 s per run). Frame avg (ms):

| Pair | main baseline | Phase B |
|---|---:|---:|
| 1 | 4.241 / 4.307 | 4.010 / 4.142 |
| 2 | 4.277 / 4.323 | 4.080 / 4.189 |
| 3 | 4.242 / 4.370 | 4.045 / 4.120 |
| **Mean** | **4.29** | **4.10** |

**Phase B is ~4–5% faster** on this workload (the render adapter's event
drain replaces the old inline edit/invalidation bookkeeping; the authoritative
edit path no longer pays render-side costs). Per-step instrumentation
(`processFinishedJobs` / `processDeferredReleases` / `updateStreaming` /
`processChunkLoading` / `generatePendingVoxels` / `meshPendingChunks`) shows
the same distribution on both builds — `meshPendingChunks` (halo fill + shell
rebuild at the 360/s dispatch budget) dominates both, unchanged.

**Measurement note (transparent):** an earlier non-alternating session showed
~6 ms/frame for Phase B vs ~3.7 ms for a cached baseline number; re-running
both builds alternately in one session reproduced ~4.1–4.3 ms for **both**,
identifying the earlier delta as transient machine state (thermal/background),
not the refactor. The Phase A doc's 3.5–3.6 ms numbers were likewise measured
under a different machine state; the same-session A/B above is the
authoritative comparison.

## CPU profiles — same-session A/B (mob / physics)

Re-measured after review with a fresh build of main (`2426f9bb`) run
alternately with the Phase B build on an idle machine (2 pairs each), so the
comparison is not confounded by machine state. `test_chunk_lifecycle
--mobs-profile` / `--physics-profile`:

| Metric | main baseline | Phase B | Delta |
|---|---:|---:|---|
| Mob update mean (ms, 48 mobs) | 0.03438 | 0.03480 | **+1.2% (noise)** |
| Mob update p95 (ms) | 0.0407 | 0.0446 | sub-0.005 ms |
| Mob cells/tick · travelled · dropped | 1596 · 2753.91 · 0 | 1596 · 2753.91 · 0 | **identical** |
| Local light sampling mean (ms) | 0.003699 | 0.003598 | −2.7% |
| Player physics avg (ms, real voxels) | 0.000606 | 0.000636 | +5% (+0.03 µs absolute) |
| Player physics p95 (ms) | 0.001 | 0.001 | identical |
| Physics cells/tick · travelled · dropped | 39.9123 · 682 · 0 | 39.9123 · 682 · 0 | **identical** |

Deterministic outputs (cells sampled, distance travelled, dropped steps) are
**bit-identical**, and the timing deltas are within run-to-run noise — the
collision/query path's extra record indirection is not measurably costly. Both
are far below the documented < 0.1 ms p95/tick physics target.

**Measurement note (transparent):** the first revision of this doc compared
mob/physics against the *Phase A doc's* cached numbers (different machine
state) and showed mob mean +48%. Review correctly flagged that only frame
timings had received a same-session A/B. The alternating A/B above replaces
that comparison: the +48% was machine state, not the refactor (mob mean is
+1.2% like-for-like).

Terrain generation is source-unchanged by this PR; `test_terrain --profile 32
1337` totals 2.68 ms/chunk vs the Phase A doc's 2.62 ms/chunk (+2.6%, machine
noise; the `TerrainGeneration` determinism suite passes byte-identical).

## In-game benchmark (graphical smoke)

```powershell
./build/Release/ft_vox.exe --seed 42 --benchmark 20
```

Full game loop (SDL + Vulkan + streaming + edits + GPU uploads + benchmark
report) runs clean on the final (post-review) binary, exit 0, no validation
errors:

| Metric | Value |
|---|---|
| Score | **9896 / 10000 (Grade S)** |
| Avg FPS | 521.7 |
| 1% low FPS | 202.7 |
| Frame p50 / p95 / p99 (ms) | 1.50 / 3.86 / 4.93 |
| Streaming (ms/frame) | 1.13 |
| TerrainGen (ms/chunk) | 2.49 |

Absolute FPS varies strongly with machine state (an earlier loaded-machine run
of the same code scored 9725 at 203 avg FPS); the grade is S in both and the
loop is clean. As with the CPU A/B above, the same-session comparison is the
authoritative one — the in-game run here is a graphical smoke test, not a
cross-session performance claim.

World reload (`reloadWorld`) exercised by the benchmark's warmup transition;
`EngineReload` suite covers the persistent-world close/reopen identity path.

## Review fixes (round 2)

Two memory/telemetry regressions flagged in review were fixed and re-measured
(`ft_vox --seed 42 --benchmark 5`, default view distance 512):

- **Pool gauges re-pointed at the authoritative world.** `Engine::publishFrameTelemetry`
  read `ChunkPool`'s voxel/border pools, which are unused in the game path
  (bound handles write through `VoxelWorld` records). The gauges reported 0
  while `voxel.pool.growEvents` proved the world pools were live. Now reads
  `VoxelWorld::voxelPool()/borderPool()`:

  | Gauge | Before fix | After fix |
  |---|---:|---:|
  | `voxel.pool.capacity` / `active` | 0 / 0 | 792 / 720 |
  | `voxel.pool.capacityBytes` | 0 | 51,904,512 (~49.5 MiB) |
  | `border.pool.capacity` / `active` | 0 / 0 | 246 / 221 |
  | `border.pool.capacityBytes` | 0 | 4,282,368 (~4.1 MiB) |

- **Per-column metadata no longer duplicated.** Every pooled render handle
  eagerly constructed a private `WorldChunk` (4 KiB of per-column arrays) that
  is never read while the handle is world-bound, doubling `column.bytes`
  (~73 MB vs the pre-split ~36.7 MB). The owned record is now allocated
  **lazily** (`Chunk::recordForWrite`) — only when a handle is actually used
  standalone/unbound — so idle bound handles carry no per-column backing:

  | Gauge | Before fix | After fix | Pre-split |
  |---|---:|---:|---:|
  | `column.bytes` | 73,129,984 | 36,700,160 | ~36.7 MB |
  | `occupancy.bytes` | 571,328 | 286,720 | ~287 KB |

  (`pool.capacity` stays 8894 — the render handle count is unchanged; only the
  idle per-column backing was removed.)

Also addressed: a debug assert guarding `Chunk::reset` against changing a
bound record's coordinate (would desync `VoxelWorld::m_chunks`); the stale
`SimContracts.hpp` "KNOWN DUPLICATION" comment (the copy is deleted); lock-order
notes at both acquisition sites; the `applyPendingEdits` iteration invariant
asserted in code; the event stream switched to an O(1) deque with a
`droppedEventCount()` counter; a regression test for the render-pool-exhaustion
bind retry (`m_pendingBinds`); and doc drift (`workload-telemetry.md`,
`player-physics.md`, contracts §1).

### Round 3

- `applyPendingEdits` now uses a **frozen-size index loop** (entry copied by
  value) — structurally safe against re-queue invalidation, not merely
  asserted after the fact.
- The bound-reset guard gained a **Full-mode edit-loss assert**: a Full reset
  of a bound record whose world is open (tracking armed) is refused, since it
  would discard save state behind the world's capture path. Verified in a
  Debug build that the legitimate recycle-in-place test (persistence closed)
  still passes.
- The world→chunk floor division is consolidated into **one helper**
  (`worldToChunkCoord` in `utils.hpp`); `VoxelWorld`, `WorldChunk::coord()` and
  the reset guard all delegate to it.
- `droppedEventCount()` is **wired**: `ChunkManager::drainWorldEvents` emits a
  one-shot warning when the count changes (never fires at frame cadence).
- Telemetry gating reordered: the world-pool gauges publish **before** the
  `ChunkPool` guard, so they stay correct in any configuration without a
  render pool.

## Conclusion

Phase B moves the authoritative world across the CPU/render boundary with
**no behavior change** (deterministic fixtures bit-identical, full suite green
except the pre-existing #213 visual mismatch with identical metrics) and **no
performance or memory regression** (same-session A/B ~4–5% faster on the
persistence/streaming workload; mob/physics A/B within noise; in-game benchmark
Grade S; retained-backing memory gauges restored to the live world pools and
per-column metadata de-duplicated to the pre-split ~36.7 MB after review). The
temporary `ChunkManager` render adapter and `ChunkCollisionView` light wrapper
are documented with a removal plan (contracts doc §10) for Phases C/E.
