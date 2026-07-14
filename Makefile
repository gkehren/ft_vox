# Cross-platform thin wrapper around CMake.
#
# Dependency policy (matches cmake/Dependencies.cmake):
#   Linux  — system packages (apt/dnf) by default; set USE_VCPKG=1 to force vcpkg
#   macOS  — system/Homebrew by default; USE_VCPKG=1 optional
#   Windows — vcpkg by default (see build.ps1 / README); this Makefile is for
#             Git Bash / MSYS / WSL
#
# Examples:
#   make                  # configure + build
#   make deps             # install platform packages (install_dep.sh)
#   make USE_VCPKG=1      # force vcpkg toolchain
#   make test run ARGS="--seed 1"

UNAME_S := $(shell uname -s 2>/dev/null || echo Unknown)

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
BUILD_DIR  ?= build-vk
BUILD_TYPE ?= Release
JOBS       ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# vcpkg root candidates
VCPKG_ROOT ?= $(firstword \
	$(wildcard $(HOME)/vcpkg) \
	$(wildcard $(HOME)/source/vcpkg) \
	$(wildcard /opt/vcpkg) \
	$(wildcard C:/vcpkg) \
	$(wildcard D:/vcpkg) \
	$(wildcard D:/Projects/vcpkg))

TOOLCHAIN_FILE :=
ifneq ($(VCPKG_ROOT),)
  ifeq ($(wildcard $(VCPKG_ROOT)/scripts/buildsystems/vcpkg.cmake),)
    # leave empty
  else
    TOOLCHAIN_FILE := $(VCPKG_ROOT)/scripts/buildsystems/vcpkg.cmake
  endif
endif

# USE_VCPKG: auto | 1 | 0
# auto policy:
#   Windows / MinGW  → vcpkg (required for most deps)
#   Linux            → system packages (apt/dnf); ignore a local vcpkg clone
#   macOS            → vcpkg if VCPKG_ROOT toolchain exists, else Homebrew/system
USE_VCPKG ?= auto

ifeq ($(USE_VCPKG),auto)
  ifeq ($(OS),Windows_NT)
    USE_VCPKG_EFFECTIVE := 1
  else ifneq (,$(findstring MINGW,$(UNAME_S)))
    USE_VCPKG_EFFECTIVE := 1
  else ifneq (,$(findstring MSYS,$(UNAME_S)))
    USE_VCPKG_EFFECTIVE := 1
  else ifeq ($(UNAME_S),Linux)
    USE_VCPKG_EFFECTIVE := 0
  else ifeq ($(UNAME_S),Darwin)
    ifneq ($(TOOLCHAIN_FILE),)
      USE_VCPKG_EFFECTIVE := 1
    else
      USE_VCPKG_EFFECTIVE := 0
    endif
  else
    USE_VCPKG_EFFECTIVE := 0
  endif
else
  USE_VCPKG_EFFECTIVE := $(USE_VCPKG)
endif

CMAKE_EXTRA_ARGS ?=
CMAKE_CONFIGURE_ARGS := -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

ifeq ($(USE_VCPKG_EFFECTIVE),1)
  ifeq ($(TOOLCHAIN_FILE),)
    $(warning USE_VCPKG=1 but no vcpkg toolchain found. Set VCPKG_ROOT=...)
  else
    CMAKE_CONFIGURE_ARGS += -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE)
  endif
else
  # Explicit system-first mode (even if a leftover toolchain is in the env)
  CMAKE_CONFIGURE_ARGS += -DFT_VOX_DEP_MODE=system
endif

.PHONY: all configure build clean fclean re test run deps help print-config

all: build

help:
	@echo "ft_vox build wrapper"
	@echo ""
	@echo "Targets:"
	@echo "  deps         Install OS packages (./install_dep.sh)"
	@echo "  configure    cmake -B $(BUILD_DIR)"
	@echo "  build        configure + compile (-j$(JOBS))"
	@echo "  test         ctest"
	@echo "  run          launch ./$(BUILD_DIR)/ft_vox"
	@echo "  clean/fclean remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  BUILD_DIR=$(BUILD_DIR)  BUILD_TYPE=$(BUILD_TYPE)  JOBS=$(JOBS)"
	@echo "  USE_VCPKG=$(USE_VCPKG) (effective=$(USE_VCPKG_EFFECTIVE))"
	@echo "  VCPKG_ROOT=$(VCPKG_ROOT)"
	@echo "  TOOLCHAIN_FILE=$(TOOLCHAIN_FILE)"
	@echo ""
	@echo "Linux (system packages):  make deps && make"
	@echo "Linux (vcpkg):            make USE_VCPKG=1"
	@echo "macOS:                    make deps && make"
	@echo "Windows:                  use build.ps1 or cmake + vcpkg (see README)"

print-config:
	@echo "UNAME_S=$(UNAME_S)"
	@echo "USE_VCPKG_EFFECTIVE=$(USE_VCPKG_EFFECTIVE)"
	@echo "TOOLCHAIN_FILE=$(TOOLCHAIN_FILE)"
	@echo "CMAKE_CONFIGURE_ARGS=$(CMAKE_CONFIGURE_ARGS) $(CMAKE_EXTRA_ARGS)"

deps:
	@chmod +x ./install_dep.sh 2>/dev/null || true
	./install_dep.sh

configure:
	cmake -B $(BUILD_DIR) $(CMAKE_CONFIGURE_ARGS) $(CMAKE_EXTRA_ARGS)

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)

test: build
	cd $(BUILD_DIR) && ctest --output-on-failure --timeout 120

run: build
ifeq ($(UNAME_S),Darwin)
	@# MoltenVK ICD — Homebrew default (Apple Silicon); override with VK_ICD_FILENAMES
	@export VK_ICD_FILENAMES="$${VK_ICD_FILENAMES:-/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json}"; \
		if [ ! -f "$${VK_ICD_FILENAMES}" ]; then \
			export VK_ICD_FILENAMES="$${VK_ICD_FILENAMES:-/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json}"; \
		fi; \
		./$(BUILD_DIR)/ft_vox $(ARGS)
else
	./$(BUILD_DIR)/ft_vox $(ARGS)
endif

clean:
	@rm -rf $(BUILD_DIR)/CMakeFiles $(BUILD_DIR)/ft_vox 2>/dev/null || true
	@rm -f $(BUILD_DIR)/tests/test_* 2>/dev/null || true

fclean:
	@rm -rf $(BUILD_DIR)

re: fclean build
