#!/usr/bin/env bash
set -euo pipefail

# Build the Debian packaging image and run a container to produce the .deb.

IMAGE_NAME=${IMAGE_NAME:-mod-dialogflow-deb:bookworm}
GENS_DIR=${GENS_DIR:-}
BUILD_DIR=${BUILD_DIR:-build}

if [[ -z "$GENS_DIR" ]]; then
  echo "ERROR: set GENS_DIR to your generated googleapis C++ root (contains google/*)" >&2
  exit 2
fi
if [[ ! -d "$GENS_DIR/google" ]]; then
  echo "ERROR: GENS_DIR does not contain google/* : $GENS_DIR" >&2
  exit 2
fi

echo "Building image $IMAGE_NAME ..."
docker build -t "$IMAGE_NAME" -f Dockerfile.debian .

echo "Running packaging inside container ..."
docker run --rm \
  -v "$(pwd)":/src \
  -v "$GENS_DIR":/gens \
  -w /src \
  "$IMAGE_NAME" \
  bash -lc "scripts/build-deb.sh /gens $BUILD_DIR"

echo "\nDone. Artifacts in $BUILD_DIR/ :"
ls -1 "$BUILD_DIR"/*.deb || true

