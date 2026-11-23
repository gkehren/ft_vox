# Multiplayer public-network readiness

**Decision for issue #276. Keep the dedicated server on IPv4 loopback or a
trusted private network.** Wire v5 adds chunk baseline compression. It carries
the same cleartext profile key and UDP session capability as v4. The local
authority, replay and resource tests establish the private-network baseline.
Public deployment requires evidence for the additional gates below.

## Threat model

- **Assets.** Persistent actor identity, position, inventory and roster.
  Authoritative terrain edits, shared items and checkpoints. Session traffic,
  availability of the 60 Hz simulation and player access to the world.
- **Trust boundary.** The dedicated server owns gameplay and its save directory.
  Clients submit intent. The server validates reach, execution order, voxel
  revision and inventory effects. The current transport assumes that the path
  and the peers with network access are trusted. The server host, administrator,
  local credential stores and backups are part of that trusted environment.
- **Threats in scope for public exposure.** A network observer can read
  credentials or gameplay data. An active intermediary can alter traffic or
  impersonate a peer. An Internet source can send traffic with a forged address.
  Reachable unauthenticated sources or abusive authenticated users can compete
  for admission, network, CPU, memory or disk. A client can act maliciously
  within its own authenticated session, so gameplay validation remains required.
- **Host and account operations.** Compromise of the host OS or a player's machine
  requires host and credential incident response. Public matchmaking,
  moderation and gameplay anti-cheat beyond server authority require their
  own product and security decisions.

## Review of wire v5

| Area | Current controls | Public-network gap |
| --- | --- | --- |
| Player and server identity | The client creates a random 256-bit key. The server accepts a compatible new key for enrollment or looks up the existing actor. A pending or active duplicate is refused. The client checks the configured endpoint, echoed nonce and content compatibility. | The key is a reusable bearer credential. Possession selects the saved actor. The server has no independent enrollment authorization and the client has no cryptographic server identity. A seed, world ID or content fingerprint identifies world data or compatibility. |
| Confidentiality and integrity | A random 64-bit session capability, epoch and IPv4 endpoint scope the session. Packet decoding checks framing, direction and payload bounds. Chunk checksums verify decoded material bytes. | Hello exposes the profile key. Welcome and gameplay expose the session scope. Gameplay, private inventory and control messages lack authenticated encryption. The chunk checksum detects corruption. It provides no sender authentication. |
| Replay and lifetime | The exact next reliable sequence is admitted. A reliable gap leaves the cursor unchanged. Fresh cursors discard stale frames. ACKs retire sent pending frames. Input targets only the next six eligible ticks. Rejoin renews the session and process restart renews the wire epoch. | These checks filter duplicates and old scopes. Their inputs are unauthenticated. A copied profile credential can still establish a new session when the actor is available. |
| Admission and resource use | A compatible Hello can reserve a slot and cause the tick owner to provision an actor before route confirmation. A provisional join expires in 5 seconds. New profiles enter the registry only after confirmation and a successful spawn. Per-family, per-peer and shared traffic buckets and fixed mailbox sizes limit work. | Admission consumes shared slots and may reserve durable IDs before return-routability is established. Confirmed joins can fill the durable profile registry and cause registry writes. Per-source buckets and process limits provide neither fair admission across distributed sources nor protection from upstream link saturation. |
| Revocation and recovery | `server-identities.state` persists the key-to-actor binding independently of gameplay checkpoints. Departure copies actor state to the in-memory offline roster for the next checkpoint. Rejoin resumes that actor. | There is no supported per-profile revocation, credential rotation or account recovery. Restart renews the session epoch and loads the same profile keys. A backup can contain the same compromised credentials. |

Implementation references are the [wire protocol](adr/authoritative-wire-protocol.md),
[UDP sessions](dedicated-server.md#udp-sessions) and
[identity and checkpoint contracts](adr/multiplayer-checkpoints.md).

## Supported deployment and operating limits

Keep the default `127.0.0.1:7777` bind for local play. For remote private
play, bind to the trusted LAN or authenticated VPN interface and use a host
firewall to allow only the intended peers on that interface. A VPN protects
the route between its members. Everyone granted access to the game listener
is inside wire v5's trust boundary. Disable public port forwarding and
verify the listener is unreachable from outside the allowed network.

Restrict access to client profile directories, the server save directory
and backups. Use distinct player profiles. Keep credentials and packet
captures out of shared logs and reports. If exposure is suspected, restrict
network access first and stop the server if needed. Keep access restricted
until a reviewed recovery procedure has removed the exposed credential's
access. Avoid editing registry or checkpoint files in place. See the
[checkpoint recovery contract](adr/multiplayer-checkpoints.md#migration-and-recovery).

| Wire v5 limit | Value and meaning |
| --- | --- |
| Sessions and profiles | Default 16 simulation sessions including one local anchor, so 15 remote slots. Maximum configuration 256 including the anchor, so 255 remote slots. Provisional joins use the same remote pool. The world registry caps confirmed profiles at 4,096. |
| Datagram and discovery | At most 1,200 bytes per application datagram and 128 reads per IO pump. Hello is budgeted at 2/s with burst 2 in a fixed 256-bucket source table. Shared IPv4 addresses and hash collisions share the same discovery budget. |
| Traffic buckets | Per peer and direction, 128 frames/s and 128 KiB/s with bursts of 64 frames and 64 KiB. Per process and direction, 2,048 frames/s and 2 MiB/s with bursts of 256 frames and 256 KiB. Family limits can be lower. These bounds apply inside the process. |
| Lifetime and queues | 5 s for provisional joins, 10 s established idle timeout and 5 s for an outstanding reliable frame. Per peer, six input slots, one reliable frame in flight per direction, one world send slot and at most 64 pending deltas before resync. |
| Terrain and persistence work | Radius defaults to 3 and is capped at 8 chunks. The server allows at most 64 generation jobs in flight and 16 concurrent baseline payloads. Confirmed enrollment rewrites the bounded identity registry. Chunk overrides and checkpoint size also depend on the world and play history. |

Use the server's `SERVER` metrics records to watch `dropped_ticks`, players,
generation backlog, world traffic, retries and resyncs. Use host measurements
for interface and socket drops, CPU, peak
resident memory and disk latency. The configured caps are ceilings.
[Closure and soak evidence](multiplayer-closure.md#soak-baseline-and-alert-thresholds)
covers the documented controlled workloads.

## Gates for a public deployment proposal

Start with an authenticated transport design that keeps authentication and
decryption ahead of gameplay dispatch. Use a maintained protocol library
and specify how the client verifies the server and how an approved player
credential maps to a durable actor. DTLS 1.3 is a candidate for retaining
datagram semantics and needs a congestion-control design for gameplay.
QUIC with DATAGRAM is a candidate for carrying reliable controls and
unreliable snapshots under one connection's congestion control. Review
handshake replay policy, transport overhead and path MTU before selecting
a design. See [DTLS 1.3](https://www.rfc-editor.org/rfc/rfc9147.html),
[QUIC and TLS](https://www.rfc-editor.org/rfc/rfc9001.html),
[QUIC DATAGRAM](https://www.rfc-editor.org/rfc/rfc9221.html) and
[UDP usage guidance](https://www.rfc-editor.org/rfc/rfc8085.html).

Run the following acceptance tests in a controlled environment owned by
the project. Record the build, transport and edge configuration, hardware,
test limits and observed results. A failure blocks widening the supported
scope.

| Gate | Defensive test and pass criteria |
| --- | --- |
| Authenticated identity | Verify server identity and separately authorize admission and actor lookup. Wrong or expired credentials, unapproved identities and server identity mismatches produce zero admissions. Reconnect a valid identity and verify it resumes only its own actor and inventory. |
| Confidentiality and integrity | Capture the test traffic and verify credentials, capabilities and private gameplay payloads travel only in authenticated encrypted records. Modified, truncated or wrong-context records produce no gameplay event, accepted ACK, projection change or credential disclosure. Confirm there is no fallback to raw v5 on the public listener. |
| Replay and renewal | Exercise duplicates, gaps and delayed records across key updates, reconnects and process restarts. An authorized action changes state exactly once. Retired session records cause zero edits, inventory changes, projection installs or lease extensions. Gate gameplay on handshake completion and reject state-changing early data. |
| Admission and reflection | Validate the return path before expensive application work. Keep pre-auth state and deadlines bounded. Unvalidated requests cause zero actor provisioning, terrain dispatch or durable writes. Measure pre-validation datagram egress against ingress for each unvalidated path. Require at most 3× attributable ingress bytes or the transport's stricter limit, including retries. See [QUIC address validation](https://www.rfc-editor.org/rfc/rfc9000.html#section-8.1) for the reference bound. |
| Distributed load and congestion | Run the legitimate workload while independent test sources compete for admission and bandwidth. Include shared-address clients, churn, and a constrained lossy path. Enforce edge and origin ceilings, bound queued bytes and handshake work, and reserve established-session progress. At overload, shed unauthenticated work and reduce optional traffic while active peers continue making progress. Verify recovery after the load stops and compare authoritative state and checkpoint invariants with the expected workload. |
| Credential revocation and rotation | Rotate a test profile credential without losing or duplicating its actor, position or inventory. Revoke a credential, terminate its active sessions and verify the old credential is rejected after restart and restore. Verify that rollback cannot silently restore revoked access. Confirm the operator can isolate the listener and that audit events carry no secrets. |

For a candidate public pilot, start testing with **8 active remote players**
and radius **1**, the private-network soak workload's concurrency and
radius. Cap pending handshakes independently of established sessions.
Start the test with up to eight pending handshakes and a five-second
handshake deadline. Before a release decision, publish the approved build
and topology, session and handshake caps, per-identity and global admission
rates with bursts, and edge and origin packet and byte budgets per direction.
Publish numeric bounds for queue depth, process memory, CPU and checkpoint
I/O, plus alerts and an overload cutoff. Measure those limits with the
selected transport and production edge configuration in place.

As a starting performance gate for that pilot, run a 30-minute legitimate
load soak and a 30-minute mixed-load soak at the documented operating
envelope. Require p99 tick work below 16.67 ms, zero dropped ticks and
continued join, gameplay, replication and checkpoint progress within the
published service limits. Test just above the approved envelope and
verify bounded queues and memory, preserved committed state and recovery
to normal progress when load subsides. Require an operator to demonstrate
disabling public ingress and returning to private-only operation. Release
approval needs the test report, a security review of the selected design
and documented capacity and incident procedures.

## Regression evidence to carry forward

`WireProtocol`, `UdpTransport`, `WorldReplication` and
`WorldReplicationColdChunks` cover framing, scope, ordering, resync and
transfer bounds. `VoxelActions` and `EntityReplication` cover authority
and inventory isolation in the protocol. `MultiplayerClosure`,
`MultiplayerSoak`, `ServerRestart`, `ServerMigration` and
`WorldCheckpoint` cover resource reuse, gameplay and recovery. Keep these
passing during transport changes and add the public gates separately.
See the [closure test commands](multiplayer-closure.md#reproduce-the-gates).
