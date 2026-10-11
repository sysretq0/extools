#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Prove no symbol in a built binary is newer than the API 26 floor.
#
# WHY THIS EXISTS. A binary links the DEVICE's libbinder at runtime, and the
# build links whatever .so was on the build machine. Nothing in the build can
# therefore tell you a symbol is missing on the oldest device you claim to
# support - and with BIND_NOW every undefined symbol is resolved at load time,
# so the failure lands on someone else's phone as a dead binary. The
# predecessor of this project shipped exactly that: a hard reference to
# android::statusToString, which does not exist before API 30, meant the binary
# did not START on API 26-29.
#
# TWO CHECKS, and the second is the one that makes this more than a list:
#
#   1. ALLOWLIST, fails closed. Any hard `U` against android:: that is not on
#      the list below fails the check. Checking named symbols would prove only
#      that those names are weak; a NEW hand-declared symbol - which is exactly
#      how statusToString got in - would pass unnoticed. The default for a new
#      symbol is therefore "someone must justify it".
#
#   2. FLOOR EXISTENCE, from source. Every hard symbol must also be found in
#      the android-8.0.0_r1 tree under third_party/aosp-sources, which is the
#      floor release's own source, fetched by scripts/gen_compat.py. A symbol
#      absent there cannot be present on an API 26 device, whatever the header
#      we compiled against says.
#
# Weak symbols are reported, never failed: a weak reference is a deliberate
# runtime branch (null check) for a symbol that exists only on newer releases.
# statusToString (API 30+) and Stability::debugToString (API 31+) are the two
# this project carries, and docs/compat_matrix.md is the measurement behind
# both.
set -euo pipefail

cd "$(dirname "$0")/.."

FLOOR_REF="${FLOOR_REF:-android-8.0.0_r1}"
FLOOR_TREE="third_party/aosp-sources/$FLOOR_REF"

if [ ! -d "$FLOOR_TREE" ]; then
  echo "$0: no cached source tree for $FLOOR_REF - run ./scripts/gen_compat.py first" >&2
  exit 1
fi

# Every hard android:: symbol this project promises an API 26 device has.
# SIZET is substituted per ABI: size_t is 4 bytes on the 32-bit ABIs and 8 on
# the 64-bit ones, so the same source symbol demangles to `unsigned int` on one
# and `unsigned long` on the other. Both are correct for their ABI.
ALLOW=$(cat <<'EOF'
BBinder::BBinder()
BBinder::attachObject(void const*, void*, void*, void (*)(void const*, void*, void*))
BBinder::detachObject(void const*)
BBinder::dump(int, android::Vector<android::String16> const&)
BBinder::findObject(void const*) const
BBinder::getInterfaceDescriptor() const
BBinder::isBinderAlive() const
BBinder::linkToDeath(android::sp<android::IBinder::DeathRecipient> const&, void*, unsigned int)
BBinder::localBinder()
BBinder::onTransact(unsigned int, android::Parcel const&, android::Parcel*, unsigned int)
BBinder::pingBinder()
BBinder::transact(unsigned int, android::Parcel const&, android::Parcel*, unsigned int)
BBinder::unlinkToDeath(android::wp<android::IBinder::DeathRecipient> const&, void*, unsigned int, android::wp<android::IBinder::DeathRecipient>*)
BBinder::~BBinder()
BnResultReceiver::onTransact(unsigned int, android::Parcel const&, android::Parcel*, unsigned int)
BnShellCallback::onTransact(unsigned int, android::Parcel const&, android::Parcel*, unsigned int)
IBinder::checkSubclass(void const*) const
IBinder::localBinder()
IBinder::queryLocalInterface(android::String16 const&)
IBinder::remoteBinder()
IBinder::shellCommand(android::sp<android::IBinder> const&, int, int, int, android::Vector<android::String16>&, android::sp<android::IShellCallback> const&, android::sp<android::IResultReceiver> const&)
IBinder::~IBinder()
IInterface::~IInterface()
IResultReceiver::IResultReceiver()
IResultReceiver::descriptor
IResultReceiver::getInterfaceDescriptor() const
IResultReceiver::~IResultReceiver()
IShellCallback::IShellCallback()
IShellCallback::descriptor
IShellCallback::getInterfaceDescriptor() const
IShellCallback::~IShellCallback()
Parcel::checkInterface(android::IBinder*) const
Parcel::readInt32(int*) const
Parcel::readString16(android::String16*) const
Parcel::writeInt32(int)
Parcel::writeNoException()
ProcessState::self()
ProcessState::startThreadPool()
RefBase::RefBase()
RefBase::decStrong(void const*) const
RefBase::incStrong(void const*) const
RefBase::onFirstRef()
RefBase::onIncStrongAttempted(unsigned int, void const*)
RefBase::onLastStrongRef(void const*)
RefBase::onLastWeakRef(void const*)
RefBase::~RefBase()
String16::String16()
String16::String16(android::String16 const&)
String16::String16(char const*)
String16::size() const
String16::~String16()
String8::String8(android::String16 const&)
String8::~String8()
VectorImpl::VectorImpl(SIZET, unsigned int)
VectorImpl::finish_vector()
VectorImpl::insertAt(void const*, SIZET, SIZET)
VectorImpl::~VectorImpl()
defaultServiceManager()
EOF
)

# Symbols the source grep CANNOT see, each with the file that proves it exists at
# the floor. Format: <symbol-base>|<file-under-the-floor-tree-that-instantiates-it>
#
# Both of these are destructors of classes that declare NO destructor at the floor
# (android-8.0.0_r1's IResultReceiver.h / IShellCallback.h), so the compiler
# emits them as weak INLINE definitions - which is why grepping the header for
# them finds nothing, and why grepping for them is the wrong test rather than a
# failing one. What proves they exist in the floor's libbinder is that libbinder
# itself defines a class deriving from them (BpResultReceiver in
# libs/binder/IResultReceiver.cpp), which forces the emission inside the library
# rather than only in our binary. The file named here is checked to exist.
IMPLICIT=(
  "~IResultReceiver|frameworks/native/libs/binder/IResultReceiver.cpp"
  "~IShellCallback|frameworks/native/libs/binder/IShellCallback.cpp"
)

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

rc=0
for bin in "$@"; do
  [ -f "$bin" ] || { echo "$0: no such binary: $bin" >&2; exit 1; }
  case "$(readelf -h "$bin" | sed -n 's/.*Machine: *//p')" in
    *64*) sz="unsigned long" ;;
    *)     sz="unsigned int" ;;
  esac
  echo "=== $bin (size_t = $sz)"

  weak=$(nm -uC "$bin" | sed -n 's/^ *w android::\(.*\)$/\1/p' | sort -u)
  if [ -n "$weak" ]; then
    echo "  weak android:: symbols (API-gated on purpose, absent below their release):"
    printf '%s\n' "$weak" | sed 's/^/    /'
  else
    echo "  weak android:: symbols: none"
  fi

  hard=$(nm -uC "$bin" | sed -n 's/^ *U android::\(.*\)$/\1/p' | sort -u)
  echo "$hard" > "$TMP/hard.txt"
  printf '%s\n' "$ALLOW" | sed "s/SIZET/$sz/g" | sort -u > "$TMP/allow.txt"

  missing=$(comm -23 "$TMP/hard.txt" "$TMP/allow.txt" || true)
  if [ -n "$missing" ]; then
    echo "  FAIL: hard android:: symbols not on the API 26 allowlist:"
    printf '%s\n' "$missing" | sed 's/^/    /'
    rc=1
  fi
  stale=$(comm -13 "$TMP/hard.txt" "$TMP/allow.txt" || true)
  if [ -n "$stale" ]; then
    echo "  note: allowlist entries no longer referenced (a stale entry is not a safety net):"
    printf '%s\n' "$stale" | sed 's/^/    /'
  fi
  echo "  hard android:: symbols: $(wc -l < "$TMP/hard.txt")"

  # Floor existence, from the floor release's own source rather than from the
  # allowlist. Checked per symbol by its unqualified name.
  unverified=""
  while IFS= read -r sym; do
    [ -n "$sym" ] || continue
    # The unqualified name: everything before the '(', minus any return type.
    base=${sym%%(*}
    base=${base##*::}
    base=$(printf '%s' "$base" | sed 's/^ *//')
    case "$base" in
      descriptor|"") continue ;;   # a static data member, not a function
    esac
    # A cited exemption, whose file must still be present at the floor.
    exempt=0
    for entry in "${IMPLICIT[@]}"; do
      name=${entry%%|*}
      file=${entry#*|}
      if [ "$base" = "$name" ]; then
        exempt=1
        if [ ! -e "$FLOOR_TREE/$file" ]; then
          unverified="$unverified$base (cited, but $file is not in the cached $FLOOR_REF tree)\n"
        fi
      fi
    done
    [ "$exempt" -eq 1 ] && continue
    if ! grep -rql -- "$base" "$FLOOR_TREE" 2>/dev/null; then
      unverified="$unverified$base\n"
    fi
  done < "$TMP/hard.txt"
  if [ -n "$unverified" ]; then
    echo "  FAIL: not found anywhere in $FLOOR_REF's source tree:"
    printf "$unverified" | sort -u | sed 's/^/    /'
    rc=1
  else
    echo "  every hard symbol was found in $FLOOR_REF's source tree"
  fi
done

if [ "$rc" -ne 0 ]; then
  cat >&2 <<'EOF'

Either the symbol is newer than API 26 - declare it __attribute__((weak)) and
call it behind a null check - or it is legitimate, in which case add it to the
allowlist in this script WITH the AOSP citation that proves it exists at the
floor, and note it in docs/compat_matrix.md.
EOF
fi
exit "$rc"
