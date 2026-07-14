#!/usr/bin/env bash
# Optional helper for macOS Homebrew graphics stack (vcpkg handles C++ deps).
set -euo pipefail

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew not found. Install from https://brew.sh or use vcpkg only."
  exit 1
fi

echo "Installing MoltenVK + Vulkan loader + validation layers..."
brew install molten-vk vulkan-loader vulkan-validationlayers

ICD="/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json"
LAYER_DIR="/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d"

echo ""
echo "Add to your shell profile (~/.zshrc):"
echo "  export VK_ICD_FILENAMES=${ICD}"
echo "  export VK_LAYER_PATH=${LAYER_DIR}"
echo ""
echo "Optional: FT_VOX_VALIDATION=1 forces validation in Release builds."
echo "Done."
