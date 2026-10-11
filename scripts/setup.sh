#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Bootstrap everything the build needs. Run once after cloning:
#
#   ./scripts/setup.sh
#
# Two things are fetched, neither is committed:
#   1. AOSP's private binder headers. Not in the NDK; we link the device's
#      libbinder and need its headers.
#   2. The device's own libbinder.so and libutils.so, pulled off the attached
#      device. The NDK's stub is useless here: it omits symbols the NDK headers
#      also omit, and the binary links the DEVICE's implementation at runtime.
set -euo pipefail

ABI="${ABI:-arm64-v8a}"

# Where the device keeps its 64/32-bit libraries. ABI is honoured here because
# setup.sh reads it: hardcoding lib64 made ABI=x86 pull a 64-bit libbinder.so
# and then produce a 32-bit binary linked against it, which the linker accepts
# and the device rejects.
case "$ABI" in
  arm64-v8a|x86_64) LIBDIRNAME=lib64 ;;
  armeabi-v7a|x86)  LIBDIRNAME=lib ;;
  *) echo "unknown ABI '$ABI' (want arm64-v8a, armeabi-v7a, x86_64 or x86)" >&2; exit 1 ;;
esac

LIBDIR="third_party/lib"
mkdir -p "$LIBDIR"

echo "==> AOSP headers"
"$(dirname "$0")/fetch_headers.sh"

echo "==> device libraries (ABI=$ABI, /system/$LIBDIRNAME)"
if ! adb devices | grep -q 'device$'; then
  echo "    no device attached; pull these manually:" >&2
  echo "      adb pull /system/$LIBDIRNAME/libbinder.so $LIBDIR/" >&2
  echo "      adb pull /system/$LIBDIRNAME/libutils.so  $LIBDIR/" >&2
  exit 1
fi
adb pull "/system/$LIBDIRNAME/libbinder.so" "$LIBDIR/" >/dev/null
adb pull "/system/$LIBDIRNAME/libutils.so"  "$LIBDIR/" >/dev/null
ls -l "$LIBDIR"

echo
echo "setup complete - now: ./scripts/build.sh"