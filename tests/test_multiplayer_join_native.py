#!/usr/bin/env python3
"""Opt-in native smoke for in-process client joins and projection teardown."""

from __future__ import annotations

import argparse
import csv
import os
import subprocess
import tempfile
from pathlib import Path

from test_server_lifecycle import TIMEOUT, check_exit, live_server, require, wait_for_record


def run_client(client: str, first_port: str, second_port: str, profiles: Path,
               trace: Path | None) -> subprocess.CompletedProcess:
    profiles.mkdir()
    env = os.environ.copy()
    if trace is None:
        env.pop("FT_VOX_MOTION_TRACE", None)
    else:
        env["FT_VOX_MOTION_TRACE"] = str(trace)
    result = subprocess.run([client, "--join-smoke", f"127.0.0.1:{first_port}",
                             f"127.0.0.1:{second_port}", str(profiles)],
                            text=True, errors="replace", capture_output=True, timeout=TIMEOUT, env=env)
    require(result.returncode == 0,
            f"native multiplayer join smoke exited {result.returncode}\n{result.stdout}{result.stderr}")
    return result


def verify_traces(paths: list[Path]) -> None:
    header = None
    for path in paths:
        with path.open(newline="") as stream:
            rows = csv.reader(stream)
            columns = next(rows, [])
            require(columns and columns[0] == "time_ms", f"missing trace header in {path}")
            if header is None:
                header = columns
            require(columns == header, f"trace header changed in {path}")
            first_row = next(rows, [])
            require(len(first_row) == len(header), f"trace was not flushed to {path}")
            require(all(first_row[header.index(column)] == "0" for column in
                        ("inputs_sent", "inputs_failed", "first_input_tick", "last_input_tick", "last_input_seq")),
                    f"new trace retained earlier input counters in {path}")
            require(all(len(row) == len(header) for row in rows), f"invalid trace row in {path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    args = parser.parse_args()
    server, client = str(args.server.resolve()), str(args.client.resolve())
    with tempfile.TemporaryDirectory(prefix="ft_vox_join_") as directory:
        root = Path(directory)
        first_stop, second_stop = root / "stop-first", root / "stop-second"
        first_log, second_log = root / "first.log", root / "second.log"

        def server_args(world: str, seed: str, stop: Path) -> list[str]:
            return [server, "--world", world, "--seed", seed, "--saves-root", directory,
                    "--port", "0", "--stop-file", str(stop), "--interest-radius", "1", "--player-cap", "2"]

        with live_server(server_args("first", "42", first_stop), first_log) as first, \
                live_server(server_args("second", "43", second_stop), second_log) as second:
            first_port = wait_for_record(first, first_log, "ready")["port"]
            second_port = wait_for_record(second, second_log, "ready")["port"]
            trace = root / "motion.csv"
            collision = root / "motion.session-3.csv"
            collision.write_bytes(b"previous trace\n")
            result = run_client(client, first_port, second_port, root / "traced", trace)
            print(result.stdout, end="")
            first_files = [trace, *(root / f"motion.session-{i}.csv" for i in (2, 4, 5, 6, 7))]
            verify_traces(first_files)
            originals = {path: path.read_bytes() for path in [*first_files, collision]}
            require(originals[collision] == b"previous trace\n", "filename collision overwrote a trace")

            run_client(client, first_port, second_port, root / "restarted", trace)
            verify_traces([root / f"motion.session-{i}.csv" for i in range(8, 14)])
            require(all(path.read_bytes() == data for path, data in originals.items()),
                    "a second process replaced an earlier trace")

            all_traces = set(root.glob("motion*.csv"))
            disabled = run_client(client, first_port, second_port, root / "disabled", None)
            require("Motion trace:" not in disabled.stdout and "Could not open motion trace" not in disabled.stderr,
                    "disabled tracing attempted to initialize a trace")
            require(set(root.glob("motion*.csv")) == all_traces, "disabled tracing created files")

            bad_path = root / "missing" / "motion.csv"
            failure = run_client(client, first_port, second_port, root / "trace-failure", bad_path)
            require("Could not open motion trace" in failure.stderr and not bad_path.parent.exists(),
                    "trace open failure should be reported while joins continue")
            print("Motion tracing passed across switches, process restarts, disabled tracing and open failures.")
            first_stop.touch()
            second_stop.touch()
            check_exit(first, first_log, 0, "admin")
            check_exit(second, second_log, 0, "admin")


if __name__ == "__main__":
    main()
