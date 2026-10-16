#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HEADERS="${VULKAN_HEADERS:-$HOME/Vulkan-Headers/include}"
OUT="${OUTPUT_DIR:-$ROOT/out/plugin}"
if [ ! -f "$HEADERS/vulkan/vulkan.h" ]; then
  echo "Vulkan-Headers not found at $HEADERS" >&2
  exit 1
fi
mkdir -p "$OUT"
xcrun clang -arch x86_64 -bundle -fobjc-arc -O2 \
  -Wno-protocol -Wno-incomplete-implementation -I"$HEADERS" \
  -o "$OUT/NVMTLDriver" "$ROOT/plugin/NVMTLDevice.m" \
  -framework Foundation -framework Metal -framework IOSurface -framework IOKit
codesign --force --sign - "$OUT/NVMTLDriver"
echo "$OUT/NVMTLDriver"
