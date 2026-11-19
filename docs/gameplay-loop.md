# The gameplay loop (Epic #223)

**Authoritative reference** for the first persistent gameplay loop:

```
break block -> item entity drops -> auto-pickup -> inventory / hotbar -> place (consumes)
```

Owned end-to-end by the CPU simulation (`sim::GameSimulation` in
`ft_vox_core`), saved/reloaded with the world, presented by derived-only
graphical surfaces, and fully scriptable in `ft_vox_headless`. Landed across
issues #224 (A) → #225 (B) → #226 (C) → #227 (D) → #228 (E) → #229 (F) →
#230 (G, closure gate).

Related documents:

- [`simulation-render-contracts.md`](simulation-render-contracts.md) — the CPU/derived boundary and ownership table (Epic #204, extended by #223)
- [`headless-runner.md`](headless-runner.md) — the script grammar, canonical digest and SUMMARY fields that drive this loop headlessly
- [`engine-architecture.md`](engine-architecture.md) — the graphical host's input mapping and frame orchestration
- [`terrain-generation.md`](terrain-generation.md) — the block catalog the item registry aligns with

The Epic #204 arrow is unchanged and non-negotiable: **render depends on
simulation, never the reverse.** Every gameplay rule below lives in
`GameSimulation` / `src/Items/`; `Engine`, `RenderWorldBridge` and `GameUI`
only map input to commands and present snapshots.

---

## 1. Module map

| Module | Layer | Responsibility |
|---|---|---|
| `src/Items/ItemTypes.hpp` | CPU core | `ItemId`, `ItemStack`, result vocabularies, size constants |
| `src/Items/ItemRegistry.*` | CPU core | the static append-only catalog + block→item / block→drop tables + token parsing |
| `src/Items/ItemStack.*` | CPU core | pure merge/split/transfer functions with explicit results |
| `src/Items/Inventory.*` | CPU core | the fixed 36-slot container + deterministic slot operations |
| `src/Items/ItemEntitySystem.*` | CPU core | dropped-item entities: fixed step, pickup, despawn, cap, id allocator, snapshots + events |
| `src/Sim/GameSimulation.*` | CPU core | single owner: inventory + item system + interaction commands + persistence capture/restore |
| `src/World/WorldSave.*` / `WorldPersistence.*` | CPU core | `inventory.state` (FTVI) + `items.state` (FTVJ) formats and atomic I/O |
| `src/Engine/GameUIInventory.cpp` + `InventoryUi.hpp` | graphical host | hotbar HUD + inventory panel (snapshot-only displays, command callbacks) |
| `src/Renderer/ItemEntityRenderer.*` | derived render | instanced atlas cubes fed by id-sorted snapshots |
| `src/Headless/` | CPU runner | gameplay script grammar + digest coverage (issue #230) |

## 2. Item model and registry rules

- **`ItemId`** is a stable `uint16_t` registry ordinal, **append-only**,
  independent of the render-atlas `TextureType` (which carries per-face
  variants like `GRASS_TOP`/`GRASS_SIDE`). Saves and the future wire protocol
  reference `ItemId`, **never** `TextureType`. `None = 0` is the sentinel.
- **`ItemStack { ItemId item; uint16_t count; }`** — a 4-byte plain value.
  The **canonical empty** is `{None, 0}`; `isEmpty()` is `count == 0`. No
  durability/NBT/metadata in v1.
- **`ItemDefinition`**: id, stable display name, `maxStack` (64 default),
  optional `placeableBlock` (the canonical voxel `TextureType` it places).
  One static table (`itemDefinitions()`), aligned with the block catalog via
  `static_assert`s — the same discipline as `biomeTypeString`/`kBlockLayers`.
- **Block mappings live in the registry**, not the simulation or renderer:
  - `itemForBlock`: one block item per breakable/placeable block; render-only
    face variants resolve to their canonical block's item (`GRASS_SIDE` →
    GrassBlock, `OAK_LOG_TOP` → OakLog). `None` for AIR/fluids/out-of-range.
  - `dropForBlock`: **STONE → Cobblestone**, the **grass block → Dirt**,
    leaves drop themselves (explicit v1 table decision — no saplings, no
    RNG), ores drop their own block. Exactly one drop item in v1.
  - `isBreakableBlock`: every catalog block **except BEDROCK** (unbreakable)
    and **WATER/LAVA** (fluids, not breakable by hand).
- **Adding a block** requires updating all three pinned mappings together:
  `canonicalBlock` (registry), the render face remap
  (`blockTopFace`/`blockBottomFace`), and the `kFaceVariants` pin table in
  `tests/test_items.cpp` (see `AGENTS.md`).
- **Token parsing** (`parseItemId`): case-insensitive, spaces/hyphens
  normalize to underscores (`Grass Block` / `grass-block` / `GRASS_BLOCK`
  all match), or a numeric ordinal in `[1, kItemCount]`. `None` (`0`/`none`)
  and unknown tokens are rejected. This is the headless grammar seam.

## 3. Inventory and the command/result vocabulary

`items::Inventory` is a fixed **36-slot** container (**hotbar = slots 0..8**,
main = 9..35) plus the selected hotbar index. Trivially copyable, no heap
churn per operation. Owned by `GameSimulation`; the read-only seam is
`inventory()` / `inventorySnapshot()` — **every mutation goes through a
command**.

All commands apply **immediately at the call** (the host frame boundary),
before the next fixed step. There is no command buffer: suspension never
replays or reorders anything.

**Actor-aware forms (issue #247, Epic #245 Phase A2):** every gameplay command
also has a per-player form taking a `sim::PlayerId` first (e.g.
`breakBlock(id, cell)`, `placeSelectedBlock(id, cell)`, `dropFromSlot(id, slot,
count, dir)`); the legacy forms below are thin forwards to the **local**
player, so solo behavior is bit-identical. An unknown / retired actor answers
`InvalidPlayer` **without side effects**. See
[`simulation-render-contracts.md` §12](simulation-render-contracts.md).

| Command | Semantics | Result codes |
|---|---|---|
| `selectHotbarSlot(index)` | gameplay selection, 0..8 | `Ok` / `InvalidSlot` |
| `moveStack(from, to)` | whole-stack move/merge | `Ok` / `Partial` / `Full` / `InvalidSlot` / `EmptyStack` / `ItemMismatch` |
| `swapStacks(a, b)` | unconditional exchange (empty included) | `Ok` / `InvalidSlot` |
| `splitStack(from, to)` | move the **larger half** (ceil; odd counts defined) with transfer semantics | `Ok` / `Partial` / `Full` / `InvalidSlot` / `EmptyStack` / `ItemMismatch` |
| `grantItems(item, count)` | **test/admin seam**, explicitly logged (`[items] grant …`); hotbar-first merge then empty slots; counts above maxStack distribute | `AddResult{result, remainder}` |
| `dropFromSlot(slot, count, dir)` | remove up to `count` (clamped) + spawn one entity at the player eye along the host-supplied plain direction; degenerate/non-finite dir falls back to +Y; manual drops carry the longer pickup delay | `Ok` / `InvalidSlot` / `EmptyStack` / `IdExhausted` |
| `breakBlock(cell)` | registry validation + world delete + registry drop spawn (§5) | `BreakResult` (§4) |
| `placeSelectedBlock(cell)` | hotbar availability + `canPlaceVoxel` gate + world edit + **consume exactly 1 on success only** | `PlaceResult` (§4) |

`items::ItemResult` is the shared vocabulary: `Ok`, `Partial` (remainder
returned/stays in the world), `Full` (nothing applied), `InvalidSlot`,
`EmptyStack`, `InvalidItem`, `ItemMismatch`, `NotPlaceable`, `IdExhausted`
(stabilization PR3: the entity-id allocator reached its representable bound;
the operation is refused **without consuming anything**), `InvalidPlayer`
(issue #247: an unknown / retired actor handle; the per-player command is
refused **without touching any inventory**). A clamped
take/transfer of all available items is still `Ok` — availability is not an
error. The vocabulary is **append-only**: existing codes never move
(save/wire stable). **The future authoritative server reuses these exact
codes as its rejection vocabulary.**

Stack math (`mergeStacks`/`splitStack`/`transferStack`) is pure functions
over plain values with explicit results — shared by the inventory commands
and the pickup path. Odd half-splits take the **larger** half.

### Selection duality

The **hotbar selection** (`selectHotbarSlot`) is the gameplay selection
(walking mode). `selectedBlock()` remains the **creative/editor seam**
(flight mode, benchmarks, headless `place`/`delete` scripts, the `T` cycle).
The two never synchronize; both are persisted in `player.state` /
`inventory.state` respectively.

## 4. World interaction commands

- **`raycastVoxel(origin, dir, maxDist)`** — the targeting rule, moved out of
  `Engine` into the simulation (issue #227) with the exact historical march:
  0.05 float step, first ACTIVE cell, adjacent-cell fallback when the ray
  starts inside a block, unknown cells never hit. `dir` is normalized
  inside; the host passes the raw camera front as a plain value. Pure
  query — never edits the world. Returns a plain
  `RaycastHit{hit, block, adjacent}`.
  - **Authoritative effective state (stabilization PR1/PR2):** the march
    samples through a scoped `WorldCollisionView` — stored backing overlaid
    with pending logical edits — the SAME state `breakBlock`,
    `placeSelectedBlock` and physics collide with. A deferred deletion is
    already invisible to targeting and a deferred placement already
    targetable before the edit physically lands. (The creative
    `VoxelWorld::isVoxelActive` seam reads raw backing and is intentionally
    NOT the targeting rule.) Fluids/vegetation hit as before (any non-AIR
    effective type); the collision view's solid floor / open sky boundaries
    are physics policy and never create targeting hits.
  - **Input validation (stabilization PR1):** non-finite origin/direction
    components, non-finite or negative `maxDist`, degenerate directions
    (squared length < 1e-12, evaluated in double so finite-but-huge
    components cannot overflow the gate) and origins outside
    `kRaycastOriginLimit` (±32700, the supported world-coordinate envelope)
    answer a safe no-hit. `maxDist` is clamped to `kMaxRaycastDistance`
    (64 — 2× the widest UI interaction range of 32) and the march is
    iteration-bounded, so the query always terminates. Valid gameplay rays
    keep the historical float arithmetic bit for bit (CPU parity fixtures).
  - **Actor-aware form (issue #247):** `raycastVoxel(id, dir, maxDist)`
    derives the origin from the actor's authoritative eye (never
    client-supplied) and clamps `maxDist` to `kInteractionReach` (8). The
    raw-origin form above is the **privileged creative/editor host seam**;
    the actor-aware form is the network-facing gameplay query. Both share one
    march implementation; an unknown actor answers a no-hit.
- **`breakBlock(cell)`** → `BreakResult`:
  - `Ok` — block deleted through the existing world edit semantics (issue
    #114 deferred-edit ordering preserved) + the registry drop spawned at
    the cell center with deterministic id/edit-revision jitter.
    **Ordering note (review §4.4, corrected by stabilization PR2):** the
    drop spawns immediately at LOGICAL ACCEPTANCE, even when the delete
    itself defers (target record in transit / UNLOADED). Acceptance is the
    authoritative gameplay event boundary: the effective-voxel overlay
    (`WorldCollisionView` / `VoxelWorld::effectiveVoxelType`) already
    reflects the accepted deletion, so the item's physics, subsequent
    targeting and repeated-break refusal all observe the post-break state —
    the drop does NOT collide with the old block, and a delayed mesh update
    never creates a second gameplay event. Coalesced/superseded pending
    edits collapse to one entry per coordinate (one drop per accepted
    break). Accepted deferred edits are durable across unload: the world
    bakes them into the persistent override set at unload (review C1), so a
    break against a never-generated record is never silently lost.
  - `Unbreakable` — BEDROCK / fluids.
  - `UnknownTerrain` — cell unloaded / not generated (never inferred).
  - `WorldRejected` — nothing to break (AIR / out of bounds) or the world
    refused the edit.
  - `IdExhausted` (stabilization PR3) — the entity-id allocator reached its
    representable bound: the break is refused **before** the world edit so
    the registry drop is never silently lost (the block stays). Blocks with
    an empty drop-table entry need no entity and stay breakable.
  - `InvalidPlayer` (issue #247) — the actor-aware form's handle is unknown /
    retired; refused **without** editing the world or spawning a drop.
- **`placeSelectedBlock(cell)`** → `PlaceResult`:
  - `Ok` — placed; **consumed exactly 1 item** (from the actor's inventory).
  - `EmptySlot` / `NotPlaceable` — selected stack empty / item has no
    registry `placeableBlock` (every v1 catalog item is placeable).
  - `PlayerOverlap` — `canPlaceVoxel` refused: a solid block overlapping
    **any walking player body** (issue #209, generalized to all sessions by
    #247; flying players exempt).
  - `UnknownTerrain` / `WorldRejected` — target unloaded / world refused.
  - `InvalidPlayer` (issue #247) — unknown / retired actor; refused without
    consuming inventory or editing the world.
- **Edit publication**: accepted gameplay edits move out a `WorldEditResult`
  (`outEdit` parameter) so the graphical host publishes through
  `RenderWorldBridge::publishEditResult` — the **same** event drain +
  commit-group path the creative edits use. One publication path, no second
  edit path. Headless callers pass `nullptr`.
- The creative `placeVoxel`/`deleteVoxel` seams **stay** (flight mode,
  benchmarks, visual fixtures, headless `place`/`delete`): infinite-block
  semantics, no consumption, no drops.

## 5. Dropped-item entity rules

`items::ItemEntitySystem` follows the proven `MobSystem` pattern — plain
structs + owning system, **no ECS** (#132 stays deferred).

- **Fixed step 60 Hz** (own accumulator, `maxSteps = 8` catch-up cap).
  Suspension freezes the population, clears the accumulator and syncs
  previous/current — no replay burst on resume.
- **Physics**: gravity 24 blocks/s², terminal velocity 30, ground friction
  0.5/tick, body size 0.25³ — through `physics::move` on
  `WorldCollisionView` (the existing solver; **no new physics**).
- **Spawn**:
  - break drops: cell center, upward pop 2.0 + horizontal scatter 0.8,
    pickup delay **10 ticks**; jitter derives only from the entity id / edit
    revision (never wall clock, never time-seeded RNG).
  - manual drops (`dropFromSlot`): player eye, host direction × speed 3.0,
    pickup delay **40 ticks** (no instant re-pickup).
- **Auto-pickup is a deterministic tick rule**: after the delay countdown,
  any entity within **1.5 blocks** (3D distance to a player feet) merges
  into the inventory **hotbar-first** (existing same-item stacks, then empty
  slots). **Partial pickup** leaves the remainder in the world. Not a
  command — headless scripts exercise it through `wait`.
  - **Multi-player (issue #247):** every **positioned** live session is an
    eligible receiver, evaluated in **ascending-`PlayerId` order** (the
    deterministic priority). The first in-range player merges; a **partial
    remainder is offered to the next eligible player in the same tick**; a full
    inventory skips to the next. The **aggregate count is conserved** (no
    duplication, no loss). Unpositioned sessions (created but not yet
    spawned/placed) are excluded, so a default-origin body never vacuum-collects
    drops. Solo (one positioned session) is bit-identical to the historical
    local-only rule.
- **Despawn**: age-based, **18000 ticks** (5 minutes of sim time).
- **Population cap**: **1024** entities; overflow despawns **oldest-first**
  (id tie-break) — deterministic.
- **Id allocator**: simulation-owned, monotonic, starts at 1, **never
  reuses ids within a session**; the **high-water mark is persisted**
  (`items.state`) so ids are never reused **across sessions** either.
  **Exhaustion guard (stabilization PR3):** id 0 is permanently invalid;
  the largest assignable id is `kMaxEntityId = UINT64_MAX - 1`, so the
  persisted NEXT id (`high-water + 1`) stays representable and valid at
  `UINT64_MAX`. The allocator **never wraps**: once it rests at
  `kMaxNextEntityId`, spawns answer id 0 without touching the population
  (no overflow despawn on a refused spawn), `breakBlock` answers
  `IdExhausted` before editing the world, and `dropFromSlot` answers
  `IdExhausted` before consuming inventory. Restoring the same world never
  resets the allocator; the exhausted state survives save/reload.
- **Snapshots**: bounded, **id-sorted** `ItemEntitySnapshot` (previous /
  current positions for interpolation, item + count, ageTicks), filled in
  place like `mobSnapshots`. Interpolate with `itemInterpolationAlpha()`.
- **Events**: bounded spawn/remove stream (`ItemEntityEvent`, the
  `WorldEvent` drain pattern). `Removed` reasons: `PickedUp` (canonical
  empty stack — the items moved into the inventory), `Aged` / `Overflow`
  (the lost remainder). Partial pickups are **not** events — the snapshot
  counts carry them.

## 6. Persistence (issue #180 contracts preserved)

Gameplay state is captured/restored on the **same lifecycle seams** as
`player.state`: `savePlayerState` is the FULL gameplay capture (player +
inventory + items, committed as one checkpoint AFTER the flush barrier —
stabilization PR5); `finalizePlayerSpawn` restores inventory + entities +
allocator high-water; `initialize`/`reload` reset them; the `shutdown`
safety net captures them too.

| File | Magic | Size | Content |
|---|---|---|---|
| `saves/<world>/inventory.state` | `FTVI` | 153 bytes | formatVersion u32, selectedSlot u8, then 36 × (itemId u16 + count u16) in slot order |
| `saves/<world>/items.state` | `FTVJ` | 20 + 68×n bytes | formatVersion u32, **nextEntityId u64** (high-water + 1), entityCount u32, then n id-sorted records: id u64, itemId u16, count u16, pos 3×f64, vel 3×f64, ageTicks u32, pickupDelayTicks u32 |

The exact `WorldSave` discipline applies: little-endian `ByteWriter`/
`ByteReader`, magic + format version, **strict validation** (exact size,
registry-known items, count ≤ maxStack, `count == 0 ↔ itemId == 0`,
strictly ascending unique ids with `id < nextEntityId`, finite positions and
velocities), **atomic tmp+rename**, missing file = fresh world (quiet),
malformed = **loud error** (degrades to empty with a logged diagnostic, never
silently clamped).

Deliberately **not persisted**: `grounded`/`waitingForTerrain` (solver
transients) and `previous` (interpolation). Restore sets
`previous = position`, `grounded = false`; `physics::move` re-derives the
ground state on the first tick (a negligible, deterministic transient).

**Old saves load unchanged**: `player.state` v1 and `world.meta` are
untouched; missing new files mean empty inventory / no items. No migration
machinery in v1. Refusable close, stranded-edit semantics and the flush
durability barrier are unchanged and cover the new files.

**Transactional checkpoints (stabilization PR5, Phase 3)**: the multi-file
gameplay state is committed as ONE generation-based checkpoint
(`saves/<world>/checkpoints/generation-<N>/` + an atomic `manifest`), per
[`docs/adr/gameplay-persistence-transactions.md`](adr/gameplay-persistence-transactions.md).
`savePlayerState` runs the flush durability barrier FIRST, then commits the
full capture (player + inventory + items + allocator) as one checkpoint —
**success is reported only when the manifest committed**. The `shutdown`
safety net commits inventory + items (the player payload is carried
forward); `closeWorld`/`forceCloseWorldForShutdown` commit a carry-forward
checkpoint only after a COMPLETE barrier — the async drain AND the chunk
flush both succeeded (a failed drain/flush skips the commit with a distinct
diagnostic; the previous committed generation stays authoritative).
`flushWorld`/`WorldPersistence::flush` keep their historical meaning: an
intermediate chunk-durability barrier that does NOT commit a gameplay
checkpoint. **Guarantee boundary**: transactional recovery begins at the
first successful checkpoint commit; a legacy world whose first publication
fails recovers through the legacy per-file policy (additive, non-destructive,
but NOT a transactional snapshot — see the ADR §2.7). On open, recovery selects the most recent
fully valid generation (manifest → `manifest.prev` fallback), re-seeds the
live working set from it (uncommitted pre-crash edits are discarded, never
resurrected by a later commit), and routes gameplay reads to it. A corrupt
generation falls back to the previous complete one; when none validates the
open is refused with an explicit unrecoverable error — generations are never
mixed. The root `player.state`/`inventory.state`/`items.state` files and the
live `chunks/` set are maintained as **legacy compatibility shims** (old
binaries and pre-migration saves keep working; migration is additive and
non-destructive). Process-crash consistency is guaranteed by atomic renames;
power-loss durability is best-effort (file/dir sync, see the ADR §2.9).

**Deferred-edit durability (stabilization PR2)**: logical acceptance is the
authoritative gameplay event boundary — `breakBlock` spawns its drop when
the world ACCEPTS the deletion, and the effective-voxel overlay makes
physics/targeting/re-break refusal observe it immediately (see §4).
Accepted edits queued against in-transit / UNLOADED records are durable at
every durability seam: `flushWorld()` bakes their canonical pending entries
into the persistent override set (without disturbing the issue #114
publication ordering — each edit still lands exactly once when its record
becomes editable), unload bakes them (review C1), and close/shutdown
resolve them before the final capture. A mid-transit flush therefore never
persists a pre-edit chunk alongside post-edit gameplay state. Mirror
(border-shell) entries carry no independent durable content — they are
rebuilt from resident neighbors on reload.

## 7. Presentation boundary (derived only)

- **Hotbar HUD + inventory panel** (`GameUIInventory.cpp`, issue #229):
  always-on 9-slot HUD + `E` panel (27 + 9 grid). **Snapshot-only displays**
  issuing command callbacks (`GameUIFrame` pattern). Left-click move/swap,
  right-click half-split; the **drag cursor is UI-local presentation**,
  discarded on close (no duplication — authoritative moves happen only
  through commands). Item icons are per-atlas-layer ImGui descriptors over
  the block atlas (no new assets), dropped eagerly on resource-pack apply
  (device idle) and revalidated against the atlas version before next use
  (the lazy version-mismatch check remains as a safety net). Pure
  layout/cursor/click logic lives in `InventoryUi.hpp`
  (headless-testable, `test_ui_shell`).
- **`ItemEntityRenderer`** (issue #229): one shared atlas-textured unit cube,
  one instanced indexed draw for the frustum-culled visible population
  (bounded by the sim cap). Previous/current interpolation is done
  **host-side**; per-instance atlas layer + derived local light. Receives
  directional shadows, casts none (v1). Recorded in the opaque pass after
  mobs. Holds **no authoritative state**; reset by the world-epoch barrier
  (`resetWorldProjection`) on reload.
- The visual harness draws no ImGui and the reference scenes contain no
  items → **goldens hold** (the dedicated `item_entities` scene covers the
  renderer).

## 8. Headless integration (issue #230)

The loop is fully scriptable in `ft_vox_headless` — see
[`headless-runner.md`](headless-runner.md) §4–§5 for the authoritative
grammar and digest reference:

- Ops: `give`, `select`, `drop` (fixed +Z direction — no camera), `break`,
  `placeSelected`, `expect inventory`, `expect items`; auto-pickup via
  `wait`. Bounds validated at parse (exit 3); assertion mismatches exit 5.
- The canonical digest (`FTVHDIG2`) covers the selected hotbar slot, the
  36-slot inventory, the id-sorted item population (quantized motion + age)
  and the allocator high-water.
- SUMMARY carries `items=`, `inventory=`, `item_high_water=` (key=value and
  `--json`).
- The runner advances `advanceItems(dt, false)` after `advanceMobs` each
  frame; sync (`--workers 0`) and async (`--workers N`) runs produce
  **identical digests with items in flight**.
- The acceptance driver (`tests/test_headless_runner.py`) runs the gameplay
  fixture end-to-end and a **3-process save/reload round-trip** restoring
  inventory + entities + allocator (no id reuse).

## 9. Replication-readiness rules

**Design rules, not networking code** (epic §6). A future authoritative
server/client split inherits:

1. **Stable append-only item ids** (`ItemId`) — wire/save stable, never
   `TextureType`; the registry is the single vocabulary source.
2. **Persisted entity-id allocator** — monotonic, high-water durable, no id
   reuse across sessions; ids are the replication identity.
3. **Plain-value serializable state** — the same `ByteWriter`/`ByteReader`
   discipline as the save formats; no pointers, no padding-dependent
   layouts, no heap-owning authoritative types.
4. **Command/result vocabulary** — every mutation is an explicit command
   with a plain-value argument list and an explicit result enum (§3–§4);
   the server reuses the codes as rejection reasons. No host-side gameplay.
5. **Id-sorted bounded snapshots** — deterministic ordering, bounded size
   (population cap), previous/current interpolation kept presentation-side.
6. **Deterministic fixed-step rules** — 120 Hz player / 60 Hz mobs / 60 Hz
   items, bounded catch-up, suspension semantics; jitter derives only from
   ids/edit revisions; no wall clock, no time-seeded RNG.
7. **One edit publication path** — gameplay and creative edits share the
   world edit semantics + event drain + commit groups (issue #114 ordering).

Epic #245 owns transport, prediction/reconciliation and the client/server
split. Its CPU-only host and UDP session layer are documented in the
[dedicated server guide](dedicated-server.md).

## 10. Epic #223 definition of done (closed by #230)

- [x] The full loop is playable in-game: break → item entity → auto-pickup →
      hotbar → place consumes; the inventory panel manages stacks; everything
      survives save / close / reopen (#226–#229, in-game validation).
- [x] The same loop is scriptable in `ft_vox_headless` with identical results
      sync/async; the canonical digest covers inventory + item entities; a
      multi-process save/reload round-trip passes (#230,
      `tests/test_headless_runner.py`, 72 checks).
- [x] No gameplay logic remains in `Engine` / `RenderWorldBridge` / `GameUI`;
      every interaction is an explicit simulation command with a result code
      (#227 host rewiring; `CoreIncludeGuard` / `HeadlessDeps` guards).
- [x] Item ids and entity ids are stable across save/reload; the allocator
      high-water mark is persisted; snapshots are id-sorted and bounded
      (#224/#226/#228).
- [x] The CPU-only build (`FT_VOX_GRAPHICS=OFF`) builds and passes with the
      new modules; `CoreIncludeGuard` / `HeadlessDeps` / link-closure gates
      green (#230 closure record: `benchmarks/issue-230-closure.md`).
- [x] Full CTest suite + visual goldens hold; docs updated
      (`docs/gameplay-loop.md`, simulation-render contracts, headless runner,
      engine architecture, `AGENTS.md`).
