#!/usr/bin/env bash
set -euo pipefail

# Immutable upstream pins. Build artifacts stay outside the repository.
readonly REPOSITORY="https://github.com/oxfordcontrol/Clarabel.cpp.git"
readonly TAG="v0.11.1"
readonly COMMIT="0de6259a3edfd5cc041ec42b2148599ce63e73cb"
readonly RUST_COMMIT="25540f559592068d0c8a80e46ded1b21760212a1"
readonly PREFIX="${1:-/tmp/clarabel-cpp-v0.11.1}"
readonly SOURCE="${PREFIX}/source"
readonly BUILD="${PREFIX}/build"
readonly PIN_PATCH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/clarabel_cpp_cbindgen_pin.patch"

if [[ ! -d "${SOURCE}/.git" ]]; then
  mkdir -p "${PREFIX}"
  git -C "${PREFIX}" init source
  git -C "${SOURCE}" remote add origin "${REPOSITORY}"
  git -C "${SOURCE}" fetch --depth 1 origin "${COMMIT}"
  git -C "${SOURCE}" checkout --detach "${COMMIT}"
  git -C "${SOURCE}" submodule update --init --recursive --depth 1
fi

test "$(git -C "${SOURCE}" rev-parse HEAD)" = "${COMMIT}"
test "$(git -C "${SOURCE}/Clarabel.rs" rev-parse HEAD)" = "${RUST_COMMIT}"

# Upstream invokes an unversioned `cargo install cbindgen`. Pin that build tool
# without vendoring it; this is the only deliberate source-tree modification.
if grep -q 'COMMAND cargo install cbindgen$' "${SOURCE}/rust_wrapper/CMakeLists.txt"; then
  git -C "${SOURCE}" apply --check "${PIN_PATCH}"
  git -C "${SOURCE}" apply "${PIN_PATCH}"
fi
grep -q 'COMMAND cargo install cbindgen --version 0.29.4 --locked' \
  "${SOURCE}/rust_wrapper/CMakeLists.txt"
test "$(git -C "${SOURCE}" status --porcelain --untracked-files=no)" = \
  ' M rust_wrapper/CMakeLists.txt'

cmake -S "${SOURCE}" -B "${BUILD}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCLARABEL_FEATURE_SDP=sdp-accelerate \
  -DCLARABEL_BUILD_TESTS=OFF
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
cmake --build "${BUILD}" -j "${jobs}"
"${BUILD}/examples/cpp/cpp_example_sdp"

echo "CLARABEL_CPP_TAG=${TAG}"
echo "CLARABEL_CPP_COMMIT=${COMMIT}"
echo "CLARABEL_RS_COMMIT=${RUST_COMMIT}"
echo "CLARABEL_CPP_SOURCE=${SOURCE}"
echo "CLARABEL_CPP_BUILD=${BUILD}"
