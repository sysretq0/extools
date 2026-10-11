// SPDX-License-Identifier: GPL-3.0-only
#include <etools/args.hpp>

#include <etools/compat.hpp>
#include "device.h"

#include <getopt.h>

#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <climits>
#include <cstdio>

namespace etools {
namespace {

// DUMP_FLAG_PRIORITY_*, from binder/IServiceManager.h:
// CRITICAL=1<<0 HIGH=1<<1 NORMAL=1<<2 DEFAULT=1<<3, ALL=all four.
constexpr int kCritical = 1 << 0;
constexpr int kHigh     = 1 << 1;
constexpr int kNormal   = 1 << 2;
constexpr int kDefault  = 1 << 3;
constexpr int kAll      = kCritical | kHigh | kNormal | kDefault;

constexpr const char* kArgProto    = "--proto";
constexpr const char* kArgAll      = "-a";
constexpr const char* kArgPriority = "--priority";

// Last variant whose min_api is <= sdk. An sdk below the oldest entry falls back
// to the oldest, which is what device_sdk()'s 0 ("unknown") resolves to.
//
// The oldest is the safe direction because the failure modes are asymmetric.
// Serving a new release's --help or accepted-option set to an old ROM advertises
// flags that ROM rejects, which is the one direction NOTES.md 5b calls the more
// dangerous kind of wrong. Serving the oldest set to a new ROM can only reject
// something that would otherwise have worked.
const compat::DumpsysVariant* variant_for(int sdk) {
    const compat::DumpsysVariant* v = &compat::kDumpsysVariants[0];
    for (int i = 0; i < compat::kDumpsysVariantCount; ++i) {
        if (compat::kDumpsysVariants[i].min_api <= sdk) v = &compat::kDumpsysVariants[i];
    }
    return v;
}

const compat::CmdVariant* cmd_variant_for(int sdk) {
    const compat::CmdVariant* c = &compat::kCmdVariants[0];
    for (int i = 0; i < compat::kCmdVariantCount; ++i) {
        if (compat::kCmdVariants[i].min_api <= sdk) c = &compat::kCmdVariants[i];
    }
    return c;
}

// Flags that change CLIENT behaviour. After the service name dumpsys forwards them
// to the service instead of parsing, so they silently do nothing.
//
// Reads the very table that was handed to getopt_long, rather than re-deriving the
// accepted names from the generated variant. One table, one source of truth.
// `optstring` is this API level's own optstring, because "is this flag ours?"
// is a per-release question: -w is a client flag on API 37 and a service
// argument on API 34, where the real dumpsys has no such flag and forwards it.
// Hardcoding the list per release is what makes that distinction wrong.
bool is_client_side_flag(const struct option* table, const std::string& a,
                         const char* optstring) {
    if (a.size() == 2 && a[0] == '-' && std::strchr(optstring, a[1]) != nullptr &&
        a[1] != '-') {
        return true;
    }
    if (a.rfind("--", 0) != 0) return false;
    std::string name = a.substr(2);
    const size_t eq = name.find('=');
    if (eq != std::string::npos) name = name.substr(0, eq);
    for (const struct option* o = table; o->name != nullptr; ++o) {
        if (name == o->name) return true;
    }
    return false;
}

// AOSP ORs the dump-type flags into a bitmask from API 33, but before that it
// holds a single `Type type` and ASSIGNS - so on API 30-32 the LAST flag wins and
// combining is impossible. getopt delivers them in command-line order, so plain
// assignment reproduces that exactly.
void set_dump_type(const compat::DumpsysVariant* v, DumpsysOptions* out, int bit) {
    out->dump_type_flags = v->combines ? (out->dump_type_flags | bit) : bit;
}

}  // namespace

bool parse_dumpsys(int argc, char** argv, DumpsysOptions* out, std::string* error,
                   ErrorStyle* style, long default_timeout_ms, int sdk) {
    *out = DumpsysOptions{};
    out->timeout_ms = default_timeout_ms;
    if (style) *style = ErrorStyle::kMessageOnly;
    if (sdk == 0) sdk = device_sdk();

    // AOSP's table for THIS API level, built from the generated variant list.
    // The set genuinely differs by release: --pid arrives in API 30, --thread in
    // 31, and --clients/--dump/--stability in 33.
    //
    // No extension of our own lives in this table. The drop-in binaries expose
    // theirs under the reserved --ext- namespace (parsed before this point and
    // removed from argv), so a non-AOSP option never sits in a table generated
    // from AOSP data, and a ROM that one day grows its own --section cannot be
    // shadowed by ours.
    const compat::DumpsysVariant* variant = variant_for(sdk);
    struct option long_options[compat::kDumpsysMaxOptions + 1];
    int n = 0;
    for (int i = 0; i < compat::kDumpsysMaxOptions && variant->options[i][0] != '\0'; ++i) {
        const char* name = variant->options[i];
        const bool needs_arg = std::strcmp(name, "priority") == 0;
        long_options[n].name = name;
        long_options[n].has_arg = needs_arg ? required_argument : no_argument;
        long_options[n].flag = nullptr;
        long_options[n].val = 0;
        ++n;
    }
    std::memset(&long_options[n], 0, sizeof(long_options[n]));

    std::vector<char*> av;
    av.reserve(argc + 1);
    for (int i = 0; i < argc; ++i) av.push_back(argv[i]);
    av.push_back(nullptr);

    // glibc getopt caches argument ordering that optind=1 does not clear, so
    // re-parsing a second command line in one process needs optind=0.
    optind = 0;
    // opterr OFF on purpose. bionic's getopt prefixes its messages with the
    // PROCESS name, so under the real tool it prints "dumpsys: unrecognized option"
    // where real dumpsys prints "dumpsys:". We format the message ourselves.
    opterr = 0;

    for (;;) {
        int option_index = 0;
        // AOSP's own optstring, from the variant: API 26/27 are "+t:l" and reject
        // -T with "dumpsys: invalid option -- T". Accepting it there would make
        // etools more permissive than the tool it replaces.
        const int c = getopt_long(argc, av.data(), variant->short_options, long_options,
                                  &option_index);
        if (c == -1) break;

        switch (c) {
        case 0: {
            const char* name = long_options[option_index].name;
            if (!std::strcmp(name, "help"))       { out->want_help = true; }
            else if (!std::strcmp(name, "proto")) { out->as_proto = true; }
            else if (!std::strcmp(name, "skip"))  { out->skip_mode = true; }
            else if (!std::strcmp(name, "priority")) {
                out->priority_type = optarg;
                if (out->priority_type != "CRITICAL" && out->priority_type != "HIGH" &&
                    out->priority_type != "NORMAL") {
                    // dumpsys prints no message here, just a blank line + usage.
                    error->clear();
                    if (style) *style = ErrorStyle::kBlankUsage;
                    return false;
                }
            } else if (!std::strcmp(name, "dump")) {
                set_dump_type(variant, out, kTypeDump);
            } else if (!std::strcmp(name, "pid")) {
                set_dump_type(variant, out, kTypePid);
            } else if (!std::strcmp(name, "stability")) {
                set_dump_type(variant, out, kTypeStability);
            } else if (!std::strcmp(name, "thread")) {
                set_dump_type(variant, out, kTypeThread);
            } else if (!std::strcmp(name, "clients")) {
                set_dump_type(variant, out, kTypeClients);
            }
            break;
        }
        case 't': {   // seconds, per dumpsys.cpp
            char* end = nullptr;
            const long v = strtol(optarg, &end, 10) * 1000;
            if (*end != '\0' || v <= 0) {
                if (error) {
                    *error = std::string("Error: invalid timeout(seconds) number: '") + optarg + "'";
                }
                return false;   // message only, no usage
            }
            out->timeout_ms = v;
            break;
        }
        case 'T': {   // milliseconds, per dumpsys.cpp
            char* end = nullptr;
            const long v = strtol(optarg, &end, 10);
            if (*end != '\0' || v <= 0) {
                if (error) {
                    *error =
                        std::string("Error: invalid timeout(milliseconds) number: '") + optarg + "'";
                }
                return false;   // message only, no usage
            }
            out->timeout_ms = v;
            break;
        }
        case 'w':
            // API 37+. Reachability of the case depends on the optstring above,
            // which is why no other release can produce it.
            out->wait_for_service = true;
            break;

        case 'l':
            out->list_only = true;
            break;

        case '?':
        case ':': {
            // bionic renders long options in backticks: `dumpsys: unrecognized
            // option `--bogus''
            const char* shown = (optind >= 1 && optind <= argc) ? argv[optind - 1] : nullptr;
            const std::string opt = shown ? shown : std::string();
            if (opt.rfind("--", 0) == 0) {
                // Three distinct bionic messages, all arriving as '?':
                //
                //   --bogus       unrecognized option `--bogus'
                //   --priority    option `--priority' requires an argument
                //   --proto=x     option `--proto' doesn't allow an argument
                //
                // Verified against /system/bin/dumpsys on device. The third is
                // only reachable when the name is KNOWN and carries '=': that
                // is the shape of the error, so '=' in a long option means
                // "argument given to an option that takes none".
                std::string name = opt.substr(2);
                const size_t eq = name.find('=');
                const bool has_eq = eq != std::string::npos;
                if (has_eq) name = name.substr(0, eq);
                bool needs_arg = false;
                bool known = false;
                for (struct option* o = long_options; o->name != nullptr; ++o) {
                    if (name == o->name) {
                        needs_arg = o->has_arg == required_argument;
                        known = true;
                        break;
                    }
                }
                if (error) {
                    if (known && has_eq && !needs_arg) {
                        // bionic names the option, not the whole argument, and
                        // drops the '=value' part.
                        *error = "dumpsys: option `--" + name + "' doesn't allow an argument";
                    } else if (needs_arg) {
                        *error = "dumpsys: option `" + opt + "' requires an argument";
                    } else {
                        *error = "dumpsys: unrecognized option `" + opt + "'";
                    }
                }
            } else {
                // A short option. bionic reports TWO different errors here and
                // both arrive as '?' with optopt set to the offending character,
                // so optopt alone cannot tell them apart. Measured on device:
                //
                //   -t   ret='?' optopt='t'   bionic prints
                //                              "option requires an argument -- t"
                //   -q   ret='?' optopt='q'   bionic prints
                //                              "invalid option -- q"
                //
                // What distinguishes them is whether the character is one of
                // ours: a KNOWN short option can only be missing its argument,
                // an UNKNOWN one can only be invalid. So test membership in
                // the optstring rather than guessing from optopt.
                //
                // Note bionic prints NO quotes around the character, unlike the
                // backtick-quoted long-option form above. Reproduced verbatim.
                const char c = optopt ? static_cast<char>(optopt)
                                      : (shown && shown[0] == '-' && shown[1] != '\0'
                                             ? shown[1] : '?');
                const bool known = c != '?' && std::strchr(variant->short_options, c) != nullptr;
                if (error) {
                    *error = known
                        ? std::string("dumpsys: option requires an argument -- ") + c
                        : std::string("dumpsys: invalid option -- ") + c;
                }
            }
            if (style) *style = ErrorStyle::kMessageBlankUsage;
            return false;
        }
        default:
            if (error) *error = "unknown option";
            if (style) *style = ErrorStyle::kMessageBlankUsage;
            return false;
        }
    }

    for (int i = optind; i < argc; ++i) {
        if (out->skip_mode) {
            // AOSP documents --skip as taking a "comma-separated list", but its
            // IsSkipped() compares each argv element verbatim and never splits.
            // So `dumpsys --skip activity,window` skips NOTHING, while
            // `dumpsys --skip activity window` skips both. Verified against
            // /system/bin/dumpsys on device. Reproduced as-is: being right about
            // the documentation here would make etools skip services the real tool
            // dumps, which is exactly the kind of helpful divergence that makes
            // a drop-in replacement useless.
            out->skipped_services.push_back(argv[i]);
        } else if (i == optind) {
            out->services.push_back(argv[i]);
        } else {
            out->dump_args.push_back(argv[i]);
            // dumpsys backward compatibility: a bare --proto among the service
            // arguments also implies --proto, because "+" stopped getopt.
            // AOSP added this shim in API 30; before that (API 26-29) --proto
            // here is just a dump argument and as_proto stays false.
            if (!out->as_proto && variant->proto_shim &&
                out->dump_args.back() == kArgProto) {
                out->as_proto = true;
            }
        }
    }

    // dumpsys's own validation, so the same mistakes are caught the same way.
    // -w with no service named is in the same condition upstream - it would
    // otherwise wait for every service in turn, which is not what it says - and
    // draws the same response: usage, and no message.
    if (out->wait_for_service && out->services.empty()) {
        if (error) error->clear();
        if (style) *style = ErrorStyle::kUsageOnly;
        return false;
    }
    if (out->skip_mode && out->skipped_services.empty()) {
        if (error) error->clear();
        if (style) *style = ErrorStyle::kUsageOnly;
        return false;
    }
    if (out->list_only && (!out->services.empty() || !out->skipped_services.empty())) {
        if (error) error->clear();
        if (style) *style = ErrorStyle::kUsageOnly;
        return false;
    }

    for (const std::string& a : out->dump_args) {
        if (is_client_side_flag(long_options, a, variant->short_options)) {
            out->warning =
                "'" + a +
                "' comes after the service name, so dumpsys forwards it to the "
                "service as a dump argument instead of parsing it. "
                "Put dumpsys flags before the service name.";
            break;
        }
    }
    return true;
}

const char* dumpsys_usage_text(int sdk) {
    return variant_for(sdk)->usage;
}

const char* cmd_no_args_text(int sdk) {
    return cmd_variant_for(sdk)->no_args;
}

// AOSP added `cmd -w` in API 30. Before that (API 26-29) there is no such flag,
// so "-w" is the SERVICE NAME and must be reported as an unknown service.
bool cmd_has_wait_flag(int sdk) {
    return cmd_variant_for(sdk)->wait_flag;
}

// Whether dumpsys prints the "Currently running services:" header for this API
// level and this invocation. AOSP's condition is `N > 1`, widened to
// `N > 1 || -l` in API 31.
bool dumpsys_shows_service_list(int sdk, size_t service_count, bool list_only) {
    return service_count > 1 || (variant_for(sdk)->list_header_or_list && list_only);
}

// Which of the three dump-failure message formats this API level prints.
// 0 = API 26-29 ("(strerror)" and no status name), 1 = API 30-32, 2 = API 33+.
int dumpsys_error_style(int sdk) {
    return variant_for(sdk)->error_style;
}

// Whether this API level can combine dump types, or only one may be given.
bool dumpsys_combines_dump_types(int sdk) {
    return variant_for(sdk)->combines;
}

std::vector<std::string> injected_dump_args(const DumpsysOptions& o, bool dumping_all) {
    // Dumpsys::setServiceArgs() is called only when dumping everything.
    if (!dumping_all) return o.dump_args;

    std::vector<std::string> args = o.dump_args;
    int f;
    if (o.priority_type == "CRITICAL")   f = kCritical;
    else if (o.priority_type == "HIGH")   f = kHigh;
    else if (o.priority_type == "NORMAL") f = kNormal;
    else                                  f = kAll;   // unset means ALL

    if (f == kAll || f == kNormal || f == kDefault) {
        args.insert(args.begin(), kArgAll);
    }
    if (f == kCritical || f == kHigh || f == kNormal) {
        args.insert(args.begin(), o.priority_type);
        args.insert(args.begin(), kArgPriority);
    }
    if (o.as_proto) args.insert(args.begin(), kArgProto);
    return args;
}

bool parse_cmd(int argc, char** argv, CmdMode* mode, std::string* service,
               std::vector<std::string>* args, std::string* error, int sdk) {
    if (sdk == 0) sdk = device_sdk();
    *mode = CmdMode::kRun;
    if (argc == 1 && argv[0] != nullptr && !std::strcmp(argv[0], "-l")) {
        *mode = CmdMode::kList;
        return true;
    }
    int idx = 0;
    // -w arrived in API 30. Before that "-w" is a perfectly ordinary service
    // name, and dumpsys-equivalent behaviour is to try to find it and fail.
    if (cmd_has_wait_flag(sdk) && argc > 1 && argv[0] != nullptr &&
        !std::strcmp(argv[0], "-w")) {
        *mode = CmdMode::kWait;
        idx = 1;
    }
    if (idx >= argc) {
        if (error) *error = cmd_no_args_text(sdk);
        return false;
    }
    *service = argv[idx];
    for (int i = idx + 1; i < argc; ++i) args->push_back(argv[i]);
    return true;
}

}  // namespace etools
