#!/usr/bin/env python3
"""Opt-in physical-GPU closure with two native clients and one hosted child."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import time
from contextlib import ExitStack
from pathlib import Path


def run_phase(client: Path, server: Path, root: Path, phase: str) -> None:
    evidence = root / phase
    evidence.mkdir()
    with ExitStack() as logs:
        processes: list[tuple[str, subprocess.Popen, Path]] = []
        try:
            for role, launcher in (("owner", str(server)), ("guest", "guest")):
                logs_path = evidence / f"{role}.log"
                profile_root = root / role / "saves"
                output = logs.enter_context(logs_path.open("wb"))
                if phase == "menu":
                    cmd = [str(client), "--multiplayer-menu-proof", launcher, str(evidence), str(profile_root)]
                else:
                    cmd = [str(client), "--hosted-closure", launcher, str(root), str(profile_root), phase]
                env = os.environ.copy()
                env.pop("FT_VOX_MOTION_TRACE", None)
                process = subprocess.Popen(cmd, cwd=profile_root.parent, stdout=output,
                                           stderr=subprocess.STDOUT, env=env)
                processes.append((role, process, logs_path))

            # F09 (#316 round 2): the matrix no longer mines; its operations
            # are deterministic fixture rows with their own bounded deadlines
            # in the C++ scenario. The phase budget only bounds the fixture
            # stop/restart round-trip plus the two clients' lifetimes.
            deadline = time.monotonic() + 600
            while time.monotonic() < deadline:
                codes = [process.poll() for _, process, _ in processes]
                if any(code not in (None, 0) for code in codes):
                    raise RuntimeError(f"{phase} owner/guest exit codes {codes}")
                if all(code == 0 for code in codes):
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError(f"{phase} timed out")
        finally:
            # Close only these test clients. Releasing the owner's inherited
            # writer leaves its dedicated child to checkpoint and exit.
            for _, process, _ in processes:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=10)
            for role, _, path in processes:
                print(f"--- {phase} {role} ({path.name}) ---")
                log = path.read_text(encoding="utf-8", errors="replace")
                for line in log.splitlines():
                    if (line.startswith(("HOSTED ", "SERVER status=stopped ", "Native hosted",
                                          "Native multiplayer menu", "Vulkan device:", "Vulkan API:", "FAIL:", "SKIP",
                                          "[multiplayer]"))):
                        print(line)
                for code in sorted(set(re.findall(r"VUID-[\w-]+", log))):
                    print(f"Vulkan validation {code}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new directory for logs and isolated saves")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--menu", action="store_true", help="drive the native Host/Join and administration screens")
    mode.add_argument("--full-ui", action="store_true",
                      help="run the native menu and the two-client restart in separate save roots")
    args = parser.parse_args()
    server, client, root = args.server.resolve(), args.client.resolve(), args.output.resolve()
    resources = server.parent / "ressources"
    if not (resources / "default-resource-pack.zip").is_file():
        parser.error("build ft_vox so its runtime resources sit next to ft_vox_server")
    root.mkdir(parents=True)

    def setup(parent: Path) -> Path:
        for role in ("owner", "guest"):
            work = parent / role
            work.mkdir(parents=True)
            shutil.copytree(resources, work / "ressources")
        return parent

    if args.full_ui:
        run_phase(client, server, setup(root / "native-menu"), "menu")
        root = setup(root / "checkpoint-restart")
    else:
        setup(root)
    if args.menu:
        run_phase(client, server, root, "menu")
        print("Native multiplayer menu passed in both graphical clients.")
        return
    for phase in ("first", "restart"):
        run_phase(client, server, root, phase)
    print("Hosted closure passed in both native processes across checkpoint restart.")


if __name__ == "__main__":
    main()
