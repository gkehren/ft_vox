#!/usr/bin/env python3
"""Scripted multiplayer regression gate for the CPU-only headless runner."""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import subprocess
import tempfile
from itertools import zip_longest
from pathlib import Path

from test_headless_runner import HEX16, json_summary, run_runner, summary, write_script


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_match(expected: tuple[dict, list[str]], actual: tuple[dict, list[str]]) -> None:
    require(actual[0] == expected[0], f"summary differed\nexpected: {expected[0]}\nactual:   {actual[0]}")
    for expected_line, actual_line in zip_longest(expected[1], actual[1]):
        require(actual_line == expected_line,
                f"first trace difference\nexpected: {expected_line}\nactual:   {actual_line}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--fixtures", required=True, type=Path)
    options = parser.parse_args()
    runner = str(options.runner.resolve())
    fixtures = options.fixtures.resolve()
    race = fixtures / "sim-multiplayer-races.txt"
    residency = fixtures / "sim-multiplayer-residency.txt"
    reopen = fixtures / "sim-multiplayer-reopen.txt"
    remote_area = "--digest-area", "0,219,0,544,250,24"

    with tempfile.TemporaryDirectory(prefix="ft_vox_multi_") as directory:
        work = Path(directory)

        def run(script: str | Path, ticks: int, workers: int, *extra: str,
                expected_exit: int = 0, json: bool = True) -> subprocess.CompletedProcess:
            args = ["--seed", "42", "--ticks", str(ticks), "--script", str(script),
                    "--workers", str(workers), "--trace", *extra]
            if json:
                args.append("--json")
            command = [runner, *args]
            replay = subprocess.list2cmdline(command) if os.name == "nt" else shlex.join(command)
            print(replay, flush=True)
            proc = run_runner(runner, args, work)
            require(proc.returncode == expected_exit,
                    f"expected exit {expected_exit}, got {proc.returncode}\n"
                    f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
            return proc

        def snapshot(proc: subprocess.CompletedProcess) -> tuple[dict, list[str]]:
            data = json_summary(proc)
            require(data.get("digest_version") == "FTVHDIG3" and
                    HEX16.fullmatch(data.get("digest", "")),
                    f"missing multiplayer digest in {proc.stdout}")
            del data["workers"]
            return data, [line for line in proc.stderr.splitlines() if line.startswith("TRACE ")]

        baseline = snapshot(run(race, 49, 0))
        data, trace = baseline
        require(data["edits"] == 2 and data["rejected"] == 2 and data["edit_revision"] == 3,
                f"placement and break must each commit once: {data}")
        require(data["item_ids"] == [2] and data["item_high_water"] == 2,
                f"only the manual drop should remain: {data}")
        local, rejoined = data["players"]
        require((local["id"], local["inventory"], rejoined["id"], rejoined["inventory"]) == (1, 4, 3, 0),
                f"session state leaked through removal or rejoin: {data}")
        require(local["position"][0] > 8.5 and rejoined["position"][0] == 12.5 and
                rejoined["position"][1] < 225 and rejoined["position"][2] == 8.5,
                f"active selection, retired input or flight leaked into walking: {data}")
        require(any("player=2 op=BREAK result=Ok edit_id=2 edit_revision=3 "
                    "item_high_water=1 inventory=2 item_ids=1" in line for line in trace),
                f"trace omitted the winning command and its shared effects: {trace}")
        for workers in (0, 1, 2, 2, 4):
            require_match(baseline, snapshot(run(race, 49, workers, "--expect-digest", data["digest"])))

        # A peer's inventory changes the digest even when the local summary and
        # shared world are identical. A rejoined actor already has slot 0
        # selected, and changing the script's active actor has no effect.
        script = race.read_text(encoding="utf-8")
        peer_grant = write_script(work / "peer-grant.txt", script + "\ngive Glass 1\n")
        selection = write_script(work / "select-local.txt", script + "\nselect 0\nplayer select 1\n")
        changed = json_summary(run(peer_grant, 49, 0))
        require(changed["digest"] != data["digest"] and changed["inventory"] == data["inventory"],
                "multiplayer digest ignored a peer-only inventory mutation")
        require(json_summary(run(selection, 49, 0))["digest"] == data["digest"],
                "the rejoined hotbar should start at slot 0 and script actor selection is host state")
        text = summary(run(race, 49, 0, json=False))
        require(text.get("digest") == data["digest"] and text.get("player_inventories") == "1:4,3:0"
                and text.get("item_ids") == "2", f"text summary lost multiplayer state: {text}")

        # Run the eviction/regeneration path without persistence as well. It
        # must preserve the remote edit through the transient override map.
        source = residency.read_text(encoding="utf-8")
        transient = write_script(work / "transient.txt", source.replace("save\nexpect result Ok\n", "", 1))
        require_match(snapshot(run(transient, 7, 0, *remote_area)),
                      snapshot(run(transient, 7, 2, *remote_area)))

        saved_baseline = reopened_baseline = None
        for workers in (0, 2):
            save_args = "--world", "shared", "--saves-root", str(work / f"saved-{workers}")
            saved = snapshot(run(residency, 7, workers, *save_args, *remote_area))
            reopened = snapshot(run(reopen, 3, workers, *save_args, *remote_area))
            require(saved[0]["chunks"] == 18 and saved[0]["save"] == "ok" and
                    reopened[0]["save"] == "ok" and reopened[0]["item_ids"] == [1, 2],
                    "save/reopen lost a shared edit, player disk or item identity")
            if saved_baseline is None:
                saved_baseline, reopened_baseline = saved, reopened
            else:
                require_match(saved_baseline, saved)
                require_match(reopened_baseline, reopened)

        # Exit immediately after the verified save with a deliberate assertion
        # failure. Shutdown writes one newer checkpoint. Recover the exact
        # explicit-save generation via its retained manifest in this temp world.
        prefix, marker, _ = source.partition("\nexpect pending on\nwait 1\n")
        require(bool(marker), "missing boundary after the deferred-save assertion")
        barrier_script = write_script(work / "barrier.txt",
                                      prefix + "\nexpect pending on\nexpect items 0\n")
        barrier_root = work / "barrier"
        barrier_args = "--world", "shared", "--saves-root", str(barrier_root)
        interrupted = run(barrier_script, 2, 2, *barrier_args, expected_exit=5)
        require("expected 0 live item entities, got 1" in interrupted.stderr,
                "intended stop did not occur after the explicit save")
        checkpoints = barrier_root / "shared" / "checkpoints"
        shutil.copyfile(checkpoints / "manifest.prev", checkpoints / "manifest")
        verified = json_summary(run(reopen, 3, 0, *barrier_args, *remote_area))
        require(verified["save"] == "ok" and verified["item_ids"] == [1, 2],
                "the explicit barrier failed to capture the deferred edit and gameplay state")

        failures = (
            ("player create\nplayer remove 2\nplayer select 2\n", 3, "PlayerId 2"),
            ("player remove 1\n", 3, "cannot remove the local"),
            ("player select 4294967296\n", 3, "positive PlayerId"),
            ("interest radius -1\n", 3, "interest radius expects chunks"),
            ("interest 1 2 3\nplayer create\n", 3, "body-centered 'interest radius'"),
            ("player create\nexpect result WorldRejected\n", 5,
             "script line 2 at frame 0 for PlayerId 2: expected result WorldRejected, got Ok"),
        )
        for script, code, error in failures:
            path = write_script(work / "invalid.txt", script)
            proc = run(path, 1, 0, expected_exit=code)
            require(error in proc.stderr, f"missing expected diagnostic {error!r}: {proc.stderr}")
        proc = run(race, 49, 0, "--interest", "1,2,3", expected_exit=2)
        require("multiplayer uses body-centered 'interest radius'" in proc.stderr,
                "pinned CLI interest conflicted with multiplayer")


if __name__ == "__main__":
    main()
