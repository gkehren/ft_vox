# Graphical multiplayer smoke for #257

## Input timing correction

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
nonempty draw list. The logs reported the known swapchain validation
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

## Earlier diagnostic run

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
