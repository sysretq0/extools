# Notes

Decisions, the reasoning behind them, and what was measured rather than assumed.
When a decision changes, it changes here first.

---

## What is verified, and how

Stated plainly because the claim is easy to overstate, and a predecessor project
of this family claimed an API 26 floor having tested one device.

| claim | how | state |
|---|---|---|
| dumpsys/cmd behaviour per release | `scripts/gen_compat.py` fetches each release's own `dumpsys.cpp`, `cmd.cpp` and binder headers and measures them | **12 refs measured**: API 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36 and `main` |
| binder surface per release | same script: `shellCommand` arity, `listServices` arity, `IResultReceiver::send` payload, `Stability`, `BinderDebug.h`, `statusToString`, `getDebugPid` | measured, per ref, in `docs/compat_matrix.md` |
| nothing references a symbol newer than API 26 | `scripts/check_symbols.sh`: allowlist, fails closed, **plus** every hard symbol must be found in the `android-8.0.0_r1` source tree | **passes for all 8 binaries** (4 ABIs × 2 tools), 58 hard symbols each, one weak by design |
| parses reproduce AOSP per release | `tests/test_flags.cpp`, 233 assertions, runs on the host in a second | **passes** on host and on device |
| drop-in byte parity | diffed against `/system/bin/dumpsys` and `/system/bin/cmd` on an API 34 device | **`-l` and `--help` byte-identical**; `dumpsys battery` byte-identical; `power` and `deviceidle` differ only in lines the stock tool also differs from itself on |
| `cmd` exit codes | 5 runs per case against stock | **matches** for `package bogus` (255), `activity bogus` (255), `deviceidle whitelist` (0), `settings get secure ime` (0), `window dismiss-keyguard` (0); missing service → 20 with the same wording |
| runs on API 26-33, 35-37 | — | **NOT verified on hardware.** No device or emulator of those releases was available. What stands in its place: per-release source measurement, the symbol floor gate, and weak symbols for the two API-gated ones. This is a real gap, not a formality. |

`dumpsyse -l` and `dumpsyse --help` are byte-identical to stock on API 34, which
is the strongest single parity result here: both exercise paths that would
differ if the parser, the option set, the service-list filter or the wording had
drifted.

---

## Decisions

### D1. Compile UP, not down. The NDK platform is 29; the device floor is 26.

These are two different numbers and the difference is the whole rule.

`IServiceManager::listServices` is flagless on API 26/27 and takes an `int` from
API 28. The vtable slot is identical, so building against the older headers
*looks* safe. It is not: the caller passes no flags register, every newer
libbinder reads that register anyway, and the answer is **zero services** where
the real tool lists 271 — with nothing erroring anywhere. The only symptom is a
short list that looks plausible.

The Itanium C++ ABI inverts the usual advice: a callee that ignores an extra
register argument is harmless, a caller that never sets one is not. So the
headers are the newest branch whose symbols still exist on the oldest device.
API 29 is that line, and it is also what the vendored headers need to compile at
all (`Parcel.h` pulls `unique_fd.h`, which needs `<android/fdsan.h>`).

The binding constraint is **symbol existence**, which the compiler does not check
and neither does the linker, because a missing symbol is a *load-time* failure
under `BIND_NOW`. Hence `scripts/check_symbols.sh`.

### D2. The compatibility table has one row per release, and the generator fails closed.

The predecessor's table said "API 33-37 are identical" and proved it by
fetching **one** of those four. That is an inference wearing a measurement's
clothes. Here every release is fetched and measured in its own right, the matrix
records each row even where neighbours collapse into one table entry, and
`scripts/check_compat.sh` re-measures and fails if the committed artefact no
longer matches AOSP.

API 26 and 27 collapse into one table row because they were measured and are
identical — not because a threshold assumed it.

### D3. Two `IResultReceiver`s share one descriptor. The trailing payload is not ours.

`IResultReceiver` exists twice, with the same descriptor string:

| | declared | `send` writes |
|---|---|---|
| native | `frameworks/native/libs/binder/include/binder/IResultReceiver.h` | `writeInterfaceToken` + `writeInt32(resultCode)` |
| Java | `frameworks/base/core/java/com/android/internal/os/IResultReceiver.aidl` | `send(int resultCode, in Bundle resultData)` |

Measured across every ref: **the native proxy has always written the int32
alone**, on API 26 through `main`, and the Java AIDL has always had the Bundle.
So a payload after the exit code is not an API-level change, it is a fact about
*which service answered* — Java services reply through the Java proxy.

The predecessor read that tail as a `String16` and printed it as a message. That
is parsing a Bundle as a string: it happens to produce something plausible, and
it is wrong. This implementation reads the code and leaves the tail alone.

### D4. `--ext-` is a namespace, because there is no subcommand to put flags before.

A binary invoked as `dumpsyse` **is** dumpsys. Every token after it belongs to
dumpsys or to the service. The predecessor avoided the collision by requiring a
subcommand (`bidl dumpsys …`), which buys a flag position and costs an extra
word in every invocation. Here a reserved `--ext-` prefix is the placement that
cannot collide: no AOSP flag and no service argument starts with it, so an
intercept is unambiguous, an unknown `--ext-` name is a typo to report rather
than something to forward, and the tool's own flags need no positional rule at
all.

The cost is honest and stated in `--ext-help`: the flags are not short, and they
must be typed.

### D5. The tool's own `--help` is never annotated.

`dumpsyse --help` prints `dumpsys --help`, byte for byte, and `cmde --help`
prints `cmd --help`. A caller comparing the two must not find this project's name
inside another tool's documentation, and `dumpsyse --help` differing from
`dumpsys --help` would be exactly the incompatibility this project exists to
avoid. Ours is therefore `--ext-help`.

### D6. Two shapes, chosen by the caller. No table of services.

`IBinder::shellCommand` takes a callback and a receiver. Pass `nullptr` for both
and no binder object is handed out, so no inbound transaction can exist, so there
is no looper to host one, no thread pool and no descriptor. Measured on device:
the call itself is ~1-2 ms, because `shellCommand` is synchronous and the
service does the work inside the transaction. That is why a loop can afford it.

Output is unaffected: it rides the descriptors the service dups and writes to,
not the callback. What the null shape costs is the **exit code**, which arrives
on the receiver as a one-way transaction.

So the default is the receiver shape — the one that cannot diverge from the real
tool — and `--ext-shape null` opts out. There is deliberately **no per-service
table**. An earlier revision of this file had one, listing the services measured
to complete without a receiver, and it is wrong for a structural reason: whether
a service reports an exit code is a fact about that service's own shell-command
implementation, of which there are hundreds, each release-dependent and each
forkable by an OEM. A stale entry silently drops an exit code and a missing
entry does the same thing. A list whose wrongness cannot be observed is worse
than no list. The switch is explicit instead.

### D7. `checkInterface()` first, always, on any receiver we pass.

The parcel arrives at position 0, which is not the payload: `BBinder::transact`
resets the position and the buffer begins with the RPC header — StrictMode
policy, WorkSource, the `'SYST'` magic from API 31, then the descriptor. AOSP
reads it with `CHECK_INTERFACE` before anything else.

Skipping it by hand is wrong on some releases and silently wrong on all of them:
`readInt32()` then returns a policy mask or a UTF-16 length, which is a
plausible small integer. The predecessor shipped that bug and it read as "a
failing command exits 0 and a succeeding one does not" — both wrong, both in
range, nothing crashing. `checkInterface()` is also one call into the **device's**
libbinder, which knows this ROM's header shape, so no per-API header arithmetic is
needed.

### D8. The exit code and the transaction status are reported through different channels.

A service rejecting an unknown subcommand reports **-1** as its exit code. A
failed transaction is also **-1**. By value alone the two are indistinguishable,
and conflating them is how `cmde package bogus` came to answer "can't find
service" and 20 where stock answers 255: the service had run, printed its own
error, and reported its code. `shell()` therefore takes an out-parameter for the
transaction status, and the tool resolves the service first so "could not be
reached" is a fact it holds rather than something inferred from a return value.

### D9. Bounded wait for the one-way result, never an immediate read, never forever.

`IResultReceiver::send` is one-way: the service fires it and continues, so it is
already in flight while the reply is waking the caller. Reading the instant the
call returns races that delivery — measured on API 34, `cmd package bogus` lost
the value in roughly 1 run in 13. Blocking forever hangs on services that never
report at all (`cmd activity` is one, which returns promptly through the error
path). The wait is bounded at 200 ms: far longer than delivery on a warm binder,
far shorter than a user notices.

### D10. The dump runs on a thread, always, and the read end is owned here.

`dump()` is synchronous and unbounded. Calling it on the calling thread while
the output goes somewhere undrained fills the pipe, blocks the service in
`write()`, and never returns — which wedges the service and trips the watchdog.

A **pipe** is the output transport, not a socketpair and not a file: it is what
the real tool uses, and it is the only one every service accepts — SELinux
refuses a socketpair for some domains and refuses a file we create. Owning the
read end is also what makes a timeout enforceable at all; when output goes
straight to stdout there is nothing to read.

On timeout the read end is closed first, so the service's pending write fails
with `EPIPE` and it unwinds, and the thread is then joined — never abandoned
mid-transaction.

### D11. Section extraction buffers; the streaming path cannot.

A section's end is not known until the next header arrives, which may be never.
So `--ext-section` captures one service's dump and filters it, while the plain
path streams. Buffering one service is bounded and cheap — the largest single
service measured is around 1.2 MB. A truncated capture is not a dump, so the
timeout notice is printed *before* any extracted text; otherwise a section cut
off mid-way is indistinguishable from a complete one.

The grammar was measured against real service dumps, not designed. Column-0
anchoring is not negotiable (allowing indentation takes `dumpsys package` from
41 headers to 5467), matching is on a word boundary (`Brightness` finds
`BrightnessTracker state` but `Bright` does not), a title carries no `=`, and an
ambiguous name is **refused** with the candidates listed rather than guessed —
33 headers in `dumpsys wifi` repeat.

### D12. Three AOSP behaviours reproduced rather than corrected.

- `--skip` is documented as comma-separated but does not act that way: AOSP
  compares each argument verbatim. We honour the observed behaviour, because
  matching the documentation would skip services the real tool dumps.
- `--skip` with no names, and `-l` misused, print usage with no message, while
  an unknown option prints a message, a blank line, then usage. Four distinct
  shapes, all real, all transcribed.
- A trailing `--proto` among the *service* arguments implies `--proto` only from
  API 30; before that it is an ordinary dump argument.

A behavioural difference is accepted only where the underlying tool's behaviour
is itself a defect and reproducing it is the compatible choice.

---

## Known limits

- **API 26-33 and 35-37 are unverified on hardware.** Only API 34 was available.
  The symbol floor is checked mechanically for every ABI; the wire shapes and the
  service behaviours on those releases are checked against source only. A release
  that changed something the source does not show — an OEM fork of `dumpsys.cpp`,
  a service whose shell command differs — would not be caught.
- `IServiceManager::listServices` gained a flags argument in API 28. We call the
  newer signature, which is correct on every device we claim, and is the reason
  for D1.
- `--pid`, `--thread` and `--clients` are refused, not faked. `--pid` needs
  `IBinder::getDebugPid`, a non-virtual libbinder symbol added after the headers
  this compiles against, and doing the transaction by hand needs a stack
  `android::Parcel` whose layout changed after those headers. `--thread` and
  `--clients` need `libbinderdebug`. `--stability` works.
- Section extraction supports no nesting paths, and matches against headers only,
  so a section with no header of its own is unreachable. Both follow from there
  being no common grammar across services.
- `cmd -w` uses `getService()` rather than the `waitForService()` AOSP uses,
  because that method does not exist on the API 29 headers. Same documented
  behaviour (blocking briefly for a service that does not exist yet), present on
  every supported release.
- A service that asks its caller to open an output file gets a clean refusal,
  where the real tool would open it after an SELinux check a shell-uid caller
  cannot pass anyway. The refusal is written in the shape AOSP's own base class
  writes, because a bare `-EPERM` would be read as an exception code.
