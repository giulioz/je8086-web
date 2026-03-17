#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build_wasm"
EM_CACHE_DIR="${SCRIPT_DIR}/.emcache"

mkdir -p "${BUILD_DIR}"
mkdir -p "${EM_CACHE_DIR}"

EM_CACHE="${EM_CACHE_DIR}" emcmake cmake \
  -S "${SCRIPT_DIR}" \
  -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DJPWASM_ENABLE_BINARYEN=ON

EM_CACHE="${EM_CACHE_DIR}" cmake --build "${BUILD_DIR}" --config Release --target jpwasm_wasm -j 10

echo "Built ${SCRIPT_DIR}/../public/jp8000_wasm.js and ${SCRIPT_DIR}/../public/jp8000_wasm.wasm"
