#!/usr/bin/env bash
set -euo pipefail

# Generate C++ sources for Dialogflow CX and common types into an output tree.
# Usage:
#   scripts/gen_googleapis.sh /path/to/googleapis /abs/path/to/output

API_ROOT=${1:-}
OUT=${2:-}

if [[ -z "${API_ROOT}" || -z "${OUT}" ]]; then
  echo "Usage: $0 <googleapis_repo_root> <output_dir>" >&2
  exit 2
fi

if [[ ! -d "${API_ROOT}/google" ]]; then
  echo "Error: ${API_ROOT} does not look like the googleapis repo (missing /google)." >&2
  exit 2
fi

PROTOC=$(command -v protoc || true)
GRPC_PLUGIN=$(command -v grpc_cpp_plugin || true)
if [[ -z "${PROTOC}" ]]; then
  echo "Error: protoc not found. Install protobuf-compiler." >&2
  exit 2
fi
if [[ -z "${GRPC_PLUGIN}" ]]; then
  echo "Error: grpc_cpp_plugin not found. Install protobuf-compiler-grpc or gRPC C++." >&2
  exit 2
fi

mkdir -p "${OUT}"

echo "Generating Dialogflow CX v3 protos into ${OUT}..."
mapfile -t DFX_PROTOS < <(find "${API_ROOT}/google/cloud/dialogflow/cx/v3" -name '*.proto' | sort)
"${PROTOC}" \
  -I"${API_ROOT}" \
  --cpp_out="${OUT}" \
  --grpc_out="${OUT}" \
  --plugin=protoc-gen-grpc="${GRPC_PLUGIN}" \
  "${DFX_PROTOS[@]}"

echo "Generating common google types (latlng, status)..."
"${PROTOC}" -I"${API_ROOT}" --cpp_out="${OUT}" \
  google/type/latlng.proto \
  google/rpc/status.proto

echo "Generating Google API annotations (field_behavior, resource, etc.)..."
mapfile -t API_PROTOS < <(find "${API_ROOT}/google/api" -maxdepth 1 -name '*.proto' | sort)
if (( ${#API_PROTOS[@]} > 0 )); then
  "${PROTOC}" -I"${API_ROOT}" --cpp_out="${OUT}" "${API_PROTOS[@]}"
fi

echo "Generating Google Long Running Operations (LRO) protos..."
mapfile -t LRO_PROTOS < <(find "${API_ROOT}/google/longrunning" -maxdepth 1 -name '*.proto' | sort)
if (( ${#LRO_PROTOS[@]} > 0 )); then
  "${PROTOC}" -I"${API_ROOT}" --cpp_out="${OUT}" "${LRO_PROTOS[@]}"
fi

cat <<EOF
Done.

GENS_DIR ready at: ${OUT}
Contains:
  - google/cloud/dialogflow/cx/v3/*.pb.cc, *.grpc.pb.cc
  - google/type/*.pb.cc, google/rpc/*.pb.cc, google/api/*.pb.cc, google/longrunning/*.pb.cc

To build the module with CMake:
  cmake -S . -B build -DGENS_DIR=${OUT}
  cmake --build build -j
  cmake --install build
EOF
