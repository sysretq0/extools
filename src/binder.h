// SPDX-License-Identifier: GPL-3.0-only
// Binder transport. The only translation unit that includes <binder/*>.
//
// Everything above this file speaks in descriptors and exit codes; the shapes,
// the pool, the receivers and the threads all live here.
#ifndef ETOOLS_BINDER_H
#define ETOOLS_BINDER_H

#include <binder/IBinder.h>
#include <binder/IServiceManager.h>
#include <utils/String16.h>

#include <string>
#include <vector>

#include <etools/etools.hpp>

namespace etools {
namespace internal {

// Android's String16 -> UTF-8. String8 does the real encoding; a per-code-unit
// cast mangles anything above 0x7F and splits surrogate pairs.
std::string to_utf8(const android::String16& s);

// The service manager, having made this process a real Binder client (which
// opens /dev/binder). Null only if ProcessState is unavailable. vndbinder is
// for vendor processes and cannot reach framework services.
android::sp<android::IServiceManager> manager();

// Starts the pool once. A service can only call back into a process on a thread
// joined to its pool, so a call that hands out a binder object without this
// makes every callback a dead transaction.
void ensure_pool();

// One service's dump, captured into a string instead of streamed. The reason
// this exists rather than a flag on dump(): a section's end is not known until
// the next header arrives, which may be never, so a stream cannot be filtered.
long dump_capture(const std::string& service, const std::vector<std::string>& args,
                  std::string* out, unsigned timeout_ms, bool* timed_out);

// A dump as the dumpsys TOOL performs it: the requested dump types are written
// into the same pipe first, from the same thread, before the service dump, so
// the two arrive in AOSP's order.
//
// Not part of the public API. --pid/--stability/--thread/--clients are dumpsys
// flags, not a property of dumping a service, and an embedder calling dump()
// wants the dump and nothing else.
long dump_dumpsys(const std::string& service, const std::vector<std::string>& args,
                  int type_flags, int out_fd, unsigned timeout_ms, bool* timed_out,
                  std::string* capture);

}  // namespace internal
}  // namespace etools

#endif  // ETOOLS_BINDER_H
