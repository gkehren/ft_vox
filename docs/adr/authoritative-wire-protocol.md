# Authoritative wire protocol v5

Accepted for Epic #245 Phase B2 ([#251](https://github.com/gkehren/ft_vox/issues/251)).
The `ft_vox_wire` library implements framing, payload validation, receive
sequencing and chunk assembly. The dedicated server uses it through the
bounded v2 UDP transport added in #252. The
[dedicated server guide](../dedicated-server.md#udp-sessions) describes the
active queues and session lifetimes. Phase C1 (#253) implements Input and
PlayerSnapshot on this framing. Phase C2 (#254) uses ChunkFragment, WorldEdit,
ChunkRemove and Resync for the client voxel projection. Phase C4 (#256) adds
private inventories and fresh item and mob snapshots. Existing v2 layouts
are retained and the new tags use only the fresh lane. The Boost UDP prototype remains isolated in
`test_network` and uses its original v1 framing.

Phase D1 (#257) consumes these snapshots and verified terrain in the
[graphical client](../dedicated-server.md#connect-two-graphical-clients).
Phase D2 (#258) changes the envelope version to **3** and extends
PlayerSnapshot with the owner's movement timers and each player's motion
reset serial. Upgrade the server and graphical clients together. The
other payload layouts and delivery rules carry forward from v2.
Phase E1 (#259) moves the envelope to **4**, adds the player key to Hello
and persists the world and player IDs. See the
[multiplayer checkpoint ADR](multiplayer-checkpoints.md).
Issue #275 moves the envelope to **5** and adds bounded run-length encoding
to chunk baselines. Upgrade the server and graphical clients together.

## Framing

Each UDP datagram carries one frame of at most **1,200 bytes**, including
the 36-byte header. A 1,200-byte UDP payload plus the normal IPv6 and UDP
headers fits within the 1,280-byte IPv6 minimum MTU. The transport must drop
oversized or socket-truncated datagrams. There is no IP-fragmentation
dependency and no generic message fragmentation.

| Offset | Type | Field |
| --- | --- | --- |
| 0 | 4 bytes | ASCII `FTVN` (`46 54 56 4e`) |
| 4 | u16 | Envelope version, 5 |
| 6 | u8 | Message tag |
| 7 | u8 | Flags, zero |
| 8 | u16 | Payload length, 0–1,164, exactly the remaining bytes |
| 10 | u16 | Reserved, zero |
| 12 | u64 | SessionId, scope rules below |
| 20 | u64 | World epoch, scope rules below |
| 28 | u64 | Sequence, lane rules below |

Fields are packed in the listed order. Integers are big-endian. Signed
`i16` and `i32` use two's complement. `f32` and `f64` carry big-endian IEEE 754 binary32
and binary64 bits, with finite values only. Flags and enum values outside
their assigned ranges fail validation. There is no structure padding, host
endianness, implicit alignment or serialization of native C++ layouts.

## Message headers and budgets

C and S denote the client and server. A rate and burst apply per peer per
direction to the corresponding family. The byte maximum includes the header.

| Tag | Name | Sender | Lane | Max bytes | Frames/s | Burst |
| --- | --- | --- | --- | ---: | ---: | ---: |
| 1 | Hello | C | Handshake | 98 | 2 | 2 |
| 2 | Welcome | S | Handshake | 98 | 2 | 2 |
| 3 | Reject | S | Handshake | 49 | 2 | 2 |
| 4 | Input | C | Fresh | 53 | 60 | 8 |
| 5 | PlayerSnapshot | S | Fresh | 1,157 | 20 | 4 |
| 6 | Action | C | Reliable | 80 | 16 | 8 |
| 7 | ActionResult | S | Reliable | 70 | 16 | 8 |
| 8 | Inventory | S | Reliable | 197 | 10 | 4 |
| 9 | WorldEdit | S | Reliable | 70 | 32 | 16 |
| 10 | ChunkFragment | S | Reliable | 1,187 | 60 | 16 |
| 11 | ChunkRemove | S | Reliable | 52 | 16 | 8 |
| 12 | Resync | C or S | Reliable | 37 | 1 | 1 |
| 13 | Ack | C or S | Ack | 44 | 60 | 16 |
| 14 | Heartbeat | C or S | Fresh | 36 | 2 | 2 |
| 15 | Disconnect | C or S | Reliable | 37 | 1 | 1 |
| 16 | ItemSnapshot | S | Fresh | 1,193 | 10 | 3 |
| 17 | MobSnapshot | S | Fresh | 1,179 | 10 | 3 |

Use token buckets with these family bounds and an aggregate per-peer limit
of 128 frames/s and 128 KiB/s, with bursts of 64 frames and 64 KiB. Application
and retransmit frames consume the same outbound budget. Ingress also pays
the aggregate budget before dispatch, including malformed packets and
duplicates. Process-wide per-direction ceilings are 2,048 frames/s and
2 MiB/s, with bursts of 256 frames and 256 KiB. Apply the pre-session bounds
per address with a bounded tracking table under the process-wide ingress
budget. These are ceilings. Hosts can use lower rates, reserve scheduling
capacity for controls/ACKs, and throttle baseline sends under load. B3 owns
the clocks, bucket enforcement, shared queues and fair scheduling.

At the sustained limit of 60 fragments/s, a raw 60-fragment baseline uses
about one second of a client's fragment budget. These worst-case fragment counts assume a
circular interest area on the chunk grid and that every fragment slot goes
to baseline data.

| Interest radius (chunks) | Chunks in the disk | Time at the sustained rate |
| ---: | ---: | ---: |
| 1 | 5 | ~5 s |
| 3 | 29 | ~29 s |
| 8 | 197 | ~197 s |

The estimates exclude the initial burst, competing traffic and retransmits.
With the one-frame send window, round-trip time can also limit delivery.
The [cold chunk benchmark](../benchmarks/275-cold-chunk-delivery.md) records
loopback and impaired delivery measurements.

## Join, identity and versioning

- **Hello** carries `nonce:u64, minVersion:u16, maxVersion:u16, compatibility,
  playerKey:32 bytes`.
  The client picks a nonzero fresh nonce per attempt and reuses it only to
  retransmit that attempt. The inclusive version range must be nonzero and
  ordered. The bearer key has at least one nonzero byte. Hello, Welcome and
  Reject use the v5 envelope for discovery.
- **Welcome** carries `nonce:u64, worldId:u64, playerId:u32, seed:u32,
  generatorVersion:u32, compatibility, tick:u64, nextEditId:u64`. The server
  echoes the nonce, selects v5, and supplies the authorized persistent PlayerId. The seed
  is 1–2,147,483,647. The generator version is nonzero and must match the
  negotiated generator fingerprint. `compatibility` is `itemSchema:u16,
  catalogFingerprint:u64, generatorFingerprint:u64`. The server compares
  exact compatibility after selecting a common version. The client verifies
  the echoed nonce, selected version, generator version and compatibility
  before installing the new session.
- **Reject** carries `nonce:u64, reason:u8, minVersion:u16, maxVersion:u16`.
  Reasons are 1 version, 2 content, 3 full, 4 unavailable, 5 identity in use
  and 6 saved profile limit. With valid
  discovery framing but no common version, reply with reason 1 and range
  `[5,5]`. A foreign magic or envelope version is dropped. It cannot be
  interpreted as discovery or as a gameplay packet.
- Header SessionId, epoch and sequence are zero on Hello and Reject. Welcome
  has a nonzero SessionId and epoch and zero sequence. Repeated Hello
  for the same live endpoint and attempt returns the same Welcome. The
  transport gives a provisional session a deadline for its first valid
  scoped packet. A client can send Heartbeat immediately after Welcome
  to confirm receipt.
- SessionId is a nonzero, unpredictable, server-issued 64-bit capability,
  renewed on every successful join. B3 binds the issuing endpoint and
  SessionId to the server-owned PlayerId. Incoming packets carry the
  capability and epoch. PlayerId is allocated by the world owner. Zero
  and `UINT32_MAX` are invalid. On reconnect, the key resumes the same
  player under a fresh session. Gameplay packets use only the new session.
- WorldId is the server's opaque nonzero identifier for the opened world in
  Welcome. The host uses the same ID for joins to that world and assigns a
  new one on world replacement. A restarted host recovers the saved ID.
  Clients key the projection by WorldId and epoch. The
  epoch is nonzero and changes whenever a world is replaced, reloaded or
  reopened by a restarted authority. The host must distinguish epochs across
  process starts, beyond a single simulation's reload counter. Within an epoch, the server tick
  starts at zero and advances at the 60 Hz host cadence. The owner uses
  120 Hz player physics. `nextEditId` is the server's next logical world edit
  ID, initially at least 1. All existing logical edit IDs are below it.

Layout, tag, framing or wire semantic breaks require an explicit new protocol
version. Readers reject unknown tags and fields for the selected version.
Future envelopes can preserve v5 discovery or require an explicit client
upgrade. Item schema 1 carries the append-only u16 `ItemId` registry with zero
reserved for empty. The material catalog uses the append-only voxel ordinals
`[0, COUNT)` and wire AIR 255. The current core AIR sentinel is mapped at
the projection boundary. IDs are never reassigned. Catalog extensions change
the fingerprint and require a matching peer catalog. Item representation
changes require an item-schema bump and a negotiated compatible wire version.

Fingerprints are FNV-1a-64 with offset basis 14695981039346656037 and prime
1099511628211. Hash numbers as four big-endian bytes and strings as catalog
bytes followed by zero. The catalog hashes the domain string
`FTVN catalog 1`, material count, then in ordinal order each material's
catalog name, mapped item ID, drop item ID and drop count. It then hashes
255, the item count, and each item's ID, name, stack limit and placeable
material (`UINT32_MAX` if absent) in item ordinal order. The generator hashes
the domain string `FTVN generator 1`, `TerrainGenerator::kGeneratorVersion`,
16 and 256. A generator change affecting produced voxels must advance the
generator version. This fingerprint identifies the declared generator
contract. Full authoritative baselines avoid a dependency on client
worldgen reproducing server voxels.

## Sequence and delivery

After Welcome, each direction has independent fresh and reliable u64 sequence
counters. Each starts at 1, admits up to `UINT64_MAX - 1` and never wraps.
Reconnect before exhaustion. Authenticate the scope and check direction
before admitting queued messages. A wrong session or epoch cannot advance
either counter.

- **Fresh** accepts only a sequence higher than the previous admitted
  sequence for that stream. ItemSnapshot and MobSnapshot each have their
  own receive cursor within the same outgoing fresh counter. Input,
  PlayerSnapshot and Heartbeat retain their shared cursor. Loss, duplication
  and reordering can skip values. Heartbeat has
  an empty payload and is sent only when the direction is otherwise idle.
  Player snapshots are complete for their advertised set. They supersede
  earlier player snapshots and are never retransmitted. Items and mobs use
  the same complete replacement rule independently.
- **Reliable** uses cumulative ACKs and go-back-N retransmission with at most
  64 unacknowledged frames per direction. The receiver admits the exact next
  sequence after reserving queue capacity. A higher sequence is dropped and
  leaves the cursor unchanged. A repeated or older sequence is ignored.
  On accepted, repeated or future packets the receiver can re-emit its
  highest contiguous ACK, subject to budget. The ACK records admission into
  the ordered receive path. Gameplay acceptance is reported by ActionResult.
- **Ack** carries `through:u64` and uses header sequence zero. Its scope
  still must match. Ignore `through` at or below the sender's last advanced
  ACK or above the highest sequence sent. A progressing ACK retires all
  frames through that value. Senders retry the outstanding suffix with
  the same sequences and bytes after 150 ms without ACK progress, paced
  within the budgets. Close or resync after 5 seconds without progress.
  Never ACK an ACK. There is no ACK for sequence zero.

Bound the reliable retransmission queue at 64 encoded datagrams per direction
(`76,800` frame bytes). Keep at most 64 received reliable frames awaiting the
owner per peer. The active server uses one reliable stop-and-wait slot, one
latest outbound snapshot per fresh family, one latest pending inventory,
six uniquely timed inbound input slots and one outbound world update per
peer. It reserves up to 16 concurrent baseline
buffers and rotates admission among peers. It schedules reliable sends before
fresh snapshots. The process caps player snapshots at 768 frames/s with a
64-frame burst and each entity family at 256 frames/s with a 24-frame burst.
When admission
fails due to capacity, leave the reliable ACK unchanged. Throttle baseline
production and bound the per-peer delta backlog at 64 pending messages. If
it overflows or cannot make progress, abandon the projection and resync
instead of dropping an edit from an apparently complete ordered stream.

## Input, snapshots and gameplay actions

**Input** carries `intendedTick:u64, moveX:f32, moveZ:f32, flags:u8`.
Movement is in world X/Z with squared length at most 1 as calculated in
binary64. Flags bits 0–3 are sprint, jump edge, swim up and swim down.
The scope selects the server-assigned player, so the payload carries no
actor ID. If the last completed host tick is `t`, admissible intended ticks
are `t+1` through `t+6` until their inbox closes at the owner drain.
The transport rejects regressions in intended tick before advancing the
fresh cursor. A newer sequence for the same pending tick replaces the old
sample. Each intended tick receives one sample or neutral controls, and the
owner passes each consumed jump edge to the controller once. Rate excess,
late packets and packets beyond the lead window are dropped.

**PlayerSnapshot** carries `tick:u64, lastProcessedInput:u64, count:u8`
followed by 1–24 records. Each record is `playerId:u32, feetX:f64, feetY:f64,
feetZ:f64, velocityX:f32, velocityY:f32, velocityZ:f32, flags:u8,
motionReset:u32`. Three `f64` seconds values follow the records, starting
with the owner's buffered jump (0–0.12), then coyote time (0–0.10) and
swim-jump lockout (0–0.25). The motion reset serial changes when the host
resets a player's controller, including a spawn, teleport or mode change.
IDs are strictly ascending. The server includes the owner and selects
up to 23 interested remote players. Feet lie in `[-2^23,2^23)` and each
velocity component lies in `[-1024,1024]`. Flag bits 0–2 are grounded,
waiting for terrain and flight. `lastProcessedInput` is the owner's fresh
Input sequence consumed by the simulation, with zero meaning none. It is
independent of reliable ACKs. Heartbeats share the fresh lane but do not
count as processed inputs. The client rejects regressive acknowledgments,
sequences ahead of the last sent input, and acknowledgments inside its
64-entry ledger whose sequence or intended tick cannot have been processed.
Snapshots carry the wire epoch in the header
and are produced every three completed ticks. The owner is always included,
and eligible neighbors are selected in ascending ID order from the remote
players within the configured chunk-radius disk.

**Action** carries `actionId:u64, operation:u8, slot:u8, otherSlot:u8,
cell, generation:u64, revision:u64, yawTurns:f32, pitchTurns:f32`.
`cell` is `x:i32, z:i32, y:u8`, with X/Z in `[-2^23,2^23)` and Y in
`[0,255]`. The client numbers action IDs consecutively from 1 within
the session, independently of reliable frame sequence. Action IDs never
wrap and retries preserve the original ID. Every unused field is zero,
including the bit representation of unused angles.

| Operation | Intent and fields |
| --- | --- |
| 1 Break, 2 Place | Target cell, expected nonzero chunk generation and revision, aim. The server casts from the authoritative eye and checks the currently legal hit or placement cell, reach and revision. |
| 3 Select | Hotbar slot 0–8. |
| 4 DropOne, 5 DropStack | Inventory slot 0–35. The server computes the amount from that slot. |
| 6 Move, 7 Swap, 8 SplitHalf | Two distinct slots 0–35. Move attempts the whole source, swap exchanges stacks and split attempts to transfer the larger half into the destination. The server computes all resulting counts. |

For voxel actions yaw is in `[0,1)` turns, with zero along +Z and a quarter
turn along +X. Pitch is in `[-0.25,0.25]` turns, positive up. For a
versioned voxel action, the owner checks the expected generation and
revision of the target chunk. It traces up to eight blocks from the
actor's last completed eye position, stopping at unknown terrain or the
first effective non-air voxel. A place target must share the entry face
of that hit. The owner then applies the ordinary break or selected-stack
placement rules. Each tick's admitted intents resolve in ascending
`PlayerId` order. Gameplay rejection consumes the admitted action ID.

The active UDP transport admits one action per peer until its result is
acknowledged. Old and skipped action IDs leave the reliable cursor at its
previous value. Packet rates apply to both first sends and retries. Every
reliable retry preserves the admitted message bytes, including its ID.
A fresh session starts its own action IDs at one and clears any old
pending action. A departure after owner execution can lose the result
while the edit remains committed.

**ActionResult** carries `actionId:u64, tick:u64, nextEditId:u64, editId:u64,
operation:u8, code:u8`. Codes are 0 accepted, 1 partial (Move or SplitHalf),
2 invalid target, 3 out of reach, 4 terrain unavailable, 5 stale revision,
6 inventory rejected, 7 occupied, 8 rate limited, 9 forbidden and
10 unavailable. A successful Break or Place carries the resulting nonzero
`editId < nextEditId`. Every other result carries edit ID zero. The
result settles the action. `tick` is the last completed simulation tick
when the owner handled it and `nextEditId` is the global edit watermark
after handling it. The world projection applies WorldEdit/baselines.
Select, Move, Swap, SplitHalf, DropOne and DropStack run on the authenticated
owner's inventory. Drop uses a fixed server-selected +Z direction and
computes the count from the slot operation.

**Inventory** carries `tick:u64, revision:u64, selected:u8` followed by
36 pairs of `itemId:u16, count:u16` in slot order. Revision starts at 1 and
increases for newer inventory states within the session. Each pair is
either `{0,0}` or a registry item with a count between 1 and its stack
limit. Selected is a hotbar slot. The server sends this full snapshot only
to its session owner, initially and after changes. Unsent states may
coalesce. The client installs only newer revisions with nondecreasing
ticks. The ordered lane delivers the action's inventory revision, or a newer
one, before its ActionResult. This also covers actions that consume a
selected stack and autonomous pickups between actions.

## Nearby entities

**ItemSnapshot** carries `tick:u64, idHighWater:u64, count:u8` followed by up
to 38 records of `id:u64, motion, itemId:u16, count:u16`. Live stacks have
nonzero counts within the registry limit. IDs are nonzero, strictly
ascending and at most the world's allocator high-water. The high-water
survives checkpoint recovery even when all prior items are gone.

**MobSnapshot** carries `tick:u64, count:u8` followed by up to 42 records of
`id:u64, motion, species:u8`. Species are cow (0), pig (1), sheep (2)
and chicken (3). Nonzero IDs are strictly ascending and identify live
incarnations during the world epoch. A procedural group that spawns again
gets new mob IDs. Mob and item IDs have separate namespaces.

Both use `motion` in X/Y/Z order with three `i32` positions in 1/64 blocks
and three `i16` velocities in 1/64 blocks/s. Positions use the existing
signed world coordinate bounds and velocities saturate at their encoded
limits. The host samples once every six completed 60 Hz ticks. Interest
is the horizontal chunk disk centered on the authenticated player's
authoritative feet. If the eligible population exceeds a packet cap, the
server keeps the nearest records by horizontal distance, breaking ties
by ID, then serializes the selected records in ID order.

Each snapshot replaces the family's entire visible set. A new ID spawns
in the projection, a retained ID updates, and an absent ID leaves the
projection. This covers pickup, expiry, interest transitions and culling.
Empty snapshots explicitly clear the visible set. The client accepts
strictly increasing ticks and rejects a regressing item high-water. A
later complete snapshot repairs a lost spawn, update or removal. Older
packets and frames from a retired session or epoch cannot roll back a
projection. The client retains the previous and current samples and their
server ticks. Interpolate only IDs in both samples with tick differences
divided by 60 for elapsed seconds. Initialize new IDs at their current
position and remove absent IDs when the new set is installed.

## World baseline, edits and resync

Chunk coordinates are `x:i32, z:i32` in `[-524288,524288)`. Resolve a cell's
chunk by floor division by 16. A generation is nonzero and identifies one
server-owned chunk incarnation in this epoch. The host never reuses a
generation at the same coordinate within an epoch. Replication revision starts
at zero per incarnation and advances for each accepted logical target edit in
that chunk. The owner records it at acceptance, including edits waiting for
terrain backing and writes that storage later coalesces. Persisted overrides
come through the baseline. Render-border invalidations use the separate
render event stream.

**ChunkFragment** carries `transferId:u64, chunkCoord, generation:u64,
revision:u64, nextEditId:u64, checksum:u64, encodedSize:u16, index:u8, data:bytes`.
The full image contains 65,536 material bytes in X, Z, Y order
(`y * 256 + z * 16 + x`). FNV-1a-64 covers those bytes after decoding.
`encodedSize == 0` denotes the full raw image. A nonzero value gives the
exact byte length of the run-length encoded image. Each run starts with a
control byte. The low seven bits give the run length minus one. A set high
bit means one material byte follows and repeats for the run length. A clear
high bit means that many literal material bytes follow. A run may cross a
fragment boundary. The encoder uses the raw image when encoding would
equal or exceed its size.

The header scope and all transfer metadata except index must match across
fragments. The server assigns ascending nonzero transfer IDs within the
scope. With payload size `n`, fragment count is `ceil(n / 1100)` and
each zero-based index carries `min(1100, n - index * 1100)` bytes. Raw
images use 60 fragments, with 636 bytes in the last fragment.

Keep one in-flight transfer per client, one fixed 65,536-byte assembly
buffer and one 65,536-byte temporary decode buffer used on completion.
Delivery to the assembler is ordered after reliable admission. Decode
requires each run to fit both the remaining input and the output and
requires exactly 65,536 valid material bytes. There is no allocation from
the advertised size. Start a 20-second total deadline on the first fragment
and expire after five seconds without progress. A gap, expiry, changed
metadata, invalid encoding, invalid material or checksum failure discards
the assembly. Install only after the final fragment and checksum
verification. The installed image carries the scope, chunk generation,
revision and next-edit watermark.

**WorldEdit** carries `editId:u64, cell, generation:u64, baseRevision:u64,
material:u8`. It replaces one authoritative voxel at that cell. Compare
the chunk generation and base revision before applying it, then advance
the chunk revision to `baseRevision + 1`. The base must be below
`UINT64_MAX`. Logical edit IDs increase, with gaps permitted because
subscriptions filter the global edit stream.

The sim owner captures a coherent baseline and its next-edit watermark
at the same boundary. A baseline includes the effects visible at capture
for that chunk, including the effective logical-edit overlay. Enqueue
subsequent edits for that chunk after its fragments, preserving edit order.
Apply only edits at or above the baseline's watermark. Verify ID
progression and exact generation/base revision for those edits. Resync
if the exact continuation is unavailable. Interest is derived by the
server from actor state. Client requests can trigger resync without supplying
coordinates or expanding interest.

**ChunkRemove** carries `chunkCoord, generation:u64`. It removes the
matching incarnation from client interest. A baseline for a re-entered
chunk precedes its new deltas. The client ignores a removal for a
generation it no longer holds.

**Resync** carries `reason:u8`. Codes are 1 missing baseline or revision
gap, 2 checksum failure, 3 timeout and 4 epoch change. Send it in the old
scope to request a fresh join. On a server-initiated resync, the server
waits for the client's scoped Resync response with a five-second deadline.
**Disconnect** carries
`reason:u8` with codes 1 leaving, 2 shutdown and 3 resync. On resync,
disconnect, timeout or a new Welcome, discard old receive windows,
partially assembled chunks, pending action retries and state for that
scope. Reconnect via Hello with a new nonce, negotiate again, and build
from fresh server snapshots and baselines under the new SessionId.
An action with an unknown outcome is reconciled from that state without
automatically resubmitting it. Failure of a final control to arrive falls
back to timeout and the same reconnect path.

## Codec status and verification

`encode` and `decode` return `Ok` or a failure status and leave the output
untouched on failure. Header failures are `Truncated` (under 36 bytes),
`Oversized` (over 1,200), `BadMagic`, `VersionMismatch`, `UnknownKind`,
`WrongDirection` and `InvalidHeader` (scope, sequence or reserved bits).
`BadLength` reports an advertised maximum or exact-length mismatch, including
trailing bytes. `InvalidPayload` covers missing or invalid required fields.
Receive-window statuses distinguish replay, gap, wrong scope and invalid
sequence. Malformed payloads never reach the owner queue. The codec and
assembler use fixed storage and return status values without throwing.
Player snapshot and chunk fragment equality covers metadata and the active
array prefix, so callers can reuse storage between packets.

```sh
cmake -S . -B build-cpu -DFT_VOX_GRAPHICS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpu --config Release --parallel
ctest --test-dir build-cpu -C Release --output-on-failure

cmake -S . -B build-asan -G Ninja -DFT_VOX_GRAPHICS=OFF -DFT_VOX_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan --target test_wire_protocol
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-asan -R '^WireProtocol$' --output-on-failure
```

The first sequence runs on Linux or Windows. The sanitizer commands use
GCC or Clang on Linux. `WireProtocol` is registered in both CPU suites
and the existing sanitizer job. It pins all message tags with literal
byte fixtures and tests truncation, malformed headers/payloads,
transactional failure, direction, replay and gaps, chunk assembly and
fixed-seed random and mutated datagrams. CTest returns nonzero on
failure. The isolated `NetworkIntegrationTest` also runs when Boost is
available.

`MultiplayerClosure` exercises this codec with two production UDP
clients, an injected-clock IPv4 relay, lost and reordered traffic,
checksum recovery and a replay from a retired session. The
[closure report](../multiplayer-closure.md) records the loopback results
and the supported private-network scope. Wire v5 profile keys and
session capabilities travel in cleartext. Public-network readiness is
tracked separately in [#276](https://github.com/gkehren/ft_vox/issues/276).
