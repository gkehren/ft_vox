# Player physics

The normal perspective mode now controls a physical player. `V` switches to
debug flight; isometric inspection always uses flight. Walking can only be
enabled when the whole body fits inside available, non-solid terrain. The HUD
reports an invalid transition rather than moving the player into a wall.

## Controls and defaults

| Input | Walking | Swimming | Debug flight |
|---|---|---|---|
| WASD | Move relative to horizontal look direction | Move | Move |
| Space | Jump (one impulse per press) | Swim up; press near surface to jump onto a bank | Up |
| Left Shift | Sprint | Swim down | Down |
| V | Enable flight | Enable flight | Try to enable walking |
| X | No action | No action | Toggle 20 / 125 blocks/s |

One world unit is one block. The body is an upright 0.6 × 1.8 × 0.6 AABB, with
its origin at the feet and eye height 1.62. `physics::PlayerSettings` centralizes
the tuning: 4.5 walking / 7 sprinting blocks/s, acceleration 45, braking 60,
air control 12 blocks/s², gravity 24, jump impulse 8.5 and terminal fall speed
50. The jump clears one block; there is no automatic full-block step or crouch.
Coyote time is 100 ms and the pre-landing input buffer is 120 ms.

Water and kelp are non-solid water volumes. Immersion is the fraction of body
volume inside liquid cells, not just a sample at the eyes. Movement blends
towards swimming speed 2.5, gravity 4 and vertical drag 4/s. Lava is a separate
non-solid medium with half the swimming speed. A new jump press while less than
90% immersed in water can initiate a surface jump; liquid vertical drag is
suppressed for 250 ms to allow clearing a bank. Glass, leaves, ice, sand, gravel
and all other current non-fluid blocks remain solid cubes. Render transparency
does not determine collision. Underwater rendering remains an eye-level effect.

## CPU interfaces and ownership

- `Physics/VoxelCollision.hpp`: `Body`, `Aabb`, availability-aware `Cell`,
  `VoxelCollisionWorld`, swept collision/slide, bounded overlap recovery and
  immersion queries. No SDL, Chunk or Vulkan dependency.
- `Physics/PlayerController.hpp`: input intentions in world X/Z, velocity and
  jump/swim rules, fixed-step accumulator and interpolated eye position.
- `Physics/BlockPhysics.hpp`: physical block policy, independent of rendering.
- `Chunk/ChunkCollisionView.hpp`: main-thread scoped adapter. Holds the chunk
  map's shared lock once, with a bounded lookup cache destroyed at scope exit.
  No chunk/storage pointer escapes into the player or survives across frames.

The engine samples SDL input then advances the controller before streaming and
rendering. Simulation uses double precision at 120 Hz, with up to eight steps
per frame. Excess whole steps are dropped and counted; the fractional remainder
is retained. Position is interpolated between completed simulation steps, while
mouse look is immediate. Input edges are preserved until a physics step, consumed
once, and cleared when ImGui captures the keyboard. UI capture leaves gravity
running. Pause and focus loss suspend simulation and discard accumulated time
and jump timers; resuming cannot replay a long stall.

Collision sweeps only inspect the swept AABB's local voxel range. Slab tests find
the first time of impact and combine simultaneous normals; up to four slide
iterations consume the remainder. A 0.001-block contact skin and a separate
support probe avoid resting jitter and coplanar seam catches. Unknown cells block
sweeps but are never counted as genuine ground. A body already overlapping an
unknown region freezes until it becomes available. Solid penetration recovery
is limited to four corrections and one block total; failure leaves the original
position stationary, with debug flight available for recovery.

The adapter checks readable backing before sampling: missing/prepared chunks
are unknown, generated chunks are usable before meshing/upload, and a read-only
mesh job does not invalidate collisions. Atomic publication of generated backing
is respected; the map lock alone is not a lock on generation data. Negative
world coordinates use floor division. Y below zero is a solid world boundary;
Y at or above 256 is open sky. The existing horizontal camera-coordinate envelope
is retained.

Pending logical voxel edits overlay published backing, ignoring mirror writes
and edits stamped for a previous chunk incarnation. Placement into the current
physical player is refused. An accepted queued placement blocks movement before
its mesh is ready, so the player cannot enter it during a delayed remesh. A
queued deletion removes collision immediately. Physics does not modify chunk
storage, meshes, upload or retirement behavior.

Spawn searches the already bootstrapped area for support and a clear body volume,
including aquatic positions. If no safe spawn exists, debug flight remains
active with a HUD explanation. Reload resets position, velocity and timers.
Existing streaming benchmarks retain their original center, height and scripted
camera path; physics is suspended throughout the benchmark. On completion or
cancellation, the previous movement mode is restored only if the current body
volume is valid, otherwise flight stays enabled.

## Extension boundaries

Mobs can reuse the query interface, parameterized body and collision solver with
their own controllers. Falling sand/gravel and fluids can share voxel properties
and world queries but will need their own activation, scheduling and update
rules. This change does not introduce entity-to-entity collisions, a rigid-body
engine, an ECS, fluid propagation, damage or network authority.

The physics implementation remains independent of the meshing architecture
introduced by #106 and #107. Incremental streaming (#108), GPU arenas (#109),
and packed vertices (#110) remain out of scope.

## Validation

```powershell
cmake --build build --config Release -j 8
ctest --test-dir build -C Release --output-on-failure --timeout 120
./build/tests/Release/test_physics.exe --profile
./build/tests/Release/test_chunk_lifecycle.exe --physics-profile
```

`PlayerPhysics` covers collisions, different body sizes, jumps, liquid transitions,
timing invariance at 30/60/144/240 FPS, long frames, input-buffer clearing and zero
ordinary heap allocations during steady simulation. `ChunkLifecycle` additionally
checks real chunk availability/publication, concurrent generation, negative
coordinates, deferred edits, remeshing and recycled incarnations.

The standalone real-voxel profile generates seed 42 without rendering, warms up
1200 ticks and measures 12000 ticks including construction/destruction of the
scoped query adapter. It reports mean/p95 CPU time, cells/tick, distance travelled,
terrain waits and dropped steps. It is distinct from the existing fly-through
renderer benchmark. Timing is diagnostic, not a hardware-dependent CI assertion.
The in-game profiler exposes the `Input/Physics` scope and the HUD exposes step,
query and dropped-step counters.
