# Headless simulation runner (`ft_vox_headless`)

Issue #210, Epic #204 **Phase F** — the standalone CPU simulation runner;
issue #230, Epic #223 **Phase G** — the scriptable gameplay loop + canonical
digest coverage. Issue #249 adds the Epic #245 Phase A multiplayer regression
gate for several local simulation sessions in one process.
`ft_vox_headless` is a local, deterministic test/benchmark process that hosts
the **same** [`sim::GameSimulation`](../src/Sim/GameSimulation.hpp) facade the
graphical `Engine` hosts — with **no SDL3 window, Vulkan context, ImGui UI,
camera, renderer, GPU resource pool or shader asset**. It is *not* a network
server (the Epic's non-goals apply: no transport, no client/server semantics).

Related documents:

- [`gameplay-loop.md`](gameplay-loop.md) — the authoritative item/gameplay model the script grammar drives (Epic #223)
- [`simulation-render-contracts.md`](simulation-render-contracts.md) — the authoritative CPU/derived boundary (Phases A–F)
- [`engine-architecture.md`](engine-architecture.md) — the graphical host's frame orchestration
- [`building.md`](building.md) — full build requirements (the CPU-only path below needs none of the graphics stack)
- [`dedicated-server.md`](dedicated-server.md) — the standalone persistent authority host and live tick scheduler

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
- It scripts the **full gameplay loop** (issue #230, Epic #223 Phase G):
  give / select / drop / break / placeSelected / inventory + item assertions,
  with the canonical digest covering the inventory, the id-sorted item
  population and the id allocator — the executable closure gate of the epic
  (see [`gameplay-loop.md`](gameplay-loop.md)).

## 2. Building

### Full configuration (game + runner)

The runner is built alongside the game in every normal configuration:

```bash
cmake --build build-vk --target ft_vox_headless      # Linux/macOS
cmake --build build --config Release --target ft_vox_headless   # Windows
```

### CPU-only configuration (no graphics stack at all)

`-DFT_VOX_GRAPHICS=OFF` selects the CPU-only slice: `ft_vox_core`,
`ft_vox_headless`, `ft_vox_server` and the CPU-only tests. The graphical dependencies —
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
                        inclusive observed-area INTEGER cell bounds for the digest
                        (default: 32x32 columns around the bootstrap chunk, full height).
                        y inside [0,255]; volume capped at 8,000,000 cells (per-axis
                        span clamp + overflow-safe volume check; bad bounds exit 2).
  --expect-digest HEX   fail (exit 5) unless the final digest equals HEX
                        (0x and 0X prefixes both accepted, 16 hex digits)
  --timeout SEC         wall-clock budget, 1..3600 (default 120). Cooperative
                        deadline: checked between frames and inside the quiesce
                        loop, not preemptive — every reachable stall is bounded
                        by construction (fixed budgets, capped catch-up); an
                        external watchdog remains the tool for a wedged process.
  --json                emit the summary as JSON instead of key=value
  --trace               emit one deterministic TRACE record per script action to stderr
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
| 5 | script assertion or `--expect-digest` mismatch |
| 6 | run timeout (bounded teardown; never stalls indefinitely) |

Unlike the graphical host (which continues transient when a save cannot
open), the runner treats a requested-but-unopened world as an **error exit**
— a test runner must not silently lose persistence.

### Summary output

One machine-readable line on stdout (the facade's own `[world] …` /
`[items] grant …` log lines may precede it; parse the line starting with
`SUMMARY `, or — with `--json` — the line starting with `{`):

```text
SUMMARY status=ok exit_code=0 seed=42 generator=1 ticks=600 workers=0 interest_radius=96 chunks=126 edits=6 refused=0 rejected=0 edit_revision=7 mobs=14 items=1 inventory=3 item_high_water=2 player=(8.5,221.5,8.5) area=(-16,0,-16)-(15,255,15) digest=9b5013db5e550fe2 save=none
```

(The example is the shipped output of the canonical fixture run above:
`--seed 42 --ticks 600 --script tests/fixtures/sim-actions.txt`.)

`--json` emits the same fields as a single JSON object. Fields: world
identity (`seed`, `generator`), executed `ticks`, `workers`,
`interest_radius`, residency (`chunks`), `edits` (accepted place/delete/
break/placeSelected), `refused` (player-aware `canPlaceVoxel` refusals),
`rejected` (world-level edit rejections and gameplay break/place refusals —
also logged to stderr), `edit_revision` (next logical edit id), `mobs`,
`items` (live dropped-item entities, issue #230), `inventory` (sum of all 36
slot counts, issue #230), `item_high_water` (monotonic entity-id allocator
high-water — ids are never reused, issue #230), authoritative `player` feet
position, observed `area`, canonical `digest` (§5) and `save` status:

- `none` — transient session (no `--world`), nothing to persist.
- `ok` — explicit save-on-exit succeeded (player state captured + flushed +
  closed). Since stabilization PR5 the save also commits a transactional
  gameplay checkpoint (`checkpoints/manifest` in the world directory); `ok`
  means the manifest committed, and a reopen recovers that exact generation
  (see `docs/adr/gameplay-persistence-transactions.md`). The canonical digest
  is unaffected — checkpoints change durability, not simulation state.
- `failed` — the explicit save-on-exit (capture / flush / refusable close /
  checkpoint commit) failed; reported as exit 4.
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
place <x> <y> <z> <block>         CREATIVE cell edit, gated by canPlaceVoxel (player-aware rule)
delete <x> <y> <z>                CREATIVE cell edit (infinite-block semantics)
give <item> <count>               grantItems admin seam (logged; issue #230)
select <slot>                     gameplay hotbar selection, slot 0..8 (issue #230)
drop <count>                      drop from the SELECTED slot along fixed +Z (issue #230)
break <x> <y> <z>                 GAMEPLAY breakBlock: registry drop spawned (issue #230)
placeSelected <x> <y> <z>         GAMEPLAY place-from-hotbar: consumes 1 on success (issue #230)
interest <x> <y> <z>              pin the headless interest probe
interest follow                   interest follows the player again
save                              durable player-state capture + flush (requires --world)
expect voxel <x> <y> <z> <block|air|unknown>   immediate assertion (exit 5 on mismatch)
expect inventory <slot> <item|empty> <count>   immediate assertion, slot 0..35 (issue #230)
expect items <n>                  immediate live-entity-count assertion, 0..1024 (issue #230)
expect digest <hex>               final-digest assertion (checked at end of run)
```

- Cell coordinates are **integers**; edits target the cell center. `y` must
  be inside `[0, 255]`.
- `<block>` is a single token: a `TextureType` display name with underscores
  (case-insensitive, e.g. `Stone`, `OAK_LEAVES`, `oak_leaves`), a numeric
  ordinal, or `air`. Placing `AIR` clears the cell.
- `<item>` is a single token: an `items::ItemId` registry name with the same
  discipline (case-insensitive, spaces/hyphens normalize to underscores,
  e.g. `Cobblestone`, `OAK_LOG`, `oak-log`), or a numeric registry ordinal
  in `[1, kItemCount]`. The `None` sentinel (`0` / `none`) is never a valid
  grant/expect token. `empty` is only valid in `expect inventory` and pins
  count 0 (the canonical empty stack is `{None, 0}`).
- Counts (`give`, `drop`, `expect inventory`) are bounded to `[0, 65535]`
  (the `uint16_t` stack-count range) **at parse time**; out-of-range slots,
  counts, item tokens and cell bounds are grammar errors (exit 3), never
  silent runtime truncation.
- `place` is gated by the **same** `GameSimulation::canPlaceVoxel` rule the
  graphical host uses (issue #209): a solid block overlapping the
  authoritative body is refused (counted in `refused`, logged to stderr),
  never silently placed.
- `unknown` in `expect voxel` asserts the cell is **unavailable** (unloaded /
  not generated) — the simulation never infers solidity there.

### Gameplay ops vs creative ops (issue #230)

The creative `place`/`delete` seams are the infinite-block editor path; the
gameplay ops drive the survival-lite loop through the **same commands the
graphical host issues** (see [`gameplay-loop.md`](gameplay-loop.md)):

- `give <item> <count>` — the logged `grantItems` test/admin seam (epic #223
  §3). Hotbar-first merge, then empty slots; a remainder that did not fit is
  logged to stderr. Not a gameplay rule — survival never grants.
- `select <slot>` — `selectHotbarSlot`, the gameplay selection (distinct
  from the creative `selectedBlock` seam, which the creative ops use).
- `drop <count>` — `dropFromSlot` on the selected slot. The headless host
  has no camera, so the direction is the fixed plain value **+Z** (manual
  drops carry the longer pickup delay — no instant re-pickup).
- `break <x> <y> <z>` — `breakBlock`: validates against the item registry
  (BEDROCK/fluids refused), deletes through the world edit semantics and
  spawns the registry drop (STONE → Cobblestone, grass block → Dirt, …).
  An accepted break counts in `edits`; refusals count in `rejected` with a
  stderr diagnostic carrying the `BreakResult` code.
- `placeSelected <x> <y> <z>` — `placeSelectedBlock`: the selected stack
  must hold a placeable item; gated by `canPlaceVoxel`; **consumes exactly 1
  item on success only**. `PlayerOverlap` counts in `refused`, every other
  refusal in `rejected` (stderr carries the `PlaceResult` code).
- **Auto-pickup is a tick rule, not a command**: it is exercised through
  `wait` (break → wait → proximity pickup into the inventory, hotbar-first).
  The canonical fixture demonstrates the full loop: break → wait → pickup →
  select → placeSelected, and ends with a manual drop still **in flight** so
  the sync/async digest parity check covers item physics.

The canonical fixture is [`tests/fixtures/sim-actions.txt`](../tests/fixtures/sim-actions.txt).

### Multiplayer scripts (issue #249)

Run the race fixture and its CPU-only regression gate from the repository root.

```bash
./build-cpu/ft_vox_headless --seed 42 --ticks 49 --workers 2 --trace --json --script tests/fixtures/sim-multiplayer-races.txt
ctest --test-dir build-cpu -R HeadlessMultiplayer --output-on-failure
```

On Windows use `build-cpu\Release\ft_vox_headless.exe` and add `-C Release`
to the CTest command.

```text
player create                     create a session and select its new PlayerId
player select <id>                direct subsequent player actions to this session
player remove <id>                retire a session and its private state
interest radius <chunks>          set the selected player's radius in chunks (0..8)
expect item <id> <item> <count>    check a live shared entity's exact stack
expect result <name>              check the last command result (case-sensitive)
expect revision <next-edit-id>    check the world's next logical edit ID
expect pending on|off             check whether any logical edits are still deferred
```

The local player starts selected with ID 1. New sessions start empty with
neutral input, receive increasing IDs, and join spatial gameplay after
`teleport` or a successful `flight` command. Multiplayer teleport coordinates
must be in `[-32768, 32767)`. Removing the local player or referencing a
missing or retired ID produces a script error. Removing the selected player
selects ID 1 again. Shared blocks and item entities survive the removal.

A `player` command or `interest radius` anywhere in the script enables
player-driven streaming before the first action. Each positioned player's
radius defaults to `--interest-radius` rounded up to chunks and capped at
eight. A zero radius disables that player's interest. Each frame advances
positioned players in ascending ID order with their own latched movement,
sprint and swim inputs. Jump presses clear after one frame. The remaining tick
budget after the script uses the same latched commands. Streaming applies
the simulation's shared load, generation and eviction budgets to the union of
body-centered areas. The solo `--interest`, `interest <x> <y> <z>` and
`interest follow` forms are incompatible with this mode. Mob interest
continues to follow the local player.

Inventory, hotbar, flight, movement and gameplay commands target the selected
session. Within a frame the script's action order decides competing edits.
During a tick eligible players attempt shared pickup in ascending ID order.
The result assertion reads the most recent result from a player lifecycle
command, `flight`, `place`, `delete`, `give`, `select`, `drop`, `break`,
`placeSelected` or `save`. Results include `Ok`, `WorldRejected`,
`PlayerOverlap` and `EmptyStack`. Assertions and waits leave that result
available for later checks. The four new assertions and `--trace` also work
with solo scripts.

Every `TRACE` record includes the script line, completed frame count,
acting or referenced player ID, operation and result, accepted edit ID
(`0` when absent), next edit revision, that player's inventory total,
item allocator high-water and sorted live item IDs. Multiplayer script
assertion errors include line, frame and player ID. The multiplayer summary
includes `digest_version=FTVHDIG3`, player totals keyed by ID and sorted live
item IDs. JSON adds each player's authoritative feet position. The existing
`inventory` and `player` fields continue to report the local player.

`save` commits the local player and inventory together with shared overrides,
dropped items and their ID allocator. The standard checkpoint captures
accepted deferred edits, and later frames publish them to the resident
terrain. Allow those frames before exit so the existing close barrier can
complete. Remote sessions and their inventories live for the current
process. Reopening uses the same solo-compatible save format and starts
with the local player followed by any new sessions the script creates.
No save conversion is needed to return to a build that predates this harness.

## 5. The canonical state digest

The digest hashes **documented canonical fields only** — never raw pointers,
unordered iteration order, struct padding or derived/GPU state. Algorithm:
**FNV-1a 64-bit** over this byte stream (integers little-endian fixed width;
doubles the IEEE-754 binary64 bit pattern, little-endian):

1. magic `FTVHDIG2` (8 bytes — digest format version);
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
   stride f64, age f64, walking u8, randomState u64;
8. **selected hotbar slot** i32, then the 36 inventory slots in **fixed slot
   order 0..35**: item u16 (`ItemId` registry ordinal) + count u16
   (issue #230);
9. item-entity count u64, then per entity **sorted by entity id ascending**:
   id u64, item u16, count u16, **quantized** position 3×i64, quantized
   velocity 3×i64, ageTicks u32 (issue #230). Quantization is
   `llround(value × 1024)` — a 1/1024-block grid that absorbs last-bit FP
   noise while keeping any gameplay-visible difference distinct;
10. entity-id allocator **high-water** u64 (ids are never reused across
    sessions, issues #226/#228; issue #230).

Implementation: [`src/Headless/SimDigest.cpp`](../src/Headless/SimDigest.cpp).

**Format version bump (issue #230)**: `FTVHDIG1` → `FTVHDIG2`. The gameplay
fields (8–10) were appended **deliberately** — digests recorded before the
bump are not comparable with post-bump digests. No expected digest is
hardcoded in `tests/test_headless_runner.py` or the fixtures (they are
computed at runtime and compared across runs), so the bump required no
fixture rewrite; the `--expect-digest` gate simply records the new-format
digest at runtime.

**Multiplayer format (issue #249)** uses the `FTVHDIG3` magic. In place of
field 5, write the player count u64, then each active player in ascending
PlayerId order. Each entry contains id u32, positioned u8, the body fields
from field 5 and the hotbar and slots from field 8. Omit field 8 from its old
position. After the item allocator high-water, append the resident chunk
count u64 and each chunk's x i32, z i32 and backing readability u8, sorted by
`(x, z)`. The shared voxel contents are covered within the chosen
`--digest-area`. Select an area covering the cell edits you want to compare.
Legacy scripts keep the exact `FTVHDIG2` byte stream.

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
(**120 Hz player / 60 Hz mobs / 60 Hz items**) are unchanged. Per frame,
mirroring `Engine::run`'s simulation → streaming → mobs → items order minus
every presentation step. The solo frame runs these steps.

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
8. `advanceItems(dt, suspended=false)` — the dropped-item tick rule
   (issue #230); early-outs at zero cost when no items exist
9. `sim.pumpWorld()`

Multiplayer replaces steps 1–5 with an ID-ordered player advance and
`GameSimulation::advancePlayerStreaming`. Async runs publish all dispatched
generation before the next simulation step. Steps 6–9 are shared.

Lifecycle: `initialize` → `bootstrapTerrain` → `finalizePlayerSpawn` →
frames → final quiesce → digest → optional `savePlayerState(0, 0)` +
**refusable** `closeWorld` → `shutdown` (destructor safety net). Yaw/pitch
are presentation; the runner pins them to 0 in the saved player state.

## 7. Tests

| CTest | What it proves |
|---|---|
| `HeadlessSmoke` | plain fixed-seed run exits 0 with a canonical summary |
| `HeadlessRunner` | full acceptance driver (`tests/test_headless_runner.py`): sync + async determinism, save→exit→reload→edit verification across 3 processes in an isolated temp dir, interest load/unload/reload residency assertions, every failure exit code (incl. NaN/malformed options + world-rejection diagnostics), timeout teardown, JSON summary; **issue #230**: the gameplay-loop fixture (give/select/break/wait-pickup/placeSelected/drop) with a live item in flight at the digest boundary, sync/async parity with item physics, `items`/`inventory`/`item_high_water` summary fields (key=value + JSON), a 3-process gameplay round-trip restoring inventory + entities + id allocator (no reuse), and gameplay grammar/assertion exit codes |
| `HeadlessMultiplayer` | repeated race digests and traces with 0, 1, 2 and 4 workers, item/block conservation, disconnect and fresh rejoin, transient eviction and regeneration, persistent save/reopen, and recovery of the explicit checkpoint taken with a deferred border edit |
| `HeadlessDeps` | include closure + real link artifacts + no graphics discovery in CPU-only caches (`tests/check_headless_deps.py`); always-on |
| `CpuOnlyBuild` | separate clean `FT_VOX_GRAPHICS=OFF` configure/build/test run (label `cpu-only`); **opt-in** via `-DFT_VOX_CPU_ONLY_BUILD_CHECK=ON`, recommended for CI |

All integration tests have wall-clock timeouts and deterministic end
conditions; none needs a GPU, window or shader asset. The existing CPU-only
suites (`CoreContracts`, `VoxelWorld`, `GameSimulation`, `CoreIncludeGuard`,
…) run unchanged in both configurations.
