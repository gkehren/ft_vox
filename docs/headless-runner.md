# Headless simulation runner (`ft_vox_headless`)

Issue #210, Epic #204 **Phase F** — the standalone CPU simulation runner.
`ft_vox_headless` is a local, deterministic test/benchmark process that hosts
the **same** [`sim::GameSimulation`](../src/Sim/GameSimulation.hpp) facade the
graphical `Engine` hosts — with **no SDL3 window, Vulkan context, ImGui UI,
camera, renderer, GPU resource pool or shader asset**. It is *not* a network
server (the Epic's non-goals apply: no transport, no client/server semantics).

Related documents:

- [`simulation-render-contracts.md`](simulation-render-contracts.md) — the authoritative CPU/derived boundary (Phases A–F)
- [`engine-architecture.md`](engine-architecture.md) — the graphical host's frame orchestration
- [`building.md`](building.md) — full build requirements (the CPU-only path below needs none of the graphics stack)

---

## 1. What it proves

The runner is the executable evidence for the Epic #204 dependency arrow:

```
ft_vox_core (CPU-only static lib: glm + FastNoise2 + Threads)
      │
      ├── ft_vox            (graphical host: SDL3 + Vulkan + ImGui)
      └── ft_vox_headless   (this runner: nothing else)
```

- It links **only `ft_vox_core`** — enforced at configure time (transitive
  link-closure assertion in the top-level `CMakeLists.txt`) and by the
  `HeadlessDeps` CTest (`tests/check_headless_deps.py`: include closure +
  real link artifacts + discovery check).
- It drives the world through the **explicit headless interest source** — a
  plain point + radius in blocks — never a `Camera` or GPU render distance.
- It exercises the real async generation/publication path (`--workers N`)
  and the synchronous path (`--workers 0`), the issue #180 persistence
  contracts (open / edit / flush / refusable close / reload / player restore)
  and the player-aware placement rule shared with the graphical host
  (`canPlaceVoxel`, issue #209).

## 2. Building

### Full configuration (game + runner)

The runner is built alongside the game in every normal configuration:

```bash
cmake --build build-vk --target ft_vox_headless      # Linux/macOS
cmake --build build --config Release --target ft_vox_headless   # Windows
```

### CPU-only configuration (no graphics stack at all)

`-DFT_VOX_GRAPHICS=OFF` selects the CPU-only slice: `ft_vox_core`,
`ft_vox_headless` and the CPU-only tests. The graphical dependencies —
SDL3, Vulkan loader/headers, volk, VMA, miniz and the SPIR-V compilers — are
**not discovered at all** (no `find_package`/FetchContent call runs for
them), so this configures and builds on a machine with **no Vulkan SDK, no
SDL3 and no shader toolchain**. Only a C++20 compiler, CMake ≥ 3.16, Python 3
(for the test guards) and network access for the GLM/FastNoise2 FetchContent
(or pre-downloaded sources) are needed.

```bash
# Linux
cmake -B build-cpu -DFT_VOX_GRAPHICS=OFF -DFT_VOX_DEP_MODE=system -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpu -j
ctest --test-dir build-cpu --output-on-failure
```

```powershell
# Windows (no vcpkg toolchain required)
cmake -B build-cpu -DFT_VOX_GRAPHICS=OFF -DFT_VOX_DEP_MODE=system
cmake --build build-cpu --config Release
ctest --test-dir build-cpu -C Release --output-on-failure
```

```bash
# macOS
cmake -B build-cpu -DFT_VOX_GRAPHICS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpu -j
ctest --test-dir build-cpu --output-on-failure
```

The `CpuOnlyBuild` CTest (label `cpu-only`) automates exactly this as a
**separate clean configure + build + test run** in `<build>/cpu-only-check`,
reusing the parent build's FetchContent sources when present (offline-safe).
It is **opt-in** (`-DFT_VOX_CPU_ONLY_BUILD_CHECK=ON`) because it adds a full
nested configure/build (~150 s); the always-on `HeadlessDeps` guard is the
default scripted dependency check. Enable the gate in CI, or run it locally
with `ctest -L cpu-only` once enabled.

### Portable floating-point codegen (`FT_VOX_PORTABLE_FP`)

By default `ft_vox_core` keeps the historical game codegen (`-O3
-march=native` on non-Apple Linux, FMA contraction allowed). Digests are then
reproducible **per binary**. `-DFT_VOX_PORTABLE_FP=ON` compiles the core and
the runner with the canonical shared flag set — no `-march=native`,
`-ffp-contract=off` (GCC/Clang) / `/fp:precise` (MSVC) — so digests can be
compared **across machines of the same platform family** built with the same
option. This resolves the "canonical shared flag set deferred to #210" note
from Phase A.

## 3. CLI reference

```text
ft_vox_headless [options]
  --seed N              world seed, 1..2147483647 (default 42; an existing save keeps
                        its stored seed). 0/negative is rejected (exit 2): the runner
                        requires an explicit positive seed for reproducibility, unlike
                        the game which rolls a random seed for <= 0.
  --ticks N             host frames to simulate at fixed dt = 1/60 (default 600, max 1000000)
  --script PATH         deterministic action script (§4)
  --world NAME          open/create a persistent world under --saves-root
  --saves-root PATH     saves root (default 'saves'; ALWAYS use a temp dir in tests)
  --interest X,Y,Z      pin the headless interest probe (default: follow the player feet)
  --interest-radius N   streaming radius in blocks, 32..1024 (default 96)
  --workers N           generation worker threads, 0..64 (default 0 = synchronous)
  --digest-area X0,Y0,Z0,X1,Y1,Z1
                        inclusive observed-area cell bounds for the digest
                        (default: 32x32 columns around the bootstrap chunk, full height)
  --expect-digest HEX   fail (exit 5) unless the final digest equals HEX
  --timeout SEC         wall-clock budget, 1..3600 (default 120)
  --json                emit the summary as JSON instead of key=value
  --help                usage text
```

Smoke commands (copy/paste):

```bash
# Linux / macOS (from the repository root, after building)
./build-cpu/ft_vox_headless --seed 42 --ticks 600 --script tests/fixtures/sim-actions.txt
./build-cpu/ft_vox_headless --world demo --saves-root /tmp/ft_vox_demo --ticks 600
```

```powershell
# Windows
.\build-cpu\Release\ft_vox_headless.exe --seed 42 --ticks 600 --script tests\fixtures\sim-actions.txt
.\build-cpu\Release\ft_vox_headless.exe --world demo --saves-root $env:TEMP\ft_vox_demo --ticks 600
```

### Exit codes

| Code | Meaning |
|---|---|
| 0 | success |
| 2 | usage error (unknown flag, bad value) |
| 3 | script error (I/O, grammar, wait budget exceeds `--ticks`) |
| 4 | world/save I/O failure (open refused, seed mismatch, flush/close failed) |
| 5 | assertion mismatch (`expect voxel`, `expect digest`, `--expect-digest`) |
| 6 | run timeout (bounded teardown; never stalls indefinitely) |

Unlike the graphical host (which continues transient when a save cannot
open), the runner treats a requested-but-unopened world as an **error exit**
— a test runner must not silently lose persistence.

### Summary output

One machine-readable line on stdout (the facade's own `[world] …` log lines
may precede it; parse the line starting with `SUMMARY `):

```text
SUMMARY status=ok exit_code=0 seed=42 generator=1 ticks=600 workers=0 interest_radius=96 chunks=129 edits=4 refused=0 edit_revision=5 mobs=15 player=(8.301,63.001,1.301) area=(-16,0,-16)-(15,255,15) digest=3a068dd4be8ab57e save=none
```

`--json` emits the same fields as a single JSON object. Fields: world
identity (`seed`, `generator`), executed `ticks`, `workers`,
`interest_radius`, residency (`chunks`), `edits` (accepted place/delete),
`refused` (player-aware `canPlaceVoxel` refusals), `rejected` (world-level
edit rejections, e.g. an unloaded/in-transit target — also logged to stderr),
`edit_revision` (next logical edit id), `mobs`, authoritative `player` feet
position, observed `area`, canonical `digest` (§5) and `save` status:

- `none` — transient session (no `--world`), nothing to persist.
- `ok` — explicit save-on-exit succeeded (player state captured + flushed +
  closed).
- `failed` — the explicit save-on-exit (capture / flush / refusable close)
  failed; reported as exit 4.
- `skipped` — the run errored before save-on-exit, so the **explicit**
  `savePlayerState` + close was skipped. Note this does **not** mean nothing
  was written: the unconditional `shutdown()` safety net
  (`forceCloseWorldForShutdown`) still resolves stranded edits and flushes
  accepted chunk edits to disk (issue #180 contract). Only the explicit
  `player.state` capture is skipped, so a `skipped` run may persist chunk
  edits but no player state.

## 4. Action scripts

Line-based plain text; `#` starts a comment, blank lines are ignored.
Actions execute **sequentially** at host-frame granularity: `wait N` advances
N fixed-dt frames with the current player command; every other action is
instantaneous between frames. After the script ends, the runner idle-fills
the remaining `--ticks` budget — every run has a deterministic end condition.
A script whose total `wait` exceeds `--ticks` is an error (exit 3).

```text
wait <frames>                     advance N host frames (1..1000000)
move <x> <z>                      world-XZ movement direction (length <= 1 after normalize)
sprint on|off
jump                              one-frame jump press (consumed by the next frame)
swim up|down|off
flight on|off                     flight toggle at the current feet (refusals logged)
teleport <x> <y> <z>              host-driven reposition (doubles)
place <x> <y> <z> <block>         cell edit, gated by canPlaceVoxel (player-aware rule)
delete <x> <y> <z>                cell edit
interest <x> <y> <z>              pin the headless interest probe
interest follow                   interest follows the player again
save                              durable player-state capture + flush (requires --world)
expect voxel <x> <y> <z> <block|air|unknown>   immediate assertion (exit 5 on mismatch)
expect digest <hex>               final-digest assertion (checked at end of run)
```

- Cell coordinates are **integers**; edits target the cell center. `y` must
  be inside `[0, 255]`.
- `<block>` is a single token: a `TextureType` display name with underscores
  (case-insensitive, e.g. `Stone`, `OAK_LEAVES`, `oak_leaves`), a numeric
  ordinal, or `air`. Placing `AIR` clears the cell.
- `place` is gated by the **same** `GameSimulation::canPlaceVoxel` rule the
  graphical host uses (issue #209): a solid block overlapping the
  authoritative body is refused (counted in `refused`, logged to stderr),
  never silently placed.
- `unknown` in `expect voxel` asserts the cell is **unavailable** (unloaded /
  not generated) — the simulation never infers solidity there.

The canonical fixture is [`tests/fixtures/sim-actions.txt`](../tests/fixtures/sim-actions.txt).

## 5. The canonical state digest

The digest hashes **documented canonical fields only** — never raw pointers,
unordered iteration order, struct padding or derived/GPU state. Algorithm:
**FNV-1a 64-bit** over this byte stream (integers little-endian fixed width;
doubles the IEEE-754 binary64 bit pattern, little-endian):

1. magic `FTVHDIG1` (8 bytes — digest format version);
2. `seed` i32, `generatorVersion` u32, `ticks` u64, `frameDt` f64 (1/60);
3. observed-area bounds `minX,minY,minZ,maxX,maxY,maxZ` (6 × i32, inclusive);
4. area cells in **x-major → z → y** order: availability u8 + block type u8
   (type byte 0 when unavailable), sampled through `WorldCollisionView`
   (authoritative occupancy, effective pending edits included);
5. player: position 3×f64, velocity 3×f64, `grounded` u8,
   `waitingForTerrain` u8, `flight` u8, `selectedBlock` i32;
6. `editRevision` u64 (next logical edit id), `committedEdits` u64;
7. mob count u64, then per mob **sorted by entity id ascending**: id u64,
   species u8, position 3×f64, velocity 3×f64, grounded u8,
   waitingForTerrain u8, yaw f64, targetYaw f64, timer f64, gait f64,
   stride f64, age f64, walking u8, randomState u64.

Implementation: [`src/Headless/SimDigest.cpp`](../src/Headless/SimDigest.cpp).

**Default observed area**: the 32×32 column block (chunks `cx-1..cx`,
`cz-1..cz`) around the bootstrap chunk, full world height (`y 0..255`). On a
reload the bootstrap center is the *saved* player position, so pin
`--digest-area` when comparing digests across sessions.

### Determinism scope

- **Guaranteed**: repeated runs of the same binary, same seed/script/options
  → identical digest, on the same platform. Both `--workers 0` (synchronous)
  and `--workers N` (async + per-frame quiesce) produce the **same** digest:
  the runner publishes every dispatched generation job before the next
  simulation step, so worker scheduling never leaks into authoritative state.
- **Not guaranteed**: bitwise identity across platforms, compilers or FP
  flag sets (FMA contraction, libm). Use `FT_VOX_PORTABLE_FP=ON` on both
  sides for cross-machine comparison within a platform family, and treat
  cross-platform digest equality as best-effort (the Epic requires logical
  reproducibility, not cross-GPU pixel identity — the same spirit applies).

## 6. Headless frame orchestration

Fixed host cadence `dt = 1/60`; the simulation's internal fixed steps
(**120 Hz player / 60 Hz mobs**) are unchanged. Per frame, mirroring
`Engine::run`'s simulation → streaming → mobs order minus every presentation
step:

1. `advancePlayer(dt, command, suspended=false)`
2. interest update (player feet, or the pinned probe)
3. `world.updateStreaming(interest, +Z forward, settings)` — plain
   `WorldStreamSettings{maxRenderDistance=radius, minRenderDistance=max(32, radius/2), streamFrontBias=0.30}`
4. `world.processChunkLoading(8)` — fixed per-frame count, never wall-clock
5. generation: `generatePendingVoxels(interest, lodThresh², 8)` + bounded
   quiesce (`--workers N`), or `generatePendingVoxelsSync(interest, 8)`
   (`--workers 0`)
6. `world.pump()` + event drain (the headless consumer discards the stream)
7. `advanceMobs(dt, interest, radius, suspended=false)`
8. `sim.pumpWorld()`

Lifecycle: `initialize` → `bootstrapTerrain` → `finalizePlayerSpawn` →
frames → final quiesce → digest → optional `savePlayerState(0, 0)` +
**refusable** `closeWorld` → `shutdown` (destructor safety net). Yaw/pitch
are presentation; the runner pins them to 0 in the saved player state.

## 7. Tests

| CTest | What it proves |
|---|---|
| `HeadlessSmoke` | plain fixed-seed run exits 0 with a canonical summary |
| `HeadlessRunner` | full acceptance driver (`tests/test_headless_runner.py`): sync + async determinism, save→exit→reload→edit verification across 3 processes in an isolated temp dir, interest load/unload/reload residency assertions, every failure exit code (incl. NaN/malformed options + world-rejection diagnostics), timeout teardown, JSON summary |
| `HeadlessDeps` | include closure + real link artifacts + no graphics discovery in CPU-only caches (`tests/check_headless_deps.py`); always-on |
| `CpuOnlyBuild` | separate clean `FT_VOX_GRAPHICS=OFF` configure/build/test run (label `cpu-only`); **opt-in** via `-DFT_VOX_CPU_ONLY_BUILD_CHECK=ON`, recommended for CI |

All integration tests have wall-clock timeouts and deterministic end
conditions; none needs a GPU, window or shader asset. The existing CPU-only
suites (`CoreContracts`, `VoxelWorld`, `GameSimulation`, `CoreIncludeGuard`,
…) run unchanged in both configurations.
