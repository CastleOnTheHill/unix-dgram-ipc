/*
 * ipc_util.h -- logging, errno mapping and small helpers.
 */
#ifndef IPC_UTIL_H
#define IPC_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "ipc/ipc.h"

typedef enum {
    IPC_LOG_ERROR = 0,
    IPC_LOG_WARN  = 1,
    IPC_LOG_INFO  = 2,
    IPC_LOG_DEBUG = 3
} ipc_log_level_t;

/* Default level comes from the IPC_LOG_LEVEL env var (error|warn|info|debug),
 * falling back to warn. Quiet enough for tests, verbose enough to debug. */
void ipc_log_set_level(ipc_log_level_t lvl);
ipc_log_level_t ipc_log_get_level(void);
void ipc_logf(ipc_log_level_t lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define IPC_LOGE(...) ipc_logf(IPC_LOG_ERROR, __VA_ARGS__)
#define IPC_LOGW(...) ipc_logf(IPC_LOG_WARN, __VA_ARGS__)
#define IPC_LOGI(...) ipc_logf(IPC_LOG_INFO, __VA_ARGS__)
#define IPC_LOGD(...) ipc_logf(IPC_LOG_DEBUG, __VA_ARGS__)

/* Map a failing-errno value to the library error space.
 * ipc_errno_to_rc(0) == IPC_OK, which is deliberately allowed so that
 * `rc = ipc_errno_to_rc(errno)` is safe on a success path too.  Every
 * non-zero errno maps to a negative ipc_err_t. */
int ipc_errno_to_rc(int err);

/* Bounded copy; always NUL terminates.  Returns the source length. */
size_t ipc_strlcpy(char *dst, const char *src, size_t cap);

/* Monotonic nanoseconds since an arbitrary epoch. */
uint64_t ipc_mono_ns(void);

/* Wall-clock nanoseconds since the Unix epoch (for logs/journal only). */
uint64_t ipc_real_ns(void);

/* splitmix64 finalizer: used to decorrelate the instance-id mix. */
uint64_t ipc_mix64(uint64_t x);

/* Generate a process instance id that is unique for practical purposes:
 * hash(boot_id) + pid + monotonic nanoseconds, mixed.  Two successive
 * registrations in the same process already differ. */
uint64_t ipc_gen_instance_id(void);

/* Read the whole of `path` into a malloc'd NUL-terminated buffer.
 *
 * `initial_cap` is only the first allocation hint: the buffer grows as needed
 * and there is NO upper bound on the returned length.  (It used to be called
 * `max_bytes`, which read like a limit it never enforced.)
 *
 * *out_err receives an ipc_err_t on failure. */
char *ipc_read_file(const char *path, size_t initial_cap, int *out_err);

#endif /* IPC_UTIL_H */
