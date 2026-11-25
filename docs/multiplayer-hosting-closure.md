# In-game multiplayer hosting closure

This report maps [issue #289](https://github.com/gkehren/ft_vox/issues/289)
to the tests and operator checks for [Epic #282](https://github.com/gkehren/ft_vox/issues/282).
The deployment scope is IPv4 loopback and trusted private networks.

## Coverage by scenario

| Scenario | Executed coverage |
| --- | --- |
| Fresh host | The native hosted closure starts in solo mode, creates a named profile, starts a real sibling server through the Host callback and checks the auto-joined actor against the server-verified owner before starting guest admission. CPU include and link gates verify the dedicated executable stays graphics-free. |
| Two clients | Separate graphical processes use the Host and Join callbacks. Each observes both actors, walks, selects a different private hotbar slot, supplies its own Network stats, and presents Vulkan frames. The owner breaks a block, collects the drop, and both replicas observe the edit. `MultiplayerClosure` adds placement, a deliberate drop, contested pickup, private stack conservation, and collision under UDP loss, duplication and reordering. |
| Role authority | The native host grants a role and the guest attempts a direct call to the administration callback. `ServerAdministration` exercises owner and locally authorized delegate permissions, kick, ban, allowlist, active revocation, wrong scope, replay, malformed requests, and over-capacity input. |
| Checkpoint and restart | The native owner waits for a confirmed checkpoint, then new graphical processes restore the original owner and delegated role, both actor IDs and poses, private inventories, and the edited cell. `ServerRestart`, `ServerMigration`, `ServerRoles`, `ServerAdministration` and `WorldCheckpoint` exercise crash rollback, migration, interrupted authority publication, local credential recovery, and ownership preservation. |
| Startup and runtime failures | `ServerSupervisor` checks occupied ports, missing executable, corrupt metadata, world lock, stale monitoring, abrupt launcher loss, child death, stop failure and retry. `ServerLifecycle` injects a real checkpoint publication failure. `ServerRoles` verifies credential permission refusal. The native Join smoke verifies wrong seed and timeout, and confirms retry and return-to-solo are offered. |
| Lifecycle and resource retirement | The native Join smoke switches endpoints and profiles, disconnects and reconnects in process, clears the old chunk/draw/entity references and releases the render pool before rebinding. The hosted closure also verifies both clients retire their remote projection and return to the same solo simulation after shutdown. |
| Network limits | UDP framing and timeout, input recovery, prediction, exactly-once actions, bounded monitoring and the eight-client `MultiplayerSoak` remain in the CPU gate. ASan/UBSan and TSan exercise the server, transport and supervisor. |

## Security and recovery

Only the private inherited launcher channel can carry a privileged request.
The dedicated server checks the current local credential and role, the
persistent world ID, the launch epoch and the next request sequence.
Gameplay identity alone remains a cleartext bearer key used by wire v5.
A role-only grant has no remote execution path. The local controller can
read the server audit after authentication. Audit records contain the
operation, actor and target, with no credential material.

The closure recovery test starts from a valid authority with a ban and
the allowlist enabled. It rotates authority using the locked offline
procedure, relaunches under the retired owner and refuses live stop
authority, then relaunches with the replacement. The new owner receives
the verified role, bypasses the admission restriction for its own
registered profile, observes the existing policy and completes a
privileged stop. The preceding fault case retains the ban after rollback
to a previous gameplay checkpoint. The operator procedures and file
permissions are described in the [dedicated server guide](dedicated-server.md#offline-administration-and-recovery).

## CPU and sanitizer gates

| Environment | Result |
| --- | --- |
| Linux Release, Debian WSL2, GCC 14.2 | 51 CTest entries passed, including the nested clean CPU-only configure, build and test gate. |
| Windows Release, MSVC 14.52 | 50 CTest entries passed, including the nested clean CPU-only gate. The system configuration did not discover Boost, so the optional legacy UDP prototype test was absent. The current wire v5 transport and soak tests ran. |
| Linux ASan/UBSan, GCC Debug | All six targeted tests passed. They cover administration, roles, supervisor, UDP, dual-client authority and the eight-client soak. |
| Linux TSan, GCC Debug | All 14 tests in the targeted workflow selection passed. The WSL2 kernel's randomized layout caused an initial runtime startup failure. Re-running CTest with the process-local `setarch x86_64 -R` setting allowed execution, including the server processes spawned by the tests. |

The TSan selection includes `ServerAdministration` and `ServerSupervisor`.
The stale-monitor fixture waits for the parent to observe an initial
sample before pausing publication. This keeps the expiry assertion
independent of whether the writer's first opportunistic offer was dropped.
The standard [CPU workflow](../.github/workflows/cpu-boundary.yml) remains
graphics-free. The Windows Vulkan run is opt-in.

## Windows physical-GPU evidence

Tested on November 25, 2026 at source commit
`8c240e4365d16488801282e07b7639b7f11ba96e` with Windows 11 build
26300, MSVC Release, NVIDIA GeForce RTX 4070 Ti, driver 621.24,
Vulkan 1.4.357, and Khronos validation enabled.
The owner and guest each ran `test_engine_reload.exe` in an isolated
working directory. The owner started the real `ft_vox_server.exe`.
Settings were seed 42, IPv4 loopback, an ephemeral port, two seats,
radius one, one terrain worker, Low streaming, and auto-join.

```powershell
cmake --build build-host-285 --config Release --target ft_vox test_engine_reload
$env:FT_VOX_VALIDATION = "1"
$env:VK_LAYER_PATH = "D:/VulkanSDK/1.3.290.0/Bin"
python tests/test_hosted_closure_native.py --server build-host-285/Release/ft_vox_server.exe --client build-host-285/tests/Release/test_engine_reload.exe --output build-host-285/closure-289-d7d21c5b
```

| Observation | First launch | Relaunch |
| --- | --- | --- |
| Loopback port | 58110 | 64025 |
| Host actor, selected slot and sampled RTT | #2, slot index 3, 28.9 ms | #2, slot index 3, 88.0 ms |
| Guest actor, selected slot and sampled RTT | #3, slot index 6, 27.4 ms | #3, slot index 6, 43.3 ms |
| Server roster and completed ticks at sample | 2 of 2, 122 ticks | 2 of 2, 62 ticks |
| Checkpoint opened → committed | 1 → 2 | 2 → 3 |
| Native assertions, owner + guest | 9 + 7 | 12 + 7 |

The owner mined cell `(13, 62, 8)` and picked up its drop. Both clients
observed the empty cell on the first run and after relaunch. Both
recovered the full inventory witness, with the collected item present
for the owner and the guest's stack state independent. The owner still
had Owner permission after relaunch, and the guest kept its role-only
delegation. The guest's direct callback attempt failed in both phases.

The clients reported terrain sync again around the first edit, then
returned to Connected. Both reported the server shutdown and offered
reconnect. The server logged `reason=control saved=1 exit_code=0`
for each stop with zero dropped ticks. These are application observations
from the test logs and frame snapshots.

Both clients logged the existing
`VUID-VkSwapchainCreateInfoKHR-imageFormat-01778` and
`VUID-VkImageViewCreateInfo-usage-02275` errors and the RTSS hook
advisory. The loader also reported Galaxy overlay layer naming messages.
The validation result includes these messages. See the
[Vulkan validation notes](vulkan-validation.md).

The separate [native Join smoke](multiplayer-client-smoke.md#in-game-join-284)
passed 25 assertions on the same build. It also verified trace isolation
across switches and process restarts, with tracing disabled, and with a
trace path that could not be opened.

Run the [native hosted closure](multiplayer-client-smoke.md#in-game-hosting-285)
from a Windows Release build with `FT_VOX_VALIDATION=1`. Use a new output
directory. The test writes isolated owner and guest profiles, per-process
logs, initial and restart actor witnesses, and committed generation
numbers. The profile and world folders contain private credentials.

## Manual interaction still to verify

The native Computer Use pipe returned Windows error 2 after retry and
session reset. The physical run used automated frame callbacks and Vulkan
presentation. It produced process logs and durable state witnesses.
Hands-on keyboard and mouse observations and screenshots are pending.

- [ ] Open the Multiplayer panel in two regular game windows. Create and
  select profiles, choose the host world and options, start from Host,
  and join from the guest's Join form. Observe the owner, roster and
  independently updating Network values. Capture screenshots of both.
- [ ] Walk and see each other's avatars in the viewport. Mine, place,
  rearrange stacks, drop and pick up items in both windows.
- [ ] Confirm and cancel moderation from the panel. Check delegated
  controls, guest controls, and the visible result and audit. Stop from
  Administration, wait for the committed checkpoint, and reopen it.
- [ ] Check the form and log presentation for startup or save failures,
  a stale roster and a lost connection. Exercise cursor focus, profile
  switching, retry and return to solo. The loopback test supplies the
  supported local example. A separate LAN/VPN device remains unverified.

The [graphical smoke guide](multiplayer-client-smoke.md) provides the
interaction steps. Leave the Epic open until the remaining manual evidence
is recorded.
