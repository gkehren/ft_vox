#!/usr/bin/env python3
"""Opt-in native smoke for in-process client joins and projection teardown."""

import argparse
import subprocess
import tempfile
from pathlib import Path

from test_server_lifecycle import TIMEOUT, check_exit, live_server, require, wait_for_record


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    args = parser.parse_args()
    server, client = str(args.server.resolve()), str(args.client.resolve())
    with tempfile.TemporaryDirectory(prefix="ft_vox_join_") as directory:
        root = Path(directory)
        profiles = root / "client"
        profiles.mkdir()
        first_stop, second_stop = root / "stop-first", root / "stop-second"
        first_log, second_log = root / "first.log", root / "second.log"

        def server_args(world: str, seed: str, stop: Path) -> list[str]:
            return [server, "--world", world, "--seed", seed, "--saves-root", directory,
                    "--port", "0", "--stop-file", str(stop), "--interest-radius", "1", "--player-cap", "2"]

        with live_server(server_args("first", "42", first_stop), first_log) as first, \
                live_server(server_args("second", "43", second_stop), second_log) as second:
            first_port = wait_for_record(first, first_log, "ready")["port"]
            second_port = wait_for_record(second, second_log, "ready")["port"]
            result = subprocess.run([client, "--join-smoke", f"127.0.0.1:{first_port}",
                                     f"127.0.0.1:{second_port}", str(profiles)],
                                    text=True, errors="replace", capture_output=True, timeout=TIMEOUT)
            print(result.stdout, end="")
            if result.returncode:
                print(result.stderr, end="")
            first_stop.touch()
            second_stop.touch()
            check_exit(first, first_log, 0, "admin")
            check_exit(second, second_log, 0, "admin")
            require(result.returncode == 0, f"native multiplayer join smoke exited {result.returncode}")


if __name__ == "__main__":
    main()
