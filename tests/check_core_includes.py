#!/usr/bin/env python3
"""Forbidden-include-chain guard for the CPU-only core (issue #205, Epic #204 A).

The authoritative simulation / persistence library `ft_vox_core` and its public
contracts (src/Core/SimContracts.hpp) must stay render-independent: they may
depend on glm, FastNoise2, the C++ standard library and platform headers, but
NEVER on the render / platform side (Renderer/, Vulkan/, Camera/, Engine/,
ImGui/, SDL3, volk, VMA, ...).

This script walks the transitive #include closure of the core public headers and
translation units and fails if any forbidden header is reachable. Chunk/ headers
are classified by an explicit allowlist: CPU-only allowed, render-coupled never,
not-yet-migrated forbidden until Phase B/C lifts them; any unclassified Chunk/
header fails closed. It is wired as the CTest `CoreIncludeGuard` so the boundary
cannot silently regress.

Usage:
    python tests/check_core_includes.py [--verbose]

Exit code 0 = clean, 1 = at least one forbidden include chain.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC = REPO_ROOT / "src"

# Roots of the walk: the contracts header plus every ft_vox_core translation
# unit and the public headers consumers are allowed to include. Walking the
# .cpp files also catches a direct forbidden include added to an implementation.
ROOTS = [
    "Core/SimContracts.hpp",
    "World/WorldSave.cpp",
    "World/WorldPersistence.cpp",
    "Chunk/TerrainGenerator.cpp",
    "Chunk/TerrainVegetation.cpp",
    "Entities/MobSystem.cpp",
    "Entities/MobModel.cpp",
    "Physics/VoxelCollision.hpp",
    "Physics/PlayerController.hpp",
    "Physics/BlockPhysics.hpp",
    "Entities/MobTypes.hpp",
    "Entities/MobSystem.hpp",
    "Entities/MobModel.hpp",
    "World/WorldSave.hpp",
    "World/WorldPersistence.hpp",
    "Chunk/TerrainGenerator.hpp",
    "Chunk/LightSample.hpp",
]

# Project directories (relative to src/) that belong to the render / platform
# side. Any header reached under one of these is a forbidden dependency.
FORBIDDEN_DIRS = {
    "Renderer",
    "Vulkan",
    "Camera",
    "Engine",
    "ImGui",
    "ImGuiFileDialog",
    "Network",
}

# Chunk/ mixes CPU-only and render-side headers, so it is classified by an
# explicit ALLOWLIST (fail-closed): any Chunk/ header not listed below is a
# violation until someone classifies it. A denylist would let a future
# render-coupled header (e.g. ChunkRenderBridge.hpp) slip through unnoticed.

# CPU-only headers (std / glm / utils.hpp includes only) the core closure may
# reach. ChunkBorders is already included by TerrainGenerator.hpp.
CHUNK_CPU_ALLOWED = {
    "LightSample.hpp",
    "ChunkBorders.hpp",
    "StreamHelpers.hpp",
    "BiomeRegionGrid.hpp",
    "TerrainGenerator.hpp",
    "TerrainProfiles.hpp",
}

# Render-coupled — NEVER allowed from the core, in any phase: these pull
# Vulkan/VMA/Camera/Engine/Renderer headers (transitively).
CHUNK_RENDER_COUPLED = {
    "Chunk.hpp",               # Vulkan/VkBuffer, Vulkan/MeshArena, Camera, Engine, Renderer
    "ChunkManager.hpp",        # Chunk.hpp + Engine/
    "ChunkCollisionView.hpp",  # ChunkManager.hpp
    "ChunkMobWorld.hpp",       # ChunkCollisionView.hpp
}

# Not yet migrated — CPU-only, but still owned by the render-side chunk/mesh
# system. Forbidden TODAY because the migration has not happened, NOT because
# of GPU coupling: Phase B (#206) lifts the authoritative storage into the sim
# world and Phase C (#207) re-homes the derived mesh/light payloads.
CHUNK_NOT_YET_MIGRATED = {
    "VoxelPool.hpp",
    "ChunkLightPool.hpp",
    "ChunkLightHalo.hpp",
    "ChunkPool.hpp",
    "ChunkMeshResult.hpp",
    "ChunkMeshScratch.hpp",
}

# External (non-project) headers that signal a render / platform dependency.
FORBIDDEN_EXTERNAL = [
    re.compile(r"(^|/)SDL3?/"),
    re.compile(r"(^|/)SDL\.h$"),
    re.compile(r"(^|/)vulkan/"),
    re.compile(r"(^|/)volk\.h$"),
    re.compile(r"(^|/)vk_mem_alloc\.h$"),
    re.compile(r"(^|/)imgui"),
    re.compile(r"(^|/)glad/"),
    re.compile(r"(^|/)GL/"),
    re.compile(r"(^|/)GLFW/"),
    re.compile(r"(^|/)boost/"),
    re.compile(r"(^|/)miniz"),
]

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.MULTILINE)


def is_forbidden_external(raw: str) -> bool:
    return any(pat.search(raw) for pat in FORBIDDEN_EXTERNAL)


def project_violation(path: Path) -> str | None:
    """Return a reason if a resolved project header is forbidden, else None."""
    rel = path.relative_to(SRC)
    parts = rel.parts
    if parts and parts[0] in FORBIDDEN_DIRS:
        return f"render/platform directory '{parts[0]}/'"
    if parts and parts[0] == "Chunk":
        name = path.name
        if name in CHUNK_CPU_ALLOWED:
            return None
        if name in CHUNK_RENDER_COUPLED:
            return f"render-coupled Chunk header '{name}' (never allowed from the core)"
        if name in CHUNK_NOT_YET_MIGRATED:
            return (f"not-yet-migrated Chunk header '{name}' "
                    "(CPU-only but render-side; Phase B/C will lift it)")
        return f"unclassified Chunk header '{name}' (classify it in check_core_includes.py)"
    return None


def resolve(raw: str, delim: str, including: Path) -> Path | None:
    """Resolve an include to a project file under src/, or None if external."""
    candidates = []
    if delim == '"':
        candidates.append(including.parent / raw)
    candidates.append(SRC / raw)
    for cand in candidates:
        try:
            resolved = cand.resolve()
        except OSError:
            continue
        if resolved.is_file() and SRC in resolved.parents:
            return resolved
    return None


def walk(verbose: bool) -> list[str]:
    violations: list[str] = []
    visited: set[Path] = set()

    def visit(path: Path, chain: list[Path]) -> None:
        if path in visited:
            return
        visited.add(path)
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            violations.append(f"cannot read {path}: {exc}")
            return
        for match in INCLUDE_RE.finditer(text):
            delim, raw = match.group(1), match.group(2)
            new_chain = chain + [path]
            if is_forbidden_external(raw):
                violations.append(format_chain(new_chain, raw, "forbidden external header"))
                continue
            resolved = resolve(raw, delim, path)
            if resolved is None:
                continue  # external, allowed (glm / FastNoise / std / platform)
            reason = project_violation(resolved)
            if reason is not None:
                violations.append(format_chain(new_chain, str(resolved.relative_to(SRC)), reason))
                continue
            visit(resolved, new_chain)

    for root in ROOTS:
        root_path = (SRC / root).resolve()
        if not root_path.is_file():
            violations.append(f"missing root header/source: src/{root}")
            continue
        visit(root_path, [])

    if verbose:
        print(f"Visited {len(visited)} project headers/sources:")
        for path in sorted(visited):
            print(f"  src/{path.relative_to(SRC).as_posix()}")
    return violations


def format_chain(chain: list[Path], final: str, reason: str) -> str:
    steps = [f"src/{p.relative_to(SRC).as_posix()}" for p in chain]
    steps.append(final)
    return f"{reason}:\n    " + "\n    -> ".join(steps)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verbose", action="store_true", help="print the visited header set")
    args = parser.parse_args()

    violations = walk(args.verbose)
    if violations:
        print(f"FAIL: {len(violations)} forbidden include chain(s) reachable from ft_vox_core:\n")
        for violation in violations:
            print(violation)
            print()
        print("The CPU-only core must depend on glm + FastNoise2 + std + platform only.")
        print("See docs/simulation-render-contracts.md (dependency rule).")
        return 1
    print("OK: ft_vox_core public headers have no forbidden include chain.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
