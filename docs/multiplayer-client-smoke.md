# Graphical multiplayer smoke

The [issue #260 closure report](multiplayer-closure.md#physical-gpu-and-remaining-work)
records the latest physical-GPU dual-client startup, restart and strict
visual results. Manual keyboard and mouse coverage is tracked in
[#277](https://github.com/gkehren/ft_vox/issues/277).

## Local camera cadence for #277

The maintainer reported seeing the other player, walking, breaking and
placing blocks, and dropping items on wire v5 with a dedicated server and
two separate PowerShell clients. The same session showed visible stutter
in local walking. That check preceded the camera fix.

The local camera previously drew the final body position of each 60 Hz
network prediction tick. It now advances through the same 120 Hz physics
steps on the presentation clock. The `WorldReplicationClientTick`
regression samples the camera every 7 ms while walking at 4.5 blocks/s.
Before the fix, its per-frame speed ranged from 0 to 10.714 blocks/s.
After the fix, it measures 4.5 blocks/s throughout the steady walking
window. The UDP relay also measures 4.5 blocks/s at simulated render
rates of 60 and 144 FPS with 0 and 150 ms nominal RTT on clean links.
The packet-loss fixture checks reconciliation and convergence after a
frame stall.

```sh
ctest --test-dir build-cpu -C Release -V -R '^WorldReplicationClientTick$'
```

The wire-v5 Release smoke ran on Windows with MSVC, an RTX 4070 Ti,
driver 621.24, Vulkan 1.4.357 and `FT_VOX_VALIDATION=1`. Two graphical
processes connected to a seed-42 loopback server with isolated profiles.
The authority streamed terrain and reported zero dropped ticks on both
runs. It committed generations 2 and 3 on graceful stops. Both clients
timed out after the server stopped. Fresh client processes reused the
profiles, joined a fresh wire epoch and resumed the same actor IDs.
The physical-GPU `VisualRegression` and `EngineReload` tests passed.
The strict visual run passed all 24 scenes.
Validation reported the two known overlay-related swapchain VUIDs below.

The Computer Use plugin could not connect to the native Windows pipe.
The maintainer's interactive movement retest is recorded below. The
remaining keyboard, mouse and menu checks are tracked in #277.

### Rollbacks on live Windows loopback

The maintainer still felt frequent rollback after camera interpolation
was added. A temporary Release diagnostic drove held walk input on a
flat strip with two graphical clients and a separate dedicated server.
It recorded scheduled inputs, authoritative snapshots, prediction
corrections and per-frame camera positions during the steady section of
the walk. This exercised the normal swapchain and UDP threads.

The frame loop polled snapshots before the swapchain wait. The scheduler
then estimated input arrival from an older observation and skipped ticks
that could still have been accepted. Each missing tick applied neutral
controls at the authority. A delayed snapshot also shifted the estimate
abruptly. The client now polls immediately before gameplay input after
the swapchain wait, adjusts the running clock gradually, and fills short
gaps with consecutive commands. Large gaps retain the bounded resume path.

| Windows loopback, steady walk | Camera interpolation alone | With input scheduling fix |
| --- | ---: | ---: |
| Skipped target ticks | 29 of 442 | 0 of 424 |
| Prediction corrections | 12 | 0 |
| Per-frame camera speed | 0–12.21 blocks/s | 4.04–4.52 blocks/s |

The ideal walking speed is 4.5 blocks/s. The deterministic scheduler
regression covers late snapshot polling and a single delayed render
frame. The temporary diagnostic input and logs are outside the shipped
client.

### Late movement inputs after short action RTT samples

A trace from the reported playthrough showed regular frame times around
6 ms during the severe rollbacks. From roughly 45 seconds onward, server
snapshots kept arriving while the processed-input acknowledgment advanced
only in bursts. Earlier action acknowledgments had moved the RTT
estimate from roughly 35 ms toward 9–12 ms. Command ticks were still
being sent. The running server clock was following delayed snapshots
and the reduced RTT left insufficient lead for those commands to reach
the open input slots.

The scheduler now keeps the least delayed observation in a two-second
snapshot window and targets three ticks beyond estimated arrival. The
server keeps its six-tick admission policy. The regression combines
short action RTT samples with 6–56 ms snapshot delivery variation and
models the server closing the next tick's inbox. It recorded 227 late
inputs out of 240 before the fix and zero out of 241 after it.
In a separate two-client Windows run, repeated hotbar-select actions
produced an RTT estimate of 8.7–14.9 ms during steady walking. The
walking client recorded zero prediction corrections, zero hard resets
and no frame-to-frame reversal.

### Interactive movement retest

The maintainer retested the Windows Release client on loopback at commit
`111af3f` and confirmed that movement was smooth and the reported
rollbacks were gone. Their earlier interactive test also confirmed
remote-player visibility, shared break/place and item drops.

The remaining #277 checklist includes jump and sprint, contested pickup,
inventory rearrangement, cursor capture and focus transitions, walking
out of and back into interest, menu reconnect and an interactive solo
comparison. The startup and graceful server restart evidence above
covers separate Release client processes.

## Wire v4 profile restart

Two Windows Release clients created distinct profiles with
`--player-profile` and joined a seed-42 dedicated server on loopback.
The server streamed 111 terrain frames by the first sampled metric
of the run. After a graceful stop, it bound a different UDP port and
both fresh graphical processes reused their original profile directories.
The server logged restoration of actors 2 and 3, streamed 127 terrain
frames by the corresponding sample and committed generation 3 on stop.

Both clients initialized Vulkan 1.4.357 on an NVIDIA GeForce RTX 4070 Ti.
With validation enabled, logs showed the existing swapchain and image-view
messages `VUID-VkSwapchainCreateInfoKHR-imageFormat-01778` and
`VUID-VkImageViewCreateInfo-usage-02275` recorded in earlier runs below.
The window-capture helper was unavailable. Server and client logs supplied
the evidence for this run. The CPU `ServerRestart` fixture verifies the
resumed pose, hotbar, inventory and world state in greater detail.

## Terrain and camera correction for #273

Tested client commit `676f9380b1831a45f710b67679eddd0067b976b6`
on Windows 11 Pro 10.0.26300, with an NVIDIA GeForce RTX 4070 Ti,
driver 621.24, and Vulkan 1.4.357. The uninstrumented client was built
with MSVC in Release mode with `FT_VOX_GRAPHICS=ON`. The matching wire-v3
dedicated server used the Windows MSVC CPU-only Release build. Each
process used an isolated working directory, and the clients ran with
`FT_VOX_VALIDATION=1`.

```powershell
ft_vox_server.exe --world multiplayer-smoke --saves-root server-saves --seed 42 --port 0 --stop-file stop-initial --metrics-every 180
# Start two clients with the port printed in the server's ready record.
ft_vox.exe --connect 127.0.0.1:<port> --seed 42
# Standalone solo run.
ft_vox.exe --seed 42 --benchmark 5
```

- Both clients showed the synchronized-terrain waiting state, connected
  as distinct players in the same epoch, and reached 29 loaded chunks
  with 16 and 14 draw entries. Both reported `Disconnected` when the
  server saved and stopped.
- The restarted server reopened the save and a newly launched client
  joined a different world epoch. It reached 29 chunks and 14 draw
  entries, then disconnected when the server stopped. In a separate
  attempt that stopped the server while baselines were still streaming,
  the client entered resync before the harness ended.
- The five-second solo benchmark wrote its report. `EngineReload` and
  `VisualRegression` passed on the same GPU.
- Validation reported the known swapchain warnings
  `VUID-VkSwapchainCreateInfoKHR-imageFormat-01778` and
  `VUID-VkImageViewCreateInfo-usage-02275`.

Interactive SDL gameplay remains **unverified**. The Windows input
helper could not connect to its native pipe after retries and a reset.
The run covers real Vulkan startup, terrain rendering, server shutdown,
and fresh-process join after restart. Keyboard movement and yaw, jump and
sprint, mouse edits during movement, inventory drag/drop, focus and
capture transitions, boundary walking, menu reconnect, and interactive
solo play still need a manual pass.

The CPU regressions exercise deferred publication with and without a
worker lease, support removal and restoration, teleport and epoch reset,
unrelated edits, sampled obstacles, positive and negative chunk seams,
unavailable-to-readable backing, and dependency overflow. The existing
movement profiles still converge to zero position error, and a cold join
at 150 ms RTT still reaches spawn terrain and inventory in 9.386 seconds.

## Movement prediction and interpolation for #258

`WorldReplicationClientTick` runs the production UDP client and server
transport through a virtual-time IPv4 relay. The movement fixture uses a
flat walking surface and compares the first 0.01-block displacement with
a direct, no-network step of the shared controller. The RTT column shows
the configured path delay. The measured handshake RTT also includes
server tick and pump time.

| Nominal RTT | Onset in ms, direct / predicted / authority | Peak pending inputs | Corrections / peak blocks | Snapshot gaps | Final prediction error |
| --- | ---: | ---: | ---: | ---: | ---: |
| Loopback | 17 / 0 / 34 | 5 | 0 / 0 | 0% | 0 blocks |
| 30 ms | 17 / 0 / 33 | 6 | 0 / 0 | 0% | 0 blocks |
| 80 ms | 17 / 0 / 67 | 10 | 0 / 0 | 0% | 0 blocks |
| 150 ms | 16 / 0 / 100 | 14 | 0 / 0 | 0% | 0 blocks |
| 80 ms join, 40–100 ms movement RTT with faults | 17 / 0 / 84 | 16 | 37 / 0.321 | 20.7% | 0 blocks |

The fault run injects jitter, loss, delayed duplicates, reordering, a
snapshot gap and a 500 ms frame stall. The movement-only run uses
approximately 12–15 KiB received and 16–19 KiB sent over six virtual
seconds. The direct controller baseline uses zero network bytes. A
separate 150 ms cold join published the spawn chunk and private
inventory in 9,386 ms without a resync.

Two Windows Release graphical clients ran against the seed-42 dedicated
server on an NVIDIA GeForce RTX 4070 Ti with driver 621.24 and Vulkan
validation enabled. Both joined the same epoch with distinct player IDs,
reached 29 loaded chunks and nonempty draw lists, and reported
`Disconnected` after an orderly server stop. `EngineReload` and
`VisualRegression` passed on that GPU. The live clients reported the
documented swapchain validation warnings
`VUID-VkSwapchainCreateInfoKHR-imageFormat-01778` and
`VUID-VkImageViewCreateInfo-usage-02275`. The Windows input helper could
not connect to its native pipe, so interactive keyboard and menu
verification remains pending.

## Phase D1 for #257

### Input timing correction

Commit `5d4cb02aa3f2d9cd5c74f8298a2d14436f692294` was built with MSVC
in Release mode with `FT_VOX_GRAPHICS=ON`. The uninstrumented binaries
ran on Windows 10.0.26300 with an NVIDIA GeForce RTX 4070 Ti,
driver 621.24 and Vulkan 1.4.357. The seed-42 smoke save included the
same small dry stone platform as the earlier run.

The server and both clients used separate working directories under
`build/issue272-sdl`. These were the launch arguments.

```powershell
ft_vox_server.exe --world multiplayer-smoke --saves-root server-saves --seed 42 --port 7777 --stop-file stop --metrics-every 180
$env:FT_VOX_VALIDATION = '1'
ft_vox.exe --connect 127.0.0.1:7777 --seed 42
ft_vox.exe --connect 127.0.0.1:7777
```

Both uninstrumented clients reported **Connected** with different player
IDs in the same server epoch. Each reached 29 loaded chunks and a
nonempty draw list. A separate normal solo launch with `ft_vox.exe --seed 42`
also rendered terrain, and `EngineReload` and `VisualRegression` passed
on this build. The two networked windows reported the known swapchain validation
warnings `VUID-VkSwapchainCreateInfoKHR-imageFormat-01778` and
`VUID-VkImageViewCreateInfo-usage-02275`. See the
[Vulkan validation guide](vulkan-validation.md).

The native desktop input helper could not connect. Physical keyboard,
mouse, inventory, focus changes, both players' interaction scenarios,
and the Multiplayer menu still need a manual pass. The image below
belongs to the earlier diagnostic run.

`WorldReplicationClientTick` exercised unmodified server admission and
player physics over a controlled IPv4 relay. The stable-window counts
track which sent sequences appeared as processed at the authority.

| Nominal RTT | Stable window processed | Result |
| --- | --- | --- |
| Loopback | 48/48 | Movement, neutral stop and one jump |
| 40 ms | 47/47 | Sustained movement |
| 80 ms | 48/48 | Sustained movement |
| 120 ms | 48/48 | Sustained movement |
| 40–100 ms with jitter | 30/48 | Movement resumed after loss, reordering, a snapshot gap and a 500 ms frame stall |

The relay leaves chunk transfer out of the timing fixture. The existing
stop-and-wait baseline can hit its five-second assembly deadline before
a cold join is playable at higher RTTs.

### Earlier diagnostic run

The Windows Release client ran on an NVIDIA GeForce RTX 4070 Ti with two
game processes and one IPv4 loopback server. The world used seed 42 and a
small stone platform prepared with the headless runner, so the camera
and both players started above the water. A temporary diagnostic build
sent movement and actions through the graphical client's usual
submission methods and read back the displayed Vulkan frame. The
diagnostic input and readback code are outside the shipped client.

![A remote avatar, an accepted action, and cobblestone in the local hotbar](images/multiplayer-client-pickup.webp)

- Two distinct player IDs joined the same world and epoch. One player
  walked away from the shared spawn, and each client rendered the other
  avatar from its own viewpoint.
- Client A broke the stone at `(8, 69, 8)` and collected one
  cobblestone. Client B saw the same cell turn to air and still had an
  empty inventory. A placed the collected item and its inventory
  returned to empty. B then broke another block and collected its own item.
- After an orderly server stop, both running windows reported zero
  chunks and zero draw entries. The dedicated server checkpoint saved
  the edits. A restart of the same save followed by reconnect gave
  both clients a new session scope and render epoch in the same processes.
- A client expecting seed 43 refused the seed-42 server. A client pointed
  at a vacant UDP port timed out. Both windows stayed open with zero
  chunks and zero draw entries, and the client offered reconnect.

The Windows CPU-only suite passed 38 tests. The existing loopback tests
cover interest exit and return, packet loss, revision gaps, generator
compatibility, private inventory, and distinct action results.
`WorldReplication` also checks a replica chunk's occupancy and seam
after a delta, retirement while a worker holds a lease, and re-entry
with a new verified image. The Windows Vulkan `EngineReload` and
`VisualRegression` smoke tests passed, and a five-second solo benchmark
completed and wrote its report. The interactive window run
reported the existing swapchain validation warnings documented in the
[Vulkan validation guide](vulkan-validation.md).

See the [server guide](dedicated-server.md#connect-two-graphical-clients)
for the normal two-process commands and keyboard controls.
