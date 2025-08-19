#!/usr/bin/env bash
set -euo pipefail

# Build and package mod_dialogflow as a .deb using CMake + CPack.
# Usage: scripts/build-deb.sh /abs/path/to/gens [build-dir]

GENS_DIR=${1:-}
BUILD_DIR=${2:-build}
JOBS=${JOBS:-2}

if [[ -z "${GENS_DIR}" ]]; then
  echo "Usage: $0 <GENS_DIR> [build-dir]" >&2
  exit 2
fi

if [[ ! -d "${GENS_DIR}/google" ]]; then
  echo "GENS_DIR does not contain generated google/* sources: ${GENS_DIR}" >&2
  exit 2
fi

cmake -S . -B "${BUILD_DIR}" -DGENS_DIR="${GENS_DIR}" -DCMAKE_BUILD_TYPE=Release -DFS_MOD_DIR=/usr/lib/freeswitch/mod ${CMAKE_ARGS:-}
cmake --build "${BUILD_DIR}" -j"${JOBS}"
cmake --install "${BUILD_DIR}" --component mod_dialogflow || true

cmake --build "${BUILD_DIR}" --target package -j"${JOBS}"

echo "\nArtifacts in ${BUILD_DIR}:"
ls -1 "${BUILD_DIR}"/*.deb || true
