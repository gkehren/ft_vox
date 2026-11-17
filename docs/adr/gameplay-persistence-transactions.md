# ADR: Generation-based gameplay persistence checkpoints

- **Status:** accepted (stabilization pass, post-Epic #204/#223)
- **Scope:** `src/World/WorldSave.*`, `src/World/WorldPersistence.*`,
  `src/World/VoxelWorld.*`, `src/Sim/GameSimulation.*`
- **Supersedes nothing:** the issue #180 per-file atomic-write contracts and
  the desired/durable save-revision state machine stay in force. This ADR
  adds a *multi-file transactional layer* on top of them.

## 1. Problem

A persistent world today is a collection of independently committed files:

```text
saves/<world>/
  world.meta                  identity (written once at creation)
  chunks/<cx>_<cz>.chunk      sparse voxel overrides (async SaveService)
  player.state                feet + orientation + flight + creative selection
  inventory.state             36 stacks + hotbar selection      (issue #228)
  items.state                 dropped items + id allocator      (issue #228)
```

Every file is individually crash-safe (tmp + atomic rename), but the
**collection is not committed transactionally**. `savePlayerState()` writes
player → inventory → items → flushes chunk edits sequentially. A process
crash (or a failed write) between any two steps leaves a mixture of
generations: e.g. a new `items.state` (drop spawned) with an old chunk file
(block still present) — on reopen the player can break the resurrected
block a second time and **duplicate the drop**. The same class of mixture
exists between chunk files themselves when several coordinates are flushed.

The stabilization mission requires: one logically coherent checkpoint,
recovery to the most recent fully committed checkpoint, no mixed-generation
state, no mutation of the previous committed checkpoint before the next
commit succeeds, retryable failures, legacy compatibility, no full-world
serialization per save, and bounded memory/disk overhead.

## 2. Decision

Introduce **generation-based checkpoints with an atomic commit manifest**,
staged copy-on-write from the existing durable working set. The existing
async `SaveService` and the per-coordinate desired/durable state machine are
**unchanged**: they keep maintaining the live working set
(`chunks/` + root state files). A checkpoint is an immutable snapshot of
that working set plus the gameplay payloads, published by a single atomic
rename.

### 2.1 Layout

```text
saves/<world>/
  world.meta                     unchanged identity
  chunks/                        LIVE working set (SaveService, unchanged)
  player.state                   legacy compatibility shim (still written)
  inventory.state                legacy compatibility shim (still written)
  items.state                    legacy compatibility shim (still written)
  checkpoints/
    manifest                     the commit point (atomic tmp+rename)
    manifest.prev                rotated previous manifest (fault tolerance)
    generation-<N>/              immutable once committed
      player.state               (optional — presence flagged in manifest)
      inventory.state
      items.state
      chunks/<cx>_<cz>.chunk     byte copies of the live override files
```

`generation-<N>` directories are **immutable after commit**: nothing ever
writes into a committed generation again. The live `chunks/` directory is
the only place the async worker mutates files, and it is *not* referenced by
any manifest — a background write can therefore never alter a committed
checkpoint's content.

### 2.2 Manifest format

Binary, little-endian, the existing `ByteWriter`/`ByteReader` discipline
(no raw struct dumps), magic `FTVM`, format version 1:

| field | type | meaning |
|---|---|---|
| magic | 4 bytes | `FTVM` |
| formatVersion | u32 | 1 |
| committedGeneration | u64 | N — the authoritative generation |
| fallbackGeneration | u64 | N-1 retained for recovery (0 = none) |
| worldEditRevision | u64 | `VoxelWorld::nextEditId()` at the boundary (diagnostic) |
| flags | u8 | bit 0: playerPresent |
| playerChecksum | u64 | FNV-1a 64 of `player.state` bytes (0 when absent) |
| inventoryChecksum | u64 | FNV-1a 64 of `inventory.state` bytes |
| itemsChecksum | u64 | FNV-1a 64 of `items.state` bytes |
| chunkCount | u32 | number of referenced chunk files |
| per chunk | i32 x, i32 z, u64 checksum (16 bytes) | FNV-1a 64 of the whole chunk file |

Total manifest size: 61 + 16 × chunkCount bytes (exact-size validation).

The manifest references a **consistent collection**: the gameplay payloads
and every chunk override file carry checksums, and the chunk set is exactly
the durable live set at the flush barrier. A generation whose referenced
payload is missing or checksum-mismatched is **invalid as a whole** — we
never combine a valid payload from one generation with a chunk from another.

### 2.3 Commit protocol (main thread, at a durability boundary)

Precondition: the caller has established the snapshot boundary and flushed
the working set (`SaveService::flush()` barrier returned true — every
desired revision is durable, no worker is mid-write). All steps run on the
main thread; the world simulation lock is **not** held during filesystem
work (the boundary capture already happened under it).

1. **Capture** the gameplay payloads as plain values (player when the host
   supplies orientation; inventory; items + allocator high-water). This is
   the existing `capture*State()` seam — a bounded in-memory copy.
2. **Resolve accepted deferred edits** per the existing durability rules:
   `flushWorld()` captures every dirty record and (since stabilization PR2)
   bakes canonical pending edits queued against in-transit/UNLOADED records
   into the override set; close/shutdown additionally drain and resolve
   stranded edits first. The checkpoint inherits exactly these guarantees.
3. **Stage** into a fresh `checkpoints/generation-<N>/` (any orphaned
   staging directory for N is removed first):
   - write the gameplay payloads (atomic write helper),
   - copy every live `chunks/*.chunk` file byte-for-byte into
     `generation-<N>/chunks/` (copy-on-write: only the small sparse override
     files are copied — **no terrain regeneration, no full-world
     serialization**; unchanged chunks cost one file copy each),
   - record checksums while copying.
4. **Validate** the staged payloads: re-read + strict-decode each gameplay
   file; `readChunkFile` every staged chunk (magic/version/checksum/
   coordinate validation). Any failure aborts the commit.
5. **Rotate** `manifest` → `manifest.prev` (byte copy; the old committed
   manifest stays valid until the new one is published).
6. **Publish**: write `manifest.tmp`, sync it, atomically rename over
   `manifest`, sync the directory. **The rename is the single commit point.**
7. **Mark committed** in memory (active generation = N; gameplay reads route
   to `generation-<N>/`).
8. **Cleanup** generations older than N-1 (retention = current + previous).
   Cleanup is best-effort: a failure is logged and retried at the next
   commit/open; it can never invalidate the committed state.

Failure at any step before 6 returns false, leaves the previous committed
checkpoint untouched, and is **retryable** (the next save re-stages). The
orphaned staging directory is ignored by recovery and removed on the next
attempt or at open.

### 2.4 Recovery protocol (at `openOrCreate`)

1. Open/validate `world.meta` exactly as today; clean live `.tmp` leftovers.
2. If `checkpoints/` is absent or empty → **legacy mode** (scan live
   `chunks/`, root state files) — byte-for-byte the historical behavior.
3. Otherwise select a generation, in order:
   a. `manifest` → `committedGeneration`;
   b. its `fallbackGeneration` (if the committed one fails validation);
   c. `manifest.prev` → its `committedGeneration` (manifest-level fault
      tolerance: a crash during rotation/publish never loses both copies).
   Each candidate is validated as a whole (payload codecs + every referenced
   chunk checksum). The first fully valid candidate wins.
4. The winning generation's `chunks/` becomes the **durable baseline** of
   the per-coordinate state machine (instead of the live scan), and
   `readPlayerState/readInventoryState/readItemsState` route to it. The live
   working set is re-seeded from it (the SaveService keeps writing live
   files; the next checkpoint copies them again).
5. **Never combine generations.** If no candidate validates and generation
   directories exist, the open is **refused with an explicit unrecoverable-
   corruption error** — falling back to the live working set would mix
   generations (it may be ahead of the last commit). The user is told to
   restore a backup; nothing is deleted.
6. Orphaned staging directories (not referenced by the winning manifest)
   are removed after selection. A missing `player.state` inside a valid
   generation (playerPresent = 0) is the normal "no explicit save yet" case:
   quiet fresh-spawn, exactly the legacy missing-file policy.

Per-chunk corruption *inside* an otherwise referenced generation invalidates
that generation (step 3) rather than silently skipping the chunk: a
checkpoint is all-or-nothing. (The legacy loud-skip policy still applies to
legacy-mode saves, which have no manifest to validate against.)

### 2.5 Commit triggers and lifecycle contracts

| Seam | Behavior |
|---|---|
| `GameSimulation::savePlayerState(yaw, pitch)` | full capture → `flushWorld()` → **commit checkpoint (with player)** → best-effort legacy root writes. Returns false when the flush or the commit failed; **never reports success without a committed manifest**. |
| `GameSimulation::shutdown()` safety net | capture inventory + items (no host orientation) → unconditional close (resolve/drain/capture/flush) → **commit checkpoint carrying the previous player payload forward** (playerPresent reflects whether one exists). |
| `VoxelWorld::closeWorld()` | drain → capture → flush (unchanged, retryable) → **carry-forward checkpoint** so chunk edits accepted after the last gameplay save are committed at close. A refused close commits nothing. |
| `VoxelWorld::forceCloseWorldForShutdown()` | resolve → drain → capture → flush → shutdown (unchanged) → carry-forward checkpoint on success. |
| `VoxelWorld::flushWorld()` / `WorldPersistence::flush()` | **unchanged meaning**: an intermediate chunk-durability barrier only. It does NOT commit a gameplay checkpoint (documented explicitly: the recovery point stays the last committed checkpoint until the next full capture). |

Carry-forward = re-stage the payload bytes currently authoritative for
recovery (active generation, else legacy root files). A world that never
saved gameplay commits the canonical fresh payloads (empty inventory, no
items, no player) — consistent with what a legacy reopen would restore.

### 2.6 Concurrent asynchronous chunk writes

The `SaveService` worker only ever writes the **live** `chunks/` directory.
Checkpoint staging runs on the main thread *after* a successful flush
barrier, when the worker queue is drained and no capture can race (captures
are main-thread). Committed generations are immutable and never referenced
by the worker. Therefore:

- a background write cannot mutate a committed checkpoint's files;
- a crash during async I/O leaves the live set partially ahead — recovery
  ignores it and uses the last committed generation;
- the desired/durable revision machine, coalescing, supersede checks and
  barrier tickets are untouched (no second save path).

### 2.7 Legacy compatibility, migration, rollback

- **Old saves open unchanged**: no `checkpoints/` → legacy mode, identical
  to today (including the loud-skip corrupt-chunk policy).
- **Migration is additive and non-destructive**: the first commit creates
  `checkpoints/` alongside the untouched legacy files. A failed migration
  (staging/commit failure) destroys nothing — the legacy files remain.
- **Rollback**: an older binary ignores `checkpoints/` and keeps reading the
  live `chunks/` + root state files, which the new code **continues to
  maintain** as a compatibility shim. At most the last uncommitted
  checkpoint is lost on rollback — the same exposure as a legacy crash.
- New binaries reading a legacy save migrate lazily at the first commit;
  there is no bulk conversion step.

### 2.8 Retention, disk-space amplification, memory

- Retention: **current + previous** committed generation (2). Cleanup after
  commit and at open removes older ones (best-effort, logged).
- Disk: ≤ 2 × (total sparse override bytes + ≤ 70 KiB items + 218 B
  player/inventory) + one staging generation during a commit + the live
  working set. Override files are sparse (5 bytes/edited voxel + 28 B
  header); a heavily edited world with E total override bytes amplifies to
  ~3E + staging, **independent of world size** (no terrain is ever
  serialized). Measured in the fault-injection/benchmark tests.
- Memory: staging holds one payload set + one file-copy buffer (bounded by
  the largest chunk file); no per-voxel world copy. The capture itself is
  the existing bounded value copy (36 stacks + ≤ 1024 item records).

### 2.9 Durability: process crash vs power loss

- **Process-crash consistency is guaranteed** by the protocol: every
  individual file uses tmp + atomic rename (POSIX `rename`, Windows
  `MoveFileEx(MOVEFILE_REPLACE_EXISTING)`), and the manifest rename is the
  single linearization point. A crashed process leaves either the previous
  complete checkpoint or the new complete checkpoint.
- **Power-loss durability is best-effort, platform-dependent**: the commit
  path additionally syncs the manifest file and its directory before the
  rename (`fsync` + directory `fsync` on POSIX; `FlushFileBuffers` on the
  file on Windows — Windows directory-handle flushing is not reliably
  supported across filesystems). Staged payload/chunk copies are synced
  before publication on both platforms. We do **not** claim full power-loss
  durability on Windows (no directory sync guarantee) or on filesystems
  without ordered metadata journaling; the ADR's guarantee is
  process-crash consistency, with power-loss behavior documented per
  platform. The live `SaveService` writes keep their historical (non-fsync)
  behavior — only the checkpoint commit path adds syncs.

### 2.10 Windows/Linux portability

- All filesystem work uses `std::filesystem` + the existing atomic-write
  helper (already proven on MSVC/libstdc++/Windows).
- File copies use byte streams (no hardlinks/symlinks: unavailable on
  FAT/exFAT and Windows without privileges).
- Sync helpers are `#ifdef _WIN32` (`CreateFileW` + `FlushFileBuffers`) vs
  POSIX (`open` + `fsync`); directory sync is POSIX-only, documented above.
- Generation directory names are plain decimal (`generation-<N>`), safe on
  every supported filesystem; world-name validation is unchanged.

## 3. Fault-injection contract (tests)

Deterministic seams (the `SaveService::setWriteTmpFnForTests` pattern) fail
the commit at exactly these points; each test verifies recovery yields
either the previous complete state or the new complete state, never a
mixture:

1. before staging begins;
2. after the first gameplay payload (player) is staged;
3. after inventory staging;
4. after item-entity staging;
5. during chunk serialization/copy;
6. during asynchronous chunk I/O (SaveService write seam → flush fails);
7. after all payloads staged, before manifest publication;
8. during manifest publication (tmp written, rename not done);
9. immediately after manifest publication (commit succeeded);
10. during obsolete-generation cleanup (commit succeeded).

Points 1–8 must recover the **previous** generation; 9–10 the **new** one.
Legacy-save compatibility, refused-close retryability, and the headless
save/reload round-trips are re-run with checkpoints active.

## 4. Alternatives considered

- **Write-ahead log / journal of operations**: rejected — the save format is
  an override *value* table, not an operation log; a WAL would duplicate the
  desired/durable machine and add replay complexity for no gain at this
  scale.
- **Content-addressed chunk store with refcounts**: rejected for v1 —
  deduplication saves little (override files are tiny), while refcount
  cleanup adds a crash-sensitive garbage-collection problem. Plain
  per-generation copies with 2-generation retention are simpler and bounded.
- **Single monolithic save file**: rejected — violates "no forced full-world
  serialization" and the async per-chunk save architecture; every save
  would rewrite the whole world.
- **Defer item spawns until GPU publication**: rejected by the mission —
  logical acceptance is the authoritative event boundary; the effective
  voxel overlay already guarantees coherent physics/targeting (PR1/PR2).

## 5. Implementation plan (small PRs)

1. **Format + recovery layer** (`WorldCheckpoint.hpp/.cpp`, CPU-only):
   manifest codec, staging, validation, recovery selection, cleanup, sync
   helpers, fault-injection seams + unit tests (`test_world_checkpoint`).
   No runtime integration yet.
2. **Runtime integration**: `WorldPersistence` owns the checkpoint layer;
   `openOrCreate` recovery routing; `savePlayerState`/`shutdown`/`closeWorld`
   commit triggers; legacy shim writes; lifecycle + fault-injection +
   legacy-compatibility tests; headless round-trips; overhead measurement.

Each step is independently reviewable and testable; the phase is not
complete until chunk edits are inside the checkpoint (scope guard).
