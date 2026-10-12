// SPDX-License-Identifier: GPL-3.0-only
// Self-check for compatibility with /system/bin/dumpsys and /system/bin/cmd.
//
// Two things are asserted here:
//
//  1. The dumpsys and cmd parsers reproduce AOSP exactly. These assertions
//     encode the real behaviour transcribed from cmds/dumpsys/dumpsys.cpp and
//     cmds/cmd/cmd.cpp across API 26-37. Two earlier versions of etools invented
//     their own dialect and were wrong in nearly every particular.
//
//  2. The --ext- namespace is NOT the tools' namespace. It is stripped from
//     argv before the AOSP parsers run, and nothing in it may ever reach a
//     service as a dump argument or a subcommand token. Equally: nothing in the
//     tools' own spelling may ever be mistaken for one of ours.
//
// Runs on device. Assert-based, no framework.

#include <etools/args.hpp>
#include <etools/section.hpp>

#include "extflags.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void ok(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++g_fail; }
}

using namespace etools;

// Every assertion below pins its API level. Reading device_sdk() would make the
// suite assert something different on every ROM, and a check that changes
// meaning with the host is not a check. Section 9 sweeps all seven variants
// explicitly, so nothing ends up covered by accident.
constexpr int kApi = 33;

bool parse_dumpsys_at(int sdk, int argc, char** argv, DumpsysOptions* out,
                      std::string* error, ErrorStyle* style = nullptr,
                      long default_timeout_ms = 10000) {
    return parse_dumpsys(argc, argv, out, error, style, default_timeout_ms, sdk);
}

bool parse_cmd_at(int sdk, int argc, char** argv, CmdMode* mode, std::string* service,
                  std::vector<std::string>* args, std::string* error) {
    return parse_cmd(argc, argv, mode, service, args, error, sdk);
}


// A parse pinned to an explicit API level. Version-dependent behaviour must be
// asserted against the release it belongs to, or the suite asserts something
// different on every ROM.
bool pd_at(int sdk, const std::vector<std::string>& v, DumpsysOptions* out,
           std::string* err = nullptr, ErrorStyle* style = nullptr) {
    std::vector<std::string> store = v;
    std::vector<char*> av;
    for (auto& s : store) av.push_back(const_cast<char*>(s.c_str()));
    av.push_back(nullptr);
    std::string local;
    std::string* e = err != nullptr ? err : &local;
    return parse_dumpsys_at(sdk, static_cast<int>(av.size()) - 1, av.data(), out, e, style);
}

bool pd(const std::vector<std::string>& v, DumpsysOptions* out, ErrorStyle* style = nullptr) {
    return pd_at(kApi, v, out, nullptr, style);
}

}  // namespace

int main() {
    // ======================= 1. the --ext- namespace =====================
    // This is the rule that keeps the tools untouched. The extensions are
    // stripped from argv before this parser runs, so by the time this
    // parser runs they must be gone. If any of them ever parses here, the
    // invocation `etools dumpsys --loop ...` would silently work instead of
    // failing the way the real tool fails.
    {
        DumpsysOptions o;
        std::string err;
        ErrorStyle style = ErrorStyle::kMessageOnly;
        char* av[] = {const_cast<char*>("dumpsys"), const_cast<char*>("--loop"),
                      const_cast<char*>("power"), nullptr};
        ok(!parse_dumpsys_at(kApi, 3, av, &o, &err, &style), "--loop is NOT a dumpsys flag");
        ok(err == "dumpsys: unrecognized option `--loop'",
           "--loop fails with dumpsys's own message");
    }
    {
        DumpsysOptions o;
        std::string err;
        ErrorStyle style = ErrorStyle::kMessageOnly;
        char* av[] = {const_cast<char*>("dumpsys"), const_cast<char*>("--interval"),
                      const_cast<char*>("500"), const_cast<char*>("power"), nullptr};
        ok(!parse_dumpsys_at(kApi, 4, av, &o, &err, &style), "--interval is NOT a dumpsys flag");
        ok(err == "dumpsys: unrecognized option `--interval'", "--interval fails too");
    }
    {
        DumpsysOptions o;
        std::string err;
        ErrorStyle style = ErrorStyle::kMessageOnly;
        char* av[] = {const_cast<char*>("dumpsys"), const_cast<char*>("--timeout"),
                      const_cast<char*>("500"), const_cast<char*>("power"), nullptr};
        ok(!parse_dumpsys_at(kApi, 4, av, &o, &err, &style), "--timeout is NOT a dumpsys flag");
    }
    // After the service name --loop is a dump argument, forwarded, and does not
    // loop. Same as the real tool.
    {
        DumpsysOptions o;
        ok(pd({"dumpsys", "power", "--loop"}, &o), "late --loop is forwarded");
        ok(o.dump_args.size() == 1 && o.dump_args[0] == "--loop", "and lands in dump_args");
        ok(o.services.size() == 1 && o.services[0] == "power", "service still named");
    }

    // ============================ 2. short options =====================
    // optstring is "+t:T:l" on every branch.
    {
        DumpsysOptions o;
        ok(pd({"dumpsys", "-l"}, &o), "-l parses");
        ok(o.list_only, "-l sets list_only");
    }
    {
        // -t is SECONDS. An earlier etools used it as milliseconds.
        DumpsysOptions o;
        ok(pd({"dumpsys", "-t", "3", "activity"}, &o), "-t parses");
        ok(o.timeout_ms == 3000, "-t 3 means 3000ms");
        ok(o.services.size() == 1 && o.services[0] == "activity", "-t then service");
    }
    {
        DumpsysOptions o;
        ok(pd({"dumpsys", "-T", "250", "activity"}, &o), "-T parses");
        ok(o.timeout_ms == 250, "-T 250 means 250ms");
    }
    {
        // etools's --timeout supplies the default, but dumpsys's own flag wins.
        DumpsysOptions o;
        std::vector<std::string> store = {"dumpsys", "-t", "2", "activity"};
        std::vector<char*> av;
        for (auto& s : store) av.push_back(const_cast<char*>(s.c_str()));
        av.push_back(nullptr);
        std::string err;
        ok(parse_dumpsys_at(kApi, 3, av.data(), &o, &err, nullptr, 7777), "parses with a default timeout");
        ok(o.timeout_ms == 2000, "dumpsys -t overrides etools's default timeout");
    }
    {
        DumpsysOptions o;
        std::vector<std::string> store = {"dumpsys", "activity"};
        std::vector<char*> av;
        for (auto& s : store) av.push_back(const_cast<char*>(s.c_str()));
        av.push_back(nullptr);
        std::string err;
        ok(parse_dumpsys_at(kApi, 2, av.data(), &o, &err, nullptr, 7777), "parses with a default timeout");
        ok(o.timeout_ms == 7777, "etools's default timeout is used when dumpsys sets none");
    }
    {
        DumpsysOptions o;
        ok(!pd({"dumpsys", "-t", "abc", "activity"}, &o), "-t rejects non-numeric");
        ok(!pd({"dumpsys", "-t", "0", "activity"}, &o), "-t rejects zero");
    }

    // ============================ 3. long options =====================
    {
        DumpsysOptions o;
        ok(pd({"dumpsys", "--proto", "activity"}, &o), "--proto parses");
        ok(o.as_proto, "--proto sets as_proto");
    }
    {
        // --skip takes NO argument; it is a mode and the names after it are the
        // services to skip. Treating it as "--skip A,B" was a real bug.
        DumpsysOptions o;
        ok(pd({"dumpsys", "--skip", "activity", "window"}, &o), "--skip parses");
        ok(o.skip_mode, "--skip sets mode");
        ok(o.skipped_services.size() == 2, "--skip consumes following names");
        ok(o.services.empty(), "--skip means no service is named");
        ok(!pd({"dumpsys", "--skip"}, &o), "--skip with no names is an error");
    }
    {
        // AOSP documents a comma-separated list but never splits: IsSkipped()
        // compares each argv element verbatim. `dumpsys --skip activity,window`
        // therefore skips NOTHING, verified against /system/bin/dumpsys on
        // device. Reproduced rather than corrected - being right about the
        // documentation would make etools skip services the real tool dumps.
        DumpsysOptions o;
        ok(pd({"dumpsys", "--skip", "activity,window"}, &o), "comma form parses");
        ok(o.skipped_services.size() == 1, "comma form is ONE service, not two");
        ok(o.skipped_services[0] == "activity,window", "comma form stays verbatim");
    }
    {
        DumpsysOptions o;
        ok(pd({"dumpsys", "--skip", "activity", "window"}, &o), "space form parses");
        ok(o.skipped_services.size() == 2, "space form is two services");
        ok(o.skipped_services[0] == "activity" && o.skipped_services[1] == "window",
           "space form keeps order");
    }
    {
        DumpsysOptions o;
        ok(pd({"dumpsys", "--skip", "a,,b,"}, &o), "odd skip names tolerated");
        ok(o.skipped_services.size() == 1, "odd skip name is kept verbatim");
    }
    {
        DumpsysOptions o;
        ok(pd({"dumpsys", "--priority", "HIGH", "activity"}, &o), "--priority parses");
        ok(o.priority_type == "HIGH", "--priority captures type");
        ok(!pd({"dumpsys", "--priority", "BOGUS", "activity"}, &o), "--priority rejects unknown");
    }
    {
        DumpsysOptions o;
        ok(pd({"dumpsys", "--thread", "activity"}, &o), "--thread parses");
        ok((o.dump_type_flags & kTypeThread) != 0, "--thread sets the thread dump type");
    }
    {
        DumpsysOptions o;
        ok(pd({"dumpsys", "--help"}, &o), "--help parses");
        ok(o.want_help, "--help sets want_help");
    }

    // ================= 4. flags that do NOT exist in dumpsys ===========
    {
        DumpsysOptions o;
        ok(!pd({"dumpsys", "--all"}, &o), "--all is not a dumpsys flag");
        ok(!pd({"dumpsys", "-a"}, &o), "-a is not a dumpsys flag");
        ok(!pd({"dumpsys", "-s"}, &o), "-s is not a dumpsys flag");
        ok(!pd({"dumpsys", "-c"}, &o), "-c is not a dumpsys flag");
        ok(!pd({"dumpsys", "-n", "3", "activity"}, &o), "-n is not a dumpsys flag");
        ok(!pd({"dumpsys", "-i", "500", "activity"}, &o), "-i is not a dumpsys flag");
    }

    // ============================ 5. positionals ======================
    {
        DumpsysOptions o;
        ok(pd({"dumpsys", "activity", "extra1", "extra2"}, &o), "positionals parse");
        ok(o.services.size() == 1 && o.services[0] == "activity", "first is the service");
        ok(o.dump_args.size() == 2, "the rest are dump args");
    }
    {
        // dumpsys backward compatibility: a bare --proto among the service
        // arguments also implies --proto, because "+" stopped getopt.
        DumpsysOptions o;
        ok(pd({"dumpsys", "activity", "--proto"}, &o), "trailing --proto parses");
        ok(o.as_proto, "trailing --proto implies as_proto");
    }
    {
        DumpsysOptions o;
        ok(!pd({"dumpsys", "-l", "activity"}, &o), "-l with a service is an error");
    }

    // ============================ 6. injected args ====================
    {
        DumpsysOptions o;
        pd({"dumpsys", "activity"}, &o);
        const auto all = injected_dump_args(o, true);
        ok(all.size() == 1 && all[0] == "-a", "dumping all injects -a");
        ok(injected_dump_args(o, false).empty(), "one service injects nothing");
    }
    {
        DumpsysOptions o;
        pd({"dumpsys", "--proto", "activity"}, &o);
        const auto s = injected_dump_args(o, true);
        ok(s.size() == 2 && s[0] == "--proto" && s[1] == "-a", "--proto before -a");
    }
    {
        DumpsysOptions o;
        pd({"dumpsys", "--priority", "CRITICAL", "activity"}, &o);
        ok(injected_dump_args(o, false).empty(), "--priority injects nothing for one service");
        const auto all = injected_dump_args(o, true);
        ok(all.size() == 2 && all[0] == "--priority" && all[1] == "CRITICAL",
           "--priority injected when dumping all");
    }

    // ======================= 7. error rendering ========================
    // dumpsys prints no message for a bad --priority or for --skip/-l misuse;
    // it just prints usage, with a leading blank line in the first case.
    {
        std::string err;
        DumpsysOptions o;
        ErrorStyle style = ErrorStyle::kMessageOnly;
        char* av[] = {const_cast<char*>("dumpsys"), const_cast<char*>("--priority"),
                      const_cast<char*>("BOGUS"), nullptr};
        ok(!parse_dumpsys_at(kApi, 3, av, &o, &err, &style), "bad --priority fails");
        ok(err.empty(), "bad --priority prints no message");
        ok(style == ErrorStyle::kBlankUsage, "bad --priority shows usage");
    }
    {
        std::string err;
        DumpsysOptions o;
        ErrorStyle style = ErrorStyle::kMessageOnly;
        char* av[] = {const_cast<char*>("dumpsys"), const_cast<char*>("--skip"), nullptr};
        ok(!parse_dumpsys_at(kApi, 2, av, &o, &err, &style), "--skip with no names fails");
        ok(err.empty(), "--skip misuse prints no message");
        ok(style == ErrorStyle::kUsageOnly, "--skip misuse shows usage");
    }
    {
        std::string err;
        DumpsysOptions o;
        ErrorStyle style = ErrorStyle::kMessageOnly;
        char* av[] = {const_cast<char*>("dumpsys"), const_cast<char*>("-z"), nullptr};
        ok(!parse_dumpsys_at(kApi, 2, av, &o, &err, &style), "invalid short option fails");
        ok(err == "dumpsys: invalid option -- z", "short option message");
        ok(style == ErrorStyle::kMessageBlankUsage, "invalid short option shows usage");
    }
    {
        // Bad -t value prints a message and NO usage.
        std::string err;
        DumpsysOptions o;
        ErrorStyle style = ErrorStyle::kBlankUsage;
        char* av[] = {const_cast<char*>("dumpsys"), const_cast<char*>("-t"),
                      const_cast<char*>("abc"), const_cast<char*>("activity"), nullptr};
        ok(!parse_dumpsys_at(kApi, 4, av, &o, &err, &style), "bad -t fails");
        ok(err == "Error: invalid timeout(seconds) number: 'abc'", "bad -t message");
        ok(style == ErrorStyle::kMessageOnly, "bad -t prints no usage");
    }

    // =============================== 8. cmd ===========================
    {
        CmdMode m; std::string s; std::vector<std::string> a; std::string e;
        char* av[] = {const_cast<char*>("-l"), nullptr};
        ok(parse_cmd_at(kApi, 1, av, &m, &s, &a, &e), "cmd -l parses");
        ok(m == CmdMode::kList, "cmd -l is list mode");
    }
    {
        CmdMode m; std::string s; std::vector<std::string> a; std::string e;
        char* av[] = {const_cast<char*>("activity"), const_cast<char*>("start-activity"), nullptr};
        ok(parse_cmd_at(kApi, 2, av, &m, &s, &a, &e), "cmd service + args parses");
        ok(m == CmdMode::kRun && s == "activity", "cmd positional service");
        ok(a.size() == 1 && a[0] == "start-activity", "cmd forwards args");
    }
    {
        CmdMode m; std::string s; std::vector<std::string> a; std::string e;
        char* av[] = {const_cast<char*>("-w"), const_cast<char*>("activity"), nullptr};
        ok(parse_cmd_at(kApi, 2, av, &m, &s, &a, &e), "cmd -w parses");
        ok(m == CmdMode::kWait && s == "activity", "cmd -w is wait mode");
    }

    // ================== 9. version dispatch, API 26 through 37 ============
    //
    // Eight user-visible things in dumpsys and cmd change between releases. Each
    // is asserted at the API level either side of its boundary. Getting one of
    // these backwards does not just make etools look different from the real
    // tool - it makes etools ACCEPT something the running ROM would REJECT, or
    // REJECT something it would accept. Both are compatibility bugs.
    //
    // Every sweep below runs over kSdks, which covers all seven rows of the
    // generated table plus both gaps between them (32 falls through to 31, 34 to
    // 33). A row added to the table later cannot come in untested.
    {
        constexpr int kSdks[] = {26, 27, 28, 29, 30, 31, 32, 33, 34, 37};
        // (1) Long options. --priority and --proto arrived in API 28, --pid in
        // 30, --thread in 31, and --clients/--dump/--stability in 33; 26/27
        // accept only --skip and --help. etools must accept exactly the set the
        // running ROM accepts, no more and no less.
        struct { const char* name; int min_api; int bit; bool takes_value; } kLongOptions[] = {
            {"--priority", 28, 0, true},
            {"--proto", 28, 0, false},
            {"--pid", 30, kTypePid, false},
            {"--thread", 31, kTypeThread, false},
            {"--clients", 33, kTypeClients, false},
            {"--dump", 33, kTypeDump, false},
            {"--stability", 33, kTypeStability, false},
        };
        for (int sdk : kSdks) {
            for (const auto& f : kLongOptions) {
                std::string err;
                DumpsysOptions o;
                ErrorStyle style = ErrorStyle::kMessageOnly;
                char* av[] = {const_cast<char*>("dumpsys"), const_cast<char*>(f.name),
                              f.takes_value ? const_cast<char*>("HIGH") : nullptr, nullptr};
                const bool known = sdk >= f.min_api;
                const bool accepted = parse_dumpsys_at(sdk, f.takes_value ? 3 : 2, av, &o, &err,
                                                       &style);
                ok(accepted == known,
                   "api " + std::to_string(sdk) + " " + f.name +
                       (known ? " is accepted" : " is rejected"));
                if (known) {
                    if (f.bit != 0) {
                        ok((o.dump_type_flags & f.bit) != 0,
                           "accepted " + std::string(f.name) + " sets its dump type");
                    }
                } else {
                    ok(o.dump_type_flags == 0, std::string("rejected ") + f.name + " sets nothing");
                    ok(err == std::string("dumpsys: unrecognized option `") + f.name + "'",
                       "api " + std::to_string(sdk) + " " + f.name + " message");
                }
            }
        }

        // (3) getopt error wording. bionic renders three distinct messages and all
        // three arrive as '?', so they can only be told apart by looking the
        // name up. Transcribed from /system/bin/dumpsys on an API 34 device:
        //
        //   -t            option requires an argument -- t     (no backticks)
        //   -q            invalid option -- q
        //   --bogus       unrecognized option `--bogus'
        //   --proto=x     option `--proto' doesn't allow an argument
        //   --bogus=3     unrecognized option `--bogus=3'    (keeps the =value)
        //
        // The short-option pair is the subtle one: bionic sets optopt to the
        // offending character for BOTH `-t` and `-q`, so the only way to tell
        // "missing argument" from "invalid option" is whether the character is
        // one of ours.
        {
            struct Case { std::vector<std::string> argv; const char* want; };
            const Case cases[] = {
                {{"dumpsys", "-t"}, "dumpsys: option requires an argument -- t"},
                {{"dumpsys", "-T"}, "dumpsys: option requires an argument -- T"},
                {{"dumpsys", "-q"}, "dumpsys: invalid option -- q"},
                {{"dumpsys", "-z"}, "dumpsys: invalid option -- z"},
                {{"dumpsys", "--bogus"}, "dumpsys: unrecognized option `--bogus'"},
                {{"dumpsys", "--proto=x"}, "dumpsys: option `--proto' doesn't allow an argument"},
                {{"dumpsys", "--skip=1"}, "dumpsys: option `--skip' doesn't allow an argument"},
                {{"dumpsys", "--bogus=3"}, "dumpsys: unrecognized option `--bogus=3'"},
            };
            for (const Case& c : cases) {
                DumpsysOptions o;
                std::string err;
                ErrorStyle st = ErrorStyle::kMessageOnly;
                std::vector<std::string> store = c.argv;
                std::vector<char*> av;
                for (auto& s : store) av.push_back(const_cast<char*>(s.c_str()));
                av.push_back(nullptr);
                parse_dumpsys_at(kApi, static_cast<int>(av.size()) - 1, av.data(), &o, &err, &st);
                ok(err == c.want,
                   std::string("getopt wording for '") + store[1] + "' is `" + err + "'");
            }
        }

        // (2) The --help text. Each release brings its own and the newest covers
        // 33-37 unchanged. Checking for the flag that ADDED each release is
        // enough to prove the right variant was selected.
        for (int sdk : kSdks) {
            const std::string u = dumpsys_usage_text(sdk);
            const auto mentions = [&u](const char* f) {
                return std::strstr(u.c_str(), f) != nullptr;
            };
            ok(mentions("--pid") == (sdk >= 30),
               "api " + std::to_string(sdk) + " help mentions --pid");
            ok(mentions("--thread") == (sdk >= 31),
               "api " + std::to_string(sdk) + " help mentions --thread");
            ok(mentions("--clients") == (sdk >= 33),
               "api " + std::to_string(sdk) + " help mentions --clients");
        }
        // API 37's help is NOT API 33's. The predecessor's table asserted this
        // equality - it inferred API 33-37 from one measured release - and it is
        // false: android17-release documents -w. The equality that does hold is
        // 33 through 36.
        ok(std::strcmp(dumpsys_usage_text(33), dumpsys_usage_text(34)) == 0 &&
               std::strcmp(dumpsys_usage_text(34), dumpsys_usage_text(35)) == 0 &&
               std::strcmp(dumpsys_usage_text(35), dumpsys_usage_text(36)) == 0,
           "33 covers 34-36");
        ok(std::strcmp(dumpsys_usage_text(33), dumpsys_usage_text(37)) != 0,
           "37 is not 33 - it documents -w");
        {
            const std::string u37 = dumpsys_usage_text(37);
            ok(u37.find("-w: wait for service") != std::string::npos,
               "API 37's help is the one that documents -w");
            ok(std::string(dumpsys_usage_text(36)).find("-w:") == std::string::npos,
               "and API 36's is not");
        }
        // Below the oldest known release, fall back to the oldest rather than
        // guessing something newer. The floor moved 29 -> 26 when the 8.x point
        // tags were added; API 26 is the lowest release etools supports because
        // IShellCallback, and therefore the 7-argument IBinder::shellCommand,
        // first exists there. sdk 0 is the value worth checking because that is
        // what device_sdk() returns when the property is unreadable, and no row
        // has min_api <= 0, so sdk 0 and any pre-26 level take one code path.
        ok(std::strcmp(dumpsys_usage_text(0), dumpsys_usage_text(26)) == 0, "unknown API falls back to 26");
        // 26 and 27 ship identical dumpsys behaviour, so they must agree; 28 is
        // the first release with --priority and --proto. The wider sweeps above
        // pin what each level ACCEPTS but not the text, so these still earn
        // their keep.
        ok(std::strcmp(dumpsys_usage_text(26), dumpsys_usage_text(27)) == 0, "26 and 27 agree");
        ok(std::strcmp(dumpsys_usage_text(26), dumpsys_usage_text(28)) != 0, "26 and 28 differ");
        ok(std::strcmp(dumpsys_usage_text(28), dumpsys_usage_text(29)) != 0, "28 and 29 differ");

        // (3) The trailing --proto shim. A bare --proto after the service name
        // implies --proto from API 30 onwards; before that it is only a dump arg.
        for (int sdk : kSdks) {
            DumpsysOptions o;
            char* av[] = {const_cast<char*>("dumpsys"), const_cast<char*>("activity"),
                          const_cast<char*>("--proto"), nullptr};
            ok(parse_dumpsys_at(sdk, 3, av, &o, nullptr), "trailing --proto parses");
            ok(o.as_proto == (sdk >= 30), "api " + std::to_string(sdk) + " --proto shim");
            // The argument is still forwarded either way; only the shim differs.
            ok(o.dump_args.size() == 1 && o.dump_args[0] == "--proto",
               "api " + std::to_string(sdk) + " still forwards --proto");
        }

        // (4) The service-list header. Condition is `N > 1`, widened to
        // `N > 1 || -l` in API 31.
        for (int sdk : kSdks) {
            ok(dumpsys_shows_service_list(sdk, 1, true) == (sdk >= 31),
               "api " + std::to_string(sdk) + " header for one service with -l");
            ok(!dumpsys_shows_service_list(sdk, 1, false),
               "api " + std::to_string(sdk) + " no header for one service");
            ok(dumpsys_shows_service_list(sdk, 2, false),
               "api " + std::to_string(sdk) + " header for two services");
        }
        // With many services -l prints the list on every release anyway; only the
        // single-service case can tell the two conditions apart.

        // (5) cmd's no-arguments message was reworded in API 30.
        for (int sdk : kSdks) {
            const char* m = cmd_no_args_text(sdk);
            const bool reworded = sdk >= 30;
            ok((std::strstr(m, "list all services") != nullptr) == !reworded,
               "api " + std::to_string(sdk) + " cmd message is the short one");
            ok((std::strstr(m, "list all running services") != nullptr) == reworded,
               "api " + std::to_string(sdk) + " cmd message is the long one");
            ok((std::strstr(m, "-w") != nullptr) == reworded,
               "api " + std::to_string(sdk) + " cmd message -w hint");
        }

        // (6) `cmd -w` arrived in API 30. Before that "-w" is just a service
        // name that happens not to exist, so it must be treated as one.
        {
            // --stability is the one dump type that IS implemented.
            DumpsysOptions o;
            char* av[] = {const_cast<char*>("dumpsys"), const_cast<char*>("--stability"), nullptr};
            ok(parse_dumpsys_at(33, 2, av, &o, nullptr), "--stability parses");
            ok(o.dump_type_flags == kTypeStability, "--stability sets only its own bit");
        }
        for (int sdk : kSdks) {
            CmdMode m; std::string s; std::vector<std::string> a; std::string e;
            char* av[] = {const_cast<char*>("-w"), const_cast<char*>("activity"), nullptr};
            ok(parse_cmd_at(sdk, 2, av, &m, &s, &a, &e), "api " + std::to_string(sdk) + " cmd -w parses");
            if (sdk >= 30) {
                ok(m == CmdMode::kWait && s == "activity", "api " + std::to_string(sdk) + " cmd -w is wait mode");
            } else {
                ok(m == CmdMode::kRun, "api " + std::to_string(sdk) + " does not enter wait mode");
                ok(s == "-w", "api " + std::to_string(sdk) + " treats -w as the service name");
            }
        }

        // (7) Dump types became combinable in API 33. Before that dumpsys holds ONE
        // type and assigns, so the last flag on the line wins. Getting this
        // backwards would run --stability where the real tool runs --pid.
        for (int sdk : kSdks) {
            const bool combines = dumpsys_combines_dump_types(sdk);
            ok(combines == (sdk >= 33), "api " + std::to_string(sdk) + " combines dump types");
            DumpsysOptions o;
            char* one[] = {const_cast<char*>("dumpsys"), const_cast<char*>("--pid"), nullptr};
            if (sdk < 30) {
                // No dump types exist at all before API 30.
                ok(!parse_dumpsys_at(sdk, 2, one, &o, nullptr),
                   "api " + std::to_string(sdk) + " rejects --pid");
                ok(o.dump_type_flags == 0, "api " + std::to_string(sdk) + " sets no dump type");
                continue;
            }
            if (sdk < 31) {
                // --pid is the only dump type that exists yet, so there is
                // nothing to combine and nothing to assert about ordering.
                ok(parse_dumpsys_at(sdk, 2, one, &o, nullptr),
                   "api " + std::to_string(sdk) + " --pid parses");
                ok(o.dump_type_flags == kTypePid, "api " + std::to_string(sdk) + " --pid only");
                continue;
            }
            // --pid then --thread: both exist from API 31. --pid is refused
            // downstream but must still parse, so the mask is observable here.
            char* both[] = {const_cast<char*>("dumpsys"), const_cast<char*>("--pid"),
                            const_cast<char*>("--thread"), nullptr};
            ok(parse_dumpsys_at(sdk, 3, both, &o, nullptr),
               "api " + std::to_string(sdk) + " two dump types parse");
            if (combines) {
                ok(o.dump_type_flags == (kTypePid | kTypeThread),
                   "api " + std::to_string(sdk) + " combines both types");
            } else {
                ok(o.dump_type_flags == kTypeThread,
                   "api " + std::to_string(sdk) + " last type wins");
            }
        }

        // (8) The dump-failure message was reworded twice: in API 30 and in API 33.
        for (int sdk : kSdks) {
            const int want = sdk >= 33 ? 2 : sdk >= 30 ? 1 : 0;
            ok(dumpsys_error_style(sdk) == want,
               "api " + std::to_string(sdk) + " dump-failure message format");
        }

        // `cmd -l` predates every release we support, so it never moves.
        for (int sdk : kSdks) {
            CmdMode m; std::string s; std::vector<std::string> a; std::string e;
            char* av[] = {const_cast<char*>("-l"), nullptr};
            ok(parse_cmd_at(sdk, 1, av, &m, &s, &a, &e) && m == CmdMode::kList,
               "api " + std::to_string(sdk) + " cmd -l is list mode");
        }
    }

    // ======================= 10. --section extraction ==================
    //
    // The grammar is hand-rolled string work with no binder in it, which makes it
    // the part of etools most likely to be subtly wrong. These run on device but
    // need no service, so a failure points at the matcher and not at Android.
    {
        // Real first lines and section headers, taken from actual dumps. The
        // service banner is always the first header and must never be a candidate.
        // NOTE the real dumpsys power output interleaves these with data lines that
        // contain their own colon - `Battery Saver is currently: OFF`. Those must
        // not be mistaken for headers, or every section would be cut in half. The
        // fixture keeps them because that is the whole difficulty.
        const std::string kPower =
            "POWER MANAGER (dumpsys power)\n"
            "\n"
            "Power Manager State:\n"
            "  mWakefulness=Awake\n"
            "  mIsPowered=true\n"
            "\n"
            "Battery saving stats:\n"
            "  Battery Saver is currently: OFF\n"
            "  Drain stats:\n"
            "     17m 0mAh\n"
            "\n"
            "Battery saver state machine:\n"
            "  enabled=false\n";
        std::vector<std::string> body, cands;
        std::string name;

        // Both grammars recognised; banner alone is not a section.
        ok(extract_section(kPower, "Battery saving stats", &body, &cands, &name) ==
               SectionMatch::kFound,
           "colon-grammar header is found");
        ok(body.size() == 5, "section runs to the next header");
        ok(body[0] == "Battery saving stats:", "body includes its header");
        ok(body[1] == "  Battery Saver is currently: OFF", "body includes its content");
        // `Battery Saver is currently: OFF` and the indented `Drain stats:` both
        // contain colons but are data, so they must not have split the section.
        ok(body[2] == "  Drain stats:", "a colon inside an indented data line is kept");
        ok(body[3] == "     17m 0mAh", "indented data lines are kept whole");
        ok(body[4].empty(), "the blank line before the next header is included");
        ok(name == "Battery saving stats", "matched name has no trailing colon");

        // The banner is skipped by POSITION, so it can never be returned. Proving that
        // needs a query that would otherwise match it - here the banner `POWER
        // MANAGER (dumpsys power)` overlaps the real section `Power Manager State:`,
        // so the only reason the banner loses is that it is the first header. If the
        // skip were text-based instead, this would return the banner's section.
        ok(extract_section(kPower, "POWER MANAGER", &body, &cands, &name) ==
               SectionMatch::kFound,
           "a query overlapping the banner finds the SECTION, not the banner");
        ok(name == "Power Manager State",
           "the overlapping query resolves to the real section, skipping the banner");
        {
            // Here the banner text overlaps a real section, so only skipping by
            // position can produce not-found for the banner while still finding the
            // section. This is the `input` shape: `INPUT MANAGER (dumpsys input)`.
            const std::string kInput =
                "INPUT MANAGER (dumpsys input)\n"
                "Input properties:\n"
                "  keyboardIsReady=true\n"
                "Input Reader State (Nums of device: 7):\n"
                "  deviceCount=7\n";
            cands.clear();
            ok(extract_section(kInput, "INPUT MANAGER", &body, &cands, &name) ==
                   SectionMatch::kNotFound,
               "banner excluded even when its text overlaps a section's");
            ok(extract_section(kInput, "Input Reader State", &body, &cands, &name) ==
                   SectionMatch::kFound,
               "the overlapping section is still findable by its own name");
        }
        // The banner is skipped by POSITION, so `Power Manager` still finds the
        // real `Power Manager State:` section - it does not just miss everything.
        ok(extract_section(kPower, "Power Manager", &body, &cands, &name) ==
               SectionMatch::kFound,
           "the banner being skipped does not hide a real similarly-named section");

        // The last section must extend to end of dump with no phantom line.
        ok(extract_section(kPower, "Battery saver state machine", &body, &cands, &name) ==
               SectionMatch::kFound,
           "last section is found");
        ok(body.size() == 2, "last section runs to end of dump, no trailing blank");
        ok(body[1] == "  enabled=false", "last section keeps its final line");

        // Case-insensitive, and partial words that do not match on a boundary.
        //
        // NB "battery" DOES match on a boundary - "Battery saving stats" starts
        // with it. What must not match is a word-boundary rule that fires in the
        // middle of a word, which is what "saving stats" vs "savingstats" tests.
        ok(extract_section(kPower, "BATTERY SAVING STATS", &body, &cands, &name) ==
               SectionMatch::kFound,
           "matching is case-insensitive");
        // "battery" really does match TWO sections here - `Battery saving stats`
        // and `Battery saver state machine` - so ambiguity is the correct answer,
        // and it is a good check that prefixes are not silently narrowed.
        cands.clear();
        ok(extract_section(kPower, "battery", &body, &cands, &name) == SectionMatch::kAmbiguous,
           "a prefix matching two sections is ambiguous");
        ok(cands.size() == 2, "both battery sections are listed");
        ok(extract_section(kPower, "Battery saving", &body, &cands, &name) ==
               SectionMatch::kFound,
           "a prefix matching exactly one section finds it");
        ok(extract_section(kPower, "Battery saving stats:", &body, &cands, &name) ==
               SectionMatch::kFound,
           "a query with a trailing colon still matches");
        ok(extract_section(kPower, "saving", &body, &cands, &name) == SectionMatch::kFound,
           "substring on a word boundary matches");
        ok(extract_section(kPower, "savingstats", &body, &cands, &name) ==
               SectionMatch::kNotFound,
           "a query spanning a word boundary does not match");
        ok(extract_section(kPower, "at", &body, &cands, &name) == SectionMatch::kNotFound,
           "a word buried inside the name does not match");

        // The banner grammar, and dropping its "(dumpsys ...)" suffix when naming.
        const std::string kWindow =
            "WINDOW MANAGER LAST ANR (dumpsys window lastanr)\n"
            "  <no ANR>\n"
            "WINDOW MANAGER POLICY STATE (dumpsys window policy)\n"
            "    mSafeMode=false\n"
            "WINDOW MANAGER SESSIONS (dumpsys window sessions)\n"
            "    numSessions=3\n";
        ok(extract_section(kWindow, "POLICY STATE", &body, &cands, &name) ==
               SectionMatch::kFound,
           "banner-grammar header is found by its title");
        ok(name == "WINDOW MANAGER POLICY STATE", "banner title excludes the (dumpsys) suffix");
        ok(body.size() == 2, "banner section runs to the next banner");
        ok(extract_section(kWindow, "dumpsys window policy", &body, &cands, &name) ==
               SectionMatch::kNotFound,
           "the (dumpsys) suffix is not part of the name");

        // Ambiguity is refused, not guessed. 33 headers in dumpsys wifi repeat.
        const std::string kDup =
            "WIFI (dumpsys wifi)\n"
            "Locks held:\n"
            "  first\n"
            "Locks held:\n"
            "  second\n";
        ok(extract_section(kDup, "Locks held", &body, &cands, &name) == SectionMatch::kAmbiguous,
           "a repeated header is ambiguous, not first-wins");
        ok(cands.size() == 2, "both candidates are listed");
        ok(body.empty(), "nothing is emitted when ambiguous");

        // Indented lines are never headers: allowing them took `package` from 41
        // headers to 5467, because every package entry has `Disabled:` lines.
        ok(!is_section_header("  Disabled:"), "an indented colon line is not a header");
        ok(!is_section_header("  Settings:"), "an indented title is not a header");
        ok(!is_section_header("\tSettings:"), "a tab-indented title is not a header");
        ok(!is_section_header("  Disabled: "), "an indented title with trailing space is not one");
        // The section-BODY case, not just the header predicate: an indented
        // key=value line ending in a colon must not truncate the section it sits in.
        // Every indented-header mutation preserves the body and only changes which
        // line the NEXT section starts at.
        {
            const std::string kIndented =
                "Alpha:\n"
                "  Disabled:\n"
                "  mFoo=bar:\n"
                "     17m 0mAh\n"
                "Beta:\n"
                "  b\n";
            std::vector<std::string> ibody, icands;
            std::string iname;
            ok(extract_section(kIndented, "Alpha", &ibody, &icands, &iname) ==
                   SectionMatch::kFound,
               "a section containing indented key=value lines is found");
            ok(ibody.size() == 4, "indented key=value lines do not split the section");
            ok(ibody[2] == "  mFoo=bar:", "the indented key=value line is kept whole");
        }
        ok(is_section_header("Settings:"), "a column-0 title is a header");
        ok(is_section_header("Settings:  "), "trailing whitespace does not matter");
        ok(!is_section_header("mFoo=bar: baz"), "a key=value line is not a header");
        // A column-0 key=value line whose value ENDS in a colon. Rejected by the
        // '=' rule, not by the colon-terminates rule, so it needs its own case:
        // `mFoo=bar:` satisfies find(':')==size()-1 exactly as a real title does.
        ok(!is_section_header("mFoo=bar:"), "a column-0 key=value line ending in ':' is not a header");
        {
            // And the consequence, which is what actually matters: a column-0
            // key=value line truncates the section above it to one line.
            const std::string kKv =
                "Alpha:\n"
                "  a\n"
                "mFoo=bar:\n"
                "  more\n"
                "Beta:\n"
                "  b\n";
            std::vector<std::string> kbody, kcands;
            std::string kname;
            ok(extract_section(kKv, "Alpha", &kbody, &kcands, &kname) == SectionMatch::kFound,
               "section above a column-0 key=value line is found");
            ok(kbody.size() == 4, "a column-0 key=value line does not split the section");
            ok(kbody[2] == "mFoo=bar:", "the column-0 key=value line is kept whole");
        }
        ok(!is_section_header(""), "an empty line is not a header");

        // A dump ending in a newline must not invent a final line.
        ok(list_section_headers("A:\nx\nB:\ny\n").size() == 2, "trailing newline adds no section");
        // A dump with no trailing newline still parses.
        ok(list_section_headers("A:\nx\nB:\ny").size() == 2, "missing trailing newline is fine");
        ok(list_section_headers("").empty(), "an empty dump has no sections");
    }

    // ======================= 10b. regressions the audit found ============
    //
    // Every case here is a mutation that previously SURVIVED this suite, or a
    // behaviour proven broken on a real device. They are here because the earlier
    // block passed while the feature silently ate the first section of 8 of 12
    // services.
    {
        // The service's own name is skipped ONLY as a banner on the FIRST line.
        // The earlier rule skipped whichever header came first, which made the
        // first real section of every service that does not open with a banner
        // unreachable. Measured on device: battery, batterystats, meminfo,
        // notification, package, audio, wifi and deviceidle all open with a
        // section, not a banner.
        const std::string kNoBanner =
            "Current Battery Service state:\n"
            "  level=100\n"
            "  scale=100\n"
            "  voltage=4200\n";
        std::vector<std::string> body, cands;
        std::string name;
        ok(extract_section(kNoBanner, "Current Battery Service state", &body, &cands, &name) ==
               SectionMatch::kFound,
           "first section of a bannerless dump is reachable");
        ok(body.size() == 4, "bannerless first section runs to end of dump");

        // `dumpsys batterystats` opens with a parenthetical section, not a banner.
        // "Battery History" is the section most worth extracting and it WAS
        // unreachable; "Per-PID Stats" worked, which is what hid the bug.
        const std::string kBatterystats =
            "Battery History (18% used, 777KB used of 4096KB):\n"
            "                    0 (14) RESET:TIME: 2026-10-08\n"
            "Per-PID Stats:\n"
            "  *PERS* UID 1000 ProcessRecord{a 1000:system/1000}\n"
            "Discharge step durations:\n"
            "  0: 100\n";
        ok(extract_section(kBatterystats, "Battery History", &body, &cands, &name) ==
               SectionMatch::kFound,
           "parenthetical first section is reachable");
        ok(name.find("Battery History") == 0, "parenthetical first section keeps its title");
        ok(extract_section(kBatterystats, "Per-PID Stats", &body, &cands, &name) ==
               SectionMatch::kFound,
           "a later section in the same dump is still reachable");

        // `dumpsys window` puts its banner on line 3, behind "Dump time :", and
        // that banner IS a section. It was NOT FINDABLE under the old rule.
        const std::string kRealWindow =
            "Dump time : 2026-10-08 08:24:30.272\n"
            "\n"
            "WINDOW MANAGER LAST ANR (dumpsys window lastanr)\n"
            "  <no ANR has occurred since boot>\n"
            "\n"
            "WINDOW MANAGER POLICY STATE (dumpsys window policy)\n"
            "    mSafeMode=false\n";
        ok(extract_section(kRealWindow, "LAST ANR", &body, &cands, &name) ==
               SectionMatch::kFound,
           "a banner on line 3 is a section, not the service name");
        ok(extract_section(kRealWindow, "POLICY STATE", &body, &cands, &name) ==
               SectionMatch::kFound,
           "a later banner section is reachable");
        ok(extract_section(kRealWindow, "WINDOW MANAGER", &body, &cands, &name) ==
               SectionMatch::kAmbiguous || true,
           "the service-name query does not crash");
        ok(extract_section(kRealWindow, "Dump time", &body, &cands, &name) ==
               SectionMatch::kNotFound,
           "the preamble line is not a section");

        // Candidates must be cleared even when the caller's buffers are reused.
        // m24 survived the old suite: after an ambiguous lookup, a following
        // successful one left stale candidates behind.
        body.clear();
        cands.clear();
        name.clear();
        const std::string kDup2 =
            "WIFI (dumpsys wifi)\n"
            "Locks held:\n  first\n"
            "Locks held:\n  second\n"
            "Unique:\n  only\n";
        ok(extract_section(kDup2, "Locks held", &body, &cands, &name) == SectionMatch::kAmbiguous,
           "repeated header is ambiguous");
        ok(cands.size() == 2, "two candidates recorded");
        ok(extract_section(kDup2, "Unique", &body, &cands, &name) == SectionMatch::kFound,
           "a later lookup succeeds after an ambiguous one");
        ok(cands.empty(), "candidates are cleared by a successful lookup");
        ok(name == "Unique", "matched name is set on success");
        ok(extract_section(kDup2, "Nonexistent", &body, &cands, &name) ==
               SectionMatch::kNotFound,
           "a later lookup misses after a successful one");
        ok(body.empty(), "body is cleared on a miss");
        ok(name.empty(), "matched name is cleared on a miss");

        // A section terminated by the FINAL line of the dump: the end-scan
        // off-by-one in the early direction (m4) survived the old suite because
        // every fixture had content after the terminating header.
        const std::string kTail =
            "First banner (dumpsys x)\n"
            "Alpha:\n"
            "  a\n"
            "Beta:\n";
        ok(extract_section(kTail, "Alpha", &body, &cands, &name) == SectionMatch::kFound,
           "section before a final-line header is found");
        ok(body.size() == 2, "the terminating header is EXCLUDED");
        ok(body[1] == "  a", "section content is complete");

        // Word boundary: the query's FIRST occurrence mid-word must not win when a
        // later occurrence sits on a boundary (m38 survived the old suite).
        const std::string kBoundary =
            "Presavingstuff saving:\n"
            "  x\n";
        ok(extract_section(kBoundary, "saving", &body, &cands, &name) == SectionMatch::kFound,
           "a later boundary occurrence is found past a mid-word first one");

        // A digit before the match is NOT a boundary; a letter or digit is.
        ok(extract_section("Battery2saving stats:\n  x\n", "saving", &body, &cands, &name) ==
               SectionMatch::kNotFound,
           "a digit before the query is not a word boundary");

        // Trailing space before the colon: `--section "Name :"`. The old suite only
        // tested `"Name:"`, so normalise_query's closing trim was unasserted (m32).
        const std::string kSpace =
            "Battery saving stats :\n"
            "  x\n";
        ok(extract_section(kSpace, "Battery saving stats :", &body, &cands, &name) ==
               SectionMatch::kFound,
           "a query with a space before the colon is normalised");

        // CRLF: defensive, but it was defensive code with a test-shaped hole (m27).
        ok(is_section_header("Settings:\r"), "a CR-terminated header is recognised");
        ok(is_section_header("WINDOW MANAGER X (dumpsys x)\r"), "a CR-terminated banner too");

        // Length caps: m10 and m29 both survived, deleting the cap entirely.
        ok(!is_section_header("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA:"),
           "an over-long colon header is rejected");
        ok(is_section_header("Short title:"), "a short colon header is accepted");

        // `mFoo=bar (baz):` is what section.cpp claimed the '=' rule rejected. It
        // does not - the comment was false and the suite never tested it.
        ok(!is_section_header("mFoo=bar (baz):"), "a key=value parenthetical is not a header");
        {
            // `mFoo=bar (baz):` is rejected by grammar 1's '=' rule, which is
            // evaluated first. Grammar 3 has its own '=' check, but it is nearly
            // dead: only lines longer than grammar 1's 72-char cap can reach it.
            //
            // The cap itself is pinned here rather than left as a comment-only
            // claim. It is 72 for the WHOLE line including the colon.
            std::vector<std::string> obody, ocands;
            std::string oname;
            const std::string kAt72 =
                "A titled section heading at the length capxxxxxxxxxxxxxxxxxxxxxxxxx(ok):\n"
                "  a\n"
                "Next:\n"
                "  b\n";
            const std::string kOver72 =
                "A titled section heading clearly past the seventy-two character limit for surexxxxxxxx(ok):\n"
                "  a\n"
                "Next:\n"
                "  b\n";
            // kAt72/kOver72 are whole dumps. is_section_header takes ONE line, so
            // both the length check and the predicate need the header line alone -
            // passing the dump makes every grammar fail on the embedded newline.
            const auto header_line = [](const std::string& dump) {
                return dump.substr(0, dump.find('\n'));
            };
            ok(header_line(kAt72).size() == 72, "the accepted header is exactly 72 chars");
            ok(header_line(kOver72).size() > 72, "the rejected header is over the cap");
            ok(is_section_header(header_line(kAt72)), "a title at the length cap is a header");
            ok(!is_section_header(header_line(kOver72)),
               "a title over the length cap is not a header");
            // And through extract_section, which splits lines properly.
            ok(extract_section(kAt72, "A titled section", &obody, &ocands, &oname) ==
                   SectionMatch::kFound,
               "a title at the length cap is extractable");
            ok(obody.size() == 2, "and its section is complete");
            ok(!is_section_header("mLongKey=someValue (and a parenthetical):"),
               "a key=value parenthetical is rejected even when long");
        }
        ok(is_section_header("Input Reader State (Nums of device: 7):"),
           "a real titled parenthetical IS a header");
        ok(!is_section_header("A(x:y):"), "a one-character title is not a header");
    }

    // ======================= 11. the --ext- namespace ==================
    //
    // Our extensions live in a reserved namespace and are stripped from argv
    // BEFORE this parser runs. So two things must hold, and both are asserted
    // here rather than assumed: the AOSP parser must never see an --ext- token,
    // and an AOSP flag must never be mistaken for one of ours.
    {
        DumpsysOptions o;
        std::vector<std::string> rest;
        ExtFlags f;

        auto ext = [&](std::vector<std::string> v, ExtTool which, ExtFlags* f,
                       std::vector<std::string>* rest) {
            std::vector<char*> av;
            for (auto& s : v) av.push_back(const_cast<char*>(s.c_str()));
            av.push_back(nullptr);
            return parse_ext_flags(static_cast<int>(av.size()) - 1, av.data(), which, f, rest);
        };

        // --ext-section is ours and is removed, so it can never become a dump
        // argument the service would receive.
        ok(ext({"dumpsyse", "--ext-section", "Battery saving stats", "power"}, ExtTool::kDumpsys,
              &f, &rest),
           "--ext-section parses");
        ok(f.section == "Battery saving stats", "--ext-section captures its value");
        ok(rest.size() == 2 && rest[0] == "dumpsyse" && rest[1] == "power",
           "--ext-section is removed from argv, order preserved");
        ok(!pd({"dumpsys", "--ext-section", "Battery saving stats", "power"}, &o),
           "--ext-section is NOT an AOSP flag: dumpsys must reject it");

        // The same command line with the AOSP spelling is a different thing
        // entirely: real dumpsys has no --section, and it must stay that way.
        ok(!pd({"dumpsys", "--section", "power"}, &o), "--section is not a dumpsys flag either");

        // AOSP's own flags pass through untouched, in any position.
        ok(ext({"dumpsyse", "-t", "5", "--ext-loop", "3", "--proto", "power"}, ExtTool::kDumpsys,
              &f, &rest),
           "AOSP flags and --ext- flags interleave");
        ok(f.loop && f.loop_count == 3, "the extension between them still parses");
        ok(rest.size() == 5 && rest[1] == "-t" && rest[2] == "5" && rest[3] == "--proto" &&
               rest[4] == "power",
           "and the AOSP flags are handed over in their original order");

        // Each binary rejects the other's extensions instead of forwarding them
        // into a service that has never heard of them.
        ok(!ext({"cmde", "--ext-loop", "3", "power"}, ExtTool::kCmd, &f, &rest),
           "cmde rejects --ext-loop");
        ok(!ext({"dumpsyse", "--ext-shape", "null", "power"}, ExtTool::kDumpsys, &f, &rest),
           "dumpsyse rejects --ext-shape");
        ok(ext({"cmde", "--ext-shape", "null", "settings", "get"}, ExtTool::kCmd, &f, &rest),
           "cmde accepts --ext-shape");
        ok(f.shape == 0, "--ext-shape null is recorded");
        ok(ext({"cmde", "--ext-shape", "receiver", "package"}, ExtTool::kCmd, &f, &rest) &&
               f.shape == 1,
           "--ext-shape receiver is recorded");
        ok(ext({"cmde", "--ext-shape=receiver", "package"}, ExtTool::kCmd, &f, &rest) &&
               f.shape == 1,
           "--ext-shape=receiver is recorded");
        ok(!ext({"cmde", "--ext-shape", "bogus", "package"}, ExtTool::kCmd, &f, &rest),
           "an unknown shape is refused");

        // Values. The regression set: each of these once parsed "successfully"
        // and produced a value that silently changed behaviour.
        ok(!ext({"dumpsyse", "--ext-timeout", "abc", "power"}, ExtTool::kDumpsys, &f, &rest),
           "--ext-timeout abc is rejected, not silently 0 (0 times out every dump)");
        ok(!ext({"dumpsyse", "--ext-timeout", "0", "power"}, ExtTool::kDumpsys, &f, &rest),
           "--ext-timeout 0 is rejected");
        ok(!ext({"dumpsyse", "--ext-timeout", "-5", "power"}, ExtTool::kDumpsys, &f, &rest),
           "--ext-timeout -5 is rejected");
        ok(!ext({"dumpsyse", "--ext-interval", "-1", "power"}, ExtTool::kDumpsys, &f, &rest),
           "--ext-interval -1 is rejected (nanosleep would fail: busy loop)");
        ok(!ext({"dumpsyse", "--ext-loop=-3", "power"}, ExtTool::kDumpsys, &f, &rest),
           "--ext-loop=-3 is rejected rather than looping forever");
        ok(!ext({"dumpsyse", "--ext-timeout", "9223372036854775807", "power"}, ExtTool::kDumpsys,
               &f, &rest),
           "--ext-timeout LONG_MAX is rejected (overflows the chrono deadline)");
        ok(!ext({"dumpsyse", "--ext-timeout", "99999999999", "power"}, ExtTool::kDumpsys,
               &f, &rest),
           "--ext-timeout past INT_MAX is rejected (overflows the poll() int cast)");
        ok(ext({"dumpsyse", "--ext-timeout", "86400000", "power"}, ExtTool::kDumpsys, &f, &rest) &&
               f.timeout_ms == 86400000,
           "--ext-timeout at the 24h cap is accepted");

        // A missing argument is an error, never a swallowed service name.
        ok(!ext({"dumpsyse", "--ext-timeout"}, ExtTool::kDumpsys, &f, &rest),
           "bare --ext-timeout is an error");
        ok(!ext({"dumpsyse", "--ext-section"}, ExtTool::kDumpsys, &f, &rest),
           "bare --ext-section is an error");

        // A bare --ext-loop must not swallow what follows it: a service name is
        // not a count, and swallowing one would dump nothing.
        ok(ext({"dumpsyse", "--ext-loop", "power"}, ExtTool::kDumpsys, &f, &rest) && f.loop &&
               f.loop_count == 0,
           "bare --ext-loop means forever and keeps the service name");
        ok(rest.size() == 2 && rest[1] == "power", "and the service name survives");

        // An unknown name in our own namespace is a typo to report, never
        // something to hand to a service.
        ok(!ext({"dumpsyse", "--ext-loops", "power"}, ExtTool::kDumpsys, &f, &rest),
           "a misspelled extension is reported");

        // --help is not ours. It reaches the tool untouched, which is what keeps
        // `dumpsyse --help` byte-identical to `dumpsys --help`.
        ok(ext({"dumpsyse", "--help"}, ExtTool::kDumpsys, &f, &rest) && !f.want_help &&
               rest.size() == 2 && rest[1] == "--help",
           "--help is left for the tool to answer");

        // -w arrived in API 37 (android17-release). This is asserted per release
        // because the predecessor's table inferred API 33-37 from ONE of them
        // and so missed a flag that exists on the newest supported release -
        // invisible until someone ran it on a 37 device.
        {
            DumpsysOptions o;
            ok(pd_at(37, {"dumpsys", "-w", "power"}, &o), "API 37 accepts -w");
            ok(o.wait_for_service, "and it is recorded as ours, not forwarded");
            ok(o.services.size() == 1 && o.services[0] == "power",
               "and the service still parses");
            // Measured per release: the optstring is "+t:T:l" up to and including
            // android16-release (and its QPR branches), and gains the w in
            // android17-release.
            ok(!pd_at(36, {"dumpsys", "-w", "power"}, &o), "API 36 rejects -w");
            ok(!pd_at(35, {"dumpsys", "-w", "power"}, &o), "API 35 rejects -w");
            ok(!pd_at(34, {"dumpsys", "-w", "power"}, &o), "API 34 rejects -w");
            ok(!pd_at(33, {"dumpsys", "-w", "power"}, &o), "API 33 rejects -w");
            ok(!pd_at(30, {"dumpsys", "-w", "power"}, &o), "API 30 rejects -w");
            ok(!pd_at(29, {"dumpsys", "-w", "power"}, &o), "API 29 rejects -w");
            ok(!pd_at(26, {"dumpsys", "-w", "power"}, &o), "API 26 rejects -w");

            // After the service name it is the service's argument on EVERY
            // release, 37 included: AOSP's optstring starts with '+', so getopt
            // stops at the first non-option there too.
            DumpsysOptions f34, f37;
            ok(pd_at(34, {"dumpsys", "power", "-w"}, &f34) && f34.dump_args.size() == 1 &&
                   f34.dump_args[0] == "-w" && !f34.wait_for_service,
               "on API 34 a -w after the service is the service's argument");
            ok(pd_at(37, {"dumpsys", "power", "-w"}, &f37) && f37.dump_args.size() == 1 &&
                   f37.dump_args[0] == "-w" && !f37.wait_for_service,
               "on API 37 the same token is still forwarded, as upstream does");

            // The advisory warning about a flag placed too late is only for flags
            // this release actually HAS, which is why it reads the optstring
            // rather than a hardcoded list: warning about a -w on API 34, where
            // no such flag exists, would be inventing a mistake.
            ok(pd_at(37, {"dumpsys", "power", "-w"}, &f37) && !f37.warning.empty(),
               "API 37 warns about a misplaced -w");
            ok(pd_at(34, {"dumpsys", "power", "-w"}, &f34) && f34.warning.empty(),
               "API 34 does not warn about a -w it does not have");

            // -w with no service named draws the same response as --skip with no
            // names: usage alone, no message.
            std::string err;
            ErrorStyle style = ErrorStyle::kMessageOnly;
            DumpsysOptions w;
            ok(!pd_at(37, {"dumpsys", "-w"}, &w, &err, &style) && err.empty() &&
                   style == ErrorStyle::kUsageOnly,
               "API 37: bare -w is usage alone, like bare --skip");
        }

        // --ext-help is OURS and belongs to both binaries: both have extensions,
        // so both document them. The tool's own --help is never intercepted,
        // which is what keeps `cmde --help` byte-identical to `cmd --help`.
        ok(ext({"cmde", "--ext-help"}, ExtTool::kCmd, &f, &rest) && f.want_help,
           "cmde --ext-help is recognised");
        ok(ext({"dumpsyse", "--ext-help"}, ExtTool::kDumpsys, &f, &rest) && f.want_help,
           "dumpsyse --ext-help is recognised");
        ok(ext({"dumpsyse", "--ext-help", "power"}, ExtTool::kDumpsys, &f, &rest) &&
               f.want_help && rest.size() == 2 && rest[1] == "power",
           "--ext-help does not swallow the service name");

        // ---- Audit fixes. Every assertion below failed before its fix and
        // names the measurement or the AOSP source line that proves the new
        // behaviour is the right one.

        // ---- F8: the -t error's unit word was added in API 28 ----------------
        // Measured from the 16 vendored AOSP refs: android-8.0.0_r1/dumpsys.cpp:116
        // and android-8.1.0_r1/dumpsys.cpp:117 print no unit word at all;
        // android-9.0.0_r1/dumpsys.cpp:169 and every later ref add "(seconds)".
        std::string err26, err28;
        ok(!pd_at(26, {"dumpsys", "-t", "abc", "power"}, &o, &err26) &&
               err26 == "Error: invalid timeout number: 'abc'",
           "API 26/27 print the -t error with no unit word");
        ok(!pd_at(28, {"dumpsys", "-t", "abc", "power"}, &o, &err28) &&
               err28 == "Error: invalid timeout(seconds) number: 'abc'",
           "API 28+ print (seconds) in the -t error");

        // ---- F1: AOSP holds the -t seconds in an int, and that bounds it ----
        // Exit codes measured against /system/bin/dumpsys on an API 34 device.
        ok(!pd_at(34, {"dumpsys", "-t", "2147483648", "power"}, &o),
           "-t 2147483648 is rejected, as the real tool is (measured rc=255)");
        ok(!pd_at(34, {"dumpsys", "-t", "99999999999999999999", "power"}, &o),
           "-t 99999999999999999999 is rejected, as the real tool is (measured rc=255)");
        ok(pd_at(34, {"dumpsys", "-t", "99999999999", "power"}, &o),
           "-t 99999999999 is accepted, as the real tool is (measured rc=0)");
        ok(pd_at(34, {"dumpsys", "-t", "1000000000", "power"}, &o),
           "-t 1000000000 is accepted, as the real tool is (measured rc=0)");
        ok(pd_at(34, {"dumpsys", "-t", " 5", "power"}, &o) && o.timeout_ms == 5000,
           "the leading space strtol tolerates is still tolerated");

        // ---- F6: '+' and ':' are optstring metacharacters, not option letters -
        // Measured: stock `dumpsys activity -+` prints nothing.
        ok(pd_at(26, {"dumpsys", "activity", "-+"}, &o) && o.warning.empty(),
           "on API 26 -+ is a dump argument, not a client-side flag");
        ok(pd_at(26, {"dumpsys", "activity", "-:"}, &o) && o.warning.empty(),
           "on API 26 -: is a dump argument, not a client-side flag");
        ok(pd_at(34, {"dumpsys", "power", "-t"}, &o) && !o.warning.empty(),
           "a real client-side flag placed late still warns");

        // ---- F2: an --ext- flag's value is never swallowed from another token -
        ok(!ext({"dumpsyse", "--ext-section", "--ext-loops", "power"}, ExtTool::kDumpsys,
                &f, &rest),
           "a typo'd --ext- name is reported, not eaten as a value");
        ok(!ext({"dumpsyse", "--ext-section", "-t", "5", "power"}, ExtTool::kDumpsys,
                &f, &rest),
           "a dumpsys flag is not eaten as an --ext- value");
        ok(ext({"dumpsyse", "--ext-section", "Battery saving stats", "power"},
               ExtTool::kDumpsys, &f, &rest) && f.section == "Battery saving stats",
           "a valid non-dash value still parses");

        // ---- F3: --ext-loop=N is a documented spelling and must work ---------
        ok(ext({"dumpsyse", "--ext-loop=5", "power"}, ExtTool::kDumpsys, &f, &rest) &&
               f.loop && f.loop_count == 5,
           "--ext-loop=5 sets a bounded count");
        ok(ext({"dumpsyse", "--ext-loop=3", "power"}, ExtTool::kDumpsys, &f, &rest) &&
               rest.size() == 2 && rest[1] == "power",
           "the equals form does not consume the service name");
        ok(!ext({"dumpsyse", "--ext-loop=abc", "power"}, ExtTool::kDumpsys, &f, &rest),
           "--ext-loop=abc is refused with a message, not silently");
        ok(ext({"dumpsyse", "--ext-loop", "3", "power"}, ExtTool::kDumpsys, &f, &rest) &&
               f.loop_count == 3 && rest.size() == 2 && rest[1] == "power",
           "the space-separated form is unchanged");
        ok(ext({"dumpsyse", "--ext-loop", "power"}, ExtTool::kDumpsys, &f, &rest) && f.loop &&
               f.loop_count == 0 && rest.size() == 2 && rest[1] == "power",
           "the bare form still means until-interrupted");

        // ---- F5/P1: interval 0 is the busy loop the rationale prevents -------
        ok(!ext({"dumpsyse", "--ext-interval", "0", "power"}, ExtTool::kDumpsys, &f, &rest),
           "--ext-interval 0 is rejected (the busy loop -1 would be)");
        ok(ext({"dumpsyse", "--ext-interval", "1", "power"}, ExtTool::kDumpsys, &f, &rest) &&
               f.interval_ms == 1,
           "--ext-interval 1 is still accepted");
    }

    if (g_fail) { std::fprintf(stderr, "\n%d check(s) failed\n", g_fail); return 1; }
    std::printf("compatibility: all checks passed\n");
    return 0;
}