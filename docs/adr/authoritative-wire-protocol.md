# Authoritative wire protocol v2

Accepted for Epic #245 Phase B2 ([#251](https://github.com/gkehren/ft_vox/issues/251)).
The `ft_vox_wire` library implements framing, payload validation, receive
sequencing and chunk assembly. The dedicated server starts using the protocol
when #252 adds the transport. Replication producers and consumers follow in
Phase C. The Boost UDP prototype remains isolated in `test_network` and uses
its original v1 framing.

## Framing

Each UDP datagram carries one frame of at most **1,200 bytes**, including
the 36-byte header. A 1,200-byte UDP payload plus the normal IPv6 and UDP
headers fits within the 1,280-byte IPv6 minimum MTU. The transport must drop
oversized or socket-truncated datagrams. There is no IP-fragmentation
dependency and no generic message fragmentation.

| Offset | Type | Field |
| --- | --- | --- |
| 0 | 4 bytes | ASCII `FTVN` (`46 54 56 4e`) |
| 4 | u16 | Envelope version, 2 |
| 6 | u8 | Message tag |
| 7 | u8 | Flags, zero |
| 8 | u16 | Payload length, 0–1,164, exactly the remaining bytes |
| 10 | u16 | Reserved, zero |
| 12 | u64 | SessionId, scope rules below |
| 20 | u64 | World epoch, scope rules below |
| 28 | u64 | Sequence, lane rules below |

Fields are packed in the listed order. Integers are big-endian. Signed
`i32` uses two's complement. `f32` and `f64` carry big-endian IEEE 754 binary32
and binary64 bits, with finite values only. Flags and enum values outside
their assigned ranges fail validation. There is no structure padding, host
endianness, implicit alignment or serialization of native C++ layouts.

## Message headers and budgets

C and S denote the client and server. A rate and burst apply per peer per
direction to the corresponding family. The byte maximum includes the header.

| Tag | Name | Sender | Lane | Max bytes | Frames/s | Burst |
| --- | --- | --- | --- | ---: | ---: | ---: |
| 1 | Hello | C | Handshake | 66 | 2 | 2 |
| 2 | Welcome | S | Handshake | 98 | 2 | 2 |
| 3 | Reject | S | Handshake | 49 | 2 | 2 |
| 4 | Input | C | Fresh | 53 | 60 | 8 |
| 5 | PlayerSnapshot | S | Fresh | 1,037 | 20 | 4 |
| 6 | Action | C | Reliable | 80 | 16 | 8 |
| 7 | ActionResult | S | Reliable | 70 | 16 | 8 |
| 8 | Inventory | S | Reliable | 197 | 10 | 4 |
| 9 | WorldEdit | S | Reliable | 70 | 32 | 16 |
| 10 | ChunkFragment | S | Reliable | 1,185 | 60 | 16 |
| 11 | ChunkRemove | S | Reliable | 52 | 16 | 8 |
| 12 | Resync | C or S | Reliable | 37 | 1 | 1 |
| 13 | Ack | C or S | Ack | 44 | 60 | 16 |
| 14 | Heartbeat | C or S | Fresh | 36 | 2 | 2 |
| 15 | Disconnect | C or S | Reliable | 37 | 1 | 1 |

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

## Join, identity and versioning

- **Hello** carries `nonce:u64, minVersion:u16, maxVersion:u16, compatibility`.
  The client picks a nonzero fresh nonce per attempt and reuses it only to
  retransmit that attempt. The inclusive version range must be nonzero and
  ordered. Hello, Welcome and Reject use the v2 envelope for discovery.
- **Welcome** carries `nonce:u64, worldId:u64, playerId:u32, seed:u32,
  generatorVersion:u32, compatibility, tick:u64, nextEditId:u64`. The server
  echoes the nonce, selects v2, and assigns the authorized PlayerId. The seed
  is 1–2,147,483,647. The generator version is nonzero and must match the
  negotiated generator fingerprint. `compatibility` is `itemSchema:u16,
  catalogFingerprint:u64, generatorFingerprint:u64`. The server compares
  exact compatibility after selecting a common version. The client verifies
  the echoed nonce, selected version, generator version and compatibility
  before installing the new session.
- **Reject** carries `nonce:u64, reason:u8, minVersion:u16, maxVersion:u16`.
  Reasons are 1 version, 2 content, 3 full and 4 unavailable. With valid
  discovery framing but no common version, reply with reason 1 and range
  `[2,2]`. A foreign magic or envelope version is dropped. It cannot be
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
  capability and epoch. PlayerId is allocated by the running simulation
  and is scoped to that authority instance. Zero and `UINT32_MAX` are
  invalid. The server resolves the actor from the bound session on a fresh
  join. Persistent reconnect identity belongs to #259.
- WorldId is the server's opaque nonzero identifier for the opened world in
  Welcome. The host uses the same ID for joins to that world and assigns a
  new one on world replacement. A restarted host can recover the ID or
  choose a fresh one. Clients key the projection by WorldId and epoch. The
  epoch is nonzero and changes whenever a world is replaced, reloaded or
  reopened by a restarted authority. The host must distinguish epochs across
  process starts, beyond a single simulation's reload counter. Within an epoch, the server tick
  starts at zero and advances at the 60 Hz host cadence. The owner uses
  120 Hz player physics. `nextEditId` is the server's next logical world edit
  ID, initially at least 1. All existing logical edit IDs are below it.

Layout, tag, framing or wire semantic breaks require an explicit new protocol
version. Readers reject unknown tags and fields for the selected version.
Future envelopes can preserve v2 discovery or require an explicit client
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

- **Fresh** accepts only a sequence higher than the previous admitted fresh
  packet. Loss, duplication and reordering can skip values. Heartbeat has
  an empty payload and is sent only when the direction is otherwise idle.
  Player snapshots are complete for their advertised set. They supersede
  earlier player snapshots and are never retransmitted.
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
owner per peer and at most one pending fresh sample per peer. When admission
fails due to capacity, leave the reliable ACK unchanged. Throttle baseline
production and bound the per-peer delta backlog at 64 pending messages. If
it overflows or cannot make progress, abandon the projection and resync
instead of dropping an edit from an apparently complete ordered stream.

## Input, snapshots and gameplay actions

**Input** carries `intendedTick:u64, moveX:f32, moveZ:f32, flags:u8`.
Movement is in world X/Z with squared length at most 1 as calculated in
binary64. Flags bits 0–3 are sprint, jump edge, swim up and swim down.
The session layer owns input age and lead limits. The sim owner validates
and maps input into fixed steps and never applies the same jump edge twice.

**PlayerSnapshot** carries `tick:u64, lastProcessedInput:u64, count:u8`
followed by 1–24 records. Each record is `playerId:u32, feetX:f64, feetY:f64,
feetZ:f64, velocityX:f32, velocityY:f32, velocityZ:f32, flags:u8`.
IDs are strictly ascending. The server includes the owner and selects
up to 23 interested remote players. Feet lie in `[-2^23,2^23)` and each
velocity component lies in `[-1024,1024]`. Flag bits 0–2 are grounded,
waiting for terrain and flight. `lastProcessedInput` is the owner's fresh
Input sequence consumed by the simulation, with zero meaning none. It is
independent of reliable ACKs and heartbeat sequences.

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
turn along +X. Pitch is in `[-0.25,0.25]` turns, positive up. The expected
version is a precondition and the cell and aim are targeting intent. The
owner validates the actor, authorizes the operation and deduplicates by
action ID in that session before mutating the simulation.

**ActionResult** carries `actionId:u64, tick:u64, nextEditId:u64, editId:u64,
operation:u8, code:u8`. Codes are 0 accepted, 1 partial (Move or SplitHalf),
2 invalid target, 3 out of reach, 4 terrain unavailable, 5 stale revision,
6 inventory rejected, 7 occupied, 8 rate limited, 9 forbidden and
10 unavailable. A successful Break or Place carries the resulting nonzero
`editId < nextEditId`. Every other result carries edit ID zero. The
result settles the action. The world projection applies WorldEdit/baselines.

**Inventory** carries `tick:u64, revision:u64, selected:u8` followed by
36 pairs of `itemId:u16, count:u16` in slot order. Revision starts at 1 and
increases for newer inventory states within the session. Each pair is
either `{0,0}` or a registry item with a count between 1 and its stack
limit. Selected is a hotbar slot. The server sends this full snapshot only
to the inventory owner. Nearby dropped item and mob state will require
additional negotiated schemas in #256.

## World baseline, edits and resync

Chunk coordinates are `x:i32, z:i32` in `[-524288,524288)`. Resolve a cell's
chunk by floor division by 16. A generation is nonzero and identifies one
server-owned chunk incarnation in this epoch. The host never reuses a
generation at the same coordinate within an epoch. Replication revision starts
at zero per incarnation and advances for each authored voxel edit in
that chunk. It is a wire counter. The server maps from authoritative edits
and chunk lifetimes without exposing render-border invalidations as voxel edits.

**ChunkFragment** carries `transferId:u64, chunkCoord, generation:u64,
revision:u64, nextEditId:u64, checksum:u64, index:u8, materials:bytes`.
This is one full 65,536-byte chunk in row-major X then Z then Y order
(`y * 256 + z * 16 + x`), with one material byte per voxel and no compression.
The checksum is FNV-1a-64 over those raw material bytes for corruption
detection. The header's scope and
all transfer metadata other than index must match across fragments. The
server assigns ascending nonzero transfer IDs within the scope. The index
is 0–59. Indices 0–58 carry exactly 1,100 bytes, and index 59 carries 636
bytes. Offset is `index * 1100` and is never supplied by the peer.

Allow one in-flight transfer per client and one fixed 65,536-byte assembly
buffer. Delivery to the assembler is ordered after reliable admission.
There is no allocation from an advertised size or fragment count. Start
the 5-second steady-clock deadline on the first fragment. A gap, expiry,
changed metadata, invalid material or checksum failure discards the
assembly. Install the chunk only after index 59 and full checksum
verification. An assembled baseline is associated with its scope, chunk
generation, revision and next-edit watermark.

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
scope to request a fresh join. **Disconnect** carries
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
