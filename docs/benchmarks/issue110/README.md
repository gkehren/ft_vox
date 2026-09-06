# Issue #110 — Packed Voxel Mesh Vertices & Per-Draw Chunk Origin (A/B)

Fix: Pack voxel `Vertex` from 28 bytes to 16 bytes, chunk-local packed coordinates, and per-draw chunk origin (`VoxelDrawData`) via frame-mapped SSBO indexed by `gl_InstanceIndex` (`firstInstance`).
Baseline: `main` @ `c868716` (28-byte uncompressed `Vertex` with world-space coordinates).

Both binaries built Release from clean trees with CMake on Windows (MSVC 2022, x64).

## Protocol

- `ft_vox --seed 42 --benchmark 10` (automated Vulkan streaming benchmark)
- Device: NVIDIA GeForce RTX 4070 Ti, 1920x1080, ViewDist 512, FrontBias 0.3, VSync off
- Raw reports archived in this directory:
  - Baseline: `bench_20260906_174831_c8687168b7e2_s42_sc9926.txt`
  - PR (Packed): `bench_20260906_175733_c8687168b7e2__s42_sc9861.txt`
    (provenance caveat: built from a dirty tree, so the revision label shows
    the parent hash + `*`; the binary was the packed build)

## Interleaved 30 s A/B (3 pairs, clean trees, post-review confirmation)

`M,P,M,P,M,P` vs main `c868716`, same settings at 30 s duration
(`bench_20260906_181*` files in this directory):

| Metric | main | PR | Δ |
|---|---|---|---|
| `upload.vertexBytes` / frame | 148-157 KB | **80-88 KB** | **≈ −45 %** (28→16 ratio) |
| `upload.indexBytes` / frame | 33-35 KB | 32-35 KB | parity (indices unchanged) |
| `cpu.mesh.capacityBytes` | 19.6-21.2 MB | 13.7-16.6 MB | −25…−31 % |
| `gpu.live.bytes` | 1.07 GB (10 arena pages) | 0.81 GB (8 pages) | −25 % |
| Record avg | 0.362-0.387 ms | 0.333-0.366 ms | ≈ −5 % (within noise) |
| GPU Shadow avgMs | 0.253-0.291 | **0.181-0.212** | **−28 %** (vertex fetch) |
| GPU Opaque avgMs | 0.986-1.039 | 0.947-1.049 | parity |
| Score | 9909-9922 (S) | 9911-9916 (S) | parity |

The −45 % vertex upload bytes with flat index bytes confirm the packing is
exactly the 28→16 stride change with no extra geometry; the shadow pass
improves most because it re-fetches the whole scene's vertices per cascade.

## Visual validation

- Spawn view (grass/trees/snow/water): geometry coherent, no chunk seams,
  trees and foliage intact, greedy quads correct.
- Far-from-origin view (10000, 110, 10000): large greedy water surface
  renders perfectly smooth — no precision artifacts at 10k blocks (positions
  are chunk-local; the ivec3 origin keeps full precision).

## Memory & Workload Telemetry

| Metric | Baseline (28B) | PR #110 (16B) | Δ |
|---|---|---|---|
| **`sizeof(Vertex)`** | 28 bytes | **16 bytes** | **−42.86 %** |
| **`upload.vertexBytes`** | 652.45 MB | **424.86 MB** | **−34.88 %** |
| **`cpu.opaque.vertex.capacityBytes`** | 12.93 MB | **8.17 MB** | **−36.84 %** |
| **`cpu.water.vertex.capacityBytes`** | 533.65 KB | **330.69 KB** | **−38.03 %** |
| **`cpu.mesh.capacityBytes`** | 16.43 MB | **11.77 MB** | **−28.37 %** |
| **`arena.highWaterBytes`** | 186.43 MB | **145.36 MB** | **−41.07 MB** |
| **`arena.pages` peak** | 5 pages | **4 pages** | **−1 page** |
| **`staging.slice.bytes` peak** | 923.39 KB | **762.62 KB** | **−17.41 %** |
| **`gpu.retired.bytes` peak** | 134.22 MB | **0 B** | **−100 %** |

*(Note: During the 10-second benchmark, PR #110 processed +577 more uploaded chunks and +123 more frames due to higher throughput, yet still uploaded 227.6 MB fewer vertex bytes overall).*

## Performance Telemetry

| Metric | Baseline (28B) | PR #110 (16B) | Δ |
|---|---|---|---|
| **Average FPS** | 436.71 | **449.10** | **+2.84 %** |
| **Frame time (avg)** | 2.290 ms | **2.227 ms** | **−2.75 %** |
| **GPU Uploads avg** | 0.0127 ms | **0.0079 ms** | **−37.79 %** |
| **GPU Shadow avg** | 0.2087 ms | **0.1441 ms** | **−30.95 %** |
| **CPU Record avg** | 0.2263 ms | **0.2062 ms** | **−8.88 %** |

## Conclusion

Packing voxel mesh vertices to 16 bytes reduces memory bandwidth and cache pressure across CPU meshing, GPU upload queues, and Vulkan vertex fetching/shading. Per-draw chunk origin submission via a single shared SSBO in descriptor set 0 introduces zero descriptor binds/updates per chunk and cleanly decouples chunk world positions from the vertex buffers.
