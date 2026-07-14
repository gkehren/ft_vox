#!/usr/bin/env bash
# Install build dependencies for ft_vox.
#
# Policy:
#   Linux  → apt or dnf system packages (no vcpkg required)
#   macOS  → Homebrew (Vulkan/MoltenVK + libs); C++ deps can also use vcpkg
#   Windows → prints vcpkg instructions (use build.ps1)
#
# Usage:
#   ./install_dep.sh           # install packages
#   ./install_dep.sh --check   # only report what is missing
set -euo pipefail

CHECK_ONLY=0
if [[ "${1:-}" == "--check" ]]; then
  CHECK_ONLY=1
fi

info()  { printf '==> %s\n' "$*"; }
warn()  { printf '!!  %s\n' "$*" >&2; }
have()  { command -v "$1" >/dev/null 2>&1; }

need_cmd() {
  local c="$1"
  if have "$c"; then
    info "found $c: $(command -v "$c")"
    return 0
  fi
  warn "missing command: $c"
  return 1
}

# ---------------------------------------------------------------------------
# Windows (Git Bash / MSYS) — point at vcpkg
# ---------------------------------------------------------------------------
if [[ "${OS:-}" == "Windows_NT" ]] || [[ "$(uname -s 2>/dev/null || true)" =~ (MINGW|MSYS|CYGWIN) ]]; then
  info "Windows detected — use vcpkg for C++ dependencies."
  cat <<'EOF'

  1. Install Visual Studio 2022 (C++ desktop) + CMake, or Build Tools.
  2. Install vcpkg:
       git clone https://github.com/microsoft/vcpkg C:\vcpkg
       C:\vcpkg\bootstrap-vcpkg.bat
  3. GPU driver with Vulkan support (NVIDIA/AMD/Intel).
  4. Build:
       .\build.ps1
     or:
       cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
       cmake --build build --config Release

  Manifest dependencies are listed in vcpkg.json (sdl3[vulkan], boost, volk, VMA, glslang…).

EOF
  exit 0
fi

UNAME="$(uname -s)"
MISSING=0

# ---------------------------------------------------------------------------
# macOS — Homebrew
# ---------------------------------------------------------------------------
if [[ "$UNAME" == "Darwin" ]]; then
  if ! have brew; then
    warn "Homebrew not found. Install from https://brew.sh"
    warn "Alternatively: make USE_VCPKG=1 with a local vcpkg clone."
    exit 1
  fi

  PKGS=(
    cmake
    pkg-config
    sdl3
    boost
    molten-vk
    vulkan-loader
    vulkan-headers
    glslang
    # optional validation (Debug)
    vulkan-validationlayers
  )

  if [[ "$CHECK_ONLY" -eq 1 ]]; then
    for p in "${PKGS[@]}"; do
      if brew list --formula "$p" >/dev/null 2>&1 || brew list --cask "$p" >/dev/null 2>&1; then
        info "brew: $p installed"
      else
        warn "brew: $p missing"
        MISSING=1
      fi
    done
    need_cmd cmake || MISSING=1
    need_cmd glslangValidator || need_cmd glslc || MISSING=1
    exit "$MISSING"
  fi

  info "Installing Homebrew packages: ${PKGS[*]}"
  brew install "${PKGS[@]}"

  ICD_ARM="/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json"
  ICD_X64="/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json"
  LAYER_ARM="/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d"
  LAYER_X64="/usr/local/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d"

  ICD="$ICD_ARM"
  LAYER="$LAYER_ARM"
  [[ -f "$ICD" ]] || ICD="$ICD_X64"
  [[ -d "$LAYER" ]] || LAYER="$LAYER_X64"

  cat <<EOF

Done (macOS). Add to ~/.zshrc if needed:

  export VK_ICD_FILENAMES=${ICD}
  export VK_LAYER_PATH=${LAYER}

Then:

  make            # system/Homebrew deps (default)
  make USE_VCPKG=1   # optional: force vcpkg for SDL3/Boost/volk/…

EOF
  exit 0
fi

# ---------------------------------------------------------------------------
# Linux — apt or dnf
# ---------------------------------------------------------------------------
if [[ "$UNAME" != "Linux" ]]; then
  warn "Unsupported OS: $UNAME"
  exit 1
fi

install_apt() {
  local pkgs=(
    build-essential
    cmake
    pkg-config
    git
    # Windowing
    libsdl3-dev
    # Networking
    libboost-system-dev
    libboost-dev
    # Vulkan
    libvulkan-dev
    vulkan-tools
    glslang-tools
    # Optional header package (zeux/volk is FetchContent'd if absent)
    # libvulkan-memory-allocator-dev — not on all releases; CMake will FetchContent
  )
  # Older Ubuntu may not ship libsdl3-dev yet
  if ! apt-cache show libsdl3-dev >/dev/null 2>&1; then
    warn "libsdl3-dev not in apt cache (common on Ubuntu < 24.10)."
    warn "Options: -DFT_VOX_FETCH_SDL3=ON, or make USE_VCPKG=1."
    local filtered=()
    local p
    for p in "${pkgs[@]}"; do
      [[ "$p" == "libsdl3-dev" ]] && continue
      filtered+=("$p")
    done
    pkgs=("${filtered[@]}")
  fi

  if [[ "$CHECK_ONLY" -eq 1 ]]; then
    for p in "${pkgs[@]}"; do
      if dpkg -s "$p" >/dev/null 2>&1; then
        info "apt: $p installed"
      else
        warn "apt: $p missing"
        MISSING=1
      fi
    done
    need_cmd cmake || MISSING=1
    need_cmd glslangValidator || need_cmd glslc || MISSING=1
    return "$MISSING"
  fi

  info "Installing via apt: ${pkgs[*]}"
  sudo apt-get update
  sudo apt-get install -y "${pkgs[@]}" || {
    warn "apt install reported errors (often missing libsdl3-dev on older Ubuntu)."
    warn "Fallback: make USE_VCPKG=1   or   cmake -DFT_VOX_FETCH_SDL3=ON …"
  }
}

install_dnf() {
  local pkgs=(
    gcc-c++
    cmake
    pkgconf-pkg-config
    git
    SDL3-devel
    boost-devel
    vulkan-loader-devel
    vulkan-headers
    glslang
    vulkan-tools
  )

  if [[ "$CHECK_ONLY" -eq 1 ]]; then
    for p in "${pkgs[@]}"; do
      if rpm -q "$p" >/dev/null 2>&1; then
        info "dnf/rpm: $p installed"
      else
        warn "dnf/rpm: $p missing"
        MISSING=1
      fi
    done
    need_cmd cmake || MISSING=1
    need_cmd glslangValidator || need_cmd glslc || MISSING=1
    return "$MISSING"
  fi

  info "Installing via dnf: ${pkgs[*]}"
  sudo dnf install -y "${pkgs[@]}"
}

install_pacman() {
  local pkgs=(
    base-devel
    cmake
    pkgconf
    git
    sdl3
    boost
    vulkan-headers
    vulkan-icd-loader
    glslang
    vulkan-tools
  )

  if [[ "$CHECK_ONLY" -eq 1 ]]; then
    for p in "${pkgs[@]}"; do
      if pacman -Q "$p" >/dev/null 2>&1; then
        info "pacman: $p installed"
      else
        warn "pacman: $p missing"
        MISSING=1
      fi
    done
    return "$MISSING"
  fi

  info "Installing via pacman: ${pkgs[*]}"
  sudo pacman -S --needed --noconfirm "${pkgs[@]}"
}

if have apt-get; then
  install_apt
elif have dnf; then
  install_dnf
elif have pacman; then
  install_pacman
elif have zypper; then
  info "openSUSE: installing via zypper"
  if [[ "$CHECK_ONLY" -eq 1 ]]; then
    need_cmd cmake || exit 1
    exit 0
  fi
  sudo zypper install -y cmake gcc-c++ pkg-config libboost_system-devel \
    vulkan-devel glslang-devel || true
  warn "Install SDL3 devel manually if not available (SDL3-devel)."
else
  warn "No supported package manager found (apt/dnf/pacman/zypper)."
  warn "Install: cmake, C++20 compiler, SDL3, Boost.System, Vulkan headers/loader, glslang."
  warn "Or use vcpkg: make USE_VCPKG=1"
  exit 1
fi

cat <<'EOF'

Linux deps ready (as far as the package manager allows).

  make                 # system packages + FetchContent (volk, VMA, GLM, FastNoise2)
  make USE_VCPKG=1     # full vcpkg path (Windows-like)

If SDL3 packages are unavailable on your distro:

  cmake -B build-vk -DFT_VOX_FETCH_SDL3=ON -DFT_VOX_DEP_MODE=system -DCMAKE_BUILD_TYPE=Release
  cmake --build build-vk -j

Ensure a Vulkan ICD is installed for your GPU (mesa-vulkan-drivers / nvidia-driver / amdvlk).

EOF
