# Benchmark memory and workload telemetry

Issue #101 adds an independent, process-local telemetry registry. It does not
change CPU scope sampling, worker profiler epochs, GPU timestamps or scoring.
`BenchmarkReport::workload` provides structured arrays with the enum keys and
stable text labels in `Engine/WorkloadTelemetry.hpp`. Saved reports and clipboard
output include the same snapshot in a **Memory / workload** section.

Set `FT_VOX_TELEMETRY=0` before launching to disable publications, event atomics
and worker clocks. The setting is immutable for the lifetime of the process;
changing it during a run would invalidate ownership accounting. Reports say
`disabled` instead of presenting zeroes as measurements. The default is enabled.

## Quantities and boundaries

All byte fields use bytes (divide by 1,048,576 for MiB). Gauges expose current
and peak values. Events expose interval totals and averages per measured frame.

| Fields | Meaning |
| --- | --- |
| `voxel.bytes` | Resident voxel storage capacity owned by active, voxel-backed chunks (free pool slots carry 0) |
| `voxel.pool.*` | VoxelStoragePool state. **Persistent:** `capacity` and `capacityBytes` describe retained backing memory (64 KiB blocks including the free list) and survive capture boundaries. **Capture-local:** `active` and `free` are sampled during measured frames and reset at the beginning of each capture, so previous runs cannot contribute their peaks. `voxel.pool.growEvents` counts allocations beyond the free list in the interval (steady-state streaming should be near zero) |
| `shell.sizeBytes`, `shell.capacityBytes` | Legacy names kept for #101 compatibility: the currently borrowed compact border bytes (17,408 per meshing chunk), 0 once returned after upload. The true retained metric is `border.pool.capacityBytes` |
| `border.pool.*` | BorderPool state sampled once per measured frame. **Persistent:** `capacity`, `capacityBytes` (retained blocks including the free list). **Capture-local:** `active`, `free`. `border.pool.growEvents` counts allocations beyond the free list in the interval |
| `cpu.{opaque,water}.{vertex,index}.*` | Since #104 these describe the pooled mesh build results (issue #104), not per-chunk vectors: `sizeBytes` is the live payload of results in flight or attached to chunks awaiting upload, `capacityBytes` is retained capacity across the whole pool (active + free list). Chunks no longer own mesh build buffers. Counters are maintained by pool bookkeeping under the pool mutex (`finishBuild`/`release`), never by reading vectors a worker may be mutating |
| `cpu.mesh.capacityBytes` | Combined retained mesh build capacity across the `MeshResultPool`, with its own simultaneous peak. Scales with concurrent meshing/upload work, not with the number of pooled chunks |
| `mesh.pool.*` | MeshResultPool state sampled once per measured frame. **Persistent:** `capacity`, `capacityBytes` (retained build-result blocks including the free list). **Capture-local:** `active` (blocks in flight or attached pending upload), `free`. `mesh.pool.growEvents` counts allocations beyond the free list in the interval. The pool is a high-water reusable pool whose capacity follows the observed in-flight/backlogged working set - not a hard-bounded pool; these gauges expose any growth |
| `column.bytes`, `occupancy.bytes` | Inline biome/color/height arrays and occupancy bitsets, including free chunks |
| `pool.*` | ChunkPool state only: capacity, acquired and free chunk slots; rejected acquisitions are interval events (VoxelPool blocks are reported by `voxel.pool.*`) |
| `chunks.active`, `chunks.deferred` | End-of-measured-frame manager counts, including the deferred chunk-release backlog |
| `gpu.{opaque,water}.{vertex,index}.bytes` | Requested buffer bytes from successful mesh VMA allocations until retirement or immediate destruction |
| `gpu.live.bytes` | Sum of those four live gauges, with its own simultaneous peak |
| `gpu.retired.*` | Mesh buffer bytes and count in the deferred GPU destruction queue |
| `mesh.allocations.*` | Successful mesh buffer creations and actual destructions; includes allocations rolled back after a staging failure |
| `upload.*` | Successful asynchronous chunk uploads and copied vertex/index bytes; deferred counts are failed chunk attempts, not unique chunks |
| `staging.slice.bytes` | Aligned cursor usage in the current frame slice; peak includes space consumed by subsequently aborted upload attempts |
| `staging.failures` | Allocations rejected for lack of room in that slice |
| `draws.*` | Actual chunk indexed draw commands, after culling/readiness checks; opaque, water and each of the three shadow cascades, plus shadow total |

GPU byte gauges describe requested buffer sizes, **not** VMA block size or
physical VRAM residency. A newly allocated replacement is live until it is
destroyed or retired, so old/new overlap is counted. Other VMA resources,
textures, render targets, driver overhead and staging allocation capacity are
not mesh bytes. No global VMA allocation walk is performed.

CPU gauges are ownership accounting, **not** process RSS. They exclude allocator
overhead, vector control objects, worker-local scratch vectors, terrain-generator
temporary data and container overhead in ChunkManager/ChunkPool. Fixed per-column
and occupancy storage is reported separately from vector allocations.

The exclusive chunk owner publishes voxel/border/column ownership at the end of
construction, generation, meshing, upload, reset and shell mutation. Move and
destruction update ownership as well. The sampler never reads vectors while a
worker modifies them. Published peaks capture those boundaries, not transient
vector reallocation overlap or a partially completed mesh. Already allocated
free-pool memory remains included even when active chunk count drops.

Since #104 the CPU mesh payload no longer lives on chunks: workers build into
pooled `MeshBuildResult` blocks and the upload stage returns them. The mesh
byte gauges are published once per measured frame from a coherent
`MeshResultPoolStats` snapshot (live payload sizes of active blocks, retained
capacity of every block), so a meshed-then-idle chunk retains nothing.

## Mesh stages

Each full mesh job accumulates elapsed nanoseconds locally for skylight,
block light, occupancy discovery and the combined face/greedy/AO/output loop.
LOD generation has its own timer. A completed job publishes one batch under a
mutex. Empty meshes naturally have no face-loop sample.

The current mesher fuses face detection, mask updates, greedy merging, AO and
output in nested loops. Its report includes initialized mask cells, actual AO
vertex evaluations, and produced opaque/water vertex/index counts. These are
operation counts, not independent AO/mask elapsed times. There are no per-voxel
atomics or clock reads. This preserves a low-cost reference before future
changes separate those algorithms. Full-mesh and LOD output counts are summed.

## Capture lifecycle

World reload, benchmark request and the warmup-to-measurement transition reset
events and worker accumulators. Persistent ownership gauges are retained across
capture boundaries and their peaks are rebased to the current ownership value,
so the world built during warmup keeps reporting its real footprint.
Capture-local gauges such as `staging.slice.bytes`, the end-of-frame
chunk-manager samples (`chunks.active`, `chunks.deferred`) and the pool
active/free splits (`voxel.pool.*`, `border.pool.*`, `mesh.pool.*`) start at
zero at every capture boundary: warmup state cannot become a measurement peak,
and the first measured frame publishes the real sampled values. Classification
lives in `isCaptureLocalGauge()`; a new gauge defaults to persistent ownership.
Warmup-zero starts use the same boundary.
Worker batches carry an epoch obtained at mesh entry; a batch from an earlier
epoch is discarded in full even if it finishes during measurement. Memory
ownership remains valid across epochs. Work still running when the final report
is snapshotted is not included in completed-job timings.

Draw/upload events originate on the main thread, while worker batches and CPU
memory publications are synchronized. Active/deferred manager gauges are
sampled once per measured frame. Reports freeze their snapshot at finalization;
later background activity cannot alter an already completed report.

See [the fixed seed-42 reference](benchmarks/issue101/README.md) for commands,
complete reports and validation results.
