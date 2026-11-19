#!/usr/bin/env python3
"""Headless dependency guard (issue #210, Epic #204 Phase F, acceptance 1).

Three scripted checks that the standalone simulation runner and server are
graphics-free:

  1. INCLUDE closure: every src/Headless and src/Server source/header is walked
     with the same rules as the core include guard (check_core_includes.py):
     no Renderer/, Vulkan/, Camera/, Engine/, ImGui*/, no SDL /
     vulkan / volk / VMA / imgui / boost / miniz external headers, and the
     Chunk/ allowlist stays fail-closed. The dedicated server may include
     the explicitly listed v2 codec and UDP files under Network/.

  2. LINK artifacts: the actual generated link inputs for ft_vox_headless,
     ft_vox_server, ft_vox_core, ft_vox_wire and ft_vox_udp must contain no
     graphical library (Ninja link edge, Makefile link.txt, or MSVC link fields).
     A Ninja static-library edge carries no LINK_LIBRARIES variable (deps are on
     the build statement line), so the whole edge is scanned. On MSVC a static
     library has no link step, so the ft_vox_core scan reads only its
     <ProjectReference> entries — the authoritative MSVC link gate is the
     configure-time transitive link-closure assertion in the top-level
     CMakeLists.txt, which this check complements.

  3. DISCOVERY: when the build cache says FT_VOX_GRAPHICS=OFF, no graphics
     discovery may have happened at all — the cache must carry no Vulkan /
     SDL3 / volk / VMA / miniz / shader-compiler entries.

Usage (registered as CTest HeadlessDeps):
    python3 tests/check_headless_deps.py --build-dir <build> [--verbose]

Exit code 0 = clean, 1 = at least one violation. Python 3 stdlib only.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import check_core_includes as core_guard

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC = REPO_ROOT / "src"
SERVER_NETWORK_FILES = {
    SRC / "Network" / name for name in (
        "WireProtocol.cpp", "WireProtocol.hpp", "UdpSocket.cpp", "UdpSocket.hpp", "UdpLink.hpp",
        "ServerTransport.cpp", "ServerTransport.hpp", "ClientTransport.cpp", "ClientTransport.hpp",
    )
}

# Library / package names that must never appear in the runner's link inputs.
FORBIDDEN_LINK_TOKENS = [
    "sdl3", "sdl2", "vulkan", "volk", "vk_mem_alloc", "vulkanmemoryallocator",
    "miniz", "imgui", "glfw", "glad",
]

# Cache entries that prove a graphics discovery ran (FT_VOX_GRAPHICS=OFF mode).
FORBIDDEN_CACHE_PREFIXES = (
    "Vulkan_", "SDL3_", "SDL2_", "volk_", "VulkanMemoryAllocator_", "miniz_",
    "GLSLC", "GLSLANG_VALIDATOR", "FT_VOX_VMA_INCLUDE_DIR", "FT_VOX_SDL3",
)


def check_includes(verbose: bool) -> list[str]:
    """Walk both CPU host include closures with the core guard's rules."""
    violations: list[str] = []
    visited: set[tuple[Path, bool]] = set()

    roots = []
    for directory in (SRC / "Headless", SRC / "Server"):
        sources = sorted(directory.rglob("*.cpp"))
        if not sources:
            violations.append(f"no CPU host sources found under {directory}")
        roots.extend((path, directory.name == "Server") for path in sources + sorted(directory.rglob("*.hpp")))
    roots.extend((path, True) for path in sorted(SERVER_NETWORK_FILES))

    def visit(path: Path, chain: list[Path], allow_udp: bool) -> None:
        if (path, allow_udp) in visited:
            return
        visited.add((path, allow_udp))
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            violations.append(f"cannot read {path}: {exc}")
            return
        for match in core_guard.INCLUDE_RE.finditer(text):
            delim, raw = match.group(1), match.group(2)
            new_chain = chain + [path]
            if core_guard.is_forbidden_external(raw):
                violations.append(core_guard.format_chain(new_chain, raw, "forbidden external header"))
                continue
            resolved = core_guard.resolve(raw, delim, path)
            if resolved is None:
                continue
            reason = core_guard.project_violation(resolved)
            if reason is not None and not (allow_udp and resolved in SERVER_NETWORK_FILES):
                violations.append(core_guard.format_chain(
                    new_chain, str(resolved.relative_to(SRC)), reason))
                continue
            visit(resolved, new_chain, allow_udp)

    for root, allow_udp in roots:
        visit(root.resolve(), [], allow_udp)

    if verbose:
        files = {path for path, _ in visited}
        print(f"CPU host include closure: {len(files)} project files visited")
        for path in sorted(files):
            print(f"  src/{path.relative_to(SRC).as_posix()}")
    return violations


def find_link_inputs(build_dir: Path, target: str) -> list[tuple[str, str]]:
    """Return (kind, text) pairs describing a target's real link inputs.

    Covers ft_vox_headless, ft_vox_server and their CPU libraries:
    the dependency-direction guard applies to the whole CPU slice (issue #211).
    """
    found: list[tuple[str, str]] = []
    name = re.escape(target)
    out_name = rf"(?:lib)?{name}(?:\.exe|\.a|\.lib)?"

    # Ninja: the link edge (executable or static library) carries LINK_LIBRARIES.
    ninja = build_dir / "build.ninja"
    if ninja.is_file():
        text = ninja.read_text(encoding="utf-8", errors="replace")
        pattern = re.compile(
            rf"^build\s+(?:\./)?{out_name}\s*:\s*CXX_(?:EXECUTABLE|STATIC_LIBRARY)_LINKER[^\n]*\n"
            r"((?:[ \t]+[^\n]*\n)*)",
            re.MULTILINE)
        for match in pattern.finditer(text):
            # The build statement lists implicit/order-only deps (a static
            # library archives objects and carries NO LINK_LIBRARIES variable;
            # its dependencies appear on the build line, e.g. `|| libFastNoise.a`).
            # The indented block carries LINK_LIBRARIES for executables. Scan
            # the whole edge (build line + block) so both forms are covered.
            found.append((f"build.ninja link edge [{target}]", match.group(0)))

    # Makefiles: one link.txt per target.
    for link_txt in build_dir.glob(f"CMakeFiles/{target}.dir/link.txt"):
        found.append((str(link_txt.relative_to(build_dir)),
                      link_txt.read_text(encoding="utf-8", errors="replace")))

    # MSVC: only the LINK fields matter — AdditionalDependencies and the
    # project references. (The whole vcxproj also lists ZERO_CHECK reconfigure
    # inputs, whose CMake-module paths may mention graphics packages without
    # anything being linked.)
    for vcxproj in build_dir.glob(f"{target}.vcxproj"):
        text = vcxproj.read_text(encoding="utf-8", errors="replace")
        fields = re.findall(r"<AdditionalDependencies>([^<]*)</AdditionalDependencies>", text)
        fields += re.findall(r'<ProjectReference\s+Include="([^"]*)"', text)
        found.append((str(vcxproj.relative_to(build_dir)) + " [link fields]", "\n".join(fields)))

    return found


def check_link_artifacts(build_dir: Path) -> list[str]:
    violations: list[str] = []
    configured = (build_dir / "CMakeCache.txt").is_file()
    for target in ("ft_vox_headless", "ft_vox_server", "ft_vox_core", "ft_vox_wire", "ft_vox_udp"):
        inputs = find_link_inputs(build_dir, target)
        if not inputs:
            # Fail-CLOSED on a configured tree: the target should have build
            # artifacts, and a silent skip would let the guard go quiet
            # (issue #210 review). Only an unconfigured tree may skip.
            if configured:
                violations.append(
                    f"configured build tree has no {target} link artifacts "
                    "(build.ninja / link.txt / .vcxproj) — build the target before "
                    "running the guard; refusing to skip the link scan")
            else:
                print(f"NOTE: no CMakeCache.txt and no {target} link artifacts in "
                      f"{build_dir} (unconfigured tree); link scan skipped")
            continue
        for kind, text in inputs:
            lowered = text.lower()
            for token in FORBIDDEN_LINK_TOKENS:
                if token in lowered:
                    violations.append(f"graphical dependency '{token}' in {target} link inputs ({kind})")
    return violations


def check_discovery(build_dir: Path) -> list[str]:
    violations: list[str] = []
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        print(f"NOTE: no CMakeCache.txt in {build_dir}; discovery check skipped")
        return violations
    graphics_off = False
    entries: list[str] = []
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("//") or ":" not in line:
            continue
        key = line.split(":", 1)[0]
        if key == "FT_VOX_GRAPHICS":
            graphics_off = line.split("=", 1)[-1].strip().upper() in ("OFF", "FALSE", "0")
        entries.append(key)
    if not graphics_off:
        return violations  # full configuration: discovery is expected
    for key in entries:
        if key.startswith(FORBIDDEN_CACHE_PREFIXES):
            violations.append(
                f"FT_VOX_GRAPHICS=OFF but graphics discovery entry '{key}' is in CMakeCache.txt")
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, help="CMake build directory to inspect")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    build_dir = Path(args.build_dir).resolve()

    violations = check_includes(args.verbose)
    violations += check_link_artifacts(build_dir)
    violations += check_discovery(build_dir)

    if violations:
        print(f"FAIL: {len(violations)} headless dependency violation(s):\n")
        for violation in violations:
            print(violation)
            print()
        print("ft_vox_headless and ft_vox_server must stay graphics-free.")
        print("See docs/headless-runner.md and docs/dedicated-server.md.")
        return 1
    print("OK: CPU host include closures, link inputs and discovery are graphics-free.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
