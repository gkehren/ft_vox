# Multiplayer closure evidence

Issue [#260](https://github.com/gkehren/ft_vox/issues/260) adds the final
automated evidence for [Epic #245](https://github.com/gkehren/ft_vox/issues/245).
The supported deployment scope is loopback and trusted private networks.

## Reproduce the gates

```sh
cmake -S . -B build-cpu -DFT_VOX_GRAPHICS=OFF -DFT_VOX_CPU_ONLY_BUILD_CHECK=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpu --config Release --parallel
ctest --test-dir build-cpu -C Release --output-on-failure
ctest --test-dir build-cpu -C Release -L multiplayer -V
```

These commands work on Linux and Windows. See the [dedicated server guide](dedicated-server.md)
for live builds, isolated player profiles, stop-file shutdown and the two-window
playthrough. The [CPU workflow](../.github/workflows/cpu-boundary.yml) runs
Linux and Windows CPU suites, Linux ASan/UBSan and targeted TSan, and Linux
graphical compilation. Run physical-GPU tests separately using the
[GPU validation profile](gpu-validation.md).

| Gate | Assertion |
| --- | --- |
| `HeadlessMultiplayer` | Existing canonical headless scripts compare state digests across worker counts, player races, residency and checkpoint reopen. |
| `MultiplayerClosure` | Two independent UDP clients join one production host. Their loaded images match the owner by generation, revision and material hash. They mine and replace a voxel, drop an item, walk into the same pickup range, and collide with an authoritative wall. Inventory comparisons cover the full owner-only snapshot. The conservation ledger accounts for one stone becoming one cobblestone, one glass moving from inventory into the world, and three dirt units remaining across the two inventories after exactly one contested pickup. |
| `MultiplayerClosure` faults | The relay delays, drops, duplicates and reorders movement traffic. It loses a reliable edit and action result, replays an older edit and item snapshot, corrupts a baseline checksum, and verifies a fresh scope for the same actor. A bounded burst of truncated frames and a retired DropOne leave gameplay unchanged. A valid private action still completes. |
| `ServerRestart`, `ServerMigration`, `WorldCheckpoint` | Live server processes commit and restart distinct profiles. Both clients agree on the committed chunk image and observe a different uncommitted image before a process kill. Recovery restores the committed bytes and gameplay state with a new epoch and monotonic identity reservations. Migration and checkpoint fixtures also cover manifest publication failure and legacy data. |
| `MultiplayerSoak` | Eight clients run three join/move/leave waves, settle three disjoint radius-one interests per wave and leave while baselines are busy. Checks cover fair progress, reusable voxel storage, send slots, bounded allocation and checkpoint reopen. |

The loopback fixture runs the same script with synchronous generation and
two workers quiesced per tick. It compares settled wire poses, selected
slots and full inventories, sorted chunk versions and hashes, live item
records and allocator/edit watermarks. It also compares each client to
the same run's authority. Ports, random session credentials, epochs,
delivery timestamps, local camera prediction, presentation interpolation
and GPU pixels are outside that equality. A restart can reconstruct the
same terrain bytes under different chunk revisions in a new epoch.
The comparison is within each toolchain. Release runs reported the same
five chunks, next edit ID 82 and item high-water 2. The central chunk
hash was `7650601372971455612` on GCC and `13127139472443455587` on MSVC
with their default floating-point settings.

## Ownership and privilege review

| Boundary | Contract and regression coverage |
| --- | --- |
| Client and server | A client sends movement vectors, flags and action intent. Endpoint and current session scope determine the actor. There is no gameplay packet for granting items, changing position, editing authoritative terrain directly, selecting a different actor or requesting server shutdown. `WireProtocol`, `UdpTransport`, `VoxelActions` and `EntityReplication` exercise wrong-direction payloads, wrong actor scope, reach, stale revisions and private inventory. |
| IO and simulation | The IO worker validates framing, bounds, scope, rate and sequence and publishes to bounded mailboxes. The main thread drains one inbox and resolves admitted actions in ascending actor order. It runs a 60 Hz server tick, two 120 Hz player steps, one 60 Hz mob step and one 60 Hz item step. Logs report dropped pacing ticks. See the [wire ADR](adr/authoritative-wire-protocol.md) and [server tick policy](dedicated-server.md#tick-ownership-and-pacing). |
| Work and teardown | IO joins before the owner retires sessions and destroys the host. Terrain workers deliver tagged results; the world owns adoption and drains accepted deferred edits during the checkpoint barrier. A retired scope clears the client assembler, prediction and projections. Render leases and uploads remain epoch-bound. `WorldReplication`, `PlayerStreaming`, `ServerHost` and `EngineReload` exercise these lifetimes. |
| Persistence | The manifest selects one complete generation for terrain overrides, roster, inventories and shared items. A failed publish or abrupt process exit leaves the preceding committed generation. The persistent actor key resumes the actor through a freshly negotiated scope. See the [checkpoint ADR](adr/multiplayer-checkpoints.md). |

IPv4 discovery has 256 fixed buckets, each pump reads up to 128 datagrams,
and datagrams are capped at 1,200 bytes. The per-peer budget is 128 frames
and 128 KiB per second. The process budget is 2,048 frames and 2 MiB per
second. Family limits are narrower where appropriate. Each peer has a
six-tick input window, one reliable frame in flight, one world send slot,
and at most 64 buffered edits before resync. One baseline is 64 KiB in
60 fragments, with at most 16 concurrent baseline allocations for the
server. Wire v4 carries profile keys and session capabilities in cleartext.
Public-network validation remains open ([#276](https://github.com/gkehren/ft_vox/issues/276)).

## Soak baseline and alert thresholds

`MultiplayerSoak` prints a `SOAK` record. World queue depth counts logical
sends staged or in flight, excluding kernel buffers. Baseline allocations
count staged 64 KiB transfers. Voxel pool capacity counts allocated 64 KiB
storage blocks. Peak RSS and CPU time are whole-fixture process measurements
including the eight clients, relays and worker threads. Virtual time and
quiesced terrain make the run reproducible. Timing, RSS and retransmit
counts can vary with the platform and loopback socket buffers.

| Metric | Linux Release | Windows Release | Review threshold for this workload |
| --- | ---: | ---: | --- |
| Peak tracked chunk records / allocated voxel blocks | 61 / 45 | 61 / 45 | Fail above 77, or if voxel blocks grow by over eight after warm-up. |
| Peak world send slots / peak baselines | 8 / 8 | 8 / 8 | Fail above eight world slots or the protocol's 16-baseline cap. Both must drain on departure. |
| Baseline allocations / outbound world bytes | 418 / 29,596,478 | 418 / 29,596,478 | Fail outside 360–600 allocations. Investigate repeatable traffic growth above 25% on the same platform. |
| Process wall / CPU / peak RSS | 1,031 ms / 1,229 ms / 19,656 KiB | 2,505 ms / 2,563 ms / 21,888 KiB | Investigate a median increase above 25% over three Release runs on the same machine. |
| Current checkpoint bytes / restart to ready | 2,123 B / 21.8 ms | 2,123 B / 52.1 ms | Fail at 1 MiB for this no-edit fixture. Review restart latency using the same relative threshold. |

Reference environment is the canonical Windows machine running Debian
under WSL2, GCC Release, two terrain workers, and seed 19. This fixture
spends most of its runtime streaming cold terrain across many local
endpoints. It measures bounded local churn and recovery. The Windows live
`ServerRestart` run committed 889 bytes for the edited two-player world,
stopped in 71.2 ms, and reached readiness after the injected crash in 114.2 ms.

## Physical GPU and remaining work

On Windows 11 with MSVC Release, RTX 4070 Ti, driver 621.24 and Vulkan
1.4.357, two uninstrumented seed-42 graphical clients reached 29 chunks
and 14 draw entries each. Both disconnected after a graceful server stop.
Fresh client processes reused both profiles against the restarted server
on a new port. The actor IDs and world identity matched, the wire epoch
changed, and both drew terrain again. The server saved generations 2 and
3 on successive stops and reported zero dropped ticks.

`VisualRegression`, `EngineReload` and `ft_vox_visual_tests --strict`
passed on the same physical GPU. Strict mode reported all 24 scenes
within tolerance. The existing swapchain diagnostics
`VUID-VkSwapchainCreateInfoKHR-imageFormat-01778` and
`VUID-VkImageViewCreateInfo-usage-02275` appeared with validation enabled.
See the [GPU report](multiplayer-client-smoke.md) and
[#99](https://github.com/gkehren/ft_vox/issues/99).

- [#277](https://github.com/gkehren/ft_vox/issues/277) tracks manual
  keyboard, mouse, inventory, avatar and menu verification. The Windows
  native input pipe was unavailable on this run.
- [#276](https://github.com/gkehren/ft_vox/issues/276) tracks the threat
  model and deployment review for public networking.
- [#275](https://github.com/gkehren/ft_vox/issues/275) tracks cold chunk
  delivery. At 150 ms RTT the earlier fixture needed about 9.4 s for
  playable spawn terrain and inventory.

## Merged implementation tracker

| Phase | Issues and merged PRs |
| --- | --- |
| A, simulation ownership | #246 → [#261](https://github.com/gkehren/ft_vox/pull/261), #247 → [#262](https://github.com/gkehren/ft_vox/pull/262), #248 → [#263](https://github.com/gkehren/ft_vox/pull/263), #249 → [#264](https://github.com/gkehren/ft_vox/pull/264) |
| B, server and transport | #250 → [#265](https://github.com/gkehren/ft_vox/pull/265), #251 → [#266](https://github.com/gkehren/ft_vox/pull/266), #252 → [#267](https://github.com/gkehren/ft_vox/pull/267) |
| C, authority and replication | #253 → [#268](https://github.com/gkehren/ft_vox/pull/268), #254 → [#269](https://github.com/gkehren/ft_vox/pull/269), #255 → [#270](https://github.com/gkehren/ft_vox/pull/270), #256 → [#271](https://github.com/gkehren/ft_vox/pull/271) |
| D, clients and netcode | #257 → [#272](https://github.com/gkehren/ft_vox/pull/272), #258 → [#273](https://github.com/gkehren/ft_vox/pull/273) |
| E1, checkpoints | #259 → [#274](https://github.com/gkehren/ft_vox/pull/274) |
