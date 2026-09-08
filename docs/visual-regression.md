# Visual regression testing

Authoritative doc for the deterministic visual-regression harness (issue #142):
what it catches, how scenes are made reproducible, how frames are compared, and
how the committed reference images are maintained.

---

## 1. Purpose

Unit tests prove code is *correct*; they cannot prove a frame *looks right*. A
renderer change can be technically valid — every Vulkan call legal, every
assertion green — and still visibly wrong: a double gamma application, shadow
acne across a cliff face, ambient-occlusion haloing around block corners, bloom
clipping to flat white, an exposure regression that washes out night scenes.

`ft_vox_visual_tests` renders fixed scenes through the production renderer and
compares the resulting pixels against committed golden images, catching exactly
this class of regression. It **complements** the existing coverage and replaces
nothing: Vulkan validation still guards API misuse, and functional tests still
guard logic.

## 2. Harness

- Executable: `ft_vox_visual_tests`; ctest test name: `VisualRegression`.
- Each scene renders **offscreen at 640×360** through the **production
  `WorldRenderer` pass graph** — Shadow → Opaque → Water → Sky → Post — into a
  caller-owned composite target. No ImGui, no present transition.
- The offscreen entry point is `WorldRenderer::recordFrameToImage`. The runtime
  renderer stays asynchronous; the test performs a **test-only synchronous GPU
  readback** of the composite target after the pass graph completes.
- **Golden-image contract**: the harness requires the surface to deliver
  exactly 640×360 and an 8-bit sRGB composite format
  (`R8G8B8A8_SRGB`/`B8G8R8A8_SRGB`). Anything else would compare frames
  against references produced under a different resolution/encoding, so the
  harness **skips (exit 77)** with an explicit message instead of rendering.
- **Validation gating**: initialization is split — device/swapchain creation
  may carry errors from injected overlays (RTSS, see
  `docs/vulkan-validation.md`) and is only a warned baseline. Everything from
  `WorldRenderer::init` onward (descriptors, pipelines, internal images, the
  offscreen target) and every scene render must be **validation-clean**;
  any error there fails the run.

## 3. Determinism levers

Every scene is a closed inputs → pixels function:

- **Fixed world seed** per scene; the world bootstrap uses the synchronous
  `generateInitialArea` path, so no worker-thread scheduling is involved.
- **Fixed camera**: position, yaw, pitch, 80° FOV.
- **Pinned `dayTime`** (sun/moon direction, light color).
- **Pinned animation `time`**: water waves, foliage wind, star twinkle and film
  grain all derive from it.
- **Fixed `ShaderParameters` / `PostProcessSettings`** — no UI knobs, no
  per-run autotuning; golden scenes additionally pin auto exposure off
  (`autoExposureEnabled = false`, see §4).
- **Voxel fixtures** (carved caves, shorelines, emissive blocks…) are applied
  after bootstrap and remeshed synchronously, so geometry is identical run to run.

Each scene is rendered **twice** per invocation and must produce bit-identical
output — a self-determinism check that fails independently of any reference
comparison. (Auto-exposure scenes keep this contract via the off→on re-seed
toggle described in §4.)

## 4. Scenes

| Scene | Covers |
|-------|--------|
| `noon_terrain` | Color space, mips/filtering, fog, CSM shadows, base post |
| `cascade_transition` | CSM split seam across a cascade boundary |
| `cave_emissive` | Block light, emissive blocks, bloom, dark-scene exposure |
| `water_shore` | Refraction, absorption, foam and the deep → shallow → shoreline → terrain transition (camera stands in the water looking back at a low beach) |
| `sunset` | Low sun angle, god rays, fog/horizon gradient |
| `midnight` | Moon/stars, night exposure |
| `mob_lighting` | Entity vs terrain lighting consistency |
| `underwater` | Fully submerged camera |
| `underwater_deep` | Distance-based extinction falloff and world-anchored caustics at noon (submerged camera looking horizontally across open water) |
| `auto_exposure_noon` | The `noon_terrain` inputs (same seed/viewpoint/atmosphere) through the live auto-exposure path: metering, adaptation, composite consumption |
| `auto_exposure_cave` | Auto exposure in a sealed, unlit carved room; the adapted exposure climbs toward the max-EV clamp |
| `aa_silhouette` | Spatial AA on (dedicated FXAA 3.11 pass, issue #143): a 32-step diagonal stone staircase with attached leaf clusters against the noon sky — diagonal voxel edges, foliage borders, hard sky contrast |
| `aa_silhouette_off` | The identical `aa_silhouette` scene with the spatial AA pass disabled (issue #143): A/B pair locking the composite-straight-to-swapchain bypass path from the same camera |

On top of the whole-frame comparison, each scene carries **targeted numeric
invariants**: the HDR scene target is scanned **pre-tonemap** for non-finite
fp16 samples (NaN/Inf — compositing quantizes them into undefined bytes, so
they must be caught before it runs), shadowed terrain darker than lit terrain,
emissive peaks present in dark caves, night readability bounds, and water /
underwater blue-shift checks. Invariants catch direction-of-change errors that
averaged pixel metrics would smooth over.

**Exposure pinning policy (issue #140).** Every golden scene renders with
`PostProcessSettings::autoExposureEnabled = false`: the committed references
were captured with the fixed manual exposure, so pinning it keeps every
existing reference valid — none had to be regenerated. The live auto path gets
the dedicated `auto_exposure_*` scenes above, which opt in via their scene
spec and ship committed references of their own.

**Auto-scene self-determinism (off→on re-seed toggle).** Every recorded frame
advances the adaptation state that the composite consumes in the same frame,
so back-to-back auto frames can never be bit-identical while adapting. The
harness exploits the mode-transition rule instead: `PostStack::recordExposure`
re-seeds the adaptation from the manual exposure on the auto-mode **rising
edge**, and a seeded frame's output depends only on its push constants and the
static scene meter. Each compared frame of an auto scene is therefore rendered
immediately after an off→on toggle of `autoExposureEnabled`; the off frames in
between run the manual composite and are discarded, and the two-renders
bit-identical contract stays intact.

**Synthetic meter check (`auto-exposure-meter`).** A standalone GPU-exercised
run — always part of the suite, also selectable via
`--scene auto_exposure_meter` — injects known HDR values directly into the
metering chain through a tooling probe (`PostStack::recordExposureProbe`, no
world rendering) and validates exact meter readings: uniform greys must read
0 / +2 / −2 EV, and a block-aligned 25/75 vertical split (+4 EV quarter over
−4 EV) must average to exactly −2 EV (the historical central-2×2 sampling
bias would read −4 EV). Validation errors are judged as a before/after delta
of the probe only (device/swapchain baseline excluded).

**Temporal adaptation check (`auto-exposure-adaptation`).** A standalone
GPU-exercised run — always part of the suite, also selectable via
`--scene auto_exposure_adaptation` — drives the production metering +
adaptation passes over many consecutive frames in a sealed dark room and
validates the CPU debug readout (`WorldRenderer::exposureReadout`). The
render path performs no per-frame readback: each sample refreshes the
readout explicitly after its (synchronous) render, so it observes exactly
the state that frame produced. It asserts:

- **Monotonic, overshoot-free adaptation:** 40 observed states climbing toward
  the target never decrease (1e-5 log2 jitter allowed per step) and never
  overshoot the target (1e-3 log2 at the end state).
- **Frame-split independence:** 30×1/30 s vs 60×1/60 s of identical simulated
  time land within **5e-3 relative error** — the per-step alpha
  `1 − exp(−speed·dt)` is the exact exponential integral, so only fp32
  rounding may separate the two splits.
- **Clamp-state reporting:** with `autoExposureMaxEv` forced to +1, the dark
  room must report `clampState = 2` (max clamp); a vacuity guard requires the
  room's metered luminance to be ≤ −1 EV first.
- **Frame-in-flight slot alternation:** two runs with identical dt sequences —
  one on a fixed frame slot, one alternating slots 0/1 like the runtime — must
  land on the same adaptation (1e-3 relative). The temporal state is a single
  logical history, not per-slot; per-slot independent state would diverge here.
- **`dt ≤ 0` is a strict no-op:** consecutive zero-dt frames must observe
  identical readouts — a zero step can never snap the exposure to the target
  (the exact-settle rule fires only on arrival within 1e-4 EV).
- No non-finite HDR samples in any of the dark-room frames.

## 5. Comparison policy

Comparison is **not bit-exact** — GPUs and drivers differ in rounding, and
chasing bit-exactness across vendors is a losing game. Metrics are computed
per channel (alpha ignored) and **normalized to [0,1]** (1.0 = full-scale 255
delta), so the thresholds below read directly as LSB budgets. Metrics per
scene:

- per-channel **mean absolute error**
- **RMS** error
- **max** channel delta
- **hot-pixel ratio**: fraction of pixels with any channel delta > 20

Default tolerances (strict mode):

| Metric | Threshold |
|--------|-----------|
| Mean abs error | ≤ 3/255 |
| RMS | ≤ 5/255 |
| Hot-pixel ratio | ≤ 2% |

The metric units themselves are unit-tested (`ctest -R VisualImageMetrics`):
identical images, one-LSB shifts, at-threshold/failing deltas, hot-pixel
counting, size mismatches and the PNG round-trip.

## 6. Reference policy

- Reference PNGs live in `tests/visual-references/<scene>.png` and are
  **committed to git** — one 640×360 PNG per scene.
- References are generated on a **canonical developer GPU** (initial set:
NVIDIA GeForce RTX 4070 Ti, Windows, Vulkan).
- Normal runs **never** write references.
- A **missing reference is a test failure** with instructions on how to
  generate it — never a silent pass, never a silent overwrite.

Update references explicitly (run from the repo root so `./ressources/`
resolves):

```bash
# Windows (multi-config build)
./build/tests/Release/ft_vox_visual_tests.exe --update-references
./build/tests/Debug/ft_vox_visual_tests.exe --update-references   # Debug config

# Linux / macOS (make, single-config)
./build-vk/tests/ft_vox_visual_tests --update-references
```

A reference-update PR must include the PNG diff and an explanation of *why*
the pixels changed. "The image changed" is not a reviewable justification;
"mip LOD bias off by one, fixed" is. Update runs also write review artifacts
under the artifacts dir: `expected.png` holds the **previous** golden,
`diff.png` amplifies the shift, and `metrics.txt` quantifies it — the PR
reviewer sees exactly what moved without re-running anything.

## 7. Cross-vendor and CI policy

Committed references come from the canonical GPU. Other GPUs or drivers may
legitimately exceed the strict thresholds through FP rounding alone. Therefore:

- `--smoke` multiplies tolerances (5× mean/RMS, hot-pixel ratio to 10%) for
  heterogeneous machines. The per-scene numeric invariants stay strict in
  smoke mode — a direction-of-change regression still fails.
- **ctest runs in smoke mode by default** (the test environment sets
  `FT_VOX_VISUAL_SMOKE=1`) so a full `ctest` on a non-canonical GPU does not
  go false-red. The **strict gate** is a direct run without `--smoke` on the
  canonical GPU — mandatory before committing reference updates. `--strict`
  forces strict mode from the CLI (overriding the environment), and setting
  `FT_VOX_VISUAL_SMOKE=0` in the environment enables strict ctest runs on the
  canonical machine.
- A software rasterizer (`lavapipe`) can run the harness in CI once a build/
  test CI exists (issue #131). **Today no CI workflow runs ctest** (CodeQL
  only), so references are validated on developer machines.
- When test CI lands, the recommended split is: strict mode on the canonical
  stack, smoke mode elsewhere.

## 8. Failure artifacts

On mismatch — and always kept for `--update-references` runs — the harness
writes under the artifacts dir (default `build/visual-qa/<scene>/`):

| File | Contents |
|------|----------|
| `actual.png` | This run's frame (also written when a scene invariant fails) |
| `expected.png` | The committed reference |
| `diff.png` | Per-pixel error ×8 amplified |
| `metrics.txt` | All numeric metrics, thresholds, verdict |
| `errors.txt` | Invariant / determinism failure messages |

CI should upload this directory as a workflow artifact once test CI exists.

## 9. Running

Via ctest (the invocation sets `FT_VOX_VALIDATION=1` and `FT_VOX_VISUAL_SMOKE=1`
— see §7 — and the working directory to the repo root so `./ressources/`
resolves):

```bash
cd build && ctest -R VisualRegression --output-on-failure -C Release   # Windows
cd build-vk && ctest -R VisualRegression --output-on-failure           # Linux/macOS
```

Or directly from the repo root:

```bash
./build/tests/Release/ft_vox_visual_tests.exe [options]
```

CLI reference (positional args `<refs-dir> <out-dir>` are how ctest passes the
paths):

```
ft_vox_visual_tests [--update-references] [--smoke] [--strict]
                    [--scene NAME ...] [--refs DIR] [--out DIR] [<refs-dir> [<out-dir>]]
```

- `--strict` forces strict tolerances even when `FT_VOX_VISUAL_SMOKE=1`.
- A `--scene` filter matching no scene is an error (exit 1), never a
  vacuous pass.
- Artifacts dir defaults to `build/visual-qa` (relative to the working
  directory).

| Option | Effect |
|--------|--------|
| `--update-references` | Render and write reference PNGs instead of comparing |
| `--smoke` | Relaxed tolerances (5× mean/RMS, 10% hot ratio); invariants stay strict |
| `--scene NAME ...` | Run only the named scenes |
| `--refs DIR` | Reference directory (default `tests/visual-references`) |
| `--out DIR` | Artifacts directory (default `build/visual-qa`) |

Exit codes:

| Code | Meaning |
|------|---------|
| 0 | Pass |
| 1 | Failure (metrics, invariant, self-determinism, or missing reference) |
| 77 | Environment skip — no Vulkan device / no surface; ctest reports SKIP (`SKIP_RETURN_CODE 77`) |

## 10. Adding a scene

1. Extend the scene table in `tests/test_visual_regression.cpp`: name, seed,
   camera (position/yaw/pitch), `dayTime`, animation `time`, world fixture
   lambda, invariants, optional mob states.
2. Regenerate the reference:
   `./build/tests/Release/ft_vox_visual_tests.exe --update-references --scene <name>`
3. Commit code and PNG together — a scene without its reference fails.

## 11. Out of scope

The harness intentionally does **not**:

- chase bit-exact goldens across vendors;
- exercise the window/surface present path;
- measure performance (the existing `--benchmark` mode covers that);
- accept interactive input.

## Water audit (issue #139)

The explicit `--water-audit` mode is separate from golden comparisons. It creates
an edited lake with a shallow shore, cliff, tree, overhang and water-filled
kelp/seagrass. Nine camera/time variants write twelve consecutive PNG frames
each (lake, edge, bridge, shore, foreground, sunset, moon, surface crossing,
kelp). It checks finite HDR output, SSR contribution, identical-frame SSR
determinism and directional-shadow contribution with other post settings held
fixed. Renderer validation errors fail the audit. Device-init overlay errors
remain separately reported under the existing baseline policy.

Two stability probes complement the still-frame checks. Both freeze the water
animation time and strafe the camera by 0.125 world units, comparing the frame
pair with SSR (High) against the parallax baseline without SSR (Medium; water
shadows stay on in both). The SSR-on motion must stay within a generous
multiple of the baseline (mean absolute error and hot-pixel ratio are printed
for calibration). The sequence runs at the default wave strength and again at
`waterWaveStrength` 0.25 for stability only. A separate gating probe at 0.45
— the slider reaches 0.5 — asserts the contributions survive: the SSR delta
must keep at least 40% of the default-wave delta and shadow reception at
least 50%; gating keys on the geometric face normal, so wave strength must
not toggle scene reflections (regression probe for gating on the
wave-animated shading normal).

```
./build/tests/Release/ft_vox_visual_tests.exe --water-audit --out build/issue-139-qa/1080
./build/tests/Release/ft_vox_visual_tests.exe --water-audit --audit-1440 --out build/issue-139-qa/1440
```

Only this mode requests 1920x1080 or 2560x1440. Normal golden runs retain the
640x360 contract. Timings run three interleaved preset sweeps (ascending /
descending / ascending; 8 synchronous GPU samples per tier and sweep after 4
warmup frames) so clock ramp and thermal drift cannot order the tiers;
`timings.csv` publishes the median sweep means for Water and the production
pass graph GPU frame (excluding test readback). These are fixed-fixture
costs, not streaming or presentation benchmarks. No golden files are read or
updated in this mode.

The underwater composite path (issue #144) gets its own timing artifact: a
camera fixed fully below the lake surface (underwater post enabled with a
known surface height) renders the same three interleaved preset sweeps, and
each tier records the Post pass together with the nested Composite pass that
implements the depth-aware underwater model. `underwater.csv` publishes the
median sweep means and sample counts per tier, so the underwater path cost is
reported separately from the water pass and the whole-frame time.
