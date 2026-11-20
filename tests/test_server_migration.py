#!/usr/bin/env python3
"""Exercise the first dedicated checkpoint boundary with real child processes."""

from __future__ import annotations

import argparse
import struct
import tempfile
from pathlib import Path

from test_server_restart import require, run


def disk_state(world: Path) -> dict[Path, bytes]:
    return {path.relative_to(world): path.read_bytes() for path in world.rglob("*") if path.is_file()}


def migration_record(output: str) -> dict[str, int]:
    for line in output.splitlines():
        if line.startswith("MIGRATION "):
            return {key: int(value) for key, value in (field.split("=") for field in line.split()[1:])}
    raise AssertionError(f"missing migration record\n{output}")


def test_migration(server: str, client: str, root: Path, *, crash: bool,
                   foreign_registry: bytes | None = None) -> bytes:
    def fixture(mode: str, expected: int = 0) -> str:
        return run([client, mode, str(root)], expected)

    command = [server, "--world", "migration", "--saves-root", str(root), "--workers", "0", "--ticks", "1"]
    fixture("seed-migration")
    world = root / "migration"
    registry = world / "server-identities.state"
    checkpoints = world / "checkpoints"
    manifest = checkpoints / "manifest"
    stage = checkpoints / "manifest.tmp"
    prior = manifest.read_bytes()
    version, generation = struct.unpack_from("<IQ", prior, 4)
    require(version == 1, "fixture did not start with a v1 checkpoint")
    if crash:
        # The fixture exits without destructors immediately after the registry
        # write in the real ServerHost startup path.
        fixture("crash-migration", expected=77)
    else:
        stage.mkdir()
        failed = run(command, expected=3)
        require("could not establish the dedicated checkpoint baseline" in failed, failed)
        require((checkpoints / f"generation-{generation + 1}" / "players.state").is_file(),
                "the write failure did not reach v2 staging")

    require(manifest.read_bytes() == prior, "interrupted migration replaced the v1 manifest")
    pending = registry.read_bytes()
    world_id, _, _, sealed, actor_limit, item_limit, profiles = struct.unpack_from("<QiIBIQI", pending, 8)
    require(not sealed and actor_limit == 2 and profiles == 0 and item_limit > 2,
            "the process did not leave a provisional ID reservation")
    fixture("open-solo")
    require(manifest.read_bytes() == prior and registry.read_bytes() == pending,
            "solo recovery changed the committed generation or reserved IDs")
    if not crash:
        stage.rmdir()

    fixture("publish-migration")
    complete = manifest.read_bytes()
    version, generation = struct.unpack_from("<IQ", complete, 4)
    roster = checkpoints / f"generation-{generation}" / "players.state"
    require(version == 2 and roster.is_file(), "retry did not publish a complete v2 generation")
    sealed_registry = registry.read_bytes()
    require(sealed_registry[24] == 1, "successful migration did not seal its registry")
    before = disk_state(world)
    fixture("refuse-solo")
    require(disk_state(world) == before, "refused solo open changed persistent data")

    if crash:
        # The manifest rename already committed v2 even if startup exited
        # before sealing the registry.
        registry.write_bytes(pending)
        fixture("refuse-solo")
        header = complete[:8]
        manifest.write_bytes(header)
        fixture("refuse-solo")
        refused = run(command, expected=3)
        require("unsealed server identity registry cannot be matched" in refused and
                manifest.read_bytes() == header and registry.read_bytes() == pending, refused)
        manifest.write_bytes(complete)
        # Recovery can select the old v1 generation if the first v2 roster is
        # damaged. Publication still requires the existing multiplayer identity.
        roster_bytes = roster.read_bytes()
        roster.unlink()
        fixture("refuse-solo")
        if foreign_registry is not None:
            registry.write_bytes(foreign_registry)
            refused = run(command, expected=3)
            require("server identity registry belongs to another world or generator" in refused and
                    manifest.read_bytes() == complete, refused)
        registry.unlink()
        refused = run(command, expected=3)
        require("server identity registry missing, corrupt or unsupported" in refused and
                not registry.exists() and manifest.read_bytes() == complete, refused)
        registry.write_bytes(sealed_registry)
        roster.write_bytes(roster_bytes)

    enrolled = migration_record(fixture("complete-migration"))
    restored = migration_record(fixture("reconnect-migration"))
    require(enrolled["world"] == restored["world"] == world_id and
            enrolled["item"] == restored["item"] >= item_limit,
            "retry changed the world identity or reused an item reservation")

    sealed_registry = registry.read_bytes()
    committed = manifest.read_bytes()
    damaged_registry = sealed_registry[:-1] + bytes([sealed_registry[-1] ^ 1])
    registry.write_bytes(damaged_registry)
    for missing in (False, True):
        if missing:
            registry.unlink()
        refused = run(command, expected=3)
        require("server identity registry missing, corrupt or unsupported" in refused and
                manifest.read_bytes() == committed, refused)
        if missing:
            require(not registry.exists(), "dedicated startup replaced a missing registry")
        else:
            require(registry.read_bytes() == damaged_registry, "dedicated startup replaced a corrupt registry")
    return pending


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    args = parser.parse_args()
    server, client = str(args.server.resolve()), str(args.client.resolve())
    with tempfile.TemporaryDirectory(prefix="ft_vox_migration_") as directory:
        root = Path(directory)
        foreign_registry = test_migration(server, client, root / "write-failure", crash=False)
        test_migration(server, client, root / "process-exit", crash=True, foreign_registry=foreign_registry)


if __name__ == "__main__":
    main()
