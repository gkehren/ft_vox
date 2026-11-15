#!/usr/bin/env python3
"""Epic #204 Phase G (#211) performance-parity protocol driver.

Compares the PRE-EPIC baseline binary against the CURRENT binary on the SAME
machine / build flags / device / seed / settings, using the existing in-game
`--benchmark` (frame percentiles, CPU scopes, worker throughput, GPU timestamps,
queue-depth peaks, resident CPU/GPU bytes via workload telemetry).

Protocol (issue #211): repeated INTERLEAVED trials (baseline, current, baseline,
current, ...) to cancel slow machine-state drift; per-metric median across
trials. A metric whose MEDIAN regresses beyond the noise band is surfaced as
"investigate" (per-trial unanimity is reported separately, not required to
surface a median regression — review #220). Unmatched metrics are shown as
explicit `n/a` rows and warned about loudly; a metric missing from EVERY trial
is a hard error (a report-format change must not silently narrow the verdict).
The two binaries must be DIFFERENT builds (the report `Revision:` git hash is
compared) so passing the same exe twice cannot yield spurious perfect parity.

This is a GPU verification tool, NOT a CTest: it needs a Vulkan device and two
built game binaries. Run from the repository root, e.g.:

    python tests/bench_parity_211.py \
        --baseline <pre-epic-build>/ft_vox --current build/Release/ft_vox \
        --trials 3 --duration 20 --seed 42 --out docs/benchmarks/issue-211-parity.md

The committed report records binary basenames + git revisions, never local
absolute paths. Python 3 stdlib only.
"""

from __future__ import annotations

import argparse
import re
import statistics
import subprocess
import sys
from pathlib import Path

# metric key -> (regex with one capture group, unit, higher_is_better)
METRICS = {
    "score":        (r"Score:\s*(\d+)\s*/", "score", True),
    "frame_avg":    (r"avg\s+([\d.]+)\s+min", "ms", False),
    "frame_p50":    (r"p50\s+([\d.]+)\s+p95", "ms", False),
    "frame_p95":    (r"p50\s+[\d.]+\s+p95\s+([\d.]+)\s+p99", "ms", False),
    "frame_p99":    (r"p99\s+([\d.]+)\n", "ms", False),
    "avg_fps":      (r"avg FPS\s+([\d.]+)", "fps", True),
    "fps_1low":     (r"1% low FPS\s+([\d.]+)", "fps", True),
    "over33":       (r">33\.3ms:\s*(\d+)", "frames", False),
    "streaming":    (r"Streaming\s+([\d.]+)", "ms", False),
    "record":       (r"Record\s+([\d.]+)\s+MeshUpload", "ms", False),
    "terraingen":   (r"TerrainGen\s+n=\d+\s+avgMs=([\d.]+)", "ms/chunk", False),
    "meshbuild":    (r"MeshBuild\s+n=\d+\s+avgMs=([\d.]+)", "ms/mesh", False),
    "gpu_avg":      (r"samples=\d+\s+avgMs=([\d.]+)", "ms", False),
    "gpu_p95":      (r"p95Ms=([\d.]+)\s+p99Ms", "ms", False),
    "peak_chunks":  (r"Peaks:\s*chunks=(\d+)", "chunks", False),
    "peak_qgen":    (r"qLoad/Gen/Mesh(?:/Light)?=\d+/(\d+)/\d+", "jobs", False),
    "peak_qmesh":   (r"qLoad/Gen/Mesh(?:/Light)?=\d+/\d+/(\d+)", "jobs", False),
    "voxel_peak":   (r"voxel\.bytes\s+current=\d+\s+peak=(\d+)", "bytes", False),
    "gpu_live_peak": (r"gpu\.live\.bytes\s+current=\d+\s+peak=(\d+)", "bytes", False),
}
NOISE_BAND_PCT = 5.0  # per-metric noise band for surfacing a median regression
REVISION_RE = re.compile(r"Revision:\s*([0-9a-fA-F]+)")


def parse_report(text: str) -> tuple[dict[str, float], str, list[str]]:
    """Return (numeric metrics, git revision, list of METRICS keys not matched).

    Missing metrics are reported explicitly (never silently dropped) so a
    report-format change cannot quietly narrow the parity verdict.
    """
    out: dict[str, float] = {}
    missing: list[str] = []
    for key, (pattern, _unit, _hib) in METRICS.items():
        m = re.search(pattern, text)
        if m:
            out[key] = float(m.group(1))
        else:
            missing.append(key)
    grade = re.search(r"Grade:\s*([A-FS])", text)
    if grade:
        out["grade"] = grade.group(1)  # type: ignore[assignment]
    rev = REVISION_RE.search(text)
    revision = rev.group(1).lower() if rev else ""
    return out, revision, missing


def run_trial(exe: str, duration: int, seed: int, cwd: Path) -> tuple[dict[str, float], str, list[str]]:
    proc = subprocess.run(
        [exe, "--seed", str(seed), "--benchmark", str(duration)],
        cwd=str(cwd), capture_output=True, text=True, timeout=duration + 180)
    if proc.returncode != 0:
        raise RuntimeError(f"{Path(exe).name} benchmark exited {proc.returncode}: {proc.stderr[-500:]}")
    metrics, revision, missing = parse_report(proc.stdout)
    if "frame_p50" not in metrics:
        raise RuntimeError(f"could not parse benchmark report from {Path(exe).name}")
    if not revision:
        raise RuntimeError(f"no Revision: git hash in report from {Path(exe).name} "
                           "(cannot validate the two binaries are different builds)")
    return metrics, revision, missing


def aggregate(trials: list[dict[str, float]]) -> dict[str, float]:
    med: dict[str, float] = {}
    keys = set().union(*(t.keys() for t in trials))
    for k in keys:
        vals = [t[k] for t in trials if k in t and isinstance(t[k], (int, float))]
        if vals:
            med[k] = statistics.median(vals)
    return med


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--current", required=True)
    ap.add_argument("--trials", type=int, default=3)
    ap.add_argument("--duration", type=int, default=20)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    cwd = Path.cwd()
    base_trials: list[dict[str, float]] = []
    cur_trials: list[dict[str, float]] = []
    base_rev = cur_rev = ""
    unmatched: set[str] = set()
    for i in range(args.trials):
        print(f"trial {i + 1}/{args.trials}: baseline ...", flush=True)
        bm, brev, bmiss = run_trial(args.baseline, args.duration, args.seed, cwd)
        base_trials.append(bm)
        base_rev = brev
        unmatched.update(bmiss)
        print(f"trial {i + 1}/{args.trials}: current ...", flush=True)
        cm, crev, cmiss = run_trial(args.current, args.duration, args.seed, cwd)
        cur_trials.append(cm)
        cur_rev = crev
        unmatched.update(cmiss)

    # The two binaries MUST be different builds, else "parity" is spurious.
    if base_rev == cur_rev:
        raise RuntimeError(
            f"baseline and current report the SAME git revision ({base_rev}) — "
            "pass two DIFFERENT builds (passing one exe twice yields spurious parity)")

    # A metric absent from EVERY trial of a binary is a format failure, not a
    # legitimate n/a: fail loudly rather than silently narrow the verdict.
    base_keys = set().union(*(t.keys() for t in base_trials))
    cur_keys = set().union(*(t.keys() for t in cur_trials))
    dead = [k for k in METRICS if k not in base_keys or k not in cur_keys]
    if dead:
        raise RuntimeError(
            "metric(s) missing from every trial of a binary (report-format change?): "
            + ", ".join(dead) + " — fix the parser, do not silently narrow the verdict")

    base = aggregate(base_trials)
    cur = aggregate(cur_trials)
    base_grade = next((t.get("grade") for t in base_trials if t.get("grade")), "?")
    cur_grade = next((t.get("grade") for t in cur_trials if t.get("grade")), "?")

    lines = [
        "# Epic #204 Phase G (#211) — performance parity (interleaved trials)",
        "",
        f"- trials per binary: **{args.trials}** (interleaved baseline/current)",
        f"- benchmark: `--seed {args.seed} --benchmark {args.duration}` (same device/flags/settings)",
        f"- baseline: `{Path(args.baseline).name}` (git `{base_rev}`)",
        f"- current: `{Path(args.current).name}` (git `{cur_rev}`)",
        "",
        "| Metric | Unit | Baseline (median) | Current (median) | Delta | Investigate? |",
        "|---|---|---:|---:|---:|---|",
    ]
    investigate = []
    for key, (_pat, unit, hib) in METRICS.items():
        if key not in base or key not in cur:
            # Explicit n/a row (never silently dropped). `dead` above already
            # errored on metrics absent from every trial, so this is a partial
            # miss (e.g. GPU timestamps unavailable on one binary).
            lines.append(f"| {key} | {unit} | n/a | n/a | n/a | **unmatched — see warning** |")
            continue
        b, c = base[key], cur[key]
        delta_pct = ((c - b) / b * 100.0) if b else 0.0
        worse = (c < b) if hib else (c > b)
        # Per-trial unanimity is reported, but a MEDIAN regression beyond the
        # band surfaces as "investigate" regardless (review #220 finding 2).
        consistent = True
        for bt, ct in zip(base_trials, cur_trials):
            if key not in bt or key not in ct:
                consistent = False
                break
            bb, cc = bt[key], ct[key]
            d = ((cc - bb) / bb * 100.0) if bb else 0.0
            trial_worse = (cc < bb) if hib else (cc > bb)
            if not trial_worse or abs(d) < NOISE_BAND_PCT:
                consistent = False
                break
        median_regression = worse and abs(delta_pct) >= NOISE_BAND_PCT
        if median_regression:
            investigate.append(key)
            flag = "**INVESTIGATE** (all trials)" if consistent else "**INVESTIGATE** (median; not all trials)"
        else:
            flag = "no"
        lines.append(f"| {key} | {unit} | {b:.4g} | {c:.4g} | {delta_pct:+.1f}% | {flag} |")
    lines.append("")
    lines.append(f"Grade: baseline {base_grade} / current {cur_grade}")
    lines.append("")
    if unmatched:
        lines.append(f"> **WARNING — unmatched metrics:** {', '.join(sorted(unmatched))}. "
                     "These were not parsed from at least one report (shown as `n/a`); the "
                     "verdict is narrowed for them. Check the report format / GPU availability.")
        lines.append("")
    if investigate:
        lines.append(f"**Median regressions above the {NOISE_BAND_PCT}% noise band: "
                     f"{', '.join(investigate)} — investigate (do not hide).** "
                     "'(all trials)' = consistent across every interleaved trial; "
                     "'(median; not all trials)' = the median regresses but at least one trial "
                     "was faster (machine-state spread). Both warrant investigation.")
    else:
        lines.append(f"**No median regression above the {NOISE_BAND_PCT}% noise band across "
                     f"{args.trials} interleaved trials.** Per-trial spread is machine noise; "
                     "deterministic workload character (chunks drawn, meshes built, queue peaks, "
                     "resident bytes) is unchanged.")
    report = "\n".join(lines) + "\n"
    # Per-trial appendix: lets a reviewer judge consistency vs machine noise.
    detail_keys = ["frame_p50", "frame_p95", "frame_p99", "fps_1low", "streaming",
                   "terraingen", "meshbuild", "gpu_avg", "peak_chunks"]
    report += "\n## Per-trial values (interleaved)\n\n"
    report += "| Trial | " + " | ".join(detail_keys) + " |\n"
    report += "|---|" + "---:|" * len(detail_keys) + "\n"
    for i in range(args.trials):
        for label, trials in (("baseline", base_trials), ("current", cur_trials)):
            row = [f"{i + 1} {label}"]
            for k in detail_keys:
                row.append(f"{trials[i].get(k, float('nan')):.4g}")
            report += "| " + " | ".join(row) + " |\n"
    print(report)
    if args.out:
        Path(args.out).write_text(report, encoding="utf-8")
        print(f"written: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
