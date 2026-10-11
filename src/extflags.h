// SPDX-License-Identifier: GPL-3.0-only
// The reserved --ext- flag namespace, and nothing else.
//
// No binder dependency on purpose: this is the piece that decides what belongs
// to this project and what belongs to the tool it extends, so it is the piece a
// host test can reach. The runners that use it cannot be built off-device.
#ifndef ETOOLS_EXTFLAGS_H
#define ETOOLS_EXTFLAGS_H

#include <string>
#include <vector>

namespace etools {

// Extensions the binaries understand. Parsed out of argv before the AOSP
// parser sees it, and removed, so everything that is left is byte-identical to
// what the real tool would have been handed.
//
// THE NAMESPACE EXISTS BECAUSE THERE IS NO SUBCOMMAND. A tool invoked as
// `dumpsyse` IS dumpsys: every token belongs to dumpsys or to the service, and
// there is no position where "our" flags could be inserted without either
// stealing the tool's own spelling or being swallowed as a dump argument. A
// reserved --ext- prefix is the one placement that cannot collide: no AOSP flag
// or service argument starts with it, so an intercept is unambiguous, and an
// unknown --ext- name is a typo to report rather than something to forward.
struct ExtFlags {
    bool loop = false;
    long loop_count = 0;        // 0 == until interrupted
    long interval_ms = 1000;
    long timeout_ms = 10000;    // default for a dump; -t/-T still override it
    std::string section;

    // -1 == not given, which means the receiver shape. 0 == null shape, 1 ==
    // receiver shape. Only cmde acts on it.
    int shape = -1;

    bool want_help = false;
};

// Which binary is parsing. Each rejects the other's extensions instead of
// forwarding them to a service that has never heard of them.
enum class ExtTool { kDumpsys, kCmd };

// Removes the --ext- flags from argv, filling `out`. `rest` receives argv with
// the extension flags (and their values) removed, in their original order.
// Returns false after printing the reason for a malformed flag.
bool parse_ext_flags(int argc, char** argv, ExtTool which, ExtFlags* out,
                     std::vector<std::string>* rest);

}  // namespace etools

#endif  // ETOOLS_EXTFLAGS_H
