#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Build every ABI WITHOUT a device attached.
#
# WHY THIS EXISTS. scripts/setup.sh pulls libbinder.so and libutils.so off an
# attached device, because linking the device's own implementation is what makes
# the binary work on that device. That is correct locally and impossible in CI,
# and it also means a single-ABI build only ever link-checks one ROM.
#
# HOW. Link against stub libraries carrying only the SONAME. The result still
# records DT_NEEDED: libbinder.so, so at load time the DEVICE's real libbinder
# resolves every android:: symbol - which is the whole design already, stated
# once more. The linker is told not to complain about symbols it cannot see yet.
#
# WHAT IT DOES NOT PROVE, and what does. The link cannot check that a symbol
# exists on the device, because the device's library was not there. So this
# proves compilation, codegen and DT_NEEDED wiring for four ABIs; the symbol
# floor is proved separately by scripts/check_symbols.sh, which is the only
# thing that can catch a symbol the oldest supported device lacks.
#
# Usage:  ./scripts/build_all.sh
set -euo pipefail

cd "$(dirname "$0")/.."

API="${API:-29}"
NDK="${NDK:-/opt/ndk}"
OUT="build/release"
INC="${AOSP_INC:-third_party/aosp/include}"

[ -f "$INC/binder/Parcel.h" ] || {
  echo "no AOSP headers in $INC - run ./scripts/fetch_headers.sh first" >&2
  exit 1
}

CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++"
[ -x "$CC" ] || CC="$NDK/toolchains/llvm/prebuilt/linux-arm64/bin/clang++"

STUB_DIR="$(mktemp -d)"
trap 'rm -rf "$STUB_DIR"' EXIT
mkdir -p "$OUT"

# One stub per library we link. A single function and a SONAME each: the only
# job is to make the linker record the dependency, so the device's own library
# does the resolving at run time.
cat > "$STUB_DIR/stub.c" <<'EOF'
void etools_link_stub(void) {}
EOF
for abi in arm64-v8a armeabi-v7a x86_64 x86; do
  # The NDK's ABI names are not clang target triples. arm64-v8a is
  # "aarch64-linux-android", 32-bit arm is "arm-linux-androideabi", and passing
  # "armeabi-v7a-linux-android29" to clang is an error, not a fallback.
  case "$abi" in
    arm64-v8a)   TRIPLE="aarch64-linux-android$API" ;;
    armeabi-v7a) TRIPLE="arm-linux-androideabi$API" ;;
    x86_64)      TRIPLE="x86_64-linux-android$API" ;;
    x86)         TRIPLE="i686-linux-android$API" ;;
    *) echo "unknown ABI '$abi'" >&2; exit 1 ;;
  esac

  # The stubs are rebuilt per ABI, inside the loop, because a stub compiled for
  # one target is rejected by the linker for another ("incompatible with
  # armelf_linux_eabi") - a cross build that reuses one stub directory fails on
  # the 32-bit ABIs and only there, which looks like a toolchain problem.
  for lib in libbinder libutils; do
    "$CC" --target="$TRIPLE" -shared -fPIC -o "$STUB_DIR/$lib.so" \
      -Wl,-soname,$lib.so "$STUB_DIR/stub.c"
  done

  # -Wall -Wextra here as well as in CMakeLists.txt: this is the path CI builds
  # and releases from, so the build with the fewest warnings enabled was also
  # the one whose warnings nobody saw.
  #
  # -Wno-... for warnings that fire only inside the vendored AOSP headers, which
  # are byte-identical to upstream and not ours to fix. Scoped to those two
  # diagnostics rather than to third_party/ as a whole, so a warning in OUR code
  # is not lost in the noise.
  "$CC" --target="$TRIPLE" -std=gnu++20 -O2 -fno-rtti \
    -I "$INC" -I include -I src \
    -Wall -Wextra -Wno-deprecated -Wno-nontrivial-memcall \
    src/binder.cpp src/args.cpp src/device.cpp src/dump_types.cpp \
    src/extflags.cpp src/section.cpp src/tool.cpp src/dumpsyse_main.cpp \
    -o "$OUT/dumpsyse-$abi" \
    -L "$STUB_DIR" -lbinder -lutils \
    -Wl,--unresolved-symbols=ignore-all -Wl,--allow-shlib-undefined \
    -llog

  "$CC" --target="$TRIPLE" -std=gnu++20 -O2 -fno-rtti \
    -I "$INC" -I include -I src \
    -Wall -Wextra -Wno-deprecated -Wno-nontrivial-memcall \
    src/binder.cpp src/args.cpp src/device.cpp src/dump_types.cpp \
    src/extflags.cpp src/section.cpp src/tool.cpp src/cmde_main.cpp \
    -o "$OUT/cmde-$abi" \
    -L "$STUB_DIR" -lbinder -lutils \
    -Wl,--unresolved-symbols=ignore-all -Wl,--allow-shlib-undefined \
    -llog

  printf '%-14s dumpsyse %8s bytes  DT_NEEDED: %s\n' "$abi" \
    "$(wc -c < "$OUT/dumpsyse-$abi")" \
    "$(readelf -d "$OUT/dumpsyse-$abi" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')"
done

echo
echo "built: $OUT/{dumpsyse,cmde}-$abi for four ABIs"
