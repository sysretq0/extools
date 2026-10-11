// SPDX-License-Identifier: GPL-3.0-only
#include "binder.h"

#include "device.h"
#include "dump_types.h"

#include <etools/args.hpp>

#include <etools/etools.h>

#include <binder/Binder.h>
#include <binder/IBinder.h>
#include <binder/IResultReceiver.h>
#include <binder/IServiceManager.h>
#include <binder/IShellCallback.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <utils/String8.h>
#include <utils/String16.h>

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>

namespace etools {
namespace {

using android::IBinder;
using android::IServiceManager;
using android::sp;
using android::status_t;
using android::String16;
using android::Vector;

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// NO TABLE OF SERVICES, and that is a decision rather than an omission.
//
// The shape question is "does this service need a receiver", which is a fact
// about one service's shellCommand implementation - and there are hundreds of
// them, each of them changing between releases, each of them forkable by an
// OEM. Listing the ones measured today would produce two failure modes and no
// signal: an entry that goes stale silently drops that service's exit code,
// and a service missing from the list is treated as unknown, which is the same
// problem wearing a different hat. A list whose wrongness cannot be observed is
// worse than no list.
//
// So the default is the shape that cannot be wrong - the receiver, which is what
// the real tool uses and which therefore cannot diverge from it - and null is
// something a caller ASKS for, having decided that it does not need the exit
// code. An embedder that knows its service pays nothing; one that does not know
// keeps parity.

// How long a one-way status is waited for once the call has returned. Long
// compared to delivery (microseconds on a warm binder), short compared to a
// user. Unbounded waits hang on services that never report at all.
constexpr auto kResultWait = std::chrono::milliseconds(200);

// Receives the status a service reports through IResultReceiver.
//
// THE INTERFACE TOKEN MUST BE SKIPPED FIRST. The parcel arrives at position 0,
// which is not the payload: BBinder::transact() resets the position and the
// buffer starts with the RPC header - StrictMode policy, WorkSource, the 'SYST'
// magic on API 31+, then the descriptor. AOSP reads it with CHECK_INTERFACE
// before anything else. Skipping it by hand is wrong on some release and a
// silent wrong answer on all of them: readInt32() then returns a policy mask
// or a UTF-16 length, which is a plausible small integer, so a failing command
// exits 0 and a succeeding one does not. checkInterface() is one call into the
// DEVICE's libbinder, which knows this ROM's header shape.
class ResultReceiver : public android::BnResultReceiver {
public:
    status_t onTransact(uint32_t code, const android::Parcel& data, android::Parcel* reply,
                        uint32_t flags) override {
        if (code != IBinder::FIRST_CALL_TRANSACTION) {   // IResultReceiver::OP_SEND
            return android::BnResultReceiver::onTransact(code, data, reply, flags);
        }
        if (!data.checkInterface(this)) return android::PERMISSION_DENIED;
        int32_t result = 0;
        data.readInt32(&result);
        // Anything after the code is NOT ours to interpret. Two different
        // interfaces share this descriptor and this transaction: the native one
        // sends the int32 alone, the Java one (send(int, in Bundle), same
        // descriptor string, frameworks/base) appends a Bundle. Reading a
        // String16 here parses whatever follows as a string - which is how a
        // predecessor ended up printing a Bundle blob as a "message" - so the
        // tail is left alone. See docs/compat_matrix.md.
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = result;
        have_result_ = true;
        cond_.notify_all();
        return android::NO_ERROR;
    }

    // Never called: onTransact handles OP_SEND itself, which is what keeps the
    // device's own send() - the one that would call the version-dependent
    // virtual - from ever running.
    void send(int32_t) override {}

    int result_or(int fallback) {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait_for(lock, kResultWait, [this] { return have_result_; });
        return have_result_ ? result_ : fallback;
    }

private:
    std::mutex mutex_;
    std::condition_variable cond_;
    bool have_result_ = false;
    int32_t result_ = 0;
};

// What a service asks its caller for when it wants an output file.
//
// Refused, and refused in the shape AOSP's own base class writes: a bare
// -EPERM is read by IShellCallback::openFile as an EXCEPTION CODE rather than
// a descriptor, so the service would see a malformed parcel instead of a clean
// "no". Verified: the service turns this into an error line on its err fd.
class ShellCallback : public android::BnShellCallback {
public:
    status_t onTransact(uint32_t code, const android::Parcel& data, android::Parcel* reply,
                        uint32_t flags) override {
        if (code != IBinder::FIRST_CALL_TRANSACTION) {   // OP_OPEN_OUTPUT_FILE
            return android::BnShellCallback::onTransact(code, data, reply, flags);
        }
        if (!data.checkInterface(this)) return android::PERMISSION_DENIED;
        String16 path, context, mode;
        data.readString16(&path);
        data.readString16(&context);
        data.readString16(&mode);
        if (reply != nullptr) {
            reply->writeNoException();
            reply->writeInt32(0);   // 0 == no file descriptor follows
        }
        return android::NO_ERROR;
    }

    // Never called: onTransact handles the transaction itself.
    int openFile(const String16&, const String16&, const String16&) override { return -EPERM; }
};

}  // namespace

namespace internal {

Vector<String16> to16(const std::vector<std::string>& args) {
    Vector<String16> out;
    for (const std::string& a : args) out.push_back(String16(a.c_str()));
    return out;
}

sp<IBinder> find(const IServiceManager* sm, const std::string& service) {
    return sm->checkService(String16(service.c_str()));
}

std::string to_utf8(const String16& s) { return android::String8(s).string(); }

sp<IServiceManager> manager() {
    if (android::ProcessState::self().get() == nullptr) return nullptr;
    return android::defaultServiceManager();
}

void ensure_pool() { android::ProcessState::self()->startThreadPool(); }

}  // namespace internal

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

const char* version() { return ETOOLS_VERSION_STRING; }

void pool_start() { internal::ensure_pool(); }

bool service_available(const std::string& service) {
    const sp<IServiceManager> sm = internal::manager();
    return sm != nullptr && internal::find(sm.get(), service) != nullptr;
}

std::vector<std::string> list_services(bool only_retrievable, int priority_flags,
                                       bool proto_only) {
    std::vector<std::string> out;
    const sp<IServiceManager> sm = internal::manager();
    if (sm == nullptr) return out;

    const int flags =
        priority_flags >= 0 ? priority_flags : IServiceManager::DUMP_FLAG_PRIORITY_ALL;
    auto collect = [&out](const Vector<String16>& v) {
        out.reserve(out.size() + v.size());
        for (size_t i = 0; i < v.size(); ++i) out.push_back(internal::to_utf8(v[i]));
    };
    collect(sm->listServices(flags));

    if (proto_only) {
        // AOSP intersects the priority list with the proto list; the proto dump
        // is the only thing that identifies which services speak it.
        Vector<String16> proto = sm->listServices(IServiceManager::DUMP_FLAG_PROTO);
        std::vector<std::string> have;
        have.reserve(proto.size());
        for (size_t i = 0; i < proto.size(); ++i) have.push_back(internal::to_utf8(proto[i]));
        std::sort(out.begin(), out.end());
        std::sort(have.begin(), have.end());
        std::vector<std::string> both;
        std::set_intersection(have.begin(), have.end(), out.begin(), out.end(),
                              std::back_inserter(both));
        out.swap(both);
    } else {
        std::sort(out.begin(), out.end());
    }

    if (only_retrievable) {
        std::vector<std::string> filtered;
        filtered.reserve(out.size());
        for (const std::string& s : out) {
            if (sm->checkService(String16(s.c_str())) != nullptr) filtered.push_back(s);
        }
        out.swap(filtered);
    }
    return out;
}

namespace {

// One dump, either streamed to a descriptor or captured into a string. Capture
// is not a nicety: a section's end is not known until the next header arrives,
// which may be never, so --section cannot work on a stream.
long dump_into(const std::string& service, const std::vector<std::string>& args, int out_fd,
               std::string* capture, unsigned timeout_ms, bool* timed_out, int type_flags = 0) {
    if (timed_out != nullptr) *timed_out = false;
    const sp<IServiceManager> sm = internal::manager();
    if (sm == nullptr) return -1;

    const sp<IBinder> target = internal::find(sm.get(), service);
    if (target == nullptr) return -1;

    int fds[2];
    if (pipe(fds) != 0) return -1;

    // A PIPE, which is what AOSP's dumpsys uses and what every service tested
    // accepts. A socketpair is refused by SELinux for SurfaceFlinger's domain,
    // and a temp file we create is refused too.
    Vector<String16> a16 = internal::to16(args);

    // dump() is synchronous and unbounded: calling it on this thread while the
    // output goes anywhere else fills the pipe, blocks the service in write(),
    // and never returns - which wedges the service and trips the watchdog.
    status_t err = android::UNKNOWN_ERROR;
    std::thread dumper([&] {
        // AOSP writes the --pid/--stability/... output into the same pipe, from
        // this same thread, BEFORE the service dump - so it has to be here and
        // not after the join, or the two lines swap places.
        //
        // A failure here is deliberately NOT stored in `err`: that is the dump's
        // status and is reported below, so sharing it would print one failure
        // twice under two different labels.
        if (type_flags != 0 && (type_flags & ~kTypeDump)) {
            write_dump_types(fds[1], target, type_flags);
        }
        if (type_flags == 0 || (type_flags & kTypeDump)) {
            err = target->dump(fds[1], a16);
        } else {
            err = android::NO_ERROR;
        }
        close(fds[1]);   // the reader's EOF, once the service is finished
    });

    long total = 0;
    const int64_t deadline = timeout_ms > 0 ? now_ms() + timeout_ms : 0;
    for (;;) {
        // Each wait is against the deadline, not a fixed slice, so a service
        // that dribbles its answer out cannot hold the caller indefinitely.
        int slice = -1;
        if (deadline != 0) {
            const int64_t left = deadline - now_ms();
            if (left <= 0) {
                if (timed_out != nullptr) *timed_out = true;
                break;
            }
            slice = static_cast<int>(left);
        }
        struct pollfd pfd { fds[0], POLLIN, 0 };
        const int n = poll(&pfd, 1, slice);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) {
            if (timed_out != nullptr) *timed_out = true;   // the bound expired, not EOF
            break;
        }
        char buf[8192];
        const ssize_t rn = read(fds[0], buf, sizeof buf);
        if (rn > 0) {
            const size_t want = static_cast<size_t>(rn);
            if (capture != nullptr) {
                capture->append(buf, want);
            } else {
                size_t done = 0;
                while (done < want) {
                    const ssize_t wn = write(out_fd, buf + done, want - done);
                    if (wn <= 0) {
                        if (errno == EINTR) continue;
                        break;   // the caller's descriptor is gone; stop copying
                    }
                    done += static_cast<size_t>(wn);
                }
            }
            total += rn;
            continue;
        }
        if (rn == 0) break;   // the service is finished
        if (errno != EINTR) break;
    }
    close(fds[0]);

    // Always joined. On timeout the read end is already closed, so the service's
    // pending write fails with EPIPE and it unwinds; abandoning a thread
    // mid-transaction is worse than waiting for it.
    dumper.join();
    if (err != android::NO_ERROR) return -1;
    return total;
}

}  // namespace

long dump(const std::string& service, const std::vector<std::string>& args, int out_fd,
          unsigned timeout_ms, bool* timed_out) {
    return dump_into(service, args, out_fd, nullptr, timeout_ms, timed_out);
}

long dump_capture(const std::string& service, const std::vector<std::string>& args,
                  std::string* out, unsigned timeout_ms, bool* timed_out) {
    out->clear();
    return dump_into(service, args, -1, out, timeout_ms, timed_out);
}

namespace internal {

long dump_dumpsys(const std::string& service, const std::vector<std::string>& args,
                  int type_flags, int out_fd, unsigned timeout_ms, bool* timed_out,
                  std::string* capture) {
    return dump_into(service, args, out_fd, capture, timeout_ms, timed_out, type_flags);
}

}  // namespace internal

int shell(const std::string& service, const std::vector<std::string>& args, int in, int out,
          int err, Shape shape, int* binder_status) {
    if (binder_status != nullptr) *binder_status = 0;
    const sp<IServiceManager> sm = internal::manager();
    if (sm == nullptr) return -1;

    const Shape used = shape;
    const sp<IBinder> target = internal::find(sm.get(), service);
    if (target == nullptr) return -1;

    Vector<String16> a16 = internal::to16(args);

    if (used == Shape::kNull) {
        // The null shape: nothing is handed out, so no inbound transaction can
        // exist, so there is no looper to host one and no thread to start.
        // Output still arrives - it rides out_fd and err_fd, which the service
        // dups and writes to directly. What is lost is the service's exit code.
        const status_t rc = IBinder::shellCommand(target, in, out, err, a16, nullptr, nullptr);
        if (binder_status != nullptr) *binder_status = static_cast<int>(rc);
        return rc < 0 ? static_cast<int>(rc) : 0;
    }

    // The receiver shape: a service may call back while producing output, and
    // only a thread joined to the pool can receive that transaction. The exit
    // code arrives as a one-way send, which is why this shape exists at all.
    internal::ensure_pool();
    sp<ShellCallback> callback = new ShellCallback();
    sp<ResultReceiver> receiver = new ResultReceiver();

    const status_t rc = IBinder::shellCommand(target, in, out, err, a16, callback, receiver);
    if (binder_status != nullptr) *binder_status = static_cast<int>(rc);
    if (rc < 0) return static_cast<int>(rc);
    return receiver->result_or(0);
}

bool query(const std::string& service, const std::vector<std::string>& args, std::string* out,
           unsigned timeout_ms, bool* complete) {
    out->clear();
    if (complete != nullptr) *complete = false;

    const sp<IServiceManager> sm = internal::manager();
    if (sm == nullptr) return false;
    const sp<IBinder> target = internal::find(sm.get(), service);
    if (target == nullptr) return false;

    int fds[2];
    if (pipe(fds) != 0) return false;

    Vector<String16> a16 = internal::to16(args);
    // Null shape, always: this is the shape an embedder wants, and a bounded
    // capture has nothing to receive - the answer is what was written to the fd.
    const status_t rc =
        IBinder::shellCommand(target, STDIN_FILENO, fds[1], fds[1], a16, nullptr, nullptr);
    // The write end is dropped here. The service holds its own duplicate, so
    // closing this one turns its finished answer into an EOF instead of leaving
    // the read below waiting out the whole bound every time.
    close(fds[1]);
    if (rc != android::NO_ERROR) {
        close(fds[0]);
        return false;
    }

    const int64_t deadline = now_ms() + static_cast<int64_t>(timeout_ms);
    for (;;) {
        const int64_t left = deadline - now_ms();
        if (left <= 0) break;
        struct pollfd pfd { fds[0], POLLIN, 0 };
        const int n = poll(&pfd, 1, static_cast<int>(left));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;   // the bound expired, not EOF
        char buf[4096];
        const ssize_t rn = read(fds[0], buf, sizeof buf);
        if (rn > 0) {
            out->append(buf, static_cast<size_t>(rn));
            continue;
        }
        if (rn == 0) {
            if (complete != nullptr) *complete = true;
            break;
        }
        if (errno != EINTR) break;
    }
    close(fds[0]);
    return true;
}

}  // namespace etools

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

extern "C" {

long etools_dump(const char* service, const char* const* args, int out_fd, unsigned timeout_ms,
                 int* timed_out) {
    if (service == nullptr) return -1;
    std::vector<std::string> a;
    for (int i = 0; args != nullptr && args[i] != nullptr; ++i) a.emplace_back(args[i]);
    bool to = false;
    const long n = etools::dump(service, a, out_fd, timeout_ms, &to);
    if (timed_out != nullptr) *timed_out = to ? 1 : 0;
    return n;
}

int etools_shell(const char* service, const char* const* args, int in_fd, int out_fd,
                 int err_fd, int shape, int* out_status) {
    if (out_status != nullptr) *out_status = 0;
    if (service == nullptr) return -1;
    std::vector<std::string> a;
    for (int i = 0; args != nullptr && args[i] != nullptr; ++i) a.emplace_back(args[i]);
    return etools::shell(service, a, in_fd, out_fd, err_fd, static_cast<etools::Shape>(shape),
                         out_status);
}

int etools_query(const char* service, const char* const* args, char* out, size_t out_cap,
                 unsigned timeout_ms, size_t* out_len, int* complete) {
    if (service == nullptr || out == nullptr || out_cap == 0) return -1;
    std::vector<std::string> a;
    for (int i = 0; args != nullptr && args[i] != nullptr; ++i) a.emplace_back(args[i]);
    std::string got;
    bool done = false;
    if (!etools::query(service, a, &got, timeout_ms, &done)) {
        out[0] = '\0';
        if (out_len != nullptr) *out_len = 0;
        if (complete != nullptr) *complete = 0;
        return -1;
    }
    const size_t n = std::min(got.size(), out_cap - 1);
    std::memcpy(out, got.data(), n);
    out[n] = '\0';
    if (out_len != nullptr) *out_len = n;
    if (complete != nullptr) *complete = done ? 1 : 0;
    return 0;
}

int etools_device_sdk(void) { return etools::device_sdk(); }

const char* etools_version(void) { return ETOOLS_VERSION_STRING; }

}  // extern "C"
