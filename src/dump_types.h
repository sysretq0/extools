// SPDX-License-Identifier: GPL-3.0-only
// dumpsys's --pid and --stability dump types.
//
// Both are separate IBinder operations rather than dump arguments, and both
// arrived after API 26 - the oldest release etools supports. So neither can be
// written directly against the headers we compile against. Neither needs the
// headers moved forward, which is what matters: pinning them to a newer branch
// would admit newer-only symbols everywhere and silently drop support for the
// older devices the API 26 headers exist to keep working.
//
// --stability has no public equivalent, but its symbol is declared WEAK below.
// It is a STATIC member function, so declaring the class reproduces the mangled
// name exactly:
//   _ZN7android8internal9Stability13debugToStringERKNS_2spINS_7IBinderEEE
// Weak leaves that name compiler-derived and the reference undefined: the loader
// fills it in from the device's libbinder where the symbol exists (API 31+) and
// leaves it null where it does not. So a wrong or missing symbol is a null check
// at the call site, never a garbage call. The link does NOT fail on it -- that is
// the whole point, since the symbol is absent before API 31. (The previous
// revision dlsym'd it, which hardcoded that mangled name as a string literal and
// would have degraded silently forever had it drifted.)
//
// --pid is deliberately NOT here. IBinder::getDebugPid is a non-virtual libbinder
// symbol added after API 29, so it cannot be called from the headers we compile
// against, and both workarounds are worse than refusing; etools.cpp carries the
// checked reasons, per flag.

#pragma once

#include <binder/IBinder.h>

#include <sys/types.h>

#include <string>

namespace android { namespace internal {
// android::internal::Stability is API 31+ and absent on API 26/27/28/29/30.
// Declared here __attribute__((weak)) rather than called directly: an undefined
// weak reference is filled in by the already-linked libbinder where it exports the
// symbol and stays null where it does not, so etools never carries a hard load-time
// dependency on it.
class Stability {
public:
    // Weak: absent before API 31. See the file comment for why that is a null
    // check rather than a link error.
    __attribute__((weak)) static std::string debugToString(const android::sp<android::IBinder>&);
};
}}  // namespace android::internal

namespace android {
// android::statusToString(status_t), from system/core/libutils/Errors.cpp.
//
// The signature must match AOSP's EXACTLY: it returns std::string, not
// const char*. Returning a std::string by value is an AAPCS64 *indirect*
// result - the caller allocates the buffer and passes its address in x8, and
// the callee returns that same pointer in x0. A caller declaring const char*
// passes no x8 and reads x0 as the pointer, so the callee writes 24 bytes of
// std::string through whatever x8 happened to hold. Measured: SIGSEGV, exit 139.
//
// The mangled name cannot catch this. C++ does not encode a function's return
// type, so `std::string f(int)` and `const char* f(int)` are both
// _ZN7android14statusToStringEi and the link SUCCEEDS. This is the one class of
// bug the compiler and the linker are both blind to, which is why it survived
// a byte-identical comparison against /system/bin/dumpsys: the dump-failure
// path was never taken during testing.
//
// Weak, because the symbol is API 30+. Errors.cpp does not exist in
// android-8.0.0_r1 (26), android-9.0.0_r1 (27) or android10-release (29); it
// appears in android11-release (30) and every branch after. A hard reference
// with BIND_NOW - which both build paths produce - is a load-time failure, not
// a link warning: etools would not start at all on a quarter of the releases it
// claims to support. Same reasoning, and the same null check, as Stability above.
__attribute__((weak)) std::string statusToString(android::status_t status);
}  // namespace android

namespace etools {

// The `what` AOSP passes to reportDumpError for a failed dump, verbatim. It appears
// in "Error with service 'X' while dumping: ..." (API 33+ wording).
inline constexpr const char* kDumpContext = "dumping";

// Writes the requested dump type to `fd`. The regular service dump runs after this.
//
// Only --stability is implemented; --pid, --thread and --clients are refused in
// etools.cpp, where the comment for each says why it is unreachable from the API 29
// headers. Returns void because the one implemented type cannot fail.
void write_dump_types(int fd, const android::sp<android::IBinder>& service, int type_flags);

// Reports a failed dump in whichever of the three formats this API level uses.
// `context` ("dumping PID", "dumping", ...) is only used by API 33+, which is
// the only one that names the operation.
void report_dump_error(int sdk, const std::string& service_name, android::status_t err,
                       const char* context);

}  // namespace etools