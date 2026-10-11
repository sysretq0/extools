#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Verify the committed compatibility artefacts still match AOSP.
#
# WHY. include/etools/compat.hpp is generated from each supported release's own
# source, so this project's promise - that it behaves like the tools of the
# device it runs on - is only as good as that file. When a branch changes, the
# table goes stale silently and nothing else notices. Regenerating and diffing
# turns a silent incompatibility into a red build.
#
# It needs no device, no NDK and about ten seconds, so it runs on every change.
set -euo pipefail

cd "$(dirname "$0")/.."
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cp include/etools/compat.hpp "$TMP/compat.hpp"
cp docs/compat_matrix.md "$TMP/compat_matrix.md"

if ! ./scripts/gen_compat.py >/dev/null 2>"$TMP/err"; then
  echo "FAIL: the generator itself errored:" >&2
  cat "$TMP/err" >&2
  cp "$TMP/compat.hpp" include/etools/compat.hpp
  exit 1
fi

rc=0
if ! diff -u "$TMP/compat.hpp" include/etools/compat.hpp; then
  echo >&2
  echo "FAIL: include/etools/compat.hpp no longer matches AOSP. A dumpsys or cmd" >&2
  echo "      change on some branch is not modelled. Adopt the regenerated file and" >&2
  echo "      record the new behaviour in docs/notes.md." >&2
  rc=1
fi
if ! diff -u "$TMP/compat_matrix.md" docs/compat_matrix.md; then
  echo >&2
  echo "FAIL: docs/compat_matrix.md no longer matches AOSP either." >&2
  rc=1
fi

if [ "$rc" -eq 0 ]; then
  echo "compat: both artefacts are reproducible from AOSP source (API 26-37 + main tip)"
fi
exit "$rc"
