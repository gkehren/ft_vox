#!/usr/bin/env python3
"""Commit and roll back two player profiles through live server process restarts."""

from __future__ import annotations

import argparse
import struct
import subprocess
import tempfile
import time
from pathlib import Path

from test_server_lifecycle import TIMEOUT, check_exit, live_server, require, wait_for_record


def run(command: list[str], expected: int = 0) -> str:
    result = subprocess.run(command, capture_output=True, text=True, timeout=TIMEOUT)
    output = result.stdout + result.stderr
    require(result.returncode == expected, f"{command} exited {result.returncode}\n{output}")
    return output


def read_client(client: str, mode: str, port: str) -> dict[str, int]:
    output = run([client, mode, port])
    fields = output.strip().split()
    require(fields and fields[0] == "RESTART", output)
    return {key: int(value) for key, value in (field.split("=", 1) for field in fields[1:])}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve())
    client = str(args.client.resolve())
    with tempfile.TemporaryDirectory(prefix="ft_vox_restart_") as directory:
        root = Path(directory)
        run([client, "seed", directory])
        base = [server, "--world", "restart", "--saves-root", directory, "--workers", "0"]
        stop = root / "stop"
        manifest = root / "restart" / "checkpoints" / "manifest"
        command = [*base, "--port", "0", "--stop-file", str(stop), "--interest-radius", "1",
                   "--metrics-every", "0"]

        log = root / "commit.log"
        with live_server(command, log) as process:
            ready = wait_for_record(process, log, "ready")
            concurrent = run([*base, "--ticks", "1"], expected=3)
            require("could not lock server world" in concurrent, concurrent)
            committed = read_client(client, "update", ready["port"])
            started = time.perf_counter()
            stop.touch()
            check_exit(process, log, 0, "admin")
            elapsed_ms = (time.perf_counter() - started) * 1000
        stop.unlink()
        baseline = manifest.read_bytes()
        generation, = struct.unpack_from("<Q", baseline, 8)
        contents = manifest.parent / f"generation-{generation}"
        size = len(baseline) + sum(path.stat().st_size for path in contents.rglob("*") if path.is_file())
        print(f"Two-player checkpoint: {size} bytes, {elapsed_ms:.1f} ms from stop signal to process exit",
              flush=True)

        log = root / "crash.log"
        with live_server(command, log) as process:
            ready = wait_for_record(process, log, "ready")
            crashed = read_client(client, "crash", ready["port"])
            process.kill()
            process.wait(timeout=TIMEOUT)
        require(manifest.read_bytes() == baseline, "uncommitted edits reached the manifest")

        log = root / "recover.log"
        with live_server(command, log) as process:
            ready = wait_for_record(process, log, "ready")
            recovered = read_client(client, "recover", ready["port"])
            stop.touch()
            check_exit(process, log, 0, "admin")
        require(committed["world"] == crashed["world"] == recovered["world"], "world identity changed")
        require(len({committed["epoch"], crashed["epoch"], recovered["epoch"]}) == 3,
                "transport epoch reused across process restarts")
        require(committed["drop"] == crashed["drop"] == recovered["drop"], "committed drop ID changed")
        require(committed["high_water"] < crashed["high_water"] < recovered["high_water"],
                "item high-water regressed across restart or crash")
        uncheckpointed = crashed["uncheckpointed"]
        require(0 < uncheckpointed == recovered["uncheckpointed"] < recovered["new_actor"],
                "actor high-water regressed or an uncheckpointed profile lost its identity")

        registry = root / "restart" / "server-identities.state"
        payload = bytearray(registry.read_bytes())
        payload[-1] ^= 1
        registry.write_bytes(payload)
        baseline = manifest.read_bytes()
        refused = run([*base, "--ticks", "1"], expected=3)
        require("server identity registry missing, corrupt or unsupported" in refused and
                manifest.read_bytes() == baseline, refused)


if __name__ == "__main__":
    main()
