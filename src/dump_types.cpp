// SPDX-License-Identifier: GPL-3.0-only
#include "dump_types.h"

#include <etools/args.hpp>

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace etools {
namespace {

// AOSP's WriteStringToFd: retry on short writes and EINTR, and swallow failure
// exactly as AOSP does.
void write_string(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = write(fd, s.data() + off, s.size() - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;
        }
        off += static_cast<size_t>(n);
    }
}

}  // namespace

// The status name, or strerror() where the device has no statusToString.
//
// Only reached on API 30+, which is exactly where statusToString exists
// (dumpsys_error_style is 0 below that, and that branch uses strerror anyway).
// The null check is what makes the weak declaration in dump_types.h safe rather
// than merely loadable: if a future floor drops the symbol, this degrades to
// the API 26 wording instead of jumping through a null function pointer.
static std::string status_name(android::status_t err) {
    if (::android::statusToString != nullptr) return ::android::statusToString(err);
    return strerror(err);
}

void report_dump_error(int sdk, const std::string& service_name, android::status_t err,
                       const char* context) {
    // AOSP reworded this twice: API 29-32 have no notion of naming the operation
    // that failed, and only API 33 introduced per-type contexts.
    switch (dumpsys_error_style(sdk)) {
    case 0:
        // AOSP passes the NEGATIVE status straight to strerror(), which wants a
        // positive errno. That is an AOSP bug, reproduced verbatim because the
        // point here is byte-identical output.
        std::fprintf(stderr, "Error dumping service info: (%s) %s\n", strerror(err),
                     service_name.c_str());
        return;
    case 1:
        std::fprintf(stderr, "Error dumping service info status_t: %s %s\n",
                     status_name(err).c_str(), service_name.c_str());
        return;
    default:
        std::fprintf(stderr, "Error with service '%s' while %s: %s\n", service_name.c_str(),
                     context, status_name(err).c_str());
        return;
    }
}

void write_dump_types(int fd, const android::sp<android::IBinder>& service, int type_flags) {
    if (type_flags & kTypeStability) {
        // The address of a weak function is null where the loader found no
        // definition, i.e. on API 26-30 where libbinder has no Stability at all.
        // So the flag degrades to "unavailable" instead of being a hard load-time
        // dependency -- the same thing real dumpsys effectively offered, since
        // --stability did not exist before API 33. The wording is deliberately not
        // real dumpsys wording (audit finding F11, verdict INTENDED).
        auto* fn = &android::internal::Stability::debugToString;
        write_string(fd, "Stability: " +
                             (fn ? fn(service) : std::string("unavailable on this Android version")) +
                             "\n");
    }
}

}  // namespace etools