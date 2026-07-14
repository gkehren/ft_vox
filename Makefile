# Thin wrapper around CMake + vcpkg. Prefer calling cmake directly.

VCPKG_ROOT ?= $(HOME)/vcpkg
TOOLCHAIN  ?= $(VCPKG_ROOT)/scripts/buildsystems/vcpkg.cmake
BUILD_DIR  ?= build-vk
BUILD_TYPE ?= Release
JOBS       ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

.PHONY: all configure build clean fclean re test run help

all: build

help:
	@echo "Targets: configure build test run clean fclean re"
	@echo "  BUILD_DIR=$(BUILD_DIR)  BUILD_TYPE=$(BUILD_TYPE)"
	@echo "  TOOLCHAIN=$(TOOLCHAIN)"

configure:
	cmake -B $(BUILD_DIR) \
		-DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)

test: build
	cd $(BUILD_DIR) && ctest --output-on-failure

run: build
	@# macOS MoltenVK ICD (no-op if path missing)
	@export VK_ICD_FILENAMES="$${VK_ICD_FILENAMES:-/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json}"; \
		./$(BUILD_DIR)/ft_vox $(ARGS)

clean:
	@rm -rf $(BUILD_DIR)/CMakeFiles $(BUILD_DIR)/ft_vox 2>/dev/null || true

fclean:
	@rm -rf $(BUILD_DIR)

re: fclean build
