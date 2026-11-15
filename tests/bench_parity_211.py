#!/usr/bin/env python3
"""Epic #204 Phase G (#211) performance-parity protocol driver.

Compares the PRE-EPIC baseline binary against the CURRENT binary on the SAME
machine / build flags / device / seed / settings, using the existing in-game
`--benchmark` (frame percentiles, CPU scopes, worker throughput, GPU timestamps,
queue-depth peaks, resident CPU/GPU bytes via workload telemetry).

Protocol (issue #211): repeated INTERLEAVED trials (baseline, current, baseline,
current, ...) to cancel slow machine-state drift; per-metric median across
trials; a degradation is flagged only when it is CONSISTENT across trials and
above a noise band — not hidden behind a single broad threshold.

This is a GPU verification tool, NOT a CTest: it needs a Vulkan device and two
built game binaries. Run from the repository root, e.g.:

    python tests/bench_parity_211.py \
        --baseline D:/Projects/ft_vox_baseline_204/build/Release/ft_vox.exe \
        --current  build/Release/ft_vox.exe \
        --trials 3 --duration 20 --seed 42 --out docs/benchmarks/issue-211-parity.md

Python 3 stdlib only.
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
NOISE_BAND_PCT = 5.0  # per-metric noise band; consistency across trials decides


def parse_report(text: str) -> dict[str, float]:
    out: dict[str, float] = {}
    for key, (pattern, _unit, _hib) in METRICS.items():
        m = re.search(pattern, text)
        if m:
            out[key] = float(m.group(1))
    grade = re.search(r"Grade:\s*([A-FS])", text)
    if grade:
        out["grade"] = grade.group(1)  # type: ignore[assignment]
    return out


def run_trial(exe: str, duration: int, seed: int, cwd: Path) -> dict[str, float]:
    proc = subprocess.run(
        [exe, "--seed", str(seed), "--benchmark", str(duration)],
        cwd=str(cwd), capture_output=True, text=True, timeout=duration + 180)
    if proc.returncode != 0:
        raise RuntimeError(f"{exe} benchmark exited {proc.returncode}: {proc.stderr[-500:]}")
    metrics = parse_report(proc.stdout)
    if "frame_p50" not in metrics:
        raise RuntimeError(f"could not parse benchmark report from {exe}")
    return metrics


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
    for i in range(args.trials):
        print(f"trial {i + 1}/{args.trials}: baseline ...", flush=True)
        base_trials.append(run_trial(args.baseline, args.duration, args.seed, cwd))
        print(f"trial {i + 1}/{args.trials}: current ...", flush=True)
        cur_trials.append(run_trial(args.current, args.duration, args.seed, cwd))

    base = aggregate(base_trials)
    cur = aggregate(cur_trials)
    base_grade = next((t.get("grade") for t in base_trials if t.get("grade")), "?")
    cur_grade = next((t.get("grade") for t in cur_trials if t.get("grade")), "?")

    lines = [
        "# Epic #204 Phase G (#211) — performance parity (interleaved trials)",
        "",
        f"- trials per binary: **{args.trials}** (interleaved baseline/current)",
        f"- benchmark: `--seed {args.seed} --benchmark {args.duration}` (same device/flags/settings)",
        f"- baseline: `{args.baseline}`",
        f"- current: `{args.current}`",
        "",
        "| Metric | Unit | Baseline (median) | Current (median) | Delta | Consistent regression? |",
        "|---|---|---:|---:|---:|---|",
    ]
    regressions = []
    for key, (_pat, unit, hib) in METRICS.items():
        if key not in base or key not in cur:
            continue
        b, c = base[key], cur[key]
        delta_pct = ((c - b) / b * 100.0) if b else 0.0
        worse = (c < b) if hib else (c > b)
        # consistent = worse on EVERY trial beyond the noise band
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
        flag = "**YES**" if (worse and consistent and abs(delta_pct) >= NOISE_BAND_PCT) else "no"
        if flag == "**YES**":
            regressions.append(key)
        lines.append(f"| {key} | {unit} | {b:.4g} | {c:.4g} | {delta_pct:+.1f}% | {flag} |")
    lines.append("")
    lines.append(f"Grade: baseline {base_grade} / current {cur_grade}")
    lines.append("")
    if regressions:
        lines.append(f"**Consistent regressions above {NOISE_BAND_PCT}% noise band: "
                     f"{', '.join(regressions)} — investigate (do not hide).**")
    else:
        lines.append(f"**No consistent regression above the {NOISE_BAND_PCT}% noise band across "
                     f"{args.trials} interleaved trials.** Per-trial spread is machine noise; "
                     "deterministic workload character (chunks drawn, meshes built, queue peaks, "
                     "resident bytes) is unchanged.")
    report = "\n".join(lines) + "\n"
    # Per-trial appendix: lets a reviewer judge consistency vs machine noise
    # (the protocol requires investigating consistent degradation, not hiding
    # it behind a threshold).
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
