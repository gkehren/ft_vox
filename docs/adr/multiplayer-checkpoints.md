# Dedicated player identity and checkpoints

Accepted for Epic #245 Phase E1, issue #259. This extends the
[gameplay checkpoint transaction](gameplay-persistence-transactions.md)
and uses the [authoritative wire protocol](authoritative-wire-protocol.md).

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
Player-key recovery and rotation remain separate from the local owner
credential and role procedures below.

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

## World role authority

Issue #286 adds a local role authority bound to the persistent world ID
and to the registry's player ID and profile-key pairs. One registered
player is the primary owner. Delegated admins are distinct registered
players. The server treats every other actor as a player. The role lookup
does not authenticate a command or modify wire v5.

### Bootstrap and command boundaries

- The launcher writes one fixed 72-byte record to the private inherited
  stdin pipe. It contains `FTLB`, little-endian u32 version 1, the
  32-byte player key and a 32-byte local control field. The field is zero
  when reopening without a readable key. A fresh bootstrap requires a
  distinct, nonzero control key. The server
  reads it within five seconds before opening a world. Extra bytes are
  rejected and closing the writer requests graceful shutdown after startup.
  Key bytes are absent from command arguments, environment, preferences,
  readiness output and the audit. `--launcher-control stdin` remains the
  EOF-only mode for ordinary supervisors. `bootstrap` accepts the same
  bootstrap record and EOF. `admin` additionally enables live requests and
  requires the inherited monitoring response pipe.
- The local control key file is `FTVK`, little-endian u32 version 1, then a
  nonzero OS-generated 32-byte value. New files use mode 0600 on POSIX and a
  protected DACL for the current user and SYSTEM on Windows. The authority
  file uses the same private write path. The writer verifies privacy
  before putting any key bytes on disk. On Windows, reads also require a
  protected DACL with allow entries limited to the current user and
  SYSTEM, and an owner from that same set. POSIX reads refuse group or
  other permissions. Preserve these permissions when restoring
  authoritative files.
- Only a fresh world directory may use an incoming GUI bootstrap. The GUI
  refuses to launch an existing world without a role record and directs
  the operator to offline provisioning. A launcher that selects the
  recorded owner's profile must supply the matching local control key.
  Routine offline owner
  commands require the independent owner control key plus the current
  world ID and revision. Provision and recovery require local operator
  access and the world lock. They have explicit CLI verbs and audit
  entries. Recovery rotates control and clears delegations. The CLI and
  launcher are local process entry points. Hello, session capabilities and
  claimed actor IDs grant no access to them.
- Issue #288 binds live authority to the bootstrap proof and checks the
  current role again before each command. A delegated admin needs a
  separate, explicitly enrolled local control key. A command is a fixed
  104-byte `FTAQ` v1 record with world ID, fresh server launch epoch, the
  next request sequence, an opcode, target, and optional delegation proof
  for owner grants. Padding and unused proof bytes are zero. Replay, wrong
  scope, malformed input and buffered request bursts cannot mutate policy.
  An oversized burst or stalled partial record disables the privileged
  input for that launch. EOF still controls child lifetime. The inherited
  reply stream multiplexes `FTAR` v1 results with existing monitor frames.
  Replies are limited to 4,096 registered profiles and 64 audit entries.
  The launcher sends one request at a time and the child queues one reply.
  Command application runs on the simulation owner before UDP intents.
  No game packets or remote administration endpoints are added.

### Atomic records and restore

- `FTVA` **v2** adds flag bit 1 requiring role authority and a u64 role
  revision after the existing header fields. Bit 0 retains the checkpoint
  seal. Unowned registries remain v1. Missing or invalid identities
  prevent role-aware startup. The roster and the current role bindings
  are validated against the registry.
- `<saves-root>/.ft_vox..server-roles/<world-name>` stores
  `FTRL` **v2**, independent of gameplay checkpoint generations. The
  little-endian record contains magic, u32 version, u64 world ID and role
  revision, u32 owner ID, owner profile and control keys, and u32 admin
  and audit counts, followed in v2 by delegation and policy counts and the
  allowlist mode. Each sorted admin is a u32 ID and a 32-byte profile
  binding. Optional delegated local credentials are sorted ID and
  32-byte control-key pairs. Sorted admission entries record ID, ban and
  allowlist bits, and a u64 kick expiry in Unix seconds. The most recent
  64 audit entries contain a u64 revision, u8 operation, and u32 actor and
  target IDs. Zero actor identifies local bootstrap, provisioning or
  recovery. The reader accepts v1 records and treats their admission as
  open and delegations as role-only until the next authority write. A final
  u64 FNV-1a checksum covers the preceding bytes to detect corruption. The server
  flushes a private `.pending..<world-name>` record, atomically advances
  the revision in the identity registry, then promotes the staged record
  with an atomic rename. Opening the world completes a matching staged
  record if interrupted. Moderation and its audit entry share that commit.
  Admission is enforced before both reservation and activation. A role
  file older than the registry revision cannot become active. A gameplay
  checkpoint rollback preserves the latest valid authority and identities.
  Keep both files when restoring gameplay. Their permissions and the trust
  placed in the local operator are part of the security boundary. The
  checksum detects corruption and carries no protection against someone
  who can replace both private authority and registry files.
- Before the first gameplay checkpoint, the server publishes its registry
  and a pending authority record with the trusted keys and owner ID zero.
  After publishing and sealing the first dedicated checkpoint it enrolls
  the reserved profile through the existing ID allocator and finishes the
  role record. Both steps finish before UDP readiness. A crash after the
  pending write retries the original profile. A crash after enrollment
  reuses that registered ID. If the world was created but no pending record
  survived, a repeat GUI launch fails before UDP binds. The CLI can
  establish the checkpoint for explicit local provisioning.
- Gameplay rollback and world-directory restores leave the latest role
  file in the separate authority directory. Keep the current
  `server-identities.state` with it to preserve role bindings and
  ID reservations. An authority associated with a different world ID,
  missing marked authority, stale revision, or mismatching role binding
  fails closed.
  Never reactivate an old authority snapshot as part of gameplay restore.
  If the only security backup predates a role change, use the local
  recovery procedure to rotate the control credential and clear all
  delegations before serving the restored world. An operator who rolls
  back every trusted file can erase the evidence of later changes. This
  local store assumes the operator preserves the authority separately or
  explicitly recovers it.
