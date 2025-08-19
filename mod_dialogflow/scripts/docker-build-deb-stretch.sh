#!/usr/bin/env bash
set -euo pipefail

# Build the Debian Stretch packaging image and run a container to produce the .deb.

IMAGE_NAME=${IMAGE_NAME:-mod-dialogflow-deb:stretch}
GENS_DIR=${GENS_DIR:-}
BUILD_DIR=${BUILD_DIR:-build-stretch}
FS_PREFIX=${FS_PREFIX:-/usr/local/freeswitch}

if [[ -z "$GENS_DIR" ]]; then
  echo "ERROR: set GENS_DIR to your generated googleapis C++ root (contains google/*)" >&2
  exit 2
fi
if [[ ! -d "$GENS_DIR/google" ]]; then
  echo "ERROR: GENS_DIR does not contain google/* : $GENS_DIR" >&2
  exit 2
fi

echo "Building image $IMAGE_NAME (Debian Stretch) ..."
docker build -t "$IMAGE_NAME" -f Dockerfile.debian9 .

echo "Running packaging inside container (this compiles gRPC/protobuf; it may take a while) ..."
RUN_ARGS=(--rm -v "$(pwd)":/src -v "$GENS_DIR":/gens -w /src)
if [[ -d "$FS_PREFIX" ]]; then
  echo "Mounting FreeSWITCH prefix: $FS_PREFIX"
  RUN_ARGS+=(
    -v "$FS_PREFIX":/usr/local/freeswitch:ro
    -e PKG_CONFIG_PATH=/usr/local/freeswitch/lib/pkgconfig:${PKG_CONFIG_PATH:-}
  )
fi
docker run "${RUN_ARGS[@]}" "$IMAGE_NAME" bash -lc "CMAKE_ARGS='-DCMAKE_CXX_STANDARD=14 -DBUNDLE_GRPC_DEPS=ON' scripts/build-deb.sh /gens $BUILD_DIR"

echo "\nDone. Artifacts in $BUILD_DIR/ :"
ls -1 "$BUILD_DIR"/*.deb || true
