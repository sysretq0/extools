// SPDX-License-Identifier: GPL-3.0-only
// etools - the C++ surface over the C ABI in <etools/etools.h>.
//
// Same library, same two operations, spelled in C++: std::string and
// std::vector instead of C strings and arrays, and the shape policy as an
// enum instead of an int. Include <etools/etools.h> too if you need the C
// entry points.
//
// What is NOT here: the dumpsys/cmd argument parsers, the per-release
// compatibility tables and the section extractor. Those are public headers of
// their own, because an embedder reproducing tool behaviour needs them and
// re-deriving them is how tool clones drift:
//
//   <etools/args.hpp>     parse a dumpsys or cmd command line for a given API
//   <etools/compat.hpp>   the per-release behaviour tables, generated
//   <etools/section.hpp>  pull one section out of a captured dump
#ifndef ETOOLS_ETOOLS_HPP
#define ETOOLS_ETOOLS_HPP

#include <cstddef>
#include <string>
#include <vector>

#include <etools/etools.h>

namespace etools {

// Which binder objects a call hands the service. See Shape in etools.h.
//
// There is deliberately no kAuto and no per-service policy: "which services
// need a receiver" is a fact about hundreds of individual shellCommand
// implementations, each release-dependent and each forkable by an OEM, so a
// list of them could be wrong without anything observing it. The default is
// therefore the shape that cannot be wrong - the receiver, which is what the
// real tool uses - and null is something a caller asks for having decided it
// does not need the exit code.
enum class Shape {
    kNull = ETOOLS_SHAPE_NULL,
    kReceiver = ETOOLS_SHAPE_RECEIVER,
};

// Makes the Binder pool exist, if it does not. Not needed for either shape:
// kReceiver starts it on demand, and kNull never needs it. Exposed only for an
// embedder that hands a service a binder object of its own - one it can then be
// called back on, which is the only reason to want a pool without a receiver.
void pool_start();

// One service's dump, streamed to out_fd. Returns bytes written, -1 on
// failure. `timed_out` may be null.
long dump(const std::string& service, const std::vector<std::string>& args, int out_fd,
          unsigned timeout_ms = 0, bool* timed_out = nullptr);

// One shell command in a service. in/out/err are handed to the service as-is.
//
// `binder_status`, if given, receives the transaction's own status: 0, or a
// negative Binder status meaning the call never completed.
//
// IT IS NOT OPTIONAL INFORMATION. The return value is the service's exit code,
// which can itself be negative - a service that rejects an unknown subcommand
// reports -1, and that is indistinguishable from a failed transaction by value
// alone. Reporting the two through different channels is the only way a caller
// can tell "the service said no" from "the service was never reached", and they
// are answered differently: a refusal is the service's exit code, an
// unreachable service is not.
int shell(const std::string& service, const std::vector<std::string>& args, int in, int out,
          int err, Shape shape = Shape::kReceiver, int* binder_status = nullptr);

// One shell command with its output captured, bounded, and no pool. `complete`
// (if given) is cleared when the bound expired before the service finished; the
// buffer then holds whatever arrived. Returns false only if the service does
// not exist or the pipe could not be made.
bool query(const std::string& service, const std::vector<std::string>& args, std::string* out,
           unsigned timeout_ms = 2000, bool* complete = nullptr);

// Service names, sorted, as listServices() returns them. `only_retrievable`
// applies the checkService() filter the real tools apply to `-l`, which drops
// the services a shell-uid caller can name but not retrieve (netd, installd,
// incident, ...).
std::vector<std::string> list_services(bool only_retrievable = false, int priority_flags = -1,
                                       bool proto_only = false);

// Whether the service can be reached at all. Cheap: one service-manager lookup.
bool service_available(const std::string& service);

// Like service_available(), but waits briefly for a service that has not started
// yet instead of requiring it to exist already.
bool wait_for_service(const std::string& service);

// ro.build.version.sdk, read once and cached. 0 when the property is
// unreadable, which every lookup resolves as "assume an old device".
int device_sdk();

const char* version();

}  // namespace etools

#endif  // ETOOLS_ETOOLS_HPP
