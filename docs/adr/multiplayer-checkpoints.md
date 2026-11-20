# Dedicated player identity and checkpoints

Accepted for Epic #245 Phase E1, issue #259. This extends the
[gameplay checkpoint transaction](gameplay-persistence-transactions.md)
and uses [wire v4](authoritative-wire-protocol.md).

## Identity and authorization

| Identifier | Lifetime and ownership |
| --- | --- |
| World ID | A random nonzero u64 for the save directory. The server registry and checkpoint manifest bind the roster to that world. The seed and generator version must match `world.meta`. |
| Session ID and epoch | The server issues a fresh UDP capability on each join and a fresh epoch on each process start. The peer endpoint and both values scope all gameplay packets and cursors. |
| Persistent player ID | A u32 allocated by the world owner. The same ID appears in Welcome, snapshots and the roster each time that profile rejoins. ID 1 is reserved for the local compatibility anchor. |

Each client profile owns a random 256-bit key. Hello carries the key and
the server matches it against its private registry. The client cannot
choose a player ID. The first valid scoped packet confirms the route.
For new profiles the owner records the key-to-ID binding before the actor
participates in gameplay or receives inventory. A second connection with
the same key is refused while the old connection is pending or active.
Departure retires the old transport state and copies the actor into the
offline roster. A pending join that never confirms expires without making
a profile. Its provisional ID stays spent.

The key is a bearer credential. A copied key grants access to that
profile. UDP sends it in cleartext. Use loopback or a trusted private
network with protected transport, such as a VPN between hosts. Keep the
profile and server registry private. POSIX writes use mode 0600. Windows
uses the directory ACL. Losing a key makes that profile inaccessible.
There is currently no key recovery, rotation or account administration.

## Durable formats and ownership

- `FTVM` **v2** appends the world ID and the `players.state` checksum to
  the v1 manifest. The `FTVR` **v1** roster holds at most 4,096 records in
  ascending persistent ID order. Each has the existing player pose and
  flight/creative settings, the complete inventory including selected
  hotbar slot, and linear velocity. Contact flags are re-derived from
  terrain. Movement timers, buffered input and network cursors start
  fresh. Remote camera orientation remains client-local.
- The server checkpoints the roster, the local anchor, shared items and
  sparse chunk overrides through one publication. Ambient mobs regenerate
  from the world seed and residency. They currently have no durable
  gameplay state. A restored position waits for terrain where needed. If
  a loaded location is embedded beyond solver recovery, the server uses
  a safe spawn and retains the actor's inventory and settings.
- The manifest's world edit revision records the capture boundary for
  diagnostics. Replication compares edit IDs and chunk revisions within
  a transport epoch. A restarted server advertises the new epoch's
  edit watermark and sends fresh baselines from the committed overrides.
- The independent `FTVA` **v1** registry, `server-identities.state`, holds
  only world identity, confirmed profile bindings and exclusive ID
  reservation limits. The owner reserves 256 player IDs or 1,048,576 item
  IDs per allocation range. It publishes the reservation before use.
  Restart skips the old ranges' unused tail. This prevents reusing IDs
  observed before a crash, including IDs of rolled-back drops. An identity
  confirmed after the last checkpoint rejoins with a fresh actor state
  under its existing ID.
- A native lock allows one dedicated owner per world directory. Offline
  records have no live body, streaming interest, pickup rights or timeout.
  They remain in the roster. The profile limit refuses new keys and leaves
  existing profiles eligible to reconnect. Disconnect takes no checkpoint.
  A new confirmed profile only rewrites the bounded registry.

## Migration and recovery

Existing v1 checkpoints and valid legacy loose saves open in the dedicated
server. Their singleton player and inventory map to the local anchor and
are never assigned to a remote key. Their shared chunks and dropped items
carry forward. Missing legacy gameplay files use the established fresh
defaults. Corrupt or unreadable legacy payloads prevent server migration.
The existing solo reader retains its legacy behavior on solo saves.

The server publishes an initial v2 generation before admitting gameplay.
The registry is written first to reserve IDs. At this point it is unsealed
and has no actor allocations or profile bindings. If startup fails or the
process exits before publishing the v2 manifest, solo can still open the
prior v1 checkpoint or loose save with this validated provisional registry
present. The provisional registry must match the save's seed and generator.
It remains on disk so a dedicated retry can keep the same world ID and
skip the already reserved item range.

The atomic rename to `checkpoints/manifest` commits the migration. A
temporary manifest and unreferenced generation directories are staging
data. Once v2 is published, solo refuses the save even if the registry is
still unsealed. The server seals the registry before accepting gameplay.
The seal keeps a save dedicated if checkpoint recovery selects its v1
fallback. A recognizable v2 manifest also requires the existing registry
even if its payloads fail validation and recovery selects v1. If the
manifest's world ID can be read, the registry must match it. An unreadable
registry cannot authorize solo access. A missing or corrupt registry for
a published v2 save prevents dedicated startup. The server also refuses
an unsealed registry if the published v2 manifest has lost its world ID.

Graceful stop retires transport and drains terrain, resolves accepted
deferred edits, flushes chunk overrides and publishes one generation from
the owner thread. A failed barrier or prepublication write leaves the
prior manifest authoritative. The shutdown path releases resources
without an implicit gameplay commit. Recovery validates the current
generation as a unit and can fall back to the previous valid generation.
The server verifies the generator and requires every roster ID to exist
in the registry. Unknown
manifest versions refuse the open. Codecs reject unknown fields and
trailing bytes in the selected version. A roster format change requires
a manifest version bump. `world.meta` still identifies the seed and
terrain generator version used by the shared world.

Retention keeps the active and previous checkpoint. With 226 bytes per
actor and a 20-byte header, the roster is at most 925,716 bytes. The
registry is at most 147,505 bytes. Chunk work follows the existing sparse
override checkpoint path. The transaction protects against process failure.
Power-loss durability follows the existing file and directory sync
contract. Back up the complete save directory, including the registry,
while the server is stopped. Use a pre-migration backup to return to an
older binary or the solo editor after publishing v2. Recovery assumes one
writer and at least one surviving record of a completed migration.
Dedicated v2 commits leave legacy root gameplay files at their prior state.
