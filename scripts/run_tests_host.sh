#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Build and run the compatibility self-check on the HOST, not on a device.
#
# WHY A HOST SUITE. The checks here guard every compatibility decision this
# project makes: the per-release parsers and the --ext- namespace rule. Run on
# device, they need a phone attached and never run at all; here they run in a
# second on every change.
#
# The suite links no android:: symbols at all (nm -u shows only libc/libm/
# liblog) because it never touches binder. The one Android dependency is
# device_sdk(), a property read, which is stubbed.
#
# The authoritative run is still the device one, which exercises the real
# property read and the real libbinder.
set -euo pipefail

cd "$(dirname "$0")/.."

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

HOST_CC="${HOST_CC:-g++}"
CXXFLAGS=(-std=c++20 -O1 -Wall -Wextra -I include -I src)

# The stub. device_sdk() reads ro.build.version.sdk; a host has no such property.
# Default 34 is the level the hardware testing was done on; override FAKE_SDK.
cat > "$TMP/stub_device.cpp" <<'EOF'
#include "device.h"
#include <cstdlib>
namespace etools {
// Stands in for src/device.cpp on a host, where __system_property_get does not
// exist. 34 is the API level the on-device testing was done at.
int device_sdk() {
  const char* s = std::getenv("FAKE_SDK");
  return s != nullptr ? std::atoi(s) : 34;
}
}  // namespace etools
EOF

"$HOST_CC" "${CXXFLAGS[@]}" \
  -o "$TMP/test_flags" \
  src/args.cpp src/section.cpp src/extflags.cpp tests/test_flags.cpp "$TMP/stub_device.cpp"

"$TMP/test_flags"
