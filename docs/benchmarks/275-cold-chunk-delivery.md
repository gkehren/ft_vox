# Cold chunk delivery for issue #275

At 150 ms RTT and eight simultaneous clients, wire v5 reduced the worst
first-playable time from **9.630 s to 1.570 s** on a clean link. With
injected loss it fell from **11.730 s to 2.020 s**. Under the same loss,
streaming a second neighborhood took **8.340 s**, down from **57.310 s**,
and sent **805,594 bytes**, down from **6,274,125 bytes**.

## Method

The CPU-only benchmark uses real IPv4 sockets, one relay per peer and a
deterministic 5 ms poll clock with a 60 Hz server tick. Peers use distinct
loopback addresses so simultaneous joins exercise independently rate-limited
discovery. The server generates seed 19 terrain synchronously and replicates a
radius-one disk. The loss profile drops every tenth server chunk fragment
datagram and every tenth client reliable ACK, including retries, with a
different starting phase for each peer. Delay is split equally across directions.

- **First playable** is time from connect until the spawn chunk, owner
  inventory and a player snapshot are available. The tables use the slowest
  client's time.
- **Spawn neighborhood** ends when all peers have the five spawn chunks. The
  server then moves the observers to a disjoint five-chunk interest disk.
  **Shift** runs from that move until all peers have the five new chunks.
- **Traffic** is aggregate server world datagram bytes through each
  milestone, including retries and chunk removals. Movement, inventory,
  transport ACK traffic and IP/UDP overhead are outside this counter.
  These results cover two neighborhoods of one seed under a controlled link.

Run the same matrix on the measurement-only commit `fd2d7e4` for raw v4
and on the implementation commit for v5. These measurements use a CPU-only
Release build on Debian WSL with GCC 14.2 and `FT_VOX_PORTABLE_FP=OFF`.

```sh
cmake -S . -B build-cpu -DFT_VOX_GRAPHICS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpu --target test_world_replication --parallel
./build-cpu/tests/test_world_replication --measure-chunks
ctest --test-dir build-cpu -R WorldReplicationColdChunks -V
```

For a multi-config generator, add `--config Release` to the build command
and `-C Release` to CTest. The executable is in `build-cpu/tests/Release`.
`WorldReplicationColdChunks` is the v5 contention regression.

## Raw v4 → compressed v5

### Clean link

| RTT (ms) | Clients | First playable (s) | Spawn neighborhood (s) | Shift (s) | Cold traffic (KiB) | Shift traffic (KiB) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 1 | 0.775 → 0.075 | 4.795 → 0.340 | 5.075 → 0.745 | 346 → 37 | 345 → 45 |
| 0 | 2 | 0.775 → 0.075 | 4.795 → 0.340 | 5.075 → 0.745 | 692 → 75 | 690 → 90 |
| 0 | 8 | 0.775 → 0.075 | 4.795 → 0.340 | 5.075 → 0.745 | 2768 → 298 | 2761 → 359 |
| 30 | 1 | 2.170 → 0.350 | 10.585 → 1.280 | 10.745 → 1.665 | 345 → 37 | 345 → 45 |
| 30 | 2 | 2.170 → 0.350 | 10.585 → 1.280 | 10.745 → 1.665 | 690 → 75 | 690 → 90 |
| 30 | 8 | 2.170 → 0.350 | 10.585 → 1.280 | 10.745 → 1.665 | 2759 → 298 | 2761 → 359 |
| 80 | 1 | 5.295 → 0.875 | 25.710 → 3.105 | 25.995 → 3.915 | 345 → 37 | 345 → 45 |
| 80 | 2 | 5.295 → 0.875 | 25.710 → 3.105 | 25.995 → 3.915 | 690 → 75 | 690 → 90 |
| 80 | 8 | 5.295 → 0.875 | 25.710 → 3.105 | 25.995 → 3.915 | 2759 → 298 | 2761 → 359 |
| 150 | 1 | 9.630 → 1.570 | 46.840 → 5.625 | 47.330 → 7.055 | 689 → 74 | 690 → 90 |
| 150 | 2 | 9.630 → 1.570 | 46.840 → 5.625 | 47.330 → 7.055 | 1378 → 147 | 1381 → 179 |
| 150 | 8 | 9.630 → 1.570 | 46.840 → 5.625 | 47.330 → 7.055 | 5513 → 590 | 5523 → 716 |

### Injected loss

| RTT (ms) | Clients | First playable (s) | Spawn neighborhood (s) | Shift (s) | Cold traffic (KiB) | Shift traffic (KiB) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 1 | 2.585 → 0.375 | 12.200 → 1.585 | 12.375 → 1.670 | 425 → 46 | 428 → 52 |
| 0 | 2 | 2.585 → 0.375 | 12.200 → 1.585 | 12.375 → 1.670 | 849 → 90 | 856 → 105 |
| 0 | 8 | 2.585 → 0.375 | 12.200 → 1.585 | 12.375 → 1.820 | 3402 → 366 | 3411 → 441 |
| 30 | 1 | 4.420 → 0.650 | 21.235 → 2.630 | 21.545 → 3.015 | 425 → 46 | 428 → 52 |
| 30 | 2 | 4.420 → 0.650 | 21.235 → 2.630 | 21.545 → 3.015 | 849 → 90 | 856 → 105 |
| 30 | 8 | 4.420 → 0.650 | 21.235 → 2.630 | 21.545 → 3.115 | 3402 → 366 | 3411 → 441 |
| 80 | 1 | 7.545 → 1.175 | 36.360 → 4.455 | 36.795 → 5.265 | 425 → 46 | 428 → 52 |
| 80 | 2 | 7.545 → 1.175 | 36.360 → 4.455 | 36.795 → 5.265 | 849 → 90 | 856 → 105 |
| 80 | 8 | 7.545 → 1.175 | 36.360 → 4.455 | 36.795 → 5.365 | 3402 → 366 | 3411 → 441 |
| 150 | 1 | 11.730 → 1.870 | 57.040 → 6.825 | 57.230 → 8.405 | 765 → 82 | 765 → 97 |
| 150 | 2 | 11.730 → 1.870 | 57.040 → 6.825 | 57.230 → 8.405 | 1530 → 163 | 1530 → 194 |
| 150 | 8 | 11.730 → 2.020 | 57.040 → 6.975 | 57.310 → 8.340 | 6130 → 661 | 6127 → 787 |

At 150 ms RTT with eight clients and loss, cold retransmits fell from 2,931
to 331 and shift retransmits fell from 2,969 to 425. The 150 ms run also
records retries on the clean link because the ACK can arrive after the
150 ms retry timer at the relay's poll cadence.

## Transport decision and bounds

Run-length coding fits the repeated material bytes in terrain and air.
The v5 benchmark shows nearly 90% less cold traffic with the existing
one-frame send window. A larger reliable window could reduce the RTT
cost for poorly compressible chunks and would add fragment tracking,
ACK and pacing state. The compressed path delivers the observed gains
with the current ACK ordering, retry policy and rate budgets.

The raw fallback preserves a 65,536-byte payload ceiling and a 60-fragment
worst case. The server keeps one staged or in-flight 64 KiB payload per
peer, up to 16 globally, plus one 64 KiB capture scratch buffer on the
simulation owner. Each client uses a 64 KiB assembly buffer and 64 KiB of
temporary decode storage. The full decompressed material image must pass
its checksum and carries the same generation, revision and edit watermark
used to validate subsequent deltas. See the
[wire contract](../adr/authoritative-wire-protocol.md#world-baseline-edits-and-resync).

`WorldReplicationColdChunks` runs 18 peers at 150 ms RTT with injected loss,
fills all 16 transfer slots and checks eventual progress and the authority's
chunk digests for every peer. The measured worst first playable time was
**3.265 s**. All peers settled the spawn disk in **9.310 s** and the
shifted disk in **11.005 s**. The test budgets are 4 s, 12 s and 14 s,
respectively, and 128 KiB per client on average in each phase.
