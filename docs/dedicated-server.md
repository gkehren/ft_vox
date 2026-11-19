# Dedicated server (`ft_vox_server`)

Issue #250 adds the standalone, CPU-only authority host for Epic #245 Phase B1.
The process owns one `GameSimulation` and one persistent world. The listening
address, port and player cap are configuration for the future session layer.
The startup record reports `transport=disabled` until that layer is attached.
The [authoritative wire protocol ADR](adr/authoritative-wire-protocol.md)
defines the codec and delivery contract for that integration.

## Build and run

```sh
cmake -S . -B build-cpu -DFT_VOX_GRAPHICS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpu --config Release --target ft_vox_server
./build-cpu/ft_vox_server --world dev --saves-root ./server-saves --seed 42 --ticks 120
./build-cpu/ft_vox_server --world dev --saves-root ./server-saves --stop-file ./server.stop
```

On Windows with a Visual Studio generator use
`.\build-cpu\Release\ft_vox_server.exe`. The executable is also built when
`FT_VOX_GRAPHICS=ON`. It links to `ft_vox_core`. The dependency checks cover its
include closure and generated link inputs.

Use one process per save directory. A new world defaults to seed 42. Omit
`--seed` on restart to use the stored seed. An explicit seed mismatch, an
invalid world name, or a save that the existing persistence layer cannot open
prevents startup.

| Option | Default and behavior |
|---|---|
| `--world NAME` | Required. Opens or creates the named world. |
| `--saves-root PATH` | `saves`. World data lives under `PATH/NAME`. |
| `--seed N` | Stored seed or 42. An explicit seed must be positive. |
| `--listen-address ADDRESS`, `--port N` | `127.0.0.1`, `7777`. Port range 1–65535. |
| `--player-cap N` | 16 sessions, range 1–256 including the local compatibility anchor. |
| `--ticks N` | Omit for live mode. Supply 1–1,000,000 for an unpaced, deterministic run. |
| `--max-catch-up N` | 4 ticks per live scheduling batch, range 1–8. |
| `--workers N` | 2 terrain workers, range 0–64. Zero generates synchronously. |
| `--interest-radius N` | 3 chunks around the local anchor, range 0–8. Zero disables its interest. |
| `--load-budget N` | 32 records per tick, range 0–128. |
| `--generation-budget N` | 8 starts per tick, range 0–64. |
| `--eviction-budget N` | 16 evictions per tick, range 0–64. |
| `--metrics-every N` | Report every 600 completed ticks. Range 0–1,000,000. Zero disables periodic records. |
| `--stop-file PATH` | Optional admin stop marker. The owner stops at the next tick boundary when the path exists. Remove it before the next start. |

## Tick ownership and pacing

The main thread opens the save, synchronously bootstraps terrain, restores
the local compatibility player and shared item state, then activates
player-driven residency. The local anchor provides the initial streaming
and mob observer. Each 60 Hz server tick runs two 120 Hz player physics
steps for walking players, advances the shared streaming union, advances mobs
and items once each at 60 Hz, and publishes terrain and deferred edits.
Only the owner calls mutable simulation APIs. Terrain workers prepare
results for owner publication. The save worker serializes immutable write
requests.

Live mode uses steady-clock deadlines measured from the start of the loop.
After a stall, the scheduler runs up to `--max-catch-up` due ticks and records
the excess as `dropped_ticks`. Every executed tick keeps the same simulation
delta. A process suspension can therefore slow simulated time relative to
wall time without replaying an unbounded backlog. Core streaming budgets and
the 64-job outstanding generation cap provide terrain backpressure.

An unpaced `--ticks N` run calls the same fixed step N times and publishes all
dispatched terrain before advancing mobs and items in that tick. This makes
the exercised state independent of worker scheduling. Admin and signal stop
requests can end either mode early. The owner finishes the current tick
before handling a stop.

## Shutdown and recovery

On Linux and other POSIX systems, SIGINT and SIGTERM request a graceful stop.
On Windows, use Ctrl+C or Ctrl+Break in the server console, or the stop
file. The handler only sets a flag. The owner stops ticking, drains
outstanding generation with a 30-second cooperative deadline, resolves
accepted edits still deferred on unloaded records, and runs the existing
flush, gameplay checkpoint commit and refusable close. If the explicit path
fails, teardown runs the core's existing safety net and the process still
reports the failure. Terrain jobs complete before teardown releases their
world state, and the owner then joins the pool. Filesystem calls and worker
joins rely on the OS to return.

The checkpoint schema is shared with existing solo saves. The active
checkpoint carries the local actor's state and inventory, shared voxel
overrides, and dropped items with their allocator high-water. The server
preserves the restored local orientation. Existing checkpoint recovery and
refusal rules apply on restart. Rolling back to a solo build uses the same
save directory directly.

| Exit | Meaning |
|---|---|
| 0 | Tick limit, admin or signal stop completed with a committed save and clean close. `--help` also exits 0. |
| 2 | Invalid command-line configuration. |
| 3 | World identity or save open refused. |
| 4 | Runtime failure. A started world still attempts the save barrier. |
| 5 | The stop barrier, checkpoint commit or close failed. Inspect the persistence diagnostics. |

Stdout emits `SERVER` records with `status=ready`, periodic `status=metrics`,
`status=stopping` and `status=stopped`. Metrics include completed and dropped
ticks, world epoch, next logical edit ID, population, chunk backlog and
checkpoint generation. The stopped record shows the last snapshot taken
before final close, with the stop reason, `saved` flag and exit code.
Errors are emitted to stderr with a phase and diagnostic.

Run the dedicated tests with `ctest --test-dir build-cpu -R 'ServerHost|ServerLifecycle' --output-on-failure`
(add `-C Release` for Visual Studio). They cover clock stalls and cadence,
sync/async fixed steps, deferred edits at shutdown, saved-world restart,
write/commit failures, and signal/admin termination.
