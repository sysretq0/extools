# etools

`cmd` and `dumpsys`, in process, as a library and as two drop-in binaries.

No `fork`/`exec`, no `app_process`, no JVM per call, no root. Runs as `uid 2000`
(`shell`). Everything after the tool's own name is handed to the tool untouched,
so the tools stay themselves: same flags, same output bytes, same errors, same
exit codes.

- **`cmde`** — a drop-in for `/system/bin/cmd`, plus `--ext-shape`.
- **`dumpsyse`** — a drop-in for `/system/bin/dumpsys`, plus a loop, an
  interval and section extraction.
- **`libetools.a`** — the same two operations as a library, for anything that
  wants to call a system service in-process instead of shelling out to a tool.

## Why it exists

Reaching a system service from a native daemon means either spawning
`/system/bin/dumpsys` (a fork, an exec, a process, and text to parse) or talking
to Binder directly. This is the second option, packaged so it can be linked
rather than re-derived: the transport, the per-release compatibility behaviour,
and the two shapes a `shellCommand` call can take.

## Install

```console
$ ./scripts/setup.sh              # fetch AOSP's binder headers, pull the device's libs
$ ./scripts/build.sh              # library + both binaries, one ABI
$ ./scripts/build_all.sh          # all four ABIs, no device needed
$ adb push build/arm64-v8a-api29/dumpsyse /data/local/tmp/
```

Requires Android 8.0 (API 26) or newer. ABIs: `arm64-v8a`, `armeabi-v7a`,
`x86_64`, `x86`.

## Usage

```console
$ cmde -l                                  # same as: cmd -l
$ cmde deviceidle whitelist +pkg           # same as: cmd deviceidle whitelist +pkg
$ cmde --ext-shape null settings get secure ime
$ dumpsyse power                           # same as: dumpsys power
$ dumpsyse --ext-loop 10 --ext-interval 500 power
$ dumpsyse --ext-section "Battery saving stats" power
```

Extensions live in a reserved `--ext-` namespace and are stripped from `argv`
before the tool's own parser sees it, so nothing of ours can collide with a
flag the tool or a service already understands. `cmde --ext-help` and
`dumpsyse --ext-help` list them. The tool's own `--help` is never touched:
`dumpsyse --help` prints `dumpsys --help`, byte for byte.

## Using the library

```cpp
#include <etools/etools.hpp>

// One service's dump, streamed to a descriptor.
long n = etools::dump("power", {"--proto"}, STDOUT_FILENO, /*timeout_ms=*/10000);

// One shell command, in process, returning the service's own exit code.
int rc = etools::shell("package", {"list", "packages"}, STDIN_FILENO, STDOUT_FILENO,
                       STDERR_FILENO, etools::Shape::kReceiver);
```

A C ABI is in `<etools/etools.h>` for non-C++ callers. The parsers, the
per-release compatibility tables and the section extractor are public headers of
their own (`<etools/args.hpp>`, `<etools/compat.hpp>`, `<etools/section.hpp>`),
because an embedder reproducing tool behaviour needs them and re-deriving them
is how tool clones drift.

## The two shapes, and why there is no table of services

`IBinder::shellCommand` takes a callback and a result receiver. Pass `nullptr`
for both and no binder object can be handed out, so no inbound transaction can
exist, so there is no looper to host one, no thread pool, and no descriptor. The
cost is that the service's exit code arrives on the receiver, so you do not get
it. Output is unaffected either way: it rides the descriptors, not the callback.

`cmde` therefore defaults to the receiver shape, which is what the real tool
does and therefore cannot diverge from it, and `--ext-shape null` opts into the
thread-free one. There is deliberately no per-service table of who needs what:
that is a fact about each service's own shell-command implementation, it changes
between releases, and it is forkable by an OEM, so a list of them could be wrong
with nothing able to observe it. See `docs/notes.md`.

## Compatibility

Everything version-dependent is measured from AOSP source rather than
remembered: `scripts/gen_compat.py` fetches each supported release's
`dumpsys.cpp`, `cmd.cpp` and binder headers, and emits both the table the library
reads (`include/etools/compat.hpp`) and a full per-ref matrix
(`docs/compat_matrix.md`). One row per release; nothing is inferred from a
neighbouring release.

```console
$ ./scripts/gen_compat.py                 # re-measure; regenerates both artefacts
$ ./scripts/check_compat.sh               # fail if the committed table has drifted
$ ./scripts/check_symbols.sh build/release/*   # fail if any symbol is newer than API 26
$ ./scripts/run_tests_host.sh             # the parsers and the --ext- rules, on the host
```

Supported releases: API 26 through 36, plus `main`. What has been verified on
hardware and what has not is stated in `docs/notes.md` — read that before
relying on a release.

## Provenance

An independent implementation of the observable behaviour of two AOSP tools.
The behaviour it matches is transcribed from the Android Open Source Project
rather than written from memory; the generated usage text, accepted flag sets,
error wording and dispatch conditions come from `frameworks/native`'s own
sources. AOSP is Apache-2.0; see `NOTICE`.
