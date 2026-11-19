#!/usr/bin/env python3
"""Exercise the dedicated server process and its exit statuses on CPU-only builds."""

from __future__ import annotations

import argparse
import contextlib
import os
import signal
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path


TIMEOUT = 90


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def record(output: str, status: str) -> dict[str, str]:
    for line in reversed(output.splitlines()):
        if line.startswith(f"SERVER status={status} "):
            return dict(word.split("=", 1) for word in line.split()[1:])
    raise AssertionError(f"missing server status {status!r}\n{output}")


@contextlib.contextmanager
def live_server(args: list[str], log: Path, *, new_group: bool = False):
    print(args, flush=True)
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" and new_group else 0
    with log.open("wb") as stream, subprocess.Popen(
        args, stdout=stream, stderr=subprocess.STDOUT, creationflags=flags
    ) as process:
        try:
            yield process
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=TIMEOUT)


def wait_for_record(process: subprocess.Popen, log: Path, status: str) -> dict[str, str]:
    deadline = time.monotonic() + TIMEOUT
    while True:
        output = log.read_text(encoding="utf-8", errors="replace")
        if f"SERVER status={status} " in output and output.endswith("\n"):
            return record(output, status)
        require(process.poll() is None and time.monotonic() < deadline,
                f"server did not report {status}\n{output}")
        time.sleep(0.02)


def check_exit(process: subprocess.Popen, log: Path, code: int, reason: str) -> dict[str, str]:
    actual = process.wait(timeout=TIMEOUT)
    output = log.read_text(encoding="utf-8", errors="replace")
    require(actual == code, f"expected exit {code}, got {actual}\n{output}")
    stopped = record(output, "stopped")
    require(stopped["exit_code"] == str(code) and stopped["reason"] == reason and
            stopped["saved"] == ("1" if code == 0 else "0"), output)
    return stopped


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    executable = str(parser.parse_args().server.resolve())
    with tempfile.TemporaryDirectory(prefix="ft_vox_server_") as directory:
        root = Path(directory)

        def args(world: str) -> list[str]:
            return [executable, "--world", world, "--saves-root", str(root)]

        def run(world: str, expected: int, *extra: str) -> str:
            command = [*args(world), *extra]
            print(command, flush=True)
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=TIMEOUT)
            require(result.returncode == expected,
                    f"expected exit {expected}, got {result.returncode}\n{result.stdout}")
            return result.stdout

        def deterministic_run(world: str, workers: int) -> dict[str, str]:
            output = run(world, 0, "--seed", "19", "--ticks", "12", "--workers", str(workers),
                         "--listen-address", "::1", "--port", "43210", "--player-cap", "2")
            ready = record(output, "ready")
            stopped = record(output, "stopped")
            require(ready["transport"] == "disabled" and ready["restored"] == "0" and
                    ready["listen_address"] == "::1" and ready["port"] == "43210" and
                    ready["player_cap"] == "2", output)
            require(stopped["ticks"] == "12" and stopped["dropped_ticks"] == "0"
                    and stopped["reason"] == "tick_limit" and stopped["saved"] == "1"
                    and stopped["checkpoint_generation"] != "0"
                    and stopped["generation_in_flight"] == "0", output)
            require((root / world / "checkpoints" / "manifest").is_file(), output)
            return stopped

        require(deterministic_run("sync", 0) == deterministic_run("async", 2),
                "deterministic server snapshots differed by worker count")
        for payload in ("player.state", "inventory.state", "items.state"):
            require((root / "sync" / payload).read_bytes() == (root / "async" / payload).read_bytes(),
                    f"deterministic gameplay payload {payload} differed by worker count")
        restart = run("async", 0, "--ticks", "3")
        require(record(restart, "ready")["restored"] == "1" and
                record(restart, "stopped")["seed"] == "19", restart)

        manifest = root / "async" / "checkpoints" / "manifest"
        previous = manifest.read_bytes()
        mismatch = run("async", 3, "--seed", "20", "--ticks", "1")
        require("error=world_open" in mismatch and "stored seed 19 != requested seed 20" in mismatch, mismatch)
        require(manifest.read_bytes() == previous and "status=ready" not in mismatch, mismatch)
        invalid = run("../invalid", 3, "--ticks", "1")
        require("error=world_open detail=invalid world name" in invalid, invalid)
        invalid = run("invalid", 2, "--player-cap", "257")
        require("error=usage detail=--player-cap" in invalid, invalid)

        stop_file = root / "stop"
        log = root / "admin.log"
        with live_server([*args("admin"), "--stop-file", str(stop_file), "--metrics-every", "2",
                          "--port", "0"], log) as process:
            record_wait = wait_for_record(process, log, "ready")
            port = int(record_wait["port"])
            require(record_wait["mode"] == "live" and record_wait["transport"] == "udp" and port > 0, record_wait)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                probe.settimeout(5)
                hello = struct.pack(">4sHBBHHQQQQHHHQQ", b"FTVN", 3, 1, 0, 30, 0,
                                    0, 0, 0, 17, 1, 1, 1, 0, 0)
                probe.sendto(hello, ("127.0.0.1", port))
                reply, sender = probe.recvfrom(1200)
                require(sender == ("127.0.0.1", port) and
                        reply == struct.pack(">4sHBBHHQQQQBHH", b"FTVN", 3, 3, 0, 13, 0,
                                             0, 0, 0, 17, 1, 3, 3), "wire version negotiation failed")
            conflict = run("conflict", 4, "--port", str(port))
            require("bind UDP socket" in conflict and "status=ready" not in conflict, conflict)
            wait_for_record(process, log, "metrics")
            stop_file.touch()
            require(int(check_exit(process, log, 0, "admin")["ticks"]) >= 2, "live loop did not tick")
        require((root / "admin" / "checkpoints" / "manifest").is_file(), "admin stop skipped save")

        stop_file.unlink()
        log = root / "failure.log"
        with live_server([*args("failure"), "--stop-file", str(stop_file), "--port", "0"], log) as process:
            wait_for_record(process, log, "ready")
            # A directory at the manifest path makes atomic publication fail
            # even on an elevated account. This world belongs to the test.
            checkpoint = root / "failure" / "checkpoints" / "manifest"
            checkpoint.mkdir(parents=True)
            stop_file.touch()
            check_exit(process, log, 5, "admin")
            require("error=persistence" in log.read_text(encoding="utf-8"), "missing save failure diagnostic")

        if os.name == "nt":
            # Console events target a child process group in the same console.
            # CI may launch Python without a console attached.
            import ctypes
            kernel = ctypes.windll.kernel32
            if not kernel.GetConsoleCP():
                require(bool(kernel.AllocConsole()), "unable to allocate a console for the control-event test")
        log = root / "signal.log"
        with live_server([*args("signal"), "--port", "0"], log, new_group=True) as process:
            wait_for_record(process, log, "ready")
            process.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGTERM)
            check_exit(process, log, 0, "signal")
        require((root / "signal" / "checkpoints" / "manifest").is_file(), "signal stop skipped save")


if __name__ == "__main__":
    main()
