#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Fetch AOSP's private binder headers into third_party/aosp/include.
#
# WHY: we link the device's private libbinder.so (see setup.sh), so we need its
# headers, which the NDK does not ship.
#
# WHY tarballs: googlesource exposes a per-directory +archive endpoint, so this
# is ~6 HTTP requests. Fetching header-by-header was ~25 sequential requests and
# took over five minutes.
#
# WHY android10-release, and not the oldest supported branch:
#
# The naive rule is "pin the ABI floor": fetch the headers of the OLDEST release
# we support (android-8.0.0_r1, API 26). That is wrong here, and wrong in the
# dangerous direction. IServiceManager::listServices changed arity - flagless on
# API 26/27, `int dumpsysFlags` from API 28. The vtable slot is the same, so
# compiling down LOOKS safe. It is not: the caller passes no flags register,
# every newer libbinder reads that register anyway, and the answer is 0 services
# where the real tool lists 271. Nothing errors anywhere; the only symptom is a
# short list that looks plausible.
#
# The Itanium C++ ABI makes compiling UP safe - a callee ignores an extra
# register argument it does not use - so the headers are the newest branch whose
# symbols still exist on the oldest supported device. android10-release (API 29)
# is that line for this project. See src/device.h and NOTES.md section 14.
#
# The constraint that actually binds is SYMBOL EXISTENCE, not header age: every
# android:: symbol the binary references must exist on API 26. That is not
# automatic, and the compiler does not check it - `nm -u` on the built binary
# does. `statusToString` is the worked example: it is declared by hand in
# src/dump_types.h precisely because android10-release's utils/Errors.h does not
# have it, and it only actually exists from API 30. As a hard reference it made
# bidl fail to LOAD on API 26-29; it is weak for that reason.
#
# Layout is branch-dependent. On main several of these are git symlinks whose
# targets do not resolve over the HTTP API; android10-release has real files.
set -euo pipefail

OUT="third_party/aosp/include"
# AOSP has release branches only from Android 10 up. Below that the refs are
# point-release TAGS, fetched from refs/tags/ instead of refs/heads/. The
# spelling tells the script which to use, so REF=android-8.0.0_r1 works too.
BRANCH="${BRANCH:-android10-release}"
if [[ "$BRANCH" == android-* ]]; then
  REF_KIND=tags
else
  REF_KIND=heads
fi
# Without these there is nothing to build against. Every other root is
# best-effort: several legitimately 404 depending on the ref.
REQUIRED_ROOT="frameworks/native:libs/binder/include"

# "<repo>:<path-prefix-within-repo>"
ROOTS=(
  "$REQUIRED_ROOT"
  "frameworks/native:include"
  "system/core:libutils/include"
  "system/core:libcutils/include"
  "system/core:include"
  "system/core:libsystem/include"
  "system/libbase:include"
  "system/logging:liblog/include"
  # Not fallbacks: these always run, and on 10/11 their content is byte-identical
  # to the two roots above. Below Android 10 system/libbase and system/logging
  # were not separate projects, so these are the ONLY copy there. libbase's
  # headers lived in a directory named `base` (not `libbase`) - which is where
  # android-base/unique_fd.h, pulled in by Parcel.h, actually comes from.
  "system/core:liblog/include"
  "system/core:base/include"
)

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Start from an empty tree. Copying into a tree from another BRANCH would union
# the two, leaving one branch's extra headers in place - exactly the staleness
# the note above exists to prevent, and silent: the build would still link.
rm -rf "$OUT"
mkdir -p "$OUT"

ok=0
miss=0
got_required=0
for spec in "${ROOTS[@]}"; do
  repo="${spec%%:*}"
  path="${spec#*:}"
  url="https://android.googlesource.com/platform/${repo}/+archive/refs/${REF_KIND}/${BRANCH}/${path}.tar.gz"
  printf '  %-42s ' "${repo}/${path}"
  if curl -sf "$url" -o "$TMP/r.tgz"; then
    ok=$((ok + 1))
    if [ "$spec" = "$REQUIRED_ROOT" ]; then got_required=1; fi
    n="$(tar tzf "$TMP/r.tgz" | wc -l)"
    # googlesource stamps archives with server-side mtimes that can be slightly
    # ahead of the client clock; -m drops them to avoid a wall of warnings.
    ex="$TMP/x"; rm -rf "$ex"; mkdir -p "$ex"
    tar xzmf "$TMP/r.tgz" -C "$ex" 2>/dev/null
    # -L dereferences: these archives contain RELATIVE symlinks pointing outside
    # the extracted subtree (log -> ../liblog/include/log). Copying one raw
    # leaves a dangling link that fails much later as "file not found".
    cp -rL "$ex"/* "$OUT"/ 2>/dev/null || true
    rm -rf "$ex"
    echo "ok ($n files)"
  else
    miss=$((miss + 1))
    echo "MISS"
  fi
done

if [ "$got_required" -eq 0 ]; then
  echo "$0: required root $REQUIRED_ROOT not found on $BRANCH - $OUT has no binder headers, the build cannot use it" >&2
  exit 1
fi

# log/log_read.h is a legacy C header that does `#undef __unused` and then
# includes <fcntl.h>, whose NDK-fortified version uses __unused as a *type*.
# Any build with _FORTIFY_SOURCE then fails with "unknown type name __unused".
# AOSP never hits this because it does not use the NDK's fortify configuration.
# log/log.h only includes it as an ecosystem courtesy and nothing here needs it.
rm -f "$OUT/log/log_read.h"
sed -i '/#include <log\/log_read.h>/d' "$OUT/log/log.h" 2>/dev/null || true

echo "  fetched $(find "$OUT" -name '*.h' | wc -l) headers from $ok/${#ROOTS[@]} roots ($miss missed) into $OUT (branch $BRANCH)"