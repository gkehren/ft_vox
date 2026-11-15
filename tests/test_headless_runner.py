#!/usr/bin/env python3
"""Headless runner acceptance driver (issue #210, Epic #204 Phase F).

Exercises the ft_vox_headless executable end-to-end with NO GPU, window or
shader asset. Every run is a separate process with a bounded wall-clock
timeout and a deterministic end condition (fixed tick budget); persistent
runs use an isolated temporary saves root, never the real user saves.

Covers the issue #210 acceptance list:
  * fixed-seed scripted simulation twice -> identical authoritative digest
    (synchronous AND async-worker modes; both must agree);
  * save -> process exit -> reload -> edit verification (3 processes);
  * invalid script / assertion mismatch / world-open failure / usage error
    -> distinct non-zero exit codes;
  * --expect-digest gate (pass and fail);
  * run timeout -> exit 6 (teardown never stalls indefinitely).

Usage (registered as CTest HeadlessRunner):
    python3 tests/test_headless_runner.py --runner <path> --fixtures <dir>

Python 3 stdlib only.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

CHECKS = 0
FAILS = 0
HEX16 = re.compile(r"^[0-9a-f]{16}$")
RUN_TIMEOUT = 180  # per-process backstop; the runner has its own --timeout


def check(cond: bool, msg: str) -> None:
    global CHECKS, FAILS
    CHECKS += 1
    if not cond:
        FAILS += 1
        print(f"FAIL: {msg}")
    else:
        print(f"ok: {msg}")


def run_runner(runner: str, args: list[str], cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [runner, *args],
        cwd=str(cwd),
        capture_output=True,
        text=True,
        timeout=RUN_TIMEOUT,
    )


def summary(proc: subprocess.CompletedProcess) -> dict[str, str]:
    for line in proc.stdout.splitlines():
        if line.startswith("SUMMARY "):
            fields = {}
            for token in line[len("SUMMARY "):].split():
                key, _, value = token.partition("=")
                fields[key] = value
            return fields
    return {}


def write_script(path: Path, text: str) -> str:
    path.write_text(text, encoding="utf-8")
    return str(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, help="path to the ft_vox_headless executable")
    parser.add_argument("--fixtures", required=True, help="path to tests/fixtures")
    args = parser.parse_args()

    runner = str(Path(args.runner).resolve())
    fixtures = Path(args.fixtures).resolve()
    fixture = fixtures / "sim-actions.txt"
    if not Path(runner).is_file():
        print(f"FAIL: runner not found: {runner}")
        return 1
    if not fixture.is_file():
        print(f"FAIL: fixture not found: {fixture}")
        return 1

    with tempfile.TemporaryDirectory(prefix="ft_vox_headless_") as tmp:
        work = Path(tmp)

        # --- 1. Smoke: plain fixed-seed run ---------------------------------
        proc = run_runner(runner, ["--seed", "42", "--ticks", "120"], work)
        check(proc.returncode == 0, f"smoke run exits 0 (got {proc.returncode}: {proc.stderr.strip()})")
        fields = summary(proc)
        check(bool(HEX16.match(fields.get("digest", ""))), "smoke summary carries a 16-hex digest")
        check(fields.get("save") == "none", "transient smoke run reports save=none")
        check(fields.get("ticks") == "120", "smoke run executed the requested tick budget")

        # --- 2. Scripted determinism: synchronous, twice ---------------------
        script_args = ["--seed", "42", "--ticks", "600", "--script", str(fixture)]
        a = run_runner(runner, script_args, work)
        b = run_runner(runner, script_args, work)
        check(a.returncode == 0 and b.returncode == 0, "scripted sync runs exit 0")
        fa, fb = summary(a), summary(b)
        check(bool(fa) and fa == fb, "scripted sync summaries identical across runs")
        check(bool(HEX16.match(fa.get("digest", ""))), "scripted sync digest is canonical hex")
        check(fa.get("edits") == "4", "scripted fixture committed 4 edits (3 places + 1 delete)")
        check(fa.get("refused") == "0", "no placement refusals in the fixture")
        sync_digest = fa.get("digest", "")

        # --- 3. Scripted determinism: async workers, twice, == sync ----------
        async_args = script_args + ["--workers", "2"]
        c = run_runner(runner, async_args, work)
        d = run_runner(runner, async_args, work)
        check(c.returncode == 0 and d.returncode == 0, "scripted async runs exit 0")
        fc, fd = summary(c), summary(d)
        check(bool(fc) and fc == fd, "scripted async summaries identical across runs")
        check(fc.get("digest") == sync_digest,
              "async-worker digest equals synchronous digest (same authoritative behavior)")

        # --- 4. --expect-digest gate -----------------------------------------
        ok = run_runner(runner, script_args + ["--expect-digest", sync_digest], work)
        check(ok.returncode == 0, "--expect-digest with the recorded digest exits 0")
        bad = run_runner(runner, script_args + ["--expect-digest", "0" * 16], work)
        check(bad.returncode == 5, "--expect-digest mismatch exits 5")

        # --- 5. Save -> process exit -> reload -> edit verification ----------
        saves_root = work / "saves"
        world_args = ["--seed", "42", "--world", "roundtrip", "--saves-root", str(saves_root)]
        s1 = write_script(work / "s1.txt",
                          "place 8 220 8 Stone\n"
                          "expect voxel 8 220 8 Stone\n"
                          "wait 60\n"
                          "save\n")
        r1 = run_runner(runner, world_args + ["--ticks", "120", "--script", s1], work)
        check(r1.returncode == 0, f"save run exits 0 (got {r1.returncode}: {r1.stderr.strip()})")
        check(summary(r1).get("save") == "ok", "save run reports save=ok")
        world_dir = saves_root / "roundtrip"
        check((world_dir / "world.meta").is_file(), "world.meta written")
        check((world_dir / "player.state").is_file(), "player.state written")

        # Second PROCESS: reload, verify the persisted edit, revert it.
        s2 = write_script(work / "s2.txt",
                          "expect voxel 8 220 8 Stone\n"
                          "delete 8 220 8\n"
                          "expect voxel 8 220 8 air\n"
                          "wait 60\n")
        r2 = run_runner(runner, world_args + ["--ticks", "120", "--script", s2], work)
        check(r2.returncode == 0, f"reload run exits 0 (got {r2.returncode}: {r2.stderr.strip()})")
        check(summary(r2).get("save") == "ok", "reload run reports save=ok")

        # Third PROCESS: the revert must be durable too.
        s3 = write_script(work / "s3.txt",
                          "expect voxel 8 220 8 air\n"
                          "wait 30\n")
        r3 = run_runner(runner, world_args + ["--ticks", "60", "--script", s3], work)
        check(r3.returncode == 0, f"second reload exits 0 (got {r3.returncode}: {r3.stderr.strip()})")

        # --- 6. Failure paths: distinct non-zero exit codes ------------------
        bad_script = write_script(work / "bad.txt", "wait 10\nfly to the moon\n")
        p = run_runner(runner, ["--script", bad_script, "--ticks", "60"], work)
        check(p.returncode == 3, f"invalid script exits 3 (got {p.returncode})")

        missing = run_runner(runner, ["--script", str(work / "missing.txt"), "--ticks", "60"], work)
        check(missing.returncode == 3, "missing script file exits 3")

        over = write_script(work / "over.txt", "wait 100\n")
        p = run_runner(runner, ["--script", over, "--ticks", "10"], work)
        check(p.returncode == 3, "script exceeding --ticks exits 3")

        mismatch = write_script(work / "mismatch.txt", "expect voxel 8 220 8 Dirt\n")
        p = run_runner(runner, ["--seed", "42", "--ticks", "30", "--script", mismatch], work)
        check(p.returncode == 5, f"expect-voxel mismatch exits 5 (got {p.returncode})")

        # Seed mismatch against the existing save: the runner must refuse the
        # transient fallback and report a world-open error.
        p = run_runner(runner, ["--seed", "43", "--world", "roundtrip",
                                "--saves-root", str(saves_root), "--ticks", "30"], work)
        check(p.returncode == 4, f"seed-mismatch world open exits 4 (got {p.returncode})")

        p = run_runner(runner, ["--no-such-flag"], work)
        check(p.returncode == 2, f"unknown option exits 2 (got {p.returncode})")

        # Non-finite / malformed numeric options must be rejected (a NaN
        # --timeout would otherwise silently disable the wall-clock safety
        # net; issue #210 review).
        p = run_runner(runner, ["--ticks", "10", "--timeout", "nan"], work)
        check(p.returncode == 2, f"--timeout nan exits 2 (got {p.returncode})")
        p = run_runner(runner, ["--ticks", "10", "--interest", "nan,0,0"], work)
        check(p.returncode == 2, f"--interest nan exits 2 (got {p.returncode})")
        p = run_runner(runner, ["--ticks", "10", "--digest-area", "0,0,0,10.9,10,10"], work)
        check(p.returncode == 2, f"fractional --digest-area exits 2 (got {p.returncode})")
        p = run_runner(runner, ["--ticks", "10", "--digest-area", "0,0,0,1e10,10,10"], work)
        check(p.returncode == 2, f"out-of-range --digest-area exits 2 (got {p.returncode})")

        # World-level edit rejection (target outside residency) is counted and
        # diagnosed, not silent (issue #210 review).
        reject = write_script(work / "reject.txt", "place 50000 220 50000 Stone\nwait 10\n")
        p = run_runner(runner, ["--seed", "42", "--ticks", "30", "--script", reject], work)
        rf = summary(p)
        check(p.returncode == 0 and rf.get("rejected") == "1" and rf.get("edits") == "0",
              "out-of-residency place is counted as rejected, not committed")
        check("rejected by world" in p.stderr, "world rejection emits a stderr diagnostic")

        # --- 7. Timeout: bounded teardown, exit 6 -----------------------------
        p = run_runner(runner, ["--seed", "42", "--ticks", "1000000", "--timeout", "1"], work)
        check(p.returncode == 6, f"run timeout exits 6 (got {p.returncode})")

        # --- 8. JSON summary --------------------------------------------------
        p = run_runner(runner, ["--seed", "42", "--ticks", "60", "--json"], work)
        check(p.returncode == 0 and p.stdout.strip().startswith("{")
              and '"digest"' in p.stdout, "--json emits a JSON summary")

        # --- 9. Chunk load/unload/reload cycle via the interest source --------
        # Pin the interest probe far away (unloads the spawn region), then
        # follow the player again (reloads it). The script ASSERTS residency
        # churn rather than just exiting 0: a loaded air cell becomes
        # `unknown` once unloaded, then `air` again once reloaded — so the test
        # fails if streaming did nothing (issue #210 review).
        churn = write_script(work / "churn.txt",
                             "wait 30\n"
                             "expect voxel 8 220 8 air\n"
                             "interest 5000 100 5000\n"
                             "wait 120\n"
                             "expect voxel 8 220 8 unknown\n"
                             "interest follow\n"
                             "wait 120\n"
                             "expect voxel 8 220 8 air\n")
        p = run_runner(runner, ["--seed", "42", "--ticks", "300", "--script", churn], work)
        check(p.returncode == 0, f"interest load/unload/reload cycle asserts residency (got {p.returncode})")
        # Same cycle with async workers: pending gen jobs across the churn.
        p = run_runner(runner, ["--seed", "42", "--ticks", "300", "--script", churn, "--workers", "2"], work)
        check(p.returncode == 0, f"async interest churn asserts residency (got {p.returncode})")

    print(f"\n{CHECKS} checks, {FAILS} failures")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
