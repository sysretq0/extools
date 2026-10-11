// SPDX-License-Identifier: GPL-3.0-only
#include "extflags.h"

#include <errno.h>
#include <limits.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace etools {
namespace {

constexpr const char* kExtPrefix = "--ext-";

// A number from a flag value, refusing everything strtol would silently turn
// into one. strtol("abc") is 0, and a zero timeout expires before the first
// poll(), so every dump would report a timeout; strtol("-1") is -1, which turns
// an interval into a tight loop hammering the driver. A typo is an error here,
// never a behaviour change nobody can see.
//
// Capped rather than merely range-checked: the value goes into milliseconds,
// which are int64 nanoseconds internally, so LONG_MAX overflows to a NEGATIVE
// duration, the deadline lands in the past, and every dump reports a timeout
// that never happened.
constexpr long kMaxTimeoutMs = 24 * 60 * 60 * 1000;

bool parse_count(const char* text, long* out, long max_value = LONG_MAX) {
    if (text == nullptr || *text == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || v < 0) return false;
    if (v > max_value) return false;
    *out = v;
    return true;
}

// The value of --ext-loop, given as --ext-loop=5, --ext-loop 5, or a bare
// --ext-loop meaning "until interrupted". The bare form consumes the next token
// only when it is numeric, so it can never swallow a service name.
bool loop_value(const std::vector<std::string>& argv, size_t* i, const char* inline_value,
                const char* tool, long* out) {
    *out = 0;
    if (inline_value != nullptr) return parse_count(inline_value, out);
    if (*i + 1 < argv.size() && !argv[*i + 1].empty() && argv[*i + 1][0] >= '0' &&
        argv[*i + 1][0] <= '9') {
        if (!parse_count(argv[*i + 1].c_str(), out)) {
            std::fprintf(stderr, "%s: --ext-loop needs a non-negative count, got '%s'\n", tool,
                         argv[*i + 1].c_str());
            return false;
        }
        ++(*i);
    }
    return true;
}

}  // namespace

bool parse_ext_flags(int argc, char** argv, ExtTool which, ExtFlags* out,
                     std::vector<std::string>* rest) {
    *out = ExtFlags{};
    rest->clear();
    const char* tool = which == ExtTool::kDumpsys ? "dumpsyse" : "cmde";

    std::vector<std::string> all;
    all.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) all.emplace_back(argv[i]);

    for (size_t i = 0; i < all.size(); ++i) {
        const std::string& a = all[i];
        if (a.rfind(kExtPrefix, 0) != 0) {
            rest->push_back(a);
            continue;
        }

        // --ext-name or --ext-name=value. The tool's own flags keep their own
        // spelling and are never inspected here.
        std::string name = a.substr(std::strlen(kExtPrefix));
        std::string owned;
        const char* value = nullptr;
        const size_t eq = name.find('=');
        if (eq != std::string::npos) {
            owned = name.substr(eq + 1);
            name = name.substr(0, eq);
            value = owned.c_str();
        }

        auto need_value = [&](const char* flag) -> bool {
            if (value != nullptr) return true;
            if (i + 1 < all.size()) {
                ++i;
                owned = all[i];
                value = owned.c_str();
                return true;
            }
            std::fprintf(stderr, "%s: option '%s%s' requires an argument\n", tool, kExtPrefix,
                         flag);
            return false;
        };
        auto reject_value = [&](const char* flag) -> bool {
            if (value == nullptr) return true;
            std::fprintf(stderr, "%s: option '%s%s' doesn't allow an argument\n", tool,
                         kExtPrefix, flag);
            return false;
        };
        auto wrong_tool = [&](bool belongs_to_dumpsys) -> bool {
            if (belongs_to_dumpsys == (which == ExtTool::kDumpsys)) return false;
            std::fprintf(stderr, "%s: '%s%s' is not an option of %s\n", tool, kExtPrefix,
                         name.c_str(), tool);
            return true;
        };

        if (name == "loop") {
            if (wrong_tool(true) || !reject_value("loop")) return false;
            out->loop = true;
            if (!loop_value(all, &i, value, tool, &out->loop_count)) return false;
        } else if (name == "interval") {
            if (wrong_tool(true) || !need_value("interval")) return false;
            if (!parse_count(value, &out->interval_ms)) {
                std::fprintf(stderr,
                             "%s: --ext-interval needs a non-negative number of milliseconds, "
                             "got '%s'\n",
                             tool, value);
                return false;
            }
        } else if (name == "timeout") {
            if (!need_value("timeout")) return false;
            // Zero is refused with everything else rather than accepted: it
            // expires before the first poll(), so every dump would report one.
            if (!parse_count(value, &out->timeout_ms, kMaxTimeoutMs) || out->timeout_ms == 0) {
                std::fprintf(stderr,
                             "%s: --ext-timeout needs a number of milliseconds in 1..%ld, "
                             "got '%s'\n",
                             tool, kMaxTimeoutMs, value);
                return false;
            }
        } else if (name == "section") {
            if (wrong_tool(true) || !need_value("section")) return false;
            out->section = value;
        } else if (name == "shape") {
            if (wrong_tool(false) || !need_value("shape")) return false;
            const std::string v = value;
            if (v == "null") {
                out->shape = 0;
            } else if (v == "receiver") {
                out->shape = 1;
            } else {
                std::fprintf(stderr, "%s: --ext-shape takes receiver or null, got '%s'\n", tool,
                             v.c_str());
                return false;
            }
        } else if (name == "help") {
            // Both binaries have extensions, so both binaries document them. The
            // tool's own --help is never touched: `cmde --help` must print
            // exactly what `cmd --help` prints, or a caller comparing the two
            // sees this project's name in the middle of another tool's
            // documentation.
            if (!reject_value("help")) return false;
            out->want_help = true;
        } else {
            std::fprintf(stderr, "%s: unrecognized option '%s%s'\n", tool, kExtPrefix,
                         name.c_str());
            std::fprintf(stderr, "%s: (this project's own options start with '%s'; "
                                 "try %s --ext-help)\n",
                         tool, kExtPrefix, tool);
            return false;
        }
    }
    return true;
}

}  // namespace etools
