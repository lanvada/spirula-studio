#!/usr/bin/env bash
# Build the WASM modules for the standalone viewers.
# Requires the Emscripten SDK to be activated (emcc / emcmake on PATH), e.g.:
#   source ~/emsdk/emsdk_env.sh
#
# Usage: ./build.sh [ssv_wasm | ssv_telemetry | ss_select]   (default: all three)
set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR=build
emcmake cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
if [ $# -gt 0 ]; then
  cmake --build "$BUILD_DIR" --target "$1" -j
else
  cmake --build "$BUILD_DIR" -j
fi

echo "Built js/*.wasm"
