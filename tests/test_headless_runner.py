#!/usr/bin/env python3
"""Headless runner acceptance driver (issue #210, Epic #204 Phase F; gameplay
loop + closure gate issue #230, Epic #223 Phase G).

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

And the issue #230 gameplay additions:
  * the canonical fixture's gameplay leg (give/select/break/wait-pickup/
    placeSelected/drop) end-to-end, with a live item IN FLIGHT at the digest
    boundary — sync vs async digest parity covers item physics;
  * SUMMARY items= / inventory= / item_high_water= in key=value AND --json;
  * multi-process save/reload round-trip restoring inventory + item entities
    + the id allocator (no id reuse across sessions);
  * gameplay grammar error exits (bad item token, slot/count bounds) and
    expect inventory / expect items assertion mismatches (exit 5).

Usage (registered as CTest HeadlessRunner):
    python3 tests/test_headless_runner.py --runner <path> --fixtures <dir>

Python 3 stdlib only.
"""

from __future__ import annotations

import argparse
import json
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


def json_summary(proc: subprocess.CompletedProcess) -> dict:
    # grantItems logs to stdout, so the JSON object is the first '{' line —
    # not necessarily the first line of stdout (issue #230).
    for line in proc.stdout.splitlines():
        if line.startswith("{"):
            return json.loads(line)
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
        check(fa.get("edits") == "6",
              "scripted fixture committed 6 edits (3 places + 1 delete + 1 break + 1 placeSelected)")
        check(fa.get("refused") == "0", "no placement refusals in the fixture")
        # Gameplay leg end state (issue #230): the manual drop is still in
        # flight at the digest boundary; the break drop was auto-picked up.
        check(fa.get("items") == "1", "fixture ends with 1 live item entity (manual drop in flight)")
        check(fa.get("inventory") == "3", "fixture ends with inventory total 3 (Stone 5 - drop 2)")
        check(fa.get("item_high_water") == "2", "fixture id allocator high-water is 2 (no reuse)")
        sync_digest = fa.get("digest", "")

        # --- 3. Scripted determinism: async workers, twice, == sync ----------
        # The fixture ends with a live item in flight, so this parity check
        # covers item physics (issue #230), not just terrain/player/mobs.
        async_args = script_args + ["--workers", "2"]
        c = run_runner(runner, async_args, work)
        d = run_runner(runner, async_args, work)
        check(c.returncode == 0 and d.returncode == 0, "scripted async runs exit 0")
        fc, fd = summary(c), summary(d)
        check(bool(fc) and fc == fd, "scripted async summaries identical across runs")
        check(fc.get("digest") == sync_digest,
              "async-worker digest equals synchronous digest (item physics included)")

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
        # Extreme int-range bounds must be rejected by the volume clamp, not
        # overflow int32/uint64 and admit a billions-of-cells digest loop that
        # burns the wall-clock budget (issue #210 review round 2).
        p = run_runner(runner, ["--ticks", "10", "--timeout", "5",
                                "--digest-area", "-2147483647,0,-2147483647,2147483647,255,2147483647"], work)
        check(p.returncode == 2, f"extreme --digest-area span exits 2, no overflow stall (got {p.returncode})")

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

        # --- 10. Gameplay grammar error exits (issue #230) --------------------
        # Bounds are validated at PARSE time (exit 3), never at run time:
        # item tokens via the registry, slots/count/capacity ranges pinned.
        bad_grammar = [
            ("give NoSuchItem 1", "unknown item token"),
            ("give 0 1", "None sentinel is not a grantable item"),
            ("give Stone 65536", "count above uint16 range"),
            ("give Stone -1", "negative count"),
            ("select 9", "hotbar slot above 8"),
            ("select -1", "negative hotbar slot"),
            ("drop 65536", "drop count above uint16 range"),
            ("break 8 256 8", "break cell y outside the world"),
            ("placeSelected 8 -1 8", "placeSelected negative cell y"),
            ("expect inventory 36 Stone 1", "inventory slot above 35"),
            ("expect inventory 0 empty 1", "'empty' requires count 0"),
            ("expect inventory 0 NoSuchItem 1", "unknown expected item token"),
            ("expect items 1025", "expected count above the population cap"),
            ("expect items -1", "negative expected item count"),
        ]
        for text, why in bad_grammar:
            path = write_script(work / "grammar.txt", text + "\n")
            p = run_runner(runner, ["--seed", "42", "--ticks", "30", "--script", path], work)
            check(p.returncode == 3, f"grammar error exits 3: {why} ({text!r}, got {p.returncode})")

        # --- 11. Gameplay assertion mismatches exit 5 (issue #230) ------------
        mismatch = write_script(work / "inv-mismatch.txt", "expect inventory 0 Stone 99\n")
        p = run_runner(runner, ["--seed", "42", "--ticks", "30", "--script", mismatch], work)
        check(p.returncode == 5, f"expect-inventory mismatch exits 5 (got {p.returncode})")
        mismatch = write_script(work / "items-mismatch.txt", "expect items 3\n")
        p = run_runner(runner, ["--seed", "42", "--ticks", "30", "--script", mismatch], work)
        check(p.returncode == 5, f"expect-items mismatch exits 5 (got {p.returncode})")

        # --- 12. Gameplay save/reload round-trip (issue #230, 3 processes) ----
        # Inventory + item entities + the id allocator must survive a process
        # exit/reload; ids are NEVER reused (the high-water resumes).
        gp_root = work / "saves-gameplay"
        gp_args = ["--seed", "42", "--world", "items-rt", "--saves-root", str(gp_root)]
        g1 = write_script(work / "g1.txt",
                          "give Stone 5\n"
                          "select 0\n"
                          "drop 2\n"
                          "expect items 1\n"
                          "expect inventory 0 Stone 3\n"
                          "flight on\n"
                          "move 0 0\n"
                          "teleport 8.5 120 -20.5\n"   # hover away: no re-pickup
                          "wait 60\n"
                          "expect items 1\n"
                          "save\n")
        p1 = run_runner(runner, gp_args + ["--ticks", "120", "--script", g1], work)
        check(p1.returncode == 0, f"gameplay save run exits 0 (got {p1.returncode}: {p1.stderr.strip()})")
        f1 = summary(p1)
        check(f1.get("save") == "ok", "gameplay save run reports save=ok")
        check(f1.get("items") == "1" and f1.get("inventory") == "3",
              "gameplay save run ends with 1 live entity and inventory total 3")
        check(f1.get("item_high_water") == "1", "first session allocator high-water is 1")
        gp_dir = gp_root / "items-rt"
        check((gp_dir / "inventory.state").is_file(), "inventory.state written")
        check((gp_dir / "items.state").is_file(), "items.state written")

        # Second PROCESS: reload restores inventory + entity; a NEW drop must
        # get id 2 (high-water resumed), never reuse the restored entity's id 1.
        g2 = write_script(work / "g2.txt",
                          "expect inventory 0 Stone 3\n"
                          "expect items 1\n"
                          "select 0\n"
                          "drop 1\n"
                          "expect items 2\n"
                          "expect inventory 0 Stone 2\n"
                          "wait 30\n"
                          "save\n")
        p2 = run_runner(runner, gp_args + ["--ticks", "120", "--script", g2], work)
        check(p2.returncode == 0, f"gameplay reload run exits 0 (got {p2.returncode}: {p2.stderr.strip()})")
        f2 = summary(p2)
        check(f2.get("items") == "2", "reload restored the entity AND spawned a second one")
        check(f2.get("inventory") == "2", "reload restored the inventory (Stone 3 - drop 1)")
        check(f2.get("item_high_water") == "2",
              "allocator resumed at the persisted high-water (new id 2, no reuse)")

        # Third PROCESS: both entities + the drained inventory are durable.
        g3 = write_script(work / "g3.txt",
                          "expect inventory 0 Stone 2\n"
                          "expect items 2\n"
                          "wait 30\n")
        p3 = run_runner(runner, gp_args + ["--ticks", "60", "--script", g3], work)
        check(p3.returncode == 0, f"gameplay second reload exits 0 (got {p3.returncode}: {p3.stderr.strip()})")
        f3 = summary(p3)
        check(f3.get("items") == "2" and f3.get("inventory") == "2" and f3.get("item_high_water") == "2",
              "second reload restores inventory + both entities + allocator")

        # --- 13. JSON summary carries the gameplay fields (issue #230) --------
        # The fixture grants items (stdout log lines), so the JSON object is
        # located by its '{' line, not by whole-stdout prefix.
        p = run_runner(runner, script_args + ["--json"], work)
        check(p.returncode == 0, f"gameplay --json run exits 0 (got {p.returncode})")
        js = json_summary(p)
        check(js.get("items") == 1 and js.get("inventory") == 3 and js.get("item_high_water") == 2,
              "--json summary carries items/inventory/item_high_water")
        check(js.get("edits") == 6 and js.get("digest") == sync_digest,
              "--json summary agrees with the key=value gameplay run")

    print(f"\n{CHECKS} checks, {FAILS} failures")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
