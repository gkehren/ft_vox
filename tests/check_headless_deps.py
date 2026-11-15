#!/usr/bin/env python3
"""Headless dependency guard (issue #210, Epic #204 Phase F, acceptance 1).

Three scripted checks that the standalone simulation runner is genuinely
graphics-free:

  1. INCLUDE closure: every src/Headless translation unit / header is walked
     with the same rules as the core include guard (check_core_includes.py):
     no Renderer/, Vulkan/, Camera/, Engine/, ImGui*/, Network/, no SDL /
     vulkan / volk / VMA / imgui / boost / miniz external headers, and the
     Chunk/ allowlist stays fail-closed.

  2. LINK artifacts: the actual generated link inputs for ft_vox_headless in
     the build tree (Ninja build.ninja LINK_LIBRARIES, Makefile link.txt, or
     MSVC .vcxproj AdditionalDependencies) must contain no graphical library.
     This complements the configure-time transitive link-closure assertion in
     the top-level CMakeLists.txt.

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
HEADLESS_DIR = SRC / "Headless"

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
    """Walk the Headless include closure with the core guard's rules."""
    violations: list[str] = []
    visited: set[Path] = set()

    roots = sorted(HEADLESS_DIR.glob("*.cpp")) + sorted(HEADLESS_DIR.glob("*.hpp"))
    if not roots:
        return [f"no headless sources found under {HEADLESS_DIR}"]

    def visit(path: Path, chain: list[Path]) -> None:
        if path in visited:
            return
        visited.add(path)
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
            if reason is not None:
                violations.append(core_guard.format_chain(
                    new_chain, str(resolved.relative_to(SRC)), reason))
                continue
            visit(resolved, new_chain)

    for root in roots:
        visit(root.resolve(), [])

    if verbose:
        print(f"Headless include closure: {len(visited)} project files visited")
        for path in sorted(visited):
            print(f"  src/{path.relative_to(SRC).as_posix()}")
    return violations


def find_link_inputs(build_dir: Path) -> list[tuple[str, str]]:
    """Return (kind, text) pairs describing the runner's real link inputs."""
    found: list[tuple[str, str]] = []

    # Ninja: the executable link edge carries a LINK_LIBRARIES variable.
    ninja = build_dir / "build.ninja"
    if ninja.is_file():
        text = ninja.read_text(encoding="utf-8", errors="replace")
        pattern = re.compile(
            r"^build\s+(?:\./)?ft_vox_headless(?:\.exe)?\s*:\s*CXX_EXECUTABLE_LINKER[^\n]*\n"
            r"((?:[ \t]+[^\n]*\n)*)",
            re.MULTILINE)
        for match in pattern.finditer(text):
            block = match.group(1)
            libs = re.search(r"LINK_LIBRARIES\s*=\s*([^\n]*)", block)
            if libs:
                found.append(("build.ninja LINK_LIBRARIES", libs.group(1)))

    # Makefiles: one link.txt per target.
    for link_txt in build_dir.glob("CMakeFiles/ft_vox_headless.dir/link.txt"):
        found.append((str(link_txt.relative_to(build_dir)),
                      link_txt.read_text(encoding="utf-8", errors="replace")))

    # MSVC: only the LINK fields matter — AdditionalDependencies and the
    # project references. (The whole vcxproj also lists ZERO_CHECK reconfigure
    # inputs, whose CMake-module paths may mention graphics packages without
    # anything being linked.)
    for vcxproj in build_dir.glob("ft_vox_headless.vcxproj"):
        text = vcxproj.read_text(encoding="utf-8", errors="replace")
        fields = re.findall(r"<AdditionalDependencies>([^<]*)</AdditionalDependencies>", text)
        fields += re.findall(r'<ProjectReference\s+Include="([^"]*)"', text)
        found.append((str(vcxproj.relative_to(build_dir)) + " [link fields]", "\n".join(fields)))

    return found


def check_link_artifacts(build_dir: Path) -> list[str]:
    violations: list[str] = []
    inputs = find_link_inputs(build_dir)
    if not inputs:
        print("NOTE: no ft_vox_headless link artifacts found in "
              f"{build_dir} (build the target first); link scan skipped")
        return violations
    for kind, text in inputs:
        lowered = text.lower()
        for token in FORBIDDEN_LINK_TOKENS:
            if token in lowered:
                violations.append(f"graphical dependency '{token}' in link inputs ({kind})")
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
        print("ft_vox_headless must stay graphics-free (issue #210, Epic #204 Phase F).")
        print("See docs/headless-runner.md and docs/simulation-render-contracts.md.")
        return 1
    print("OK: ft_vox_headless include closure, link inputs and discovery are graphics-free.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
