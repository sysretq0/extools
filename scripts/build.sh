#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Cross-build the library and both binaries with the NDK toolchain.
#
#   ./scripts/build.sh                 # arm64-v8a, NDK platform 29, device floor 26
#   ABI=x86_64 API=29 ./scripts/build.sh
set -euo pipefail

cd "$(dirname "$0")/.."

NDK="${NDK:-/opt/ndk}"
# The NDK PLATFORM is 29, not 26, and that is not the supported-device floor.
# Compiling against the newest branch whose symbols still exist on the oldest
# supported device is what the Itanium C++ ABI requires: a callee that ignores
# an extra register argument is harmless, a caller that never sets one is not.
# Platform 29 is also what the vendored headers need to compile at all
# (Parcel.h pulls unique_fd.h, which needs <android/fdsan.h>).
API="${API:-29}"
ABI="${ABI:-arm64-v8a}"
BUILD_DIR="build/${ABI}-api${API}"

if [ ! -f third_party/lib/libbinder.so ] || [ ! -d third_party/aosp/include ]; then
  echo "missing vendored dependencies - run ./scripts/setup.sh" >&2
  exit 1
fi

cmake -S . -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM="android-${API}" \
  -DCMAKE_BUILD_TYPE=Release \
  -G Ninja >/dev/null

cmake --build "$BUILD_DIR" --target etools dumpsyse cmde test_flags
echo "built: $BUILD_DIR/{libetools.a,dumpsyse,cmde,test_flags}"
