// SPDX-License-Identifier: GPL-3.0-only
// Command-line parsing for the REAL dumpsys and cmd. Nothing else.
//
// The argv that reaches this code is byte-identical to what a user would type
// for dumpsys or cmd directly. The drop-in binaries (dumpsyse, cmde) parse their
// OWN extended flags before this point and hand the rest over untouched.
//
// Everything below is transcribed from AOSP, not invented:
//   frameworks/native/cmds/dumpsys/dumpsys.cpp
//   frameworks/native/cmds/cmd/cmd.cpp
// across android-8.0.0_r1 .. main (API 26-37).

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace etools {

// How dumpsys renders a failure. All four are real behaviours, verified against
// /system/bin/dumpsys rather than assumed.
enum class ErrorStyle {
    kMessageOnly,        // "Error: invalid timeout..."         (-t/-T bad value)
    kMessageBlankUsage,  // message, blank line, then usage      (unknown option)
    kBlankUsage,         // blank line then usage, NO message    (bad --priority)
    kUsageOnly,          // usage only                           (--skip/-l misuse)
};

// dumpsys's --dump/--pid/--stability/--thread/--clients are a bitmask, not a
// set of independent booleans: AOSP ORs them into `dumpTypeFlags`, and the
// value is compared for EQUALITY against kTypePid to decide whether the PID
// line is printed bare or with a label. Values are local to this process - they
// never cross a boundary - so these need not match AOSP's numbering.
enum DumpTypeFlag {
    kTypeDump      = 1 << 0,
    kTypePid       = 1 << 1,
    kTypeStability = 1 << 2,
    kTypeThread    = 1 << 3,   // not implemented, see etools.cpp
    kTypeClients   = 1 << 4,   // not implemented, see etools.cpp
};

// What a parsed `dumpsys` command line asks for.
struct DumpsysOptions {
    long timeout_ms = 10000;         // -t takes SECONDS, -T takes MS (API 28+)
    bool list_only = false;          // -l
    bool as_proto = false;           // --proto
    bool skip_mode = false;          // --skip: a mode, takes no argument
    std::string priority_type;       // --priority CRITICAL|HIGH|NORMAL
    int dump_type_flags = 0;         // --dump/--pid/--stability/--thread/--clients
    bool want_help = false;

    // Filled by the --ext-section layer above this parser, NOT by getopt: the
    // extension namespace is stripped from argv before the AOSP parser ever sees
    // it, and this table holds AOSP options only. Empty means "print the dump".
    std::string section;

    // positionals, per dumpsys semantics
    std::vector<std::string> services;
    std::vector<std::string> dump_args;
    std::vector<std::string> skipped_services;

    // Advisory only; parsing still succeeds.
    std::string warning;
};

// `default_timeout_ms` comes from etools's own --timeout and applies unless the
// command line sets -t/-T, so dumpsys's real flag still wins when both appear.
//
// `sdk` selects which long options are accepted, because AOSP adds --pid in
// API 30, --thread in 31, and --clients/--dump/--stability in 33. Accepting one
// the running ROM does not know would be etools being more permissive than the
// real tool, which is a compatibility bug just as much as rejecting one it does.
bool parse_dumpsys(int argc, char** argv, DumpsysOptions* out, std::string* error,
                   ErrorStyle* style = nullptr, long default_timeout_ms = 10000,
                   int sdk = 0);

// The dumpsys --help text for an API level, verbatim from AOSP.
// Returns the API 33+ text for anything newer, and the API 26 text for
// anything older than we have data for.
const char* dumpsys_usage_text(int sdk);

// cmd's no-arguments message, which AOSP reworded in API 30.
const char* cmd_no_args_text(int sdk);

// Whether `cmd -w` exists on this API level. It arrived in API 30; before that
// (API 26-29) "-w" is an ordinary (nonexistent) service name.
bool cmd_has_wait_flag(int sdk);

// Whether dumpsys prints its "Currently running services:" header here.
// AOSP's condition is `N > 1`, widened to `N > 1 || -l` in API 31.
bool dumpsys_shows_service_list(int sdk, size_t service_count, bool list_only);

// Which of the three dump-failure message formats this API level prints:
// 0 = API 26-29 ("Error dumping service info: (strerror) name"),
// 1 = API 30-32 ("Error dumping service info status_t: statusToString name"),
// 2 = API 33+ ("Error with service 'name' while context: statusToString").
int dumpsys_error_style(int sdk);

// Whether --pid/--thread/... can be combined. From API 33 they are a bitmask;
// before that dumpsys holds one type and the last flag wins.
bool dumpsys_combines_dump_types(int sdk);

// cmd modes. cmd has no getopt at all: it is purely positional.
enum class CmdMode { kRun, kList, kWait };

// `sdk` gates `-w` and the no-arguments message, both of which changed in
// API 30. Pass 0 to read the running device's API level.
bool parse_cmd(int argc, char** argv, CmdMode* mode, std::string* service,
               std::vector<std::string>* args, std::string* error, int sdk = 0);

// Dumpsys::setServiceArgs(): arguments injected into the dump call. Runs ONLY
// when dumping every service; a single named service gets its own arguments
// verbatim.
std::vector<std::string> injected_dump_args(const DumpsysOptions& o, bool dumping_all);

}  // namespace etools
