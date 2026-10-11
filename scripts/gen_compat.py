#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Measure dumpsys/cmd/binder behaviour per AOSP release, then emit:

  src/compat_text.h     the generated per-API behaviour table the library reads
  docs/compat_matrix.md the full measured matrix, one row per ref, nothing inferred

WHY A SCRIPT: hand-transcribing release behaviour is the class of bug that bit
the predecessor project twice - rows were keyed to a threshold ("33-37 are
identical") inferred from ONE measured release, so an OEM or a future branch
that diverged would be served the wrong table with nothing failing. Here every
supported API level is fetched from its own ref, and every ref is measured -
identical neighbours are collapsed into table rows, but the matrix records the
measurement either way.

WHY THE BINDER SURFACE TOO: the library makes calls whose shape changed across
releases (shellCommand's callback parameter, listServices' flags argument) and
references symbols that do not exist on every release (statusToString,
Stability::debugToString, getDebugPid). Each is measured per ref from the header
or implementation that declares it, so the weak-symbol list is a fact per API,
not a spot check.

Usage:
    ./scripts/gen_compat.py [--cache DIR]

Refs fetched (12 releases + main). AOSP has release branches only from Android
10; older levels are point-release tags.
"""
import argparse
import base64
import json
import re
import sys
import time
import urllib.error
import urllib.request

# API level -> AOSP ref. One row per supported release; none stands in for another.
# main is measured as its own row and emitted as the "37" row only if it matches
# nothing else; its API number is asserted from the source, not assumed.
REFS = {
    26: "android-8.0.0_r1",
    27: "android-8.1.0_r1",
    28: "android-9.0.0_r1",
    29: "android10-release",
    30: "android11-release",
    31: "android12-release",
    32: "android12L-release",
    33: "android13-release",
    34: "android14-release",
    35: "android15-release",
    36: "android16-release",
    37: "android17-release",
}

# Measured, but NOT given an API level and never folded into a table row.
#
# `main` is the tip: whatever API ships next, plus whatever is already on it.
# Calling it "37" would be exactly the mistake this script exists to prevent -
# assigning a release number to something that was not measured as that release -
# and folding it into the 37 row would ship whatever main has already changed
# into binaries claiming to be Android 17. So it is measured, shown, and its
# divergence from android17-release is REPORTED rather than encoded.
TIPS = ["main"]

# repo -> path -> what we grep. Fetched as +archive tarballs, extracted per ref.
FETCH = {
    # libs/binder and libs/binderdebug are fetched as whole trees; their include/
    # subdirectories come with them. Listing a subdirectory separately is not
    # merely redundant - it breaks the cache, because extracting libs/binder/include
    # CREATES libs/binder/, so the later libs/binder fetch sees the directory
    # already there and skips itself. The result is a tree with headers and no
    # sources, which fails much later as "IResultReceiver.cpp not found".
    "frameworks/native": ["cmds/dumpsys", "cmds/cmd",
                          "libs/binder", "libs/binderdebug"],
    "system/core": ["libutils"],
    "system/libbase": ["include"],   # absent before Android 10; tolerated
}

# framework-side Java AIDL files, fetched per ref. frameworks/base/core/java is
# huge, so the two files we need are fetched one by one instead of archived.
BASE_FILES = [
    "core/java/com/android/internal/os/IResultReceiver.aidl",
    "core/java/android/os/ResultReceiver.aidl",
]


def fetch_archive(repo, path, ref, attempts=4):
    kind = "tags" if ref.startswith("android-") else "heads"
    url = (f"https://android.googlesource.com/platform/{repo}"
           f"/+archive/refs/{kind}/{ref}/{path}.tar.gz")
    last = None
    for n in range(attempts):
        try:
            with urllib.request.urlopen(url, timeout=120) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            # 404: the ref lacks the path. 400: googlesource's archive endpoint
            # answers Bad Request for a directory absent on old refs (it 404s
            # the same absence on newer ones) - both mean 'not here'.
            if e.code in (400, 404):
                return None
            last = e
        except Exception as e:       # noqa: BLE001
            last = e
        if n < attempts - 1:
            time.sleep(2 * (n + 1))
    raise last


def fetch_file(repo, path, ref, attempts=4):
    kind = "tags" if ref.startswith("android-") else "heads"
    url = f"https://android.googlesource.com/platform/{repo}/+/{kind}/{ref}/{path}?format=TEXT"
    last = None
    for n in range(attempts):
        try:
            with urllib.request.urlopen(url, timeout=90) as r:
                return base64.b64decode(r.read()).decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            last = e
        except Exception as e:       # noqa: BLE001
            last = e
        if n < attempts - 1:
            time.sleep(2 * (n + 1))
    raise last


def read(cache, ref, repo, path):
    p = cache / ref / repo / path
    if p.is_file():
        return p.read_text("utf-8", "replace")
    return None


def glob(cache, ref, repo, path):
    p = cache / ref / repo / path
    return sorted(str(f) for f in p.rglob("*")) if p.is_dir() else []


# ---------------------------------------------------------------------------
# dumpsys / cmd behaviour extractors
# ---------------------------------------------------------------------------

def body_of(src, header):
    m = re.search(header + r".*?\n\}", src, re.S)
    return m.group(0) if m else ""


def dumpsys_usage(src):
    b = body_of(src, r"static void usage\(\)")
    strings = re.findall(r'"((?:[^"\\]|\\.)*)"', b)
    return "".join(strings) if strings else None


def dumpsys_options(src):
    b = re.search(r"static struct option longOptions.*?\};", src, re.S)
    if not b:
        return []
    return re.findall(r'\{"([a-z]+)",\s*(?:no_argument|required_argument)', b.group(0))


def dumpsys_short_options(src):
    m = re.search(r'getopt_long\(\s*[^,]+,\s*[^,]+,\s*"([^"]*)"', src)
    if not m:
        raise SystemExit("could not find the getopt_long optstring")
    return m.group(1)


def has_proto_shim(src):
    main = body_of(src, r"int Dumpsys::main")
    return "PriorityDumper::PROTO_ARG" in main and "asProto = true" in main


def list_header_or_list(src):
    main = body_of(src, r"int Dumpsys::main")
    m = re.search(r"if \(N > 1( \|\| showListOnly)?\)", main)
    if not m:
        raise SystemExit("could not find the service-list condition")
    return m.group(1) is not None


def combines_dump_types(src):
    return "dumpTypeFlags" in body_of(src, r"int Dumpsys::main")


def dump_error_style(src):
    if "reportDumpError" in src:
        return 2
    if "Error dumping service info status_t" in src:
        return 1
    if "Error dumping service info:" in src:
        return 0
    raise SystemExit("unrecognised dump error message")


def cmd_no_args(src):
    m = re.search(r'"(cmd: No service specified[^"]*)"', src)
    return m.group(1) if m else None


def cmd_has_wait(src):
    return '"-w"' in src


# ---------------------------------------------------------------------------
# binder surface extractors - what the library may call, per ref
# ---------------------------------------------------------------------------

def ibinder_shell_command(header):
    """(callback_param, arity) of the static IBinder::shellCommand.

    API 24/25 took (target, in, out, err, args, resultReceiver); API 26 added
    IShellCallback as a 7th parameter. The arity is a mangled-name difference,
    so it decides which releases a binary can link at all.
    """
    m = re.search(r"static\s+status_t\s+shellCommand\(.*?\);", header, re.S)
    if not m:
        return (None, 0)
    sig = " ".join(m.group(0).split())
    callback = "ShellCallback" in sig
    return (callback, sig.count(","))


def servicemanager_list_services(header):
    """listServices shape: flagless (26/27) or int flags (28+).

    The vtable slot is the same on both, so compiling DOWN looks safe and is
    not: a caller that passes no flags register meets a callee that reads it.
    The binary therefore compiles against NEWEST-supported headers, and this
    measurement is what the parity layer must NOT lean on at runtime.
    """
    out = []
    for m in re.finditer(r"virtual\s+(?:android::)?Vector<[^>]+>\s+listServices\s*\(([^)]*)\)",
                         header):
        params = " ".join(m.group(1).split())
        out.append(params)
    return out


def result_receiver_send(header):
    """IResultReceiver::send parameter list - API 30 added a String16 message."""
    m = re.search(r"virtual\s+void\s+send\s*\(([^)]*)\)", header)
    return " ".join(m.group(1).split()) if m else None


def ibinder_members(header):
    return {
        "getDebugPid": "getDebugPid" in header,
        "shellCommand": "shellCommand" in header,
    }


def stability_present(tree):
    return any(p.endswith("binder/Stability.h") for p in tree)


def status_to_string(tree, repo):
    """Which directory defines statusToString on this ref.

    It moved with libutils; the library references it weakly and the null check
    is only correct if the symbol truly is absent where we think it is.
    """
    hits = []
    for p in tree:
        if p.endswith("Errors.h") or p.endswith("Errors.cpp"):
            src = open(p, "r", encoding="utf-8", errors="replace").read()
            if "statusToString" in src:
                hits.append(p.split("/", 2)[-1])
    return sorted(hits)


def binderdebug_present(tree):
    return any(p.endswith("binderdebug/BinderDebug.h") or
               p.endswith("binder/BinderDebug.h") for p in tree)


def rr_send_native(cpp_src):
    """The NATIVE BpResultReceiver::send parcel writes, verbatim.

    Measured across every ref: it writes ONLY the int32 resultCode, on every
    release. The 'String16 message after the code' the predecessor observed on
    device is a DIFFERENT interface - the JAVA IResultReceiver
    (frameworks/base, send(int resultCode, in Bundle resultData)) which shares
    the descriptor string. Native senders are int32-only everywhere.
    """
    m = re.search(r"virtual\s+void\s+send\s*\(int32_t.*?transact\((?:.|\n)*?;", cpp_src, re.S)
    if not m:
        return None
    body = m.group(0)
    writes = re.findall(r"data\.(write\w+)\(([^)]*)\)", body)
    return [f"{w}({a.strip()})" for w, a in writes]


def rr_send_java(aidl_src):
    """The JAVA IResultReceiver method signature, from the AIDL."""
    m = re.search(r"void send\(([^)]*)\);", aidl_src or "")
    return " ".join(m.group(1).split()) if m else None


def c_literal(s):
    assert '"' not in s, "extracted literal must not contain a bare quote"
    return '"' + s + '"'


def measure_row(api, ref, cache):
    """Everything this project depends on, read from one ref's own tree.

    api is None for a tip ref: measured identically, but given no API
    number, because no release has been cut for it yet.
    """
    # frameworks/base files are fetched per-ref (not archived with the rest)
    # because core/java is a multi-gigabyte tree in archive form.
    for bf in BASE_FILES:
        p = cache / ref / "frameworks/base" / bf
        if not p.exists():
            src = fetch_file("frameworks/base", bf, ref)
            if src is None:
                sys.exit(f"{ref}: {bf} not found on frameworks/base")
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(src)
    dsrc = read(cache, ref, "frameworks/native", "cmds/dumpsys/dumpsys.cpp")
    csrc = read(cache, ref, "frameworks/native", "cmds/cmd/cmd.cpp")
    if dsrc is None or csrc is None:
        sys.exit(f"{ref}: dumpsys.cpp or cmd.cpp missing - path moved?")
    u, opts = dumpsys_usage(dsrc), dumpsys_options(dsrc)
    if not u:
        sys.exit(f"{ref}: no usage() found")
    row = dict(
        api=api, ref=ref, usage=u, options=opts,
        short_options=dumpsys_short_options(dsrc),
        proto_shim=has_proto_shim(dsrc),
        list_header_or_list=list_header_or_list(dsrc),
        combines=combines_dump_types(dsrc),
        error_style=dump_error_style(dsrc),
        cmd_no_args=cmd_no_args(csrc),
        cmd_wait=cmd_has_wait(csrc),
    )

    # binder surface
    ib = read(cache, ref, "frameworks/native", "libs/binder/include/binder/IBinder.h")
    if ib is None:
        ib = read(cache, ref, "frameworks/native", "include/binder/IBinder.h")
    if ib is None:
        sys.exit(f"{ref}: IBinder.h not found in either header root")
    ism = read(cache, ref, "frameworks/native", "libs/binder/include/binder/IServiceManager.h")
    if ism is None:
        ism = read(cache, ref, "frameworks/native", "include/binder/IServiceManager.h")
    irr = read(cache, ref, "frameworks/native", "libs/binder/include/binder/IResultReceiver.h")
    if irr is None:
        irr = read(cache, ref, "frameworks/native", "include/binder/IResultReceiver.h")
    cb, arity = ibinder_shell_command(ib)
    if cb is None:
        sys.exit(f"{ref}: shellCommand signature not found in IBinder.h")
    row["shell_callback"] = cb
    row["shell_args"] = arity
    row["list_services"] = servicemanager_list_services(ism)
    row["rr_send"] = result_receiver_send(irr)
    row["ibinder"] = ibinder_members(ib)
    irr_cpp = read(cache, ref, "frameworks/native", "libs/binder/IResultReceiver.cpp")
    if irr_cpp is None:
        sys.exit(f"{ref}: IResultReceiver.cpp not found")
    row["rr_send_writes"] = rr_send_native(irr_cpp)
    base_aidl = read(cache, ref, "frameworks/base",
                     BASE_FILES[0])
    row["rr_send_java"] = rr_send_java(base_aidl)
    tree = glob(cache, ref, "frameworks/native", "libs/binder")
    dbg_tree = glob(cache, ref, "frameworks/native", "libs/binderdebug")
    row["stability"] = stability_present(tree)
    row["binderdebug"] = binderdebug_present(dbg_tree)
    row["status_to_string"] = status_to_string(
        glob(cache, ref, "system/core", "libutils") +
        glob(cache, ref, "system/libbase", "include"), None)
    return row


# The fields two rows must share to be treated as the same variant. Collapsing
# happens for the TABLE only; the matrix keeps every measured ref, and a tip ref
# is never collapsed into a numbered row at all.
BEHAVIOUR_KEYS = ["usage", "options", "short_options", "proto_shim",
                  "list_header_or_list", "combines", "error_style",
                  "cmd_no_args", "cmd_wait", "shell_callback", "shell_args",
                  "list_services", "rr_send_writes", "rr_send_java",
                  "stability", "binderdebug", "status_to_string"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", default="third_party/aosp-sources")
    args = ap.parse_args()

    import tarfile
    import tempfile
    from pathlib import Path

    cache = Path(args.cache)
    cache.mkdir(parents=True, exist_ok=True)

    print("==> fetching per-ref sources (cached under %s)" % cache)
    for ref in list(REFS.values()) + TIPS:
        d = cache / ref
        # Per-dir check rather than a ref-level stamp: adding a directory to
        # FETCH must fetch it on refs that were cached before it was added.
        missing = [repo for repo, paths in FETCH.items()
                   for path in paths if not (d / repo / path).is_dir()]
        if not missing:
            print("  %-22s cached" % ref)
            continue
        print("  %-22s fetching %s" % (ref, ",".join(missing[:3]) + ("..." if len(missing) > 3 else "")))
        for repo, paths in FETCH.items():
            for path in paths:
                if (d / repo / path).is_dir():
                    continue
                blob = fetch_archive(repo, path, ref)
                if blob is None:
                    continue
                out = d / repo / path
                out.mkdir(parents=True, exist_ok=True)
                with tempfile.NamedTemporaryFile(suffix=".tgz") as tf:
                    tf.write(blob)
                    tf.flush()
                    try:
                        with tarfile.open(tf.name) as t:
                            # archive members are relative; refuse traversal
                            t.extractall(out, filter="data")
                    except (tarfile.TarError, OSError) as e:
                        print(f"    {repo}/{path}: {e}", file=sys.stderr)
    print()

    # ------------------------------------------------------------------ rows
    rows = [measure_row(api, ref, cache) for api, ref in sorted(REFS.items())]

    # Tips are measured by the same function and carry no API level.
    for ref in TIPS:
        rows.append(measure_row(None, ref, cache))

    # collapse consecutive identical rows into table variants. Tips are excluded:
    # whatever `main` has already changed belongs to a release that does not
    # exist yet, and folding it into a numbered row would ship it as that release.
    variants = []
    for r in [x for x in rows if x["api"] is not None]:
        if variants and all(r[k] == variants[-1][k] for k in BEHAVIOUR_KEYS):
            variants[-1]["max_api"] = r["api"]
        else:
            v = dict(r)
            v["max_api"] = r["api"]
            variants.append(v)

    # ------------------------------------------------------- compat_text.h
    n_opts = max(len(v["options"]) for v in rows)
    out = []
    w = out.append
    w("// SPDX-License-Identifier: GPL-3.0-only")
    w("// GENERATED by scripts/gen_compat.py - do not edit by hand.")
    w("//")
    w("// dumpsys/cmd behaviour per release, MEASURED from each ref's own source")
    w("// (frameworks/native cmds/dumpsys/dumpsys.cpp, cmds/cmd/cmd.cpp). No row")
    w("// stands in for an unmeasured release: rows collapse only releases whose")
    w("// behaviour is identical in every measured field, and the full matrix -")
    w("// one row per ref - lives in docs/compat_matrix.md, regenerated together")
    w("// with this file.")
    w("#pragma once")
    w("")
    w("namespace etools {")
    w("namespace compat {")
    w("")
    w("struct DumpsysVariant {")
    w("    int         min_api;")
    w("    int         max_api;")
    w("    const char* usage;          // verbatim dumpsys --help text")
    w(f"    const char* options[{n_opts}];  // long options this API accepts")
    w("    const char* short_options;  // getopt optstring")
    w("    bool        proto_shim;")
    w("    bool        list_header_or_list;")
    w("    bool        combines;")
    w("    int         error_style;")
    w("};")
    w("")
    w("inline constexpr DumpsysVariant kDumpsysVariants[] = {")
    for v in variants:
        opts = list(v["options"]) + [""] * (n_opts - len(v["options"]))
        w(f"    {{ {v['api']}, {v['max_api']},")
        w(f"      {c_literal(v['usage'])},")
        w("      { " + ", ".join(c_literal(o) for o in opts) + " },")
        w(f"      {c_literal(v['short_options'])},")
        w(f"      {str(v['proto_shim']).lower()}, {str(v['list_header_or_list']).lower()},")
        w(f"      {str(v['combines']).lower()}, {v['error_style']} }},")
    w("};")
    w("inline constexpr int kDumpsysVariantCount =")
    w("    sizeof(kDumpsysVariants) / sizeof(kDumpsysVariants[0]);")
    w(f"inline constexpr int kDumpsysMaxOptions = {n_opts};")
    w("")
    w("struct CmdVariant {")
    w("    int         min_api;")
    w("    int         max_api;")
    w("    const char* no_args;")
    w("    bool        wait_flag;")
    w("};")
    w("")
    w("inline constexpr CmdVariant kCmdVariants[] = {")
    for v in variants:
        w(f"    {{ {v['api']}, {v['max_api']}, {c_literal(v['cmd_no_args'])}, "
          f"{str(v['cmd_wait']).lower()} }},")
    w("};")
    w("inline constexpr int kCmdVariantCount =")
    w("    sizeof(kCmdVariants) / sizeof(kCmdVariants[0]);")
    w("")
    w("}  // namespace compat")
    w("}  // namespace etools")
    with open("include/etools/compat.hpp", "w") as f:
        f.write("\n".join(out) + "\n")
    print("==> include/etools/compat.hpp written (%d variants from %d measured refs)"
          % (len(variants), len(rows)))

    # ------------------------------------------------------ docs/compat_matrix.md
    md = []
    w = md.append
    w("# Compat matrix - measured, per ref")
    w("")
    w("GENERATED by `scripts/gen_compat.py`. Every row below is read from that")
    w("ref's own source tree; nothing is inferred from a neighbouring release.")
    w("Regenerate after adding a ref: `./scripts/gen_compat.py`.")
    w("")
    w("A row marked `tip` is `main`: measured, but given no API level, because no")
    w("release has been cut from it yet. It is never folded into a numbered row -")
    w("whatever it has already changed belongs to a release that does not exist.")
    w("")
    w("| API | ref | optstring | long options | proto shim | list hdr `-l` | dump types | err style | cmd no-args | cmd `-w` |")
    w("|----:|-----|-----------|--------------|------------|---------------|------------|-----------|-------------|----------|")
    for r in rows:
        w("| %s | `%s` | `%s` | %s | %s | %s | %s | %d | %s | %s |" % (
            r["api"] if r["api"] is not None else "tip", r["ref"], r["short_options"],
            ", ".join("`--%s`" % o for o in r["options"]) or "-",
            "yes" if r["proto_shim"] else "no",
            "yes" if r["list_header_or_list"] else "no",
            "bitmask" if r["combines"] else "one-of",
            r["error_style"],
            ("`%s`" % r["cmd_no_args"][:60]) if r["cmd_no_args"] else "-",
            "yes" if r["cmd_wait"] else "no"))
    w("")
    w("## Binder surface, per ref")
    w("")
    w("| API | ref | `shellCommand` callback | `listServices` | native `send` writes | java `send` | `getDebugPid` | `Stability.h` | `BinderDebug.h` | `statusToString` |")
    w("|----:|-----|-------------------------|----------------|----------------------|-------------|---------------|---------------|-----------------|------------------|")
    for r in rows:
        st = "yes" if r["status_to_string"] else "**absent**"
        w("| %s | `%s` | %s | %s | `%s` | `%s` | %s | %s | %s | %s |" % (
            r["api"] if r["api"] is not None else "tip", r["ref"],
            "yes" if r["shell_callback"] else "**no**",
            "flagless" if r["list_services"] and "dumpsysFlags" not in r["list_services"][0]
            else "flags",
            " ".join(r["rr_send_writes"]) if r["rr_send_writes"] else "-",
            r["rr_send_java"] or "-",
            "yes" if r["ibinder"]["getDebugPid"] else "no",
            "yes" if r["stability"] else "no",
            "yes" if r["binderdebug"] else "no",
            st))
    w("")
    with open("docs/compat_matrix.md", "w") as f:
        f.write("\n".join(md) + "\n")
    print("==> docs/compat_matrix.md written")

    # machine-readable dump for tests and future tooling
    with open("scripts/compat_facts.json", "w") as f:
        json.dump(rows, f, indent=1)
    print("==> scripts/compat_facts.json written")

    # What the tip already changed relative to the newest CUT release. Reported,
    # never encoded: acting on it would mean shipping unreleased behaviour as if
    # it were that release's.
    newest = max(x["api"] for x in rows if x["api"] is not None)
    base_row = next(x for x in rows if x["api"] == newest)
    for tip in [x for x in rows if x["api"] is None]:
        diffs = [k for k in BEHAVIOUR_KEYS if tip[k] != base_row[k]]
        if diffs:
            print(f"    note: {tip['ref']} already differs from API {newest} in: "
                  f"{', '.join(diffs)} (not encoded - no release yet)")
        else:
            print(f"    note: {tip['ref']} still matches API {newest} in every measured field")


if __name__ == "__main__":
    main()
