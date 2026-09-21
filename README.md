# libipc — AF_UNIX `SOCK_DGRAM` direct-connect IPC framework

Prototype implementation of a direct-connect IPC design: modules talk straight
to each other over Unix domain **datagram** sockets. There is no central
forwarding server.

> **About the original brief.** This work started from a separate handoff
> document (`handoff.md`) that is **not** part of this repository. Section
> references such as "§11" or "§13" scattered through the code, the tests and
> [`REPORT.md`](REPORT.md) point at that document. Wherever a requirement
> actually drives a decision, it is restated in the text here, so the repository
> stands on its own without it.

This document is the **contract**. It states what was implemented, what was
assumed, and — just as importantly — what was *not* verified. No source of the
three legacy C IPC frameworks was available, so legacy compatibility is
**not verified** (handoff.md §4, §14).

---

## 1. Status at a glance

| | |
|---|---|
| Library | 3422 lines (`src/*.c`, `src/*.h`, `include/ipc/ipc.h`), C11, no dependencies beyond libc + pthread |
| Public API | 4 legacy entry points (`ipc_register`, `ipc_post`, `ipc_send`, `ipc_broadcast`) + `ipc_reply`, `ipc_run`/`ipc_poll`, `ipc_stop`, `ipc_unregister`, `ipc_get_stats` |
| Unit tests | 45 / 45 pass (`tests/unit`, `make check`) |
| Integration tests | 15 / 15 pass, 374 assertions (`tests/integration`, `make integration`) |
| Sanitizers | whole integration suite replayed under ASan+UBSan: 15 / 15, 0 diagnostics across 42 captured process outputs |
| Compiler warnings | 0 at `-O2 -Wall -Wextra -Wshadow …` (see `Makefile` `WARN`) and 0 in the ASan build |
| Module footprint | 176–192 KiB private / 242–255 KiB PSS per module, 9–10 FDs; 0 KiB RSS growth over 20 000 messages |
| Legacy compatibility | **not verified** — no legacy source available |
| Cross-UID security integration | **verified** (root available in WSL; UIDs 1501–1504) |
| Topology delta (synthetic) | direct-datagram p50 **31.1 µs vs 73.5 µs** for stream-relay, CPU per round trip **31.1 µs vs 78.8 µs** |

---

## 2. The four legacy interfaces, as implemented

handoff.md §2 lists the documented semantics. Those are what the prototype
implements; everything else about them is an assumption, listed in §5 below.

| Interface | Implemented behaviour |
|---|---|
| `ipc_register` | Config lookup → identity check → non-blocking exclusive `flock` on a lifetime lock file → residue handling under the lock → `SOCK_DGRAM`/`SOCK_NONBLOCK`/`SOCK_CLOEXEC` + `SO_PASSCRED` → `bind` → group/mode via the **path** form → epoll registration. Any failure rolls back completely. |
| `ipc_post` | **Never blocks.** `IPC_ERR_AGAIN` means the peer's queue was full and the message was *not* enqueued. `IPC_ERR_OFFLINE` means the peer has no live socket. Length is validated before anything is sent. |
| `ipc_send` | Synchronous. The pending slot is registered **before** `sendto()`, so a fast peer can never lose its reply; if `sendto()` fails the slot is released and no wait happens. **Infinite wait is preserved by default** (handoff.md §8). `ipc_send_timeout()` is the bounded variant. A reply is accepted only if its source module **and** echoed instance id match the request. Calling it from an `INLINE` handler returns `IPC_ERR_DEADLOCK` instead of deadlocking. |
| `ipc_broadcast` | One datagram per target in the namespace, sequentially. Offline targets are skipped and counted, never queued for later retry. Partial success is not an error. |
| `ipc_reply` | At most once per received `IPC_TYPE_REQ`, from the handler. |

### Delivery classes

* `IPC_DISPATCH_INLINE` (default) — the handler runs on the single receive
  thread. Cheapest in memory, preserves per-source ordering, and `ipc_send()`
  is refused from a handler. One thread per context.
* `IPC_DISPATCH_POOL` — a bounded worker pool pulling from a bounded callback
  queue. `ipc_send()` is allowed from a handler. A full queue **drops** the
  event and increments `cb_dropped`; it never grows without bound.

---

## 3. Design decisions that came out of measurement

Each of these is backed by an observation recorded in
[`probes/PROBE_NOTES.md`](probes/PROBE_NOTES.md), not by intuition.

1. **Socket ownership and mode are set via the path, never via `fchmod`/
   `fchown` on the fd.** On Linux those are silent no-ops for a bound Unix
   socket. `bind()` also applies the process umask, so the mode is always set
   explicitly afterwards.
2. **`opts.rcvbuf` is accepted but logged as ineffective.** `SO_RCVBUF` does
   not size an AF_UNIX datagram receive queue. Silently ignoring the option
   would be worse than saying so.
3. **All send paths use `MSG_DONTWAIT`.** A blocking `sendto()` on a full queue
   *sleeps*; `ipc_post` is documented as non-blocking, so the failure mode is
   `IPC_ERR_AGAIN`, never an unbounded stall inside a non-blocking call.
4. **Congestion is sender-wide, not per-target.** A datagram is charged to the
   *sender's* `SO_SNDBUF` until the receiver reads it, so one stalled peer
   exhausts the sender's budget for **every** target, and `EAGAIN` is decided
   before the destination is even resolved (`broadcast_skipped` therefore stays
   0 during congestion). Asserted in `t08`.
5. **`IPC_CTRL_SIZE` is exactly one credential.** `CMSG_SPACE(sizeof(struct
   ucred))`. A sender attaching anything else (`SCM_RIGHTS`) forces `MSG_CTRUNC`
   and the datagram is dropped — `t06` forces this rather than assuming it.
6. **`SO_PASSCRED` is enabled at registration, before the loop can read.**
   A datagram already queued when the option is switched on is delivered
   *without* credentials; ordering is part of the contract.
7. **Stale sockets are never identified by inode number.** ext4 recycles
   inodes. `handle_residue()` checks the path's type, ownership and location
   and then decides; `t12` asserts the library's decision via its log lines.
8. **`fork()` is detected, not tolerated.** `CLOEXEC` does not close anything on
   `fork`. A forked child gets `IPC_ERR_STATE` from `ipc__check_alive()` rather
   than sharing the parent's pending table, and the registration `flock` stays
   held while any inherited descriptor is open.

---

## 4. Configuration and permissions

Config format (handoff.md §5.1), one line per module:

```
# namespace module uid socket_path
core A1 1001 /run/example-ipc/A/A1.sock
```

A file must be **readable but not writable by the services**. libipc refuses at
registration if the config is group- or world-writable, and `t04` asserts that a
service cannot edit it.

| Object | Owner | Mode |
|---|---|---|
| socket root | `root:ipc-members` | `0750` |
| per-service directory | `<service uid>:ipc-members` | `0750` |
| module socket | `<service uid>:ipc-members` | `0620` |

`opts.group` names the shared group so the socket's group is set explicitly.
Permissions are what stops a peer from *creating, deleting or replacing* an
address; `SCM_CREDENTIALS` is what stops a peer from *claiming* to be someone
else. Neither substitutes for the other, and the in-library UID check is not a
security boundary (handoff.md §6.1).

---

## 5. Assumptions — the compatibility gap

These are the places where the prototype had to guess because the legacy source
was unavailable (handoff.md §4 items 1–9). **Each one is a possible
incompatibility, and none has been verified against a real framework.**

| # | Assumption made here | Risk if wrong |
|---|---|---|
| 1 | Signature `int f(ctx, const char *dst, uint32_t event, const void *data, size_t len)` | Wrapper/adaptor work only |
| 2 | Handler is `void (*)(const ipc_msg_t *, void *)`, `data` valid only inside the call | Callers that retain `data` would use freed memory |
| 3 | `post` satisfies "asynchronous" by **not blocking**, so a full queue fails with `IPC_ERR_AGAIN` rather than being buffered | A legacy caller may expect the message to be queued and delivered later |
| 4 | Broadcast scope is the namespace, **excluding self** (`broadcast_include_self`, default 0) | Wrong set of recipients |
| 5 | Reply matching uses `(src module, echoed request id, echoed instance id)` | Only relevant if the legacy scheme differs |
| 6 | UID check compares the **real** UID and requires real = effective = configured by default | `allow_uid_split = 1` switches to the effective UID |
| 7 | Maximum payload 8192 by default, 65536 hard | Silently different limit from legacy |
| 8 | Registration/`unregister` are idempotent and safe to call concurrently | — |
| 9 | `send` waits forever by default; `ipc_send_timeout` is a separate, additive entry point | — |

To close this gap, someone with the legacy headers needs to build the
compatibility checklist in handoff.md §4 and diff it against this table.

---

## 6. Build and run

Everything must live on a **Linux-native filesystem**. `/mnt/c` is drvfs: no
real ownership, no real `flock`, so a run there proves nothing (handoff.md §10).
`scripts/wsl-run.sh` mirrors the tree to `$HOME` and builds there.

```bash
bash scripts/wsl-run.sh all      # mirror + build everything
bash scripts/wsl-run.sh check    # unit tests
```

Then, inside WSL:

```bash
sudo bash tests/integration/run_all.sh              # full suite, needs root
sudo bash tests/integration/run_all.sh t05_protocol # one test
sudo bash tests/integration/run_all.sh --list

# the same suite against the sanitizer build (separate build tree)
make asan
sudo BUILD=build-asan IPC_LAB=/opt/ipc-lab-asan bash tests/integration/run_all.sh
bash tests/integration/audit_sanitizer.sh /opt/ipc-lab-asan/logs
bash tests/integration/selftest_audit.sh            # proves that audit can fail

# user-space memory of a running nine-module lab (RSS / PSS / private / FDs)
sudo bash tests/integration/measure_footprint.sh
```

`sudo bash tests/integration/teardown_lab.sh` reverses the lab.

### Benchmarks

```bash
./build/bin/ipc_bench  --msgs 200000 --lat 20000 --bcast 5000 --size 256
./build/bin/ipc_bench  --pool            # adds --window back-pressure
./build/bin/ab_relay   --size 256 --rounds 20000 --repeats 5
./build/bin/rt_baseline                  # bare-socket floor, no libipc
```

`ab_relay` is the handoff.md §13 A/B: datagram-direct versus a synthetic
stream-through-a-central-relay. It is a **synthetic control, not the real legacy
frameworks**, and it is labelled as such in the output. Both arms verify
identity, use the same header and payload sizes, keep the same concurrency (one
round trip in flight) and handle framing/short reads properly.

---

## 7. What the tests do and do not cover

`tests/integration/` is one script per row of the handoff.md §11 table:

```
t01_basic        3 UIDs / 9 modules: unicast, sync, broadcast        52 checks
t02_concurrency  interleaved requests, 8 concurrent waiters          40
t03_identity     wrong UID, ambiguous module, spoofed header         26
t04_fileperm     config write, address takeover, non-member send     26
t05_protocol     12 malformed datagrams + boundary sizes             24
t06_credentials  SO_PASSCRED, mismatch, MSG_CTRUNC                   12
t07_offline      never-started / exited / SIGKILLed peers            22
t08_congestion   stalled peer, EAGAIN, sender-wide blocking          30
t09_syncwait     unbounded send blocks, bounded send times out       13
t10_reqmatching  late / duplicate / forged / wrong-instance replies  21
t11_regrace      duplicate register, 6-process race                  26
t12_crash        SIGKILL residue, reclaim, foreign residue refused   24
t13_refinherit   fork/dup reference and lock inheritance             20
t14_instance     reply to a previous generation refused              15
t15_stress       10 register cycles, 9-module shutdown, 20k soak     23
                                                            total   374
```

Every wait in the suite is a **journal line with a deadline** or a process
exit. There is exactly one place where a fixed sleep is legitimate — `t09`,
whose subject *is* "this call blocks forever" — and it is marked as such.

### Honest gaps

* **Legacy compatibility: not verified.** No legacy source was available.
* **`send` cancellation** is not implemented; handoff.md §8 allows it as a
  separate extension.
* **Attribution of memory to one arm** is not possible in `ab_relay`, because
  both arms share one process by design. Per-arm RSS needs separate processes
  and is left as future work.
* **WSL2 only.** No bare-metal run; the kernel here reports 18 CPUs and 15.4 GiB
  from a shared host.
* **`perf` is not used.** handoff.md §12 asks for whole-system sampling of the
  legacy system; that requires the legacy system. `rt_baseline` provides the
  syscall floor instead, and the §13 numbers are a synthetic topology control.
* **The config file is loaded at registration only**, so a config change needs a
  restart. That matches "the shared library loads it at init" (handoff.md §5.1)
  but means there is no live reload.

---

## 8. Reference

* [`probes/PROBE_NOTES.md`](probes/PROBE_NOTES.md) — measured kernel behaviour,
  with the raw probe output each conclusion rests on
* [`REPORT.md`](REPORT.md) — the final report: results, deltas,
  pass/fail/blocked, limitations, and the promotion recommendation
* The original brief (`handoff.md`) is not included in this repository; see the
  note at the top of this file.
