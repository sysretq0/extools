// SPDX-License-Identifier: GPL-3.0-only
// The CLI layer: the --ext- namespace, and the two drop-in tool runners.
//
// Everything here is presentation and policy. The calls themselves are
// library calls (<etools/etools.hpp>), because the tools are the second
// consumer of them and the first is an embedder.
#include "tool.h"

#include "extflags.h"

#include "binder.h"
#include "device.h"
#include "dump_types.h"

#include <etools/args.hpp>
#include <etools/compat.hpp>
#include <etools/etools.hpp>
#include <etools/section.hpp>

#include <time.h>
#include <unistd.h>

#include <binder/IBinder.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <utils/String16.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace etools {
namespace {

void dumpsys_usage(int sdk) { std::fputs(dumpsys_usage_text(sdk), stderr); }

void write_dump_header(const std::string& name, const std::string& priority_type) {
    std::printf("----------------------------------------"
                "---------------------------------------\n");
    if (priority_type.empty()) {
        std::printf("DUMP OF SERVICE %s:\n", name.c_str());
    } else {
        std::printf("DUMP OF SERVICE %s %s:\n", priority_type.c_str(), name.c_str());
    }
    std::fflush(stdout);
}

void write_dump_footer(const std::string& name, double seconds) {
    const auto finish = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_r(&finish, &tm);
    char when[32];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    std::printf("--------- %.3fs was the duration of dumpsys %s, ending at: %s\n", seconds,
                name.c_str(), when);
    std::fflush(stdout);
}

void write_timeout_notice(const std::string& name, long timeout_ms) {
    std::printf("\n*** SERVICE '%s' DUMP TIMEOUT (%ldms) EXPIRED ***\n\n", name.c_str(),
                timeout_ms);
    std::fflush(stdout);
}

bool is_skipped(const std::vector<std::string>& skipped, const std::string& name) {
    return std::find(skipped.begin(), skipped.end(), name) != skipped.end();
}

// One pass of the dump tool: exactly what `dumpsys` would do with the same argv,
// plus whatever the extensions asked for.
int dumpsys_pass(int argc, char** argv, const ExtFlags& ext) {
    const int sdk = device_sdk();
    DumpsysOptions o;
    std::string error;
    ErrorStyle style = ErrorStyle::kMessageOnly;
    if (!parse_dumpsys(argc, argv, &o, &error, &style, ext.timeout_ms, sdk)) {
        if (!error.empty()) std::fprintf(stderr, "%s\n", error.c_str());
        // dumpsys prints a blank line before the usage for some failures and not
        // for others; the style carries which.
        if (style == ErrorStyle::kMessageBlankUsage || style == ErrorStyle::kBlankUsage) {
            std::fprintf(stderr, "\n");
        }
        if (style != ErrorStyle::kMessageOnly) dumpsys_usage(sdk);
        return -1;   // dumpsys returns -1 for every parse failure
    }

    // Byte-for-byte what `dumpsys --help` prints. None of our text goes near it:
    // the tool's help is the tool's, and a caller diffing the two must not see
    // this project's name in the middle of it.
    if (o.want_help) {
        dumpsys_usage(sdk);
        return 0;
    }
    if (!o.warning.empty()) std::fprintf(stderr, "dumpsyse: %s\n", o.warning.c_str());

    o.section = ext.section;

    // --pid/--thread/--clients are refused rather than faked, each for a
    // checked reason: --pid needs IBinder::getDebugPid, a non-virtual libbinder
    // symbol added after the API 29 headers this builds against, and doing the
    // DEBUG_PID_TRANSACTION by hand needs a stack android::Parcel whose layout
    // changed after those headers (a Parcel sized by the old header overflows).
    // --thread and --clients need libbinderdebug.
    constexpr int kUnimplemented = kTypePid | kTypeThread | kTypeClients;
    if (o.dump_type_flags & kUnimplemented) {
        std::fprintf(stderr,
                     "dumpsyse: --pid/--thread/--clients are not implemented.\n"
                     "          --thread and --clients need libbinderdebug; --pid needs\n"
                     "          a Parcel layout that changed after the compiled-against\n"
                     "          headers. --stability does work. Use the real dumpsys for\n"
                     "          the rest.\n");
        return 1;
    }

    const android::sp<android::IServiceManager> sm = internal::manager();
    if (sm == nullptr) {
        std::fprintf(stderr, "service manager unavailable\n");
        return 1;
    }

    // AOSP: `if (dumpTypeFlags == 0) dumpTypeFlags = TYPE_DUMP;`
    const int type_flags = o.dump_type_flags != 0 ? o.dump_type_flags : kTypeDump;

    const bool dumping_all = o.services.empty() && !o.list_only;
    int priority_flags = android::IServiceManager::DUMP_FLAG_PRIORITY_ALL;
    if (o.priority_type == "CRITICAL") {
        priority_flags = android::IServiceManager::DUMP_FLAG_PRIORITY_CRITICAL;
    } else if (o.priority_type == "HIGH") {
        priority_flags = android::IServiceManager::DUMP_FLAG_PRIORITY_HIGH;
    } else if (o.priority_type == "NORMAL") {
        priority_flags = android::IServiceManager::DUMP_FLAG_PRIORITY_NORMAL;
    }

    // Enumerated only when something needs the names: -l, or dumping everything.
    // A named service already carries its own name, and asking for ~280 of them
    // on every iteration of a loop is a wasted round trip.
    std::vector<std::string> all;
    if (dumping_all || o.list_only) all = list_services(false, priority_flags, o.as_proto);

    for (const std::string& s : all) {
        if (!is_skipped(o.skipped_services, s)) o.services.push_back(s);
    }

    if (!o.section.empty() && o.as_proto) {
        std::fprintf(stderr, "dumpsyse: --ext-section cannot be combined with --proto: the\n"
                             "          proto dump is binary and has no section headers.\n");
        return 1;
    }
    if (!o.section.empty() && !o.list_only && o.services.size() != 1) {
        std::fprintf(stderr,
                     "dumpsyse: --ext-section needs exactly one service, got %zu.\n"
                     "          usage: dumpsyse --ext-section \"<name>\" <service>\n",
                     o.services.size());
        return 1;
    }

    const size_t n = o.services.size();
    if (dumpsys_shows_service_list(sdk, n, o.list_only)) {
        std::printf("Currently running services:\n");
        for (const std::string& s : all) {
            // The filter dumpsys applies to the printed list, and only here: a
            // service it can name but not retrieve still gets dumped, where the
            // real tool prints "Can't find service: X".
            if (sm->checkService(android::String16(s.c_str())) == nullptr) continue;
            std::printf("  %s%s\n", s.c_str(),
                        is_skipped(o.skipped_services, s) ? " (skipped)" : "");
        }
    }
    if (o.list_only) return 0;

    const std::vector<std::string> dump_args = injected_dump_args(o, dumping_all);
    const std::string priority_type =
        (o.priority_type == "CRITICAL" || o.priority_type == "HIGH" || o.priority_type == "NORMAL")
            ? o.priority_type
            : std::string();

    // dumpsys returns 0 unconditionally, even when a service is missing or its
    // dump fails. Only a parse failure returns -1.
    for (const std::string& s : o.services) {
        if (is_skipped(o.skipped_services, s)) continue;
        const bool add_separator = n > 1;
        bool timed_out = false;

        // The header is written here and only after the service was found,
        // because AOSP prints it only if the dump thread started, which is only
        // if checkService succeeded. Printing it first puts a header above the
        // "Can't find service" that has to stand alone.
        //
        // With -w (API 37) the service is waited for rather than merely looked
        // up. getService() is the closest call available from the headers this
        // compiles against: upstream uses waitForService, which waits
        // indefinitely, and is declared only in newer headers than the ones the
        // floor API requires. Calling it through its vtable slot on a device
        // whose interface does not declare it would read a slot that is not
        // there. So -w waits as long as getService waits and reports
        // "Can't find service" after that. Recorded in docs/notes.md.
        const bool found = o.wait_for_service ? wait_for_service(s) : service_available(s);
        if (!found) {
            std::fprintf(stderr, "Can't find service: %s\n", s.c_str());
            continue;
        }
        if (add_separator) write_dump_header(s, priority_type);

        const auto start = std::chrono::steady_clock::now();
        std::string captured;
        // One call for both paths: the requested dump types go into the same
        // pipe as the dump either way, because AOSP writes them there.
        const long bytes = internal::dump_dumpsys(
            s, dump_args, type_flags, o.section.empty() ? STDOUT_FILENO : -1,
            static_cast<unsigned>(o.timeout_ms), &timed_out,
            o.section.empty() ? nullptr : &captured);

        if (!o.section.empty()) {
            // A truncated capture is not a dump: on this path the timeout notice
            // has to come first, before any extracted text, or a section cut off
            // mid-way looks exactly like a complete one.
            if (timed_out) write_timeout_notice(s, o.timeout_ms);
            if (bytes < 0) {
                // The dump failed. The streaming path would have shown whatever
                // arrived before the failure, so printing it unfiltered keeps
                // --ext-section from hiding output the plain tool shows.
                if (!captured.empty()) std::fwrite(captured.data(), 1, captured.size(), stdout);
                std::fflush(stdout);
                continue;
            }
            std::vector<std::string> body, candidates;
            switch (extract_section(captured, o.section, &body, &candidates, nullptr)) {
            case SectionMatch::kFound:
                for (const std::string& line : body) std::printf("%s\n", line.c_str());
                break;
            case SectionMatch::kAmbiguous:
                std::fprintf(stderr, "dumpsyse: --ext-section \"%s\" matches %zu sections in %s:\n",
                             o.section.c_str(), candidates.size(), s.c_str());
                for (const std::string& c : candidates) {
                    std::fprintf(stderr, "        %s\n", c.c_str());
                }
                std::fprintf(stderr, "          Narrow it down, or use one of the names above.\n");
                return 1;
            case SectionMatch::kNotFound:
                // Pass the dump through rather than printing nothing: the name may
                // simply be spelled differently from the header, and the captured
                // text is more useful than an empty result. The name is never
                // forwarded - the extension layer took it out of argv.
                std::fprintf(stderr,
                             "dumpsyse: --ext-section \"%s\" matched no header in %s; printing "
                             "the dump unchanged.\n",
                             o.section.c_str(), s.c_str());
                std::fwrite(captured.data(), 1, captured.size(), stdout);
                break;
            }
            continue;
        }

        // The real tool emits the timeout notice twice, once from writeDump and
        // once from main, and suppresses the first under --proto.
        if (timed_out && !o.as_proto) write_timeout_notice(s, o.timeout_ms);
        if (bytes >= 0 && add_separator) {
            write_dump_footer(s, std::chrono::duration<double>(
                                       std::chrono::steady_clock::now() - start).count());
        }
        if (timed_out) write_timeout_notice(s, o.timeout_ms);
    }
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// runners
// ---------------------------------------------------------------------------

int run_dumpsys_tool(int argc, char** argv, const ExtFlags& ext) {
    int rc = 0;
    long iteration = 0;
    for (;;) {
        if (iteration > 0) {
            std::printf("\n===== dumpsyse iteration %ld =====\n", iteration + 1);
            std::fflush(stdout);
        }
        rc = dumpsys_pass(argc, argv, ext);
        ++iteration;
        if (!ext.loop) break;
        if (ext.loop_count > 0 && iteration >= ext.loop_count) break;
        const struct timespec ts = {
            .tv_sec = static_cast<time_t>(ext.interval_ms / 1000),
            .tv_nsec = static_cast<long>((ext.interval_ms % 1000) * 1000000L),
        };
        nanosleep(&ts, nullptr);
    }
    return rc;
}

int run_cmd_tool(int argc, char** argv, const ExtFlags& ext) {
    const int sdk = device_sdk();

    // cmd has no getopt at all: it is positional, and it indexes argv[0] as the
    // FIRST ARGUMENT, so the program name has already been dropped by main.
    CmdMode mode = CmdMode::kRun;
    std::string service;
    std::vector<std::string> args;
    std::string error;
    if (!parse_cmd(argc, argv, &mode, &service, &args, &error, sdk)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 20;   // cmd returns 20 here
    }

    if (mode == CmdMode::kList) {
        // Unlike dumpsys's list, cmd's -l list IS filtered by checkService.
        std::printf("Currently running services:\n");
        for (const std::string& s :
             list_services(true, android::IServiceManager::DUMP_FLAG_PRIORITY_ALL, false)) {
            std::printf("  %s\n", s.c_str());
        }
        return 0;
    }

    // cmd -w means "start and wait". AOSP calls IServiceManager::waitForService
    // for that, which does not exist on the API 29 headers this compiles
    // against, and faking it would mean guessing a vtable slot. getService() is
    // documented as blocking a few seconds if the service does not exist yet -
    // the same behaviour, present on every supported release.
    const android::sp<android::IServiceManager> sm = internal::manager();
    if (sm == nullptr) {
        std::fprintf(stderr, "service manager unavailable\n");
        return 1;
    }
    if (mode == CmdMode::kWait) {
        // -w is "start and wait". Resolving it here, before the call, is what
        // makes it different from a plain run: getService blocks briefly for a
        // service that does not exist yet, so a caller asking for -w gets the
        // wait rather than an immediate "can't find".
        if (sm->getService(android::String16(service.c_str())) == nullptr) {
            // "cmd:", not "cmde:". The wording is AOSP's, and a caller
            // comparing the two tools must not be able to tell which binary
            // produced it. Same reasoning as leaving --help alone.
            std::fprintf(stderr, "cmd: Can't find service: %s\n", service.c_str());
            return 20;   // cmd returns 20, not 1, for a missing service
        }
    }

    // Resolved here, so "could not be reached" is a fact this function holds
    // rather than something inferred later from a negative return. The two are
    // different answers with different exit codes - 20 versus the status - and
    // conflating them is how a service that REFUSES a subcommand ends up
    // reported as a missing service.
    if (!service_available(service)) {
        std::fprintf(stderr, "cmd: Can't find service: %s\n", service.c_str());
        return 20;   // cmd returns 20 for a missing service
    }

    const Shape shape = ext.shape == 0 ? Shape::kNull : Shape::kReceiver;
    int status = 0;
    const int rc = shell(service, args, STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO, shape,
                         &status);
    if (status < 0) {
        // AOSP names four statuses explicitly and falls back to strerror for the
        // rest. Not cosmetic: "Failed transaction" and strerror(1) are different
        // text for one failure. cmd returns the raw status_t.
        const char* errstr;
        switch (status) {
        case -android::BAD_TYPE:           errstr = "Bad type"; break;
        case -android::FAILED_TRANSACTION: errstr = "Failed transaction"; break;
        case -android::FDS_NOT_ALLOWED:    errstr = "File descriptors not allowed"; break;
        case -android::UNEXPECTED_NULL:     errstr = "Unexpected null"; break;
        default:                            errstr = std::strerror(-status); break;
        }
        std::fprintf(stderr, "cmd: Failure calling service %s: %s (%d)\n", service.c_str(),
                     errstr, -status);
        return status;
    }
    return rc;
}

void dumpsyse_usage() {
    std::fprintf(stderr,
"dumpsyse - dumpsys, extended. Same flags, same output bytes, same exit codes,\n"
"running as uid 2000 in process: no fork, no app_process, no JVM per call.\n"
"\n"
"usage: dumpsyse [--ext-...] [dumpsys flags] [SERVICE [ARGS]]\n"
"\n"
"extensions (the reserved --ext- namespace; dumpsys knows none of them):\n"
"\n"
"  --ext-loop [N]      repeat the dump N times, or until interrupted\n"
"  --ext-interval MS   delay between iterations (default 1000)\n"
"  --ext-timeout MS    default bound on one dump; -t/-T still override it\n"
"  --ext-section NAME  print one section of the dump instead of all of it\n"
"  --ext-help          this text\n"
"\n"
"examples:\n"
"  dumpsyse -l                     same as: dumpsys -l\n"
"  dumpsyse power                  same as: dumpsys power\n"
"  dumpsyse --ext-loop 10 --ext-interval 500 power\n"
"  dumpsyse --ext-section \"Battery saving stats\" power\n");
}

void cmde_usage() {
    std::fprintf(stderr,
"cmde - cmd, extended. Same arguments, same output, same exit codes, running as\n"
"uid 2000 in process: no fork, no app_process, no JVM per call.\n"
"\n"
"usage: cmde [--ext-...] [-l] [-w] SERVICE [ARGS]\n"
"\n"
"extensions (the reserved --ext- namespace; cmd is positional and knows none):\n"
"\n"
"  --ext-shape null    no callback, no receiver: no binder thread pool, and\n"
"                      the service's exit code is not returned\n"
"  --ext-help          this text\n"
"\n"
"Without --ext-shape the service's own exit code is returned, exactly as cmd\n"
"does it. There is no per-service guess about who needs a receiver: that is a\n"
"fact about each service's own shell command, and it changes between releases.\n"
"\n"
"examples:\n"
"  cmde deviceidle whitelist +pkg  same as: cmd deviceidle whitelist +pkg\n"
"  cmde -l                        same as: cmd -l\n"
"  cmde --ext-shape null settings get secure ime\n");
}

}  // namespace etools
