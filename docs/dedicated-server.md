# Dedicated server (`ft_vox_server`)

The standalone CPU-only authority host (#250) owns one `GameSimulation` and
one persistent world. Live mode listens for v2 UDP sessions (#252) and
simulates remote movement from player inputs (#253).
The [authoritative wire protocol ADR](adr/authoritative-wire-protocol.md)
defines framing, negotiation and delivery.

## Build and run

```sh
cmake -S . -B build-cpu -DFT_VOX_GRAPHICS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpu --config Release --target ft_vox_server
./build-cpu/ft_vox_server --world dev --saves-root ./server-saves --seed 42 --ticks 120
./build-cpu/ft_vox_server --world dev --saves-root ./server-saves --port 0 --stop-file ./server.stop
```

On Windows with a Visual Studio generator use
`.\build-cpu\Release\ft_vox_server.exe`. The executable is also built when
`FT_VOX_GRAPHICS=ON`. It links to `ft_vox_core` and the v2 UDP adapter.
The adapter uses Winsock and BCrypt on Windows and POSIX sockets and OS
entropy on Unix. The legacy Boost prototype remains a separate test target.
The server's dependency checks cover its include closure and generated link inputs.

Use one process per save directory. A new world defaults to seed 42. Omit
`--seed` on restart to use the stored seed. An explicit seed mismatch, an
invalid world name, or a save that the existing persistence layer cannot open
prevents startup.

| Option | Default and behavior |
|---|---|
| `--world NAME` | Required. Opens or creates the named world. |
| `--saves-root PATH` | `saves`. World data lives under `PATH/NAME`. |
| `--seed N` | Stored seed or 42. An explicit seed must be positive. |
| `--listen-address ADDRESS`, `--port N` | `127.0.0.1`, `7777`. Live mode accepts numeric IPv4 and ports 0–65535. Port 0 asks the OS to choose a port. |
| `--player-cap N` | 16 sessions, range 1–256 including the local compatibility anchor. |
| `--ticks N` | Omit for live UDP mode. Supply 1–1,000,000 for an unpaced, deterministic run with sockets disabled. |
| `--max-catch-up N` | 4 ticks per live scheduling batch, range 1–8. |
| `--workers N` | 2 terrain workers, range 0–64. Zero generates synchronously. |
| `--interest-radius N` | 3 chunks around each positioned player, range 0–8. Zero disables player-driven residency. |
| `--load-budget N` | 32 records per tick, range 0–128. |
| `--generation-budget N` | 8 starts per tick, range 0–64. |
| `--eviction-budget N` | 16 evictions per tick, range 0–64. |
| `--metrics-every N` | Report every 600 completed ticks. Range 0–1,000,000. Zero disables periodic records. |
| `--stop-file PATH` | Optional admin stop marker. The owner stops at the next tick boundary when the path exists. Remove it before the next start. |

## Tick ownership and pacing

The main thread opens the save, synchronously bootstraps terrain, restores
the local compatibility player and shared item state, then activates
player-driven residency. The local anchor provides the initial streaming
and mob observer. Each 60 Hz server tick advances walking players in ascending
`PlayerId` order. Each player runs two 120 Hz steps through the shared
`WorldCollisionView`, including the usual collision and unknown-terrain
behavior. The host then advances the shared streaming union, steps mobs and
items once each at 60 Hz, and publishes terrain and deferred edits.
Only the owner calls mutable simulation APIs. Terrain workers prepare
results for owner publication. The save worker serializes immutable write
requests.

Live mode uses steady-clock deadlines measured from the start of the loop.
After a stall, the scheduler runs up to `--max-catch-up` due ticks and records
the excess as `dropped_ticks`. Every executed tick keeps the same simulation
delta. When the scheduler drops elapsed ticks, the host clears pending
movement in both the owner queues and the UDP mailboxes and suspends the
controllers to clear buffered jump and elapsed time. The next executed tick
resumes at the normal fixed delta. Scope and sequence watermarks stay in
force. Core streaming budgets and the 64-job outstanding generation cap
provide terrain backpressure.

An unpaced `--ticks N` run calls the same fixed step N times and publishes all
dispatched terrain before advancing mobs and items in that tick. This makes
the exercised state independent of worker scheduling. Admin and signal stop
requests can end either mode early. The owner finishes the current tick
before handling a stop.

## UDP sessions

The live readiness record reports `transport=udp`, the bind address and the
actual port. Use numeric IPv4 on a local machine or a trusted LAN. The wire
capability is a 64-bit OS-generated bearer token sent in cleartext and bound
to the endpoint that joined. Anyone who can observe traffic can learn it.
There is no user identity, encryption, or protection against on-path
tampering or network saturation. Keep the default loopback bind for local use.

- A compatible Hello reserves one remote slot. The tick owner creates a
  player and hands back the Welcome metadata. Welcome echoes the join nonce
  and carries the protocol version, player ID, capability, world ID, epoch,
  seed, generator compatibility, tick and edit watermark. Repeating the
  same Hello returns the original Welcome. The process generates a new
  world ID and epoch at startup.
- A valid scoped packet confirms the join. The tick owner then finds a safe
  walking spawn around the bootstrapped center using current terrain, even
  when the saved local player was flying. If no safe loaded column exists,
  the server retires the session and attempts a scoped disconnect. A
  provisional slot has no body in the world and expires after 5 seconds
  without confirmation. Established peers send a heartbeat after
  1 second of outbound inactivity and expire after 10 seconds without newly
  admitted inbound traffic or a progressing ACK. Brief loss uses the same
  session. A replay cannot extend it.
- An ordered Disconnect or Resync retires the slot at a tick boundary.
  If the final control or ACK is lost, the departure falls back to the
  reliable or idle timeout. A later Hello uses a new nonce and gets a new
  capability, generation, and player ID. A differently-nonced Hello cannot
  replace an active session at the same endpoint. Rejoin after leaving or
  waiting out the old lease. Remote player identities are process-local and
  are recreated on join.
- The IO worker validates framing, endpoint, capability, epoch, rate, timing
  and sequence before handing typed input to the simulation owner. Each slot
  has six input slots, a latest snapshot slot and one pending action or result. A full
  reliable slot leaves its receive cursor unchanged. The adapter uses a
  one-frame reliable send window, retries after 150 ms subject to the rate
  budget, and closes after 5 seconds without progress. The maximum remote
  allocation is the player cap minus the local anchor. The IO worker reads
  up to 128 datagrams per pump and drops datagrams over 1,200 bytes. It applies
  v2 token buckets by family, peer and process, plus a fixed 256-bucket IPv4
  discovery table. Addresses that hash to the same discovery bucket share
  that budget.

## Movement and snapshots

An Input contains a bounded world X/Z vector and sprint, jump-edge and swim
flags. The endpoint and session capability select the actor. The UDP gameplay
surface accepts only these controls for movement. Spawn and other
administrative mutations are server-owned, and the stop file is a separate
local control. The server currently returns `Unavailable` for scoped actions.
Shared drops continue stepping at 60 Hz. Pickup stays with the checkpointed
local anchor until remote pickup and inventory replication land in #256.

The host accepts inputs for the six ticks after the latest published
completed tick. The inbox for a given tick closes when the owner drains it
before stepping. Send a fresh sample each tick and aim about three ticks
ahead of the estimated server tick. Inputs use nondecreasing intended ticks.
The fresh sequence cursor rejects duplicates and reordered frames, and the
highest admitted sequence replaces earlier samples for the same pending
tick. The IO and owner each keep a fixed six-slot ring per player. Rate
limits drop excess packets before queuing, and late or far-future packets
cannot advance the fresh receive cursor.

The owner consumes one sample at its intended tick and feeds its command
to the walking controller for that tick. Missing samples use neutral
controls, so normal gravity, braking, and collision still apply. Jump
edges enter the controller once and use its normal jump buffer. A dropped
sample costs one tick of input, and the next available sample uses its own
scheduled tick. Disconnect, timeout and reconnect discard input and
controller state with the old actor.

Every third completed tick, the host publishes a snapshot per active peer.
The snapshot includes its owner and up to 23 other confirmed remote
players within the configured chunk-radius disk. Selection and records are
in ascending `PlayerId` order. Records carry feet, velocity and grounded,
waiting-for-terrain and flight flags. The header carries the session scope
and wire world epoch. The payload carries the completed tick and the
owner's last consumed input sequence, which starts at zero on join. The IO
worker sends fresh snapshots at up to 20 Hz per peer within existing
budgets. New captures replace unsent snapshots, and loss is recovered by
the next complete snapshot.

The CPU-only `ClientTransport::sendInput` sends an input for the current
session. `latestPlayerSnapshot()` and `scope()` expose the latest accepted
state and its epoch. The adapter clears the snapshot on departure.

## Shutdown and recovery

On Linux and other POSIX systems, SIGINT and SIGTERM request a graceful stop.
On Windows, use Ctrl+C or Ctrl+Break in the server console, or the stop
file. The handler only sets a flag. The owner stops ticking, joins the IO
worker, retires remote sessions, then drains
outstanding generation with a 30-second cooperative deadline, resolves
accepted edits still deferred on unloaded records, and runs the existing
flush, gameplay checkpoint commit and refusable close. If the explicit path
fails, teardown runs the core's existing safety net and the process still
reports the failure. Terrain jobs complete before teardown releases their
world state, and the owner then joins the pool. Filesystem calls and worker
joins rely on the OS to return. The UDP worker attempts a final scoped
Disconnect for confirmed sessions where the reliable send window has room.
Other clients recover through their timeout.

The checkpoint schema is shared with existing solo saves. The active
checkpoint carries the local actor's state and inventory, shared voxel
overrides, and dropped items with their allocator high-water. The server
preserves the restored local orientation. Existing checkpoint recovery and
refusal rules apply on restart. Rolling back to a solo build uses the same
save directory directly.

| Exit | Meaning |
|---|---|
| 0 | Tick limit, admin or signal stop completed with a committed save and clean close. `--help` also exits 0. |
| 2 | Invalid command-line configuration, including an invalid live bind address. |
| 3 | World identity or save open refused. |
| 4 | Runtime failure, including a failed UDP bind or IO worker error. A started world still attempts the save barrier. |
| 5 | The stop barrier, checkpoint commit or close failed. Inspect the persistence diagnostics. |

Stdout emits `SERVER` records with `status=ready`, periodic `status=metrics`,
`status=stopping` and `status=stopped`. Metrics include completed and dropped
ticks, world epoch, next logical edit ID, population, chunk backlog and
checkpoint generation. The stopped record shows the last snapshot taken
before final close, with the stop reason, `saved` flag and exit code.
Errors are emitted to stderr with a phase and diagnostic.

Run the dedicated tests with `ctest --test-dir build-cpu -R 'ServerHost|ServerLifecycle|UdpTransport|WireProtocol' --output-on-failure`
(add `-C Release` for Visual Studio). They cover clock stalls and cadence,
sync/async fixed steps, deferred edits at shutdown, saved-world restart,
write/commit failures, signal/admin termination, port 0, occupied ports,
two-client join and rejoin, malformed and oversized packets, rate and queue
limits, and replay. The movement fixture compares two IPv4 loopback players
against local controller scripts at every tick with a wall, a jump, lost
samples, a simulated host stall and reconnect. `UdpTransport` also uses an
injected steady clock for timeout and retransmit decisions. Its threaded
smoke test exercises worker shutdown. CTest returns nonzero on failure.
