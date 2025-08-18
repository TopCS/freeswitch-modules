#!/usr/bin/env bash
set -euo pipefail

echo "== mod_dialogflow build & install =="

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

# Resolve GENS_DIR: prefer env, else common local path
GENS_DIR_ENV="${GENS_DIR:-}"
if [[ -z "$GENS_DIR_ENV" ]]; then
  if [[ -d "/home/andrea-batazzi/dev/gens" ]]; then
    GENS_DIR_ENV="/home/andrea-batazzi/dev/gens"
  fi
fi

usage() {
  cat << USAGE
Usage: $0 [--gens DIR] [--build-type TYPE] [--modulesdir DIR]

Options:
  --gens DIR          Path to generated googleapis C++ root (contains google/...)
  --build-type TYPE   CMake build type (Default: ${BUILD_TYPE})
  --modulesdir DIR    Destination FreeSWITCH modules dir override

Environment:
  GENS_DIR            Same as --gens
  BUILD_DIR           Build directory (Default: ${BUILD_DIR})
  BUILD_TYPE          Same as --build-type

Examples:
  GENS_DIR=/path/to/gens $0
  $0 --gens /home/andrea-batazzi/dev/gens --build-type Release
USAGE
}

MODULESDIR_OVERRIDE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0;;
    --gens) shift; GENS_DIR_ENV="${1:-}"; shift;;
    --build-type) shift; BUILD_TYPE="${1:-Release}"; shift;;
    --modulesdir) shift; MODULESDIR_OVERRIDE="${1:-}"; shift;;
    *) echo "Unknown arg: $1"; usage; exit 2;;
  esac
done

if [[ -z "$GENS_DIR_ENV" || ! -d "$GENS_DIR_ENV" ]]; then
  echo "ERROR: GENS_DIR not set or not a directory. Use --gens or export GENS_DIR." >&2
  exit 1
fi

echo "- Repo:       $REPO_ROOT"
echo "- Build dir:  $BUILD_DIR"
echo "- Build type: $BUILD_TYPE"
echo "- GENS_DIR:   $GENS_DIR_ENV"

# Configure
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DGENS_DIR="$GENS_DIR_ENV" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

# Build
JOBS="$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)"
cmake --build "$BUILD_DIR" -j"$JOBS"

# Locate output artifact
SO_PATH="$BUILD_DIR/mod_dialogflow.so"
if [[ ! -f "$SO_PATH" ]]; then
  echo "ERROR: Built artifact not found: $SO_PATH" >&2
  exit 1
fi

# Determine FreeSWITCH modules directory
MODULES_DIR=""
if [[ -n "$MODULESDIR_OVERRIDE" ]]; then
  MODULES_DIR="$MODULESDIR_OVERRIDE"
else
  MODULES_DIR="$(pkg-config --variable=modulesdir freeswitch 2>/dev/null || true)"
  if [[ -z "$MODULES_DIR" ]]; then
    # Fall back to a common local install path
    if [[ -d "/usr/local/freeswitch/mod" ]]; then
      MODULES_DIR="/usr/local/freeswitch/mod"
    fi
  fi
fi

if [[ -z "$MODULES_DIR" ]]; then
  echo "ERROR: Could not determine FreeSWITCH modules directory. Use --modulesdir to specify." >&2
  exit 1
fi

echo "- Modules dir: $MODULES_DIR"

# Install/copy
install -m 0755 "$SO_PATH" "$MODULES_DIR/"
echo "Installed: $MODULES_DIR/mod_dialogflow.so"

echo "Next steps:"
echo "  fs_cli -x 'reload mod_dialogflow'  # or 'load mod_dialogflow' if not loaded"
echo "  fs_cli -x 'dialogflow_version'"

