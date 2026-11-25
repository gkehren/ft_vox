# Dedicated server (`ft_vox_server`)

The [hosted multiplayer closure report](multiplayer-hosting-closure.md)
records the two-client Windows run, CPU gates, fault coverage, and remaining
manual checks for Epic #282.

The standalone CPU-only authority host (#250) owns one `GameSimulation` and
one persistent world. Live mode listens for authoritative UDP sessions (#252),
simulates remote movement from player inputs (#253), replicates each
player's terrain interest (#254), and resolves remote break and place
intents on the simulation owner (#255).
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
`FT_VOX_GRAPHICS=ON`. It links to `ft_vox_core` and the UDP adapter.
The adapter uses Winsock and BCrypt on Windows and POSIX sockets and OS
entropy on Unix. The legacy Boost prototype remains a separate test target.
The server's dependency checks cover its include closure and generated link inputs.

Use one process per save directory. A new world defaults to seed 42. Omit
`--seed` on restart to use the stored seed. An explicit seed mismatch, an
invalid world name, or a save that the existing persistence layer cannot open
prevents startup. Starting the server on an older save migrates it to the
dedicated format. Keep a copy for use with the solo editor or an older build.

## Host from the game

Open **Multiplayer → Show panel → Host**. Select a saved server world or
type a new name. Hosted worlds use `server-saves/` relative to the game's
working directory. Names can contain up to 48 ASCII letters, digits,
spaces, dashes, or underscores. A CLI server with `--saves-root ./server-saves`
uses the same world storage and lock.

Choose a port, player limit, interest radius, terrain workers, and streaming
preset. Port **0** requests a free port from the OS. The player limit counts
network seats, including the local graphical client when it joins. Create
profiles under **Join**, then choose or type the existing profile to use in
**Host**. The **Host** field starts with the saved selection and can be
edited independently. Starting a new world binds its primary owner to the
profile shown when you click **Start server**, with or without auto-join.
Existing worlds keep their current role state. When the child reports
readiness, **Host** shows the bound address and assigned port. The profile
field also controls **Join hosted server** for a manual join or retry.

Loopback is the default. **Allow LAN / VPN connections** requires an IPv4
address assigned to an interface on this machine in an RFC 1918 range or
`100.64.0.0/10`. The form checks the range. The server's UDP bind checks
whether the address and port can be used on this machine. Check the Host
log if binding fails. The graphical client joins through the bound
address. Anyone who can reach the address may join
under the existing [private-network rules](multiplayer-public-network-readiness.md).

The game starts the sibling `ft_vox_server` executable as a separate
process and keeps a bounded log in **Host**. It reads readiness and
checkpoint status from the child's inherited output pipe. The separate
inherited stdin pipe carries the owner bootstrap before the server opens
the world. It then carries scoped requests from the Administration panel.
Replies share the private monitoring pipe. EOF requests graceful shutdown.
The local client joins with its ordinary profile.

**Stop server** closes the control pipe and waits for the process to exit
and report a committed checkpoint. Closing the game follows the same
sequence. A save or process failure leaves the game open with the error.
If stopping is delayed, **Force stop** becomes available with a
confirmation and a warning about losing recent changes. Escalation
targets the child owned by this launcher.

If the game is terminated abruptly, the OS closes the control pipe and
the child attempts its normal checkpoint and exit. A stalled child can
retain its world lock until it finishes or its operator intervenes.
Use the Host log to diagnose a busy port, a held world lock, startup
delay, migration failure, or a failed checkpoint. CLI servers still
support `--stop-file` and the existing stop signals.

## Hosted server monitoring

The supervising game can inspect **Host → Server status** and **Players**.
The roster comes from the dedicated server's admission table and covers
all admitted sessions, including players outside the local client's
terrain interest. **Syncing** means the server assigned an actor and is
waiting for route confirmation or activation. **Connected** means the
server activated the actor. **Disconnecting** means transport has begun
retiring the slot. This phase can pass between samples. Persistent
player IDs identify the selectable rows, and the offline count covers
only registered profiles outside the current roster. Capacity counts
network seats and excludes the simulation anchor.

The server offers a sample once per monotonic second. Uptime starts after
the UDP bind. Completed and dropped ticks are lifetime counters, with a
60 Hz target and dropped deadlines counted by the server scheduler.
The last committed checkpoint is shown separately from the supervisor's
startup, save, and exit status. Streaming counters measure chunk records
awaiting registration or generation and currently dispatched jobs.
World sends count staged or in-flight logical updates, including
baselines. Kernel socket queues are outside these measurements.
UDP received/sent totals count payload bytes since startup. Oversized
input datagrams are excluded. World bytes, retransmits and resyncs
measure the reliable terrain stream.

The launcher receives a bounded, versioned FTMN v1 snapshot over a
dedicated one-way inherited pipe. The record has fixed-width little-endian
metrics and at most 255 sorted player IDs and session phases. The
simulation uses a try-lock for sampling and offers the latest sample to a
single overwrite slot. A separate worker handles pipe backpressure.
The panel clears samples older than three seconds or when the server
leaves Ready. Role credentials, UDP session capabilities and player
addresses are outside the format. The monitor supplies no server-side
player RTT. Every client can still see its own RTT, loss, traffic and
prediction under **Network**. A client attached to another process has no
local feed for that server's status or roster.

## Owner and delegated roles

A new GUI world has one primary **owner**. The server reserves and persists
the selected profile's identity before binding UDP. Reopening through a
different GUI profile preserves that owner. CLI-created and migrated worlds
start unowned and keep that state until an operator provisions them. The
GUI requires provisioning before it can launch an existing unowned world.
This also applies if a first GUI launch stopped before recording the owner.
If that world has no dedicated checkpoint yet, run the CLI server first
with `--ticks 1`, then provision it offline.

The server classifies registered identities as owner, admin, or player.
An ordinary game connection supplies only a cleartext bearer credential.
Live administration requires the private inherited pipes and a separate,
server-verified local control credential. When creating a world,
the GUI creates or reuses `owner-control.key` alongside the selected named
profile's `player.key`. When reopening, the GUI reads the existing file.
If you select the registered owner, the server requires the matching
control key. Restore the private key file or use offline `roles recover`
if it is lost. Keep control keys and the server saves root accessible only
to the trusted local user and authorized operators.

Hosting requires a filesystem that enforces private file permissions.
From WSL, use a Linux filesystem for the profile and server saves. A
Windows drive mounted without Linux permission metadata may report mode
0777 even for files created with mode 0600. For the default relative save
paths, launch the Linux game from a private working directory under your
Linux home.

### Multiplayer Administration

Open **Multiplayer → Administration** in the app that launched the server.
The panel shows the authority verified for that launch, registered player
IDs and roles, moderation results, and the latest 64 committed audit
entries. Use **Refresh players and audit** after a new profile joins.
Command confirmations identify the world and, for player actions, the
persistent player ID. The **Players** section shows current admitted
sessions. Offline registered profiles also appear in Administration.

| Verified local permission | Operations |
| --- | --- |
| Owner | List registered profiles and audit, grant or revoke admin, kick, ban or unban, change allowlist membership or mode, and stop with checkpoint confirmation. The owner is exempt from admission restrictions and cannot be targeted. |
| Delegated admin | List registered profiles and audit and kick, ban or unban players. Owners and other admins are protected from delegated moderation. |
| Player, guest, or remote connection | No administrative actions. |

**Grant admin role** assigns the role. **Authorize local control** also
binds the selected saved local profile's private `owner-control.key` to the
chosen player ID. The server requires the saved profile to match the target's
registered profile. The delegated admin can exercise the role by launching
that world from this machine with the matching profile and credential after
the current launcher has stopped. Remote administration is unsupported,
including for profiles with an admin role. Wire v5 carries gameplay only.

The default admission mode is open to anyone who can reach the configured
loopback or private-network bind, subject to bans and kicks. An enabled
allowlist admits the owner and individually allowed profiles. A profile
must have registered in this world before it can be added. Ban takes
precedence over allowlist membership. A kick retires the admitted session
and holds off rejoin for 60 seconds, measured by the server's clock and
retained across restart. A ban lasts until unban. These restrictions
apply to the registered profile rather than the network address. In open
mode a new profile can still join. Use an allowlist to restrict admission
to known profiles.

Revoking admin retires the current gameplay session and deletes its local
authorization. The profile can reconnect as a player if admission rules
permit. Banning a profile, enabling the allowlist, or removing membership
from an enabled allowlist also retires the affected sessions at the next
control boundary. The server stops accepting gameplay from retired
sessions immediately. A client can learn of retirement from the departure
packet or its normal session timeout. Admission and local admin credentials
are checked separately.

**Stop and save server** requests an owner-authorized stop. The panel
confirms a save only after the child reports a committed checkpoint and
exits successfully. The launcher can still use **Host → Stop server** or
close the app to manage the lifetime of the child it created.

### Offline administration and recovery

Stop the server and wait for its checkpoint. The `roles` subcommands open
the world under the same native lock as normal hosting, without starting
UDP. Run `roles status SAVES_ROOT WORLD` to get the decimal world ID, role
revision, owner ID, admin IDs and recent audit entries. For a registered
player, use its persistent actor ID from the client's join log. Run
`roles status` again after a mutation to get the current revision.

| Command arguments after `ft_vox_server roles` | Authority and effect |
| --- | --- |
| `keygen NEW_CONTROL_FILE` | Creates a private, non-overwriting control key. Use a path in a private folder. |
| `status SAVES_ROOT WORLD` | Local inspection of role state and audit history. |
| `provision SAVES_ROOT WORLD WORLD_ID 0 PLAYER_KEY NEW_CONTROL_FILE` | Local world operator provisions an unowned, dedicated world. Supply the target profile's existing `player.key` and its control key. Open a solo or legacy save with the CLI server first to establish its dedicated checkpoint. |
| `grant-admin SAVES_ROOT WORLD WORLD_ID REVISION PLAYER_ID OWNER_CONTROL_FILE` | Current owner delegates to a registered player. |
| `revoke-admin SAVES_ROOT WORLD WORLD_ID REVISION PLAYER_ID OWNER_CONTROL_FILE` | Current owner removes a delegation. |
| `transfer SAVES_ROOT WORLD WORLD_ID REVISION PLAYER_ID OWNER_CONTROL_FILE NEW_CONTROL_FILE` | Current owner transfers ownership to a registered player and rotates the credential. The former owner becomes a player. Other delegations stay in place. |
| `recover SAVES_ROOT WORLD WORLD_ID REVISION PLAYER_KEY NEW_CONTROL_FILE` | Trusted local world operator explicitly rebinds an owned world, rotates the credential, and clears every admin. Generate a fresh control key and use the revision reported by status, including for missing, invalid, or stale authority. Admission policy is retained when recovery starts from a valid current authority. If that record is lost, restore a current backup to recover its restrictions. |

Pass file paths as arguments and keep their contents private. The local
operator is the OS principal with write access to the save directory and
its role authority. Provision and recovery are operator procedures. Routine
grant, revoke and transfer require the current owner control key. A world
ID or revision mismatch refuses the request. New role operations are
audited with their revision and affected player IDs.

Keep the latest role authority and identity registry when restoring
gameplay. A missing, corrupt, stale or foreign authority for an owned world
blocks startup. Restore a matching current authority, or use the explicit
recovery procedure with a fresh key after restoring a valid dedicated save
and identity registry. The [checkpoint and role format ADR](adr/multiplayer-checkpoints.md#world-role-authority)
specifies what to retain in backups.

## Connect two graphical clients

Build with `FT_VOX_GRAPHICS=ON`, then run the server and two clients in
separate terminals. With a single-config build in `build-vk`:

```sh
cmake --build build-vk --target ft_vox ft_vox_server
./build-vk/ft_vox_server --world multiplayer-smoke --saves-root ./server-saves --seed 42 --port 7777
./build-vk/ft_vox --connect 127.0.0.1:7777 --seed 42 --player-profile ./saves/profiles/alice
./build-vk/ft_vox --connect 127.0.0.1:7777 --player-profile ./saves/profiles/bob
```

On Windows, build with `--config Release` and use
`build/Release/ft_vox_server.exe` and `build/Release/ft_vox.exe` with the same
arguments. Launching `ft_vox` without `--connect` starts solo play.
`--connect` takes a numeric IPv4 address and port. With it, `--seed` is an
optional server identity check. `--world`, `--benchmark`, and `--view` are solo
options. Remote play uses the normal graphics and resource-pack options.
Use matching client and server builds for wire v5. The client creates a
profile on first launch and reuses its key across launches. Without
`--player-profile`, it uses `saves/.client-profiles/<IPv4>-<port>` relative
to its working directory. Use distinct profiles for two clients on the
same machine, and keep using the same explicit profile if the server
endpoint changes.

Wait for **Connected**, then move one player away from the shared spawn.
Players in range appear as color-coded upright boxes. Use **WASD**, **Space**
and **Shift** for walking, jumping and sprinting. Move to dry ground, then aim ahead
and use **LMB** to break a block where both clients can see the edit. Walk
over the drop and check that the collecting client's hotbar changes. Use
**1–9** or the wheel to select an item, **RMB** to place it, **Q** to drop one
item and **E** to inspect or rearrange the inventory. The status HUD shows
pending and accepted or rejected actions. Chunk and inventory displays
follow the server's updates.

Release the cursor with **C**, then use **Multiplayer → Disconnect** and
**Multiplayer → Reconnect**. The other client should lose the departing
avatar, then see the same player return. The reconnecting client's
position and inventory resume after the new session synchronizes. Walk far enough to leave and re-enter
the server's interest radius and check that terrain and entities return
after their updates arrive. Try joining with `--seed 43` or with the server
stopped to exercise the refusal and retry states. The graphical client
stays open and offers reconnect to the configured endpoint.

### Join from the game

Start `ft_vox` normally, release the cursor with **C**, and choose
**Multiplayer → Show panel → Join**.

1. Enter the server's numeric IPv4 address and port, such as
   `127.0.0.1:7777`. An optional expected seed checks that you reached
   the intended world configuration. Choosing a different endpoint from
   Favorites or Recents clears the expected seed.
2. Choose a **Saved profile**, type an existing name, or enter a new name
   and click **Create profile**. Names are case-insensitive and use up to
   32 ASCII letters, digits, dashes, or underscores, starting with a
   letter or digit. **Saved profiles** shows the first 32 names in
   alphabetical order. You can type a name to select any other profile.
3. Select **Connect**. Once terrain is synchronized, release text focus
   and use **C** to capture the cursor for gameplay.
4. Use **Disconnect** and **Reconnect** for the current endpoint and
   profile. To change either, edit the form and choose **Switch server or
   profile**. The game finishes the previous session before joining the
   next one. **Return to solo** resumes the parked solo world, or starts a
   new solo world when the game was launched directly with `--connect`.

**Recent servers** contains the last eight attempted endpoints. **Toggle
favorite** adds or removes the entered endpoint from a separate list of
eight favorites. The oldest entry drops off when its list fills.
Preferences in `saves/.client-profiles/join-settings.txt` contain addresses
and the selected profile name. Named keys use
`saves/.client-profiles/named/<name>/player.key`, the same key format used
by the CLI. To resume a named profile from the CLI, pass its directory to
`--player-profile`. Keep the key to retain your actor, inventory and
position on a server. Loading a profile with a missing or damaged key
fails until the key is restored. The **Copy endpoint** button copies the current numeric
address and port.

A persistent solo world is checkpointed before switching away. A save
failure leaves the solo world active. The game parks the solo simulation
and drains its render jobs. On return, it rebuilds the projection from the
same world and restores the camera, inventory, and pause state. The
[private-network boundary](multiplayer-public-network-readiness.md) applies
to in-game joins and CLI connections alike.

### Graphical projection limits

The renderer meshes verified server chunks through the normal async
mesh and upload path. It keeps up to 197 live chunk records, publishes up
to four changed chunk images per frame, and caps outstanding mesh and
light jobs at 16 and 8 respectively. The server's interest radius limits
the terrain available to render. Desired chunks appear as each complete
baseline arrives. Replica terrain uses a fixed plains tint for grass and
foliage because the wire images carry voxel materials. The local biome
map, creative flight, isometric inspection and world pause are solo
features.

The camera follows locally predicted movement with immediate mouse look.
Up to 23 other players use delayed interpolation. Dropped items and mobs
still use their own snapshot streams, with up to 38 items and 42 mobs.
Avatars use a solid box and receive scene occlusion. Current snapshots
define which entities are present. The client
allows one reliable action at a time and waits for the server before
changing terrain or inventory. Packet loss can briefly delay streaming
or pose updates. The transport retries reliable messages and rejoins on
a bad checksum or a revision gap. The graphical host clears its world,
avatar, inventory, and item projections when that session retires. A join
timeout, compatibility refusal, or explicit disconnect leaves the client
at the connection screen until it is restarted or reconnected.

The input scheduler estimates when each command will reach the server.
It starts with a matching single-attempt Hello/Welcome round trip and
updates from Action acknowledgments when the action was sent once.
The estimate includes pump and server scheduling time. It assumes
roughly symmetric paths and uses a 50 ms fallback RTT until a usable
measurement arrives. The client targets three ticks beyond estimated
arrival and polls snapshots after swapchain acquisition, close to the
input send. Its server clock uses the least delayed snapshot observation
from the last two seconds. A prolonged gap or a five-tick phase jump
starts a fresh timing window. When a frame falls behind by up to three
input ticks, the client sends those consecutive ticks so the server can admit any that
are still open. A larger gap starts a new batch near the estimated
arrival. The server still admits only the next six ticks at its receive
boundary. The HUD reports degraded input timing
if snapshots become too old or processed input stops advancing while
the client is sending. A large path change between RTT observations can
still interrupt movement. The reliable sender waits for an ACK before the
next chunk fragment. Baseline compression reduces the number of round
trips needed to join. The delayed-UDP tests measure the first playable
chunk separately from steady movement.

See [Multiplayer smoke](multiplayer-client-smoke.md) for the physical GPU
run and the deterministic movement measurements.
The [multiplayer closure report](multiplayer-closure.md) records the
resource baseline, ownership audit, prerequisites and remaining work.

## Server options

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
observer. At the start of each 60 Hz server tick the owner drains the
connection inboxes and resolves admitted voxel actions in ascending
`PlayerId` order using the last completed poses. It then advances each
walking player in the same order. Each player runs two 120 Hz steps through
the shared `WorldCollisionView`, including the usual collision and unknown-terrain
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
actual port. Use numeric IPv4 on loopback or a trusted private network.
Wire v5 sends player keys and session capabilities in cleartext. Keep the
default loopback bind for local use. For remote private play, restrict access
to intended peers on a trusted LAN or authenticated VPN. See
[deployment guidance and public network release gates](multiplayer-public-network-readiness.md).

- A compatible Hello reserves one remote slot. The tick owner looks up the
  player key and offers the existing actor ID or reserves a new one. Welcome echoes the join nonce
  and carries the protocol version, player ID, capability, world ID, epoch,
  seed, generator compatibility, tick and edit watermark. Repeating the
  same Hello returns the original Welcome. The world ID is durable, and
  the process generates a new epoch at startup.
- A valid scoped packet confirms the join. The tick owner restores the
  actor or finds a safe walking spawn around the bootstrapped center.
  Confirmation of a new profile records its key before gameplay. If no safe loaded column exists,
  the server retires the session and attempts a scoped disconnect. A
  provisional slot has no body in the world and expires after 5 seconds
  without confirmation. Established peers send a heartbeat after
  1 second of outbound inactivity and expire after 10 seconds without newly
  admitted inbound traffic or a progressing ACK. Brief loss uses the same
  session. A replay cannot extend it.
- An ordered Disconnect or Resync retires the slot at a tick boundary.
  If the final control or ACK is lost, the departure falls back to the
  reliable or idle timeout. A later Hello uses a new nonce and gets a new
  capability and session generation. The same key resumes the same player
  ID. A differently-nonced Hello cannot
  replace an active session at the same endpoint. Rejoin after leaving or
  waiting out the old lease. A duplicate active profile is refused.
- The IO worker validates framing, endpoint, capability, epoch, rate, timing
  and sequence before handing typed input to the simulation owner. Each slot
  has six input slots, one latest snapshot slot per fresh family, one pending
  inventory, one pending action or result, and one world send slot. A full
  reliable slot leaves its receive cursor unchanged. The adapter uses a
  one-frame reliable send window, retries after 150 ms subject to the rate
  budget, and closes after 5 seconds without progress. The maximum remote
  allocation is the player cap minus the local anchor. The IO worker reads
  up to 128 datagrams per pump and drops datagrams over 1,200 bytes. It applies
  token buckets by family, peer and process, plus a fixed 256-bucket IPv4
  discovery table. Addresses that hash to the same discovery bucket share
  that budget.

## Movement and snapshots

An Input contains a bounded world X/Z vector and sprint, jump-edge and swim
flags. The endpoint and session capability select the actor. The UDP gameplay
surface accepts only these controls for movement. Spawn and other
administrative mutations are server-owned, and the stop file is a separate
local control.

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
scheduled tick. Disconnect and timeout discard the old input queues and
controller timers. The offline record retains the pose and velocity.

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
session and returns the sequence of the datagram sent. `latestPlayerSnapshot()`
and `scope()` expose the latest accepted
state and its epoch. The adapter clears the snapshot on departure.

### Client movement and presentation

The graphical client advances its local view as soon as it sends movement.
It runs the shared walking controller for two 120 Hz substeps per input
tick. On a snapshot it restores the server body and the owner's jump
timers. It drops inputs acknowledged by the server or whose intended tick
has already completed. It replays the remaining commands in intended tick
order and uses neutral controls for gaps, as the server does.

The client retains at most 32 timestamped input records and simulates at
most 32 ticks beyond a snapshot. A frame sends at most three scheduled
inputs. A 250 ms snapshot or input gap discards speculation and waits for
the next snapshot. A teleport, terrain availability change, correction of
0.75 blocks or more, or session retirement drops the old prediction and
visual offset. The client tracks up to 16 chunks actually sampled during
body validation, movement, replay and camera correction. A sampled chunk
changing revision, losing its backing or becoming readable retires the
old prediction. If the footprint overflows, the client also discards
prediction and rebuilds the footprint from the next snapshot. Physics
treats a chunk as unknown while its published replica and verified wire
versions differ.

The local camera advances on a render-frame clock through the predicted
120 Hz substeps. It keeps five recent tick poses and interpolates up to
three ticks behind the latest prediction to absorb uneven input batches.
It holds the latest pose when the buffer runs out. Small corrections may
retain up to 0.25 blocks of visual offset, fading over 100 ms. The body
must sweep clear through available terrain to the interpolated and
corrected pose. A blocked sweep uses the current predicted pose.
While a replica update is pending, the camera holds the last validated
pose for the same continuous motion. The hold supplies no input to
physics. Prediction resumes from a fresh authoritative snapshot after
the required backing is available and the body passes collision
validation. If new backing puts a solid block in the held pose, or a
teleport or session transition retires that pose, the client shows the
terrain-waiting view until it has a usable body. Predicted positions are
for the camera only. Voxel targeting uses the authoritative eye, and the
server validates actions against its own world and inventory.

Remote avatars use eight snapshots and a nominal six-tick (100 ms)
playback delay, capped at twelve ticks (200 ms). Playback interpolates
between ticks for actors present in the latest snapshot and in the
interpolation history. An absent actor disappears immediately and
teleports start a fresh path. Playback holds the last pose under loss
with zero extrapolation and hides the avatar after 300 ms without a fresh
snapshot.

Release the mouse with **C**, then choose **Multiplayer → Show panel**.
The dockable window is available from solo play and CLI connections.
**Join** shows the current server endpoint, connection state, and the
same Disconnect/Reconnect actions as the menu. **Network** shows this
client's ping estimate and sample age, snapshot age and estimated loss,
receive/send throughput, pending predicted inputs, corrections, and hard
resets. Hover a metric for its definition. Players and Administration
are placeholders for future server features.

Ping comes from unambiguous Hello/Welcome or Action/ACK exchanges. A hotbar
selection or block action can refresh the estimate, and the sample age
shows how long ago it was measured. Estimated snapshot loss comes from
gaps in the observed 20 Hz stream and includes skipped server captures and
client frame coalescing. The client needs two snapshots for a loss estimate
and one complete traffic window for throughput. Missing measurements read
**Unavailable**. Disconnect, timeout, and resync clear the session's
measurements. See the [panel smoke steps](multiplayer-client-smoke.md#multiplayer-panel-283)
for layout checks and a repeatable impaired-network fixture.

For recurring movement stutter, set `FT_VOX_MOTION_TRACE` to a CSV
path before launching the graphical client. The server uses its normal
command. In PowerShell, for example:

```powershell
$env:FT_VOX_MOTION_TRACE = Join-Path $PWD ("motion-alice-{0:yyyyMMdd-HHmmss}.csv" -f (Get-Date))
.\build\Release\ft_vox.exe --connect 127.0.0.1:7777 --player-profile .\saves\profiles\alice
```

After connecting, stand still briefly, then walk where the problem occurs
and quit normally. The first connection uses the configured filename if
it is free. A new join or a server or profile switch creates a separate
file, such as `motion.session-2.csv`. The client skips names already on
disk, including trace files from earlier runs. The fast Reconnect action
and automatic resync keep the same file while tracing is active. Each file records at
most 60 seconds or 12,000 frames.
The CSV links frame timings and camera positions to input ticks, server
acknowledgments, predicted poses, terrain waits and correction counters.
It contains gameplay telemetry and omits profile keys and session
capabilities. Unset the variable to turn off tracing.

## Voxel actions

Send Break or Place with the target cell, its observed chunk generation and
revision, and aim expressed in turns. The server checks the exact chunk
version and traces from the actor's authoritative eye up to eight blocks.
It stops on unknown terrain and tests the first effective non-air voxel,
including accepted edits whose backing writes are still pending. Break must
name that voxel. Place must name the empty cell across the face the ray
entered. The simulation then applies the shared gameplay rules for
breakability, walking body overlap, and the actor's selected hotbar stack.
An accepted break creates at most one drop and an accepted placement spends
one item. A rejection applies no edit or item effect. The trusted simulation
grant API can provision a fixture's selected stack.

Use the target chunk's `version` from `ClientTransport::chunks()` when
calling `sendAction(action, now)`. It assigns consecutive IDs beginning
at one and queues a reliable request. An empty return means the action
was invalid, the connection was unavailable, or the session still had an
outstanding action or unread result. The client pump retries automatically.
`takeActionResult()` returns the action ID, completed tick, global next-edit
watermark, accepted edit ID or zero, and the result code. A valid request
with a stale version is settled as `StaleRevision`. For a new attempt, use
the current projection version. The result settles the command. Terrain
changes arrive separately through the ordered world stream.

## Inventory and entity replication

Only positioned remote players participate in dedicated-server item pickup.
The simulation steps the item population once at 60 Hz and offers each
eligible drop in ascending PlayerId order. Inventory capacity can leave a
remainder for the next eligible player on the same tick. The server alone
resolves pickup, slot changes and drops. Each client receives its own 36 slots and hotbar
selection through `ClientTransport::inventory()`. Remote inventories start
empty for each session. A slot command's reliable result arrives after the
client has received the corresponding inventory revision or a newer one.

Passive mobs share one 60 Hz simulation and one population cap. The
server rotates spawn scans across positioned remote players within the existing
global spawn budget and retires mobs outside every observer's range.
With no positioned remotes, there are no mob spawn scans and existing mobs
retire. The local compatibility anchor serves the server's world lifecycle.
`ClientTransport::itemSnapshots()` and `mobSnapshots()` expose the previous
and current samples. Use their server ticks (60 Hz) and stable IDs for
interpolation. The current sample defines which entities are visible.
An ID can return after interest re-entry. The client clears these views
and its inventory when the session retires or resyncs.

Each peer receives up to 10 full snapshots/s per entity family. A full
38-item datagram is 1,193 bytes and a full 42-mob datagram is 1,179 bytes.
Together they add at most 23,720 wire bytes/s per peer before UDP/IP
headers. A one-mob snapshot measures 72 bytes on loopback. The server
selects the nearest in-interest entities when a packet is full, and each
family has a process-wide cap of 256 frames/s. The host uses fixed-size
selection buffers, and the client holds two fixed-size samples per
family. Fresh updates replace unsent ones. The wire format and delivery
rules are described in the [protocol ADR](adr/authoritative-wire-protocol.md#nearby-entities).

A future CPU-only load benchmark can measure per-client snapshot rate and
age, server tick latency and client progress at 16, 32 and 64 clients.

Malformed requests, old or skipped IDs, and wrong scopes are discarded
before consuming an action slot. Reliable receipt ACKs are independent of
gameplay results. The server retries the same result until it is acknowledged.
A departure can discard undrained requests and unread results. An edit
already applied by the owner stays in the world, even if its sender never
sees the result. Rejoin uses a fresh scope and baseline, starting at action
ID one. The client clears the previous action state and never automatically
resubmits it in the new scope.

## World replication

The owner derives each peer's circular chunk interest from its authoritative
feet position and `--interest-radius`. It uses the same disk as terrain
residency. Zero disables chunk replication. It selects the nearest published
chunks first, including the spawn chunk when available. Each baseline
contains a full 65,536-byte material image. The owner applies bounded
run-length encoding and sends the raw image if encoding cannot shrink it.
The 60-fragment maximum covers raw images. The checksum covers the
decoded material bytes. The client uses the negotiated seed and
generator/catalog compatibility as world
metadata and installs the bytes sent by the authority. The baseline carries
one coherent incarnation, logical revision and next-edit watermark, including
persistent overrides and any accepted pending edit overlay.

The owner journals each accepted target edit at the same boundary as the
simulation, independent of when terrain workers or deferred publication write
the backing. A client's outgoing deltas follow its baseline in reliable order.
Each move out of interest or replacement of a resident incarnation schedules
a matching removal before any new baseline. A transfer already underway can
finish after a teleport. Its removal takes priority at the next send boundary,
and subsequent subscriptions follow the player's new position.

`ClientTransport::chunks()` exposes the current, checksum-verified material
arrays, chunk versions and edit watermarks to the CPU pump owner. Pair the
view with `welcome()` and `scope()` to key derived state by world and epoch.
Arrays use y/z/x indexing with wire material bytes and AIR 255. The
graphical client derives meshes and lighting locally from those arrays.
Revision gaps, malformed baselines, checksum failures and assembly timeouts clear the
projection and trigger a fresh join. `requestWorldResync(now)` takes that same
path. A fresh scope restores the rostered actor using the same profile key.
After a server restart or an idle timeout, the caller can `connect(now)` again.
The fresh capability and epoch keep old packets out of the new receive window.

The host keeps up to 64 pending deltas per peer and a 4,096-entry per-tick
logical edit journal. A gap or overflow requests resync. The transport allows
one 64 KiB baseline payload buffer per peer and at most 16 across the server,
for 1 MiB of payload buffers. The single tick owner uses one additional
64 KiB capture buffer while packing a baseline. On the client, the
radius-eight disk caps installed material storage at 197 chunks (about
12.3 MiB), plus a 64 KiB assembly buffer and 64 KiB of temporary decode
storage. The per-peer and process-wide bandwidth buckets apply to retries as
well as first sends. `world_tx_bytes`, `world_tx_frames`, `world_retransmits`
and `world_resyncs` in `SERVER` records report replication traffic and resync
requests. `ServerTransport::worldTraffic(playerId)` exposes the same counters
for active peers.

At 150 ms RTT, the eight-client benchmark reaches the first playable chunk
for every client in 1.57 s on a clean link and 2.02 s with injected loss.
The steady-state test then streams a second neighborhood. See
[cold chunk delivery](benchmarks/275-cold-chunk-delivery.md) for the
comparison with raw v4, traffic and retry counts, and the CTest for
contended transfer slots. Stop-and-wait still costs one round trip per
fragment. The assembler triggers resync after five seconds without progress
or twenty seconds total.

## Shutdown and recovery

On Linux and other POSIX systems, SIGINT and SIGTERM request a graceful stop.
On Windows, use Ctrl+C or Ctrl+Break in the server console, or the stop
file. The handler only sets a flag. The owner stops ticking, joins the IO
worker, retires remote sessions, then drains
outstanding generation with a 30-second cooperative deadline, resolves
accepted edits still deferred on unloaded records, and runs the existing
flush and gameplay checkpoint commit. If the barrier or publication fails,
the previous manifest remains the recovery point and the process reports
the failure. Terrain jobs complete before teardown releases their
world state, and the owner then joins the pool. Filesystem calls and worker
joins rely on the OS to return. The UDP worker attempts a final scoped
Disconnect for confirmed sessions where the reliable send window has room.
Other clients recover through their timeout.

The active server checkpoint carries online and offline player records,
the local compatibility anchor, shared voxel overrides and dropped items.
A disconnect captures the actor in memory. Graceful stop publishes all
gameplay state together. Up to 4,096 profiles are retained without
expiration. Restore a pre-migration backup to return to a solo or older
build. See the format, migration and recovery rules in the
[multiplayer checkpoint ADR](adr/multiplayer-checkpoints.md).

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

Run the dedicated tests with `ctest --test-dir build-cpu -R 'ServerHost|ServerLifecycle|ServerRestart|WorldCheckpoint|UdpTransport|WireProtocol|WorldReplication|VoxelActions|EntityReplication' --output-on-failure`
(add `-C Release` for Visual Studio). They cover clock stalls and cadence,
sync/async fixed steps, deferred edits at shutdown, saved-world restart,
write/commit failures, signal/admin termination, port 0, occupied ports,
two-client join and rejoin, malformed and oversized packets, rate and queue
limits, and replay. The movement fixture compares two IPv4 loopback players
against local controller scripts at every tick with a wall, a jump, lost
samples, a simulated host stall and reconnect. `UdpTransport` also uses an
injected steady clock for timeout and retransmit decisions. Its threaded
smoke test exercises worker shutdown. `WorldReplication` checks two-client
voxel digests across shared and disjoint interest, deferred generation and
edits, fast teleports, unload/re-entry, faults and revision gaps, generator
mismatch, queue limits, server restart, and delivery on the IO worker.
`VoxelActions` verifies simultaneous break and place with actor IDs
ordered differently from transport slots, conservation, stale and invalid
requests, lost ACKs/results/deltas, deferred backing writes, chunk refetch
after departure, and checkpoint recovery in a fresh epoch.
`EntityReplication` covers two simultaneous pickup contenders, partial
pickup count changes, private inventory and action delivery, packet loss
and replay, interest boundaries and caps, expiry, and item allocator
recovery after reconnect and restart.
`ServerHost` and `EntityReplication` also cover pickup and mob observation
with the compatibility anchor at spawn, including an empty remote set.
`ServerRestart` drives two profiles against separate live server processes.
It checks an offline inventory, a placed block and a dropped item across a
graceful save, then verifies whole-generation rollback after killing the
next process. It reports the committed size and stop-to-exit duration.
`MultiplayerClosure` combines two-client gameplay and fault recovery
in one repeatable script. `MultiplayerSoak` logs queue occupancy,
terrain and baseline allocations, process usage and checkpoint costs.
Run the closure group with
`ctest --test-dir build-cpu -C Release -L multiplayer -V`.
CTest returns nonzero on failure.
