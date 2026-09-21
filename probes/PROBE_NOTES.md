# PROBE_NOTES.md — kernel behaviour this framework depends on

Every claim here was **measured on the verification host**, not copied from a
man page or from general "UDP-like" intuition. The probes are in this directory
and each one prints the observations verbatim; the assertions in the
integration suite and the comments in `src/` point back to this file instead of
restating folklore.

Target: handoff.md §10 ("do not copy network-UDP tuning to AF_UNIX; the message
limit and the queue-full condition must be established against the actual
kernel") and §7/§6.3 (sender identity, reference inheritance).

## Environment these numbers came from

| | |
|---|---|
| Kernel | `5.15.167.4-microsoft-standard-WSL2` (WSL2, x86_64) |
| Distribution | Ubuntu 22.04.3, glibc 2.35 |
| Compiler | gcc 11.4.0 (`cc (Ubuntu 11.4.0-1ubuntu1~22.04.3)`) |
| CPU | Intel Core Ultra 5 125H, 18 logical CPUs |
| `vm.mmap_rnd_bits` | 28 |

Numbers are WSL2 numbers. handoff.md §13 asks for the trend and for
implementation validation here, not for a production figure.

---

## 1. `fchmod` / `fchown` on a bound Unix socket FD are silent no-ops

**Probe:** `probe_sock_semantics.py`, first cases. Exact output on this host
(an ordinary unprivileged user, uid 1000, gid 1000, with a supplementary group
whose gid is 27):

```
fchown(fd)   gid 1000 -> 1000 (requested 27) : NO-OP
chown(path)  gid 1000 -> 27   (requested 27) : EFFECTIVE
bind under umask 0077 -> mode 0o140700
```

**Why it matters:** the natural way to publish a socket's group and mode is

```c
bind(fd, &sun, len);
fchmod(fd, 0620);     /* <-- looks right, does nothing */
fchown(fd, -1, gid);  /* <-- looks right, does nothing: measured above */
```

`sockfs` keeps inode attributes in the dentry/inode that `bind()` created;
`fchmod`/`fchown` on the *socket* file descriptor operate on a different,
disconnected inode object, the on-disk attributes do not change, and no error
is reported. A service can therefore believe it published `0620
root:ipc-members` while the socket is actually owned by itself. Only the
**path** form (`chmod()` / `chown()`) has an effect — confirmed in the second
line above.

The umask line matters too: `bind()` creates the socket file with
`0777 & ~umask`, so a service running under `umask 0077` gets `0700` and one
running under `umask 0022` gets `0755`. The published mode is a property of the
invoking process's umask unless it is set explicitly afterwards. libipc sets it
explicitly.

**Consequence for libipc:** permission and ownership are applied through the
**path form**, in the window between `bind()` and the first `recvmsg()`, and
`check_own_dir()` refuses to run at all if the target directory is writable by
group or others — otherwise a hostile process in the shared group could swap
the path for a symlink between `bind()` and `chmod()`, and that cannot be closed
from the fd. See the comment block above `check_own_dir()` in `src/ipc_ctx.c`.

**Reproduce:** `python3 probes/probe_sock_semantics.py` (the invoking user needs
a supplementary group other than its primary one to observe the `chown` case).

---

## 2. `SO_RCVBUF` does not size an AF_UNIX datagram receive queue, and the
   queue-full condition is charged to the **sender**

**Probes:** `queue_probe.c` (parameter sweep) and `probe_sock_semantics.py`
(sanity cross-check).

**Setup:** one bound `SOCK_DGRAM` receiver that never reads, one non-blocking
bound sender; fill until `sendto()` fails.

### 2a. `SO_RCVBUF` is irrelevant

Acceptance count for a 64-byte payload, sweeping the receiver's `SO_RCVBUF`
from the kernel default through 4096 / 16384 / 65536 / 1048576:

```
payload  SO_RCVBUF eff_rbuf  eff_sbuf  accepted errno
64       default  212992    212992    278      11
64       4096     8192      212992    278      11
64       16384    32768     212992    278      11
64       65536    131072    212992    278      11
64       1048576  425984    212992    278      11
```

The effective `SO_RCVBUF` changes (8192 → 425984) and the number of datagrams
the kernel accepts does not move at all: **278 every time**. `SO_RCVBUF` is a
network-stack knob; an AF_UNIX socket's receive side is a plain
`sk_receive_queue` with no buffer accounting attached to it.

`probe_sock_semantics.py` reproduces it independently with a 1000-byte payload
and a 4 MiB sender buffer, and gets the same answer for every receive-buffer
setting:

```
SO_RCVBUF req=default eff=212992   queued=185  err=EAGAIN
SO_RCVBUF req=4096    eff=8192     queued=185  err=EAGAIN
SO_RCVBUF req=16384   eff=32768    queued=185  err=EAGAIN
SO_RCVBUF req=65536   eff=131072   queued=185  err=EAGAIN
```

185 × ~2300 B ≈ 425984 B = the *sender's* buffer. Same story, different probe.

### 2b. The real knob is the sender's `SO_SNDBUF`

Same 64-byte payload, holding `SO_RCVBUF` constant and sweeping the **sender's**
`SO_SNDBUF`:

| sender `SO_SNDBUF` | datagrams accepted |
|---|---|
| 8192 | 11 |
| 131072 | 171 |
| 212992 (default) | 278 |
| 425984 | 513 |

`accepted ≈ SO_SNDBUF / ~800`. That divisor is not the payload: it is the
**`skb` truesize** (payload plus `struct sk_buff` and alignment), which is why
64-byte messages still cost roughly 800 bytes of budget each. The datagram is
charged to the sender's `sk_wmem_alloc` at `sendto()` time and is only released
when the receiver actually reads it.

### 2c. This produces *head-of-line blocking across every destination*

Because the charge sits on the **sender's** socket, a sender that also talks to
healthy peers gets `EAGAIN` for **all** of them while one peer is stalled. The
sender's budget is exhausted by the stalled peer, not by the target being
addressed. `tests/integration/t08_congestion.sh` asserts this directly: with
one peer paused, a broadcast reaches **0** healthy peers, and
`broadcast_skipped` stays 0 because `EAGAIN` is decided before the destination
is even resolved. Killing the stalled peer restores the budget immediately.

This is a design-level property, not a bug. It is recorded because the
temptation is to describe congestion as "per-target", which would be wrong.

### 2d. Queue-full surfaces as `EAGAIN` (11); oversize surfaces as `EMSGSIZE` (90)

A non-blocking sender sees `errno = EAGAIN` (11). A blocking sender would
*sleep* instead — which is why every send path in libipc uses `MSG_DONTWAIT` and
reports `IPC_ERR_AGAIN` rather than risking an unbounded stall inside a call
documented as non-blocking.

### 2e. Largest single datagram ≈ the sender's `SO_SNDBUF`

With an **empty** receiver queue, one datagram of the given size:

| sender `SO_SNDBUF` | largest payload accepted | next size up |
|---|---|---|
| 212992 (default) | 212000 | 262144 → `EMSGSIZE` |
| 425984 (asking for 1 MiB) | 262144 | 425984 → `EMSGSIZE` |

A datagram larger than the sender's send buffer cannot be enqueued at all.
`IPC_PAYLOAD_HARD_MAX` is therefore 65536, far below the ~213 KiB the default
buffer allows, so the library's own limit is the binding constraint and the
kernel's is never the thing that fails first.

(Note when reading raw probe output: `probe_sock_semantics.py` reports the
largest datagram as 200000 rather than 262144, because it does not drain the
receiver between sizes, so the earlier datagrams in that loop already occupy
part of the sender's budget. The empty-receiver numbers above come from
`queue_probe`, which drains before each measurement and is the figure to use.)

**Reproduce:** `./build/bin/queue_probe`

---

## 3. `SCM_CREDENTIALS` reports the sender's **real** UID

**Probe:** `cred_probe.c`

A child calls `setresuid(real=65530, effective=65531, saved=65531)` and sends
one datagram; the receiver has `SO_PASSCRED` on and prints the `struct ucred`:

```
RESULT SCM_CREDENTIALS pid=5232 uid=65530 gid=0
CONCLUSION kernel reports the REAL uid
```

This settles handoff.md §4 item 8 as far as it can be settled without the old
source: the *kernel* primitive exposes the real UID, so an implementation that
compares `SCM_CREDENTIALS.uid` against the configured UID is enforcing the
**real** UID. Whether the legacy frameworks meant to check the real or the
effective UID is still an open compatibility question — noted in `README.md`
and it is why `ipc_register_opts_t.allow_uid_split` exists. By default the
library requires real UID, effective UID and configured UID to agree, and
`allow_uid_split = 1` relaxes that to the effective UID for deployments where a
wrapper uses `seteuid()`.

Related, and asserted in `t06_credentials.sh`: the credential is filled by the
kernel, so a sender cannot forge it, but the receiver's control buffer must be
large enough. `IPC_CTRL_SIZE` is `CMSG_SPACE(sizeof(struct ucred))` — exactly
one credential's worth — so a sender that attaches *anything else* (for example
`SCM_RIGHTS`) forces `MSG_CTRUNC` and the datagram is dropped rather than being
processed with partial credentials. The probe forces this with
`forge_peer --send-fd`.

### 3a. Corollary: `SO_PASSCRED` must be enabled before the first datagram arrives

Measured while building the A/B harness (`tests/bench/ab_relay.c`): a datagram
that is already queued when `SO_PASSCRED` is turned on is delivered **without**
credentials. Enabling the option from a receive thread that has already been
started leaves a window in which the first message arrives unauthenticated.

`libipc` enables it during registration, before the receive loop can read
anything, and a message with no credentials is dropped as
`IPC_CFG_ERR_CRED` / `recv_rej_cred`, never delivered. Any reimplementation must
preserve that ordering.

---

## 4. `fork`/`dup` inheritance: the socket and the `flock` both outlive the
   process that created them

**Probe:** `ref_inherit.c`

Observed:

```
== 1. socket inode vs. path vs. inherited reference
PARENT send_after_unlink rc=-2 errno=2
CHILD send_to_removed_path rc=-2 errno=2
CHILD inherited_fd_still_valid=1
PARENT new_bind send_rc=0 received=5 (expect 0 and 5)

== 2. flock survives the owner's exit while an inherited fd is open
PARENT lock_acquired=1 pid=5235
CHILD locker_gone=1
CHILD lock_while_inherited_fd_open rc=-1 errno=11 (EWOULDBLOCK=11)
CHILD lock_after_last_reference_closed rc=0 (expect 0)
```

Conclusions, all of which are asserted by `tests/integration/t13_refinherit.sh`:

* After the path is unlinked, sending to *that path* fails with `ENOENT` (`-2`)
  for the parent **and** for the child, even though the child still holds a
  live inherited descriptor. `unlink()` removes the name, not the socket.
* A fresh `bind()` at the same path creates a **different** inode; the old
  socket's reference is not reachable from the new name, and the new socket
  receives only new traffic (5 bytes from the new peer, 0 from the old one).
* `flock` is held by the **open file description**, not by the process. The
  process that took the lock can die and the lock stays held as long as *any*
  inherited descriptor remains open (`EWOULDBLOCK` = 11). It is released only
  when the last reference is closed (`rc=0`).
* `CLOEXEC` does not help here: it closes descriptors on `exec()`, not on
  `fork()`.

**Consequence for libipc:** the registration lock cannot be released by process
exit, and `ipc__check_alive()` refuses API use from a forked child with
`IPC_ERR_STATE` instead of letting two processes share one context's pending
table. `t13` verifies that killing the parent leaves the address unfree, that
datagrams to the orphaned address still succeed, and that a new instance is
still refused with `IPC_BUSY` while the child holds the inherited reference.

**Reproduce:** `./build/bin/ref_inherit`

---

## 5. Related facts established by the integration suite (not separate probes)

These surfaced while writing the tests and are worth keeping next to the above,
because each one is a place where an intuitive assumption is wrong:

* **inode numbers are not identities.** `ext4` recycles inode numbers, so
  comparing `st_ino` before and after a socket is recreated can report "same
  inode" for two genuinely different sockets. `t12_crash.sh` therefore asserts
  the library's own decision — it greps for the `removing stale socket residue
  at <path>` log line and for `refusing to remove` — instead of comparing
  inode numbers.
* **`SO_RCVBUF` is reported honestly but does nothing useful.** libipc still
  accepts `opts.rcvbuf`, and logs a warning explaining that it does not size
  the receive queue. Silently ignoring it would be worse than logging.
* **A duplicate reply is refused locally, at the sink.** It never reaches the
  wire, so an on-the-wire counter will not see it; `t10_reqmatching.sh` asserts
  the exact expected counts separately for "late", "duplicate" and "forged".

---

## Reproducing all of it

```bash
# inside WSL, in a Linux-native copy of the tree (never /mnt/c)
make all                              # builds probes into ./build/bin
./build/bin/queue_probe               # sections 2a-2e
sudo ./build/bin/cred_probe           # section 3   (needs root for setresuid)
./build/bin/ref_inherit               # section 4
python3 probes/probe_sock_semantics.py  # section 1 (and a cross-check of 2)
```
