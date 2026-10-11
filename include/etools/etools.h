// SPDX-License-Identifier: GPL-3.0-only
// etools - cmd and dumpsys, in process, as a library.
//
// Two operations, both spoken directly to the framework over Binder:
//   etools_dump()  one service's dump, through the DUMP_TRANSACTION
//   etools_shell() one shell command in a service, through shellCommand()
//
// No fork, no exec, no app_process, no JVM, no /system/bin/dumpsys.
//
// LINKING. The library links the DEVICE's libbinder.so, not the NDK stub: the
// NDK binder API cannot transact against a system service at all (a service
// manager proxy cannot be associated with an AIBinder_Class, so
// AIBinder_prepareTransaction returns STATUS_INVALID_OPERATION). An embedder
// therefore needs the AOSP private headers to compile against and the device's
// own libraries at runtime - ./scripts/setup.sh fetches both. Requires
// Android 8.0 (API 26) or newer.
//
// THREADS. See Shape. By default a call hands the service nothing it can call
// back on, so no Binder thread pool is started and the calling thread is the
// only one. Output still arrives, because output rides the file descriptors
// passed to the call, not the callback.
//
// EXIT CODES. Only the RECEIVER shape can return the service's own exit code.
// Under NULL you get the Binder transaction status, which says the call
// succeeded, not what the service decided. That trade is the whole difference
// between the two shapes.
#ifndef ETOOLS_ETOOLS_H
#define ETOOLS_ETOOLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ETOOLS_VERSION_MAJOR 0
#define ETOOLS_VERSION_MINOR 1
#define ETOOLS_VERSION_PATCH 0
#define ETOOLS_VERSION_STRING "0.1.0"

/* Shape of a shellCommand call. The only thing these differ in is whether the
   caller hands the service binder objects it can call back on.

   There is no per-service default on purpose. "Which services need a receiver"
   is a fact about hundreds of individual shellCommand implementations, each of
   them release-dependent and each forkable by an OEM, so a table of them could
   be wrong with nothing able to observe it. RECEIVER is the default because it
   is the shape the real tool uses and therefore cannot diverge from it; NULL is
   something a caller asks for having decided it does not need the exit code. */
enum etools_shape {
    /* No callback, no receiver. No pool, no thread, no exit code. */
    ETOOLS_SHAPE_NULL = 0,
    /* Real IShellCallback + IResultReceiver. Starts the Binder pool on first
       use and returns the service's own exit code. This is the default. */
    ETOOLS_SHAPE_RECEIVER = 1
};

/* Dump one service to out_fd through the DUMP_TRANSACTION.
 *
 * args is a NULL-terminated argv for the service (may be NULL). The dump runs
 * on its own thread and is drained here while it is in flight: the service
 * blocks once the pipe is full and would never return otherwise.
 *
 * timeout_ms bounds the call; 0 means unbounded. On timeout the read end is
 * closed, so the service's pending write fails and it unwinds, and *timed_out
 * (if given) is set. Bytes written to out_fd are returned; -1 on failure. */
long etools_dump(const char* service, const char* const* args, int out_fd,
                 unsigned timeout_ms, int* timed_out);

/* Run one shell command in a service, in process.
 *
 * in_fd/out_fd/err_fd are handed to the service directly, exactly as the real
 * tool does, so its output lands where the caller put it. shape is one of
 * enum etools_shape.
 *
 * Returns, for RECEIVER, the service's own exit code; for NULL and AUTO that
 * resolves to NULL, the Binder status (0 = the transaction succeeded). A
 * service that reports no exit code at all yields 0 under RECEIVER, after a
 * bounded wait - never a hang.
 *
 * The return value is NOT a failure indicator on its own: a service that
 * rejects an unknown subcommand reports -1, which is indistinguishable from a
 * failed transaction by value. out_status (may be NULL) receives the
 * transaction's own status - 0, or negative if the call never completed - and
 * is how a caller tells the two apart. -1 is returned with no status set when
 * the service could not be reached at all. */
int etools_shell(const char* service, const char* const* args,
                 int in_fd, int out_fd, int err_fd, int shape, int* out_status);

/* Query-shaped variant: capture the service's output into a buffer instead of
 * handing it a descriptor, always with a bound on the call. This is the shape
 * an embedder wants - daemon-style, no pool, no threads, no fork.
 *
 * Writes at most out_cap bytes including a terminating NUL, sets *out_len (if
 * given) to the number of bytes written excluding the NUL, and returns 0. Sets
 * *complete (if given) to 0 when the bound expired before the service
 * finished - the buffer then holds whatever arrived, which may be nothing.
 * Returns -1 if the service does not exist or the pipe could not be made. */
int etools_query(const char* service, const char* const* args,
                 char* out, size_t out_cap, unsigned timeout_ms,
                 size_t* out_len, int* complete);

/* The running device's API level, 0 when the property is unreadable. One
   cached read. Public because the API level selects which dumpsys/cmd
 * behaviour applies - accepted flags, usage text, error wording - and an
   embedder reproducing tool behaviour needs it. */
int etools_device_sdk(void);

const char* etools_version(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ETOOLS_ETOOLS_H */
