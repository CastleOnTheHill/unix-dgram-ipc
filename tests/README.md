# tests — what the suites guarantee, and how they synchronise

## Layout

| Path | What it is | Needs root |
|---|---|---|
| `unit/` | 51 unit tests, `make check` | **no** |
| `integration/` | 15 scripts, one per row of the handoff §11 table, plus the lab and the audit tools | **yes** (`setpriv`) |
| `bench/` | `ipc_bench` (library throughput/latency), `ab_relay` (synthetic topology A/B) | no |

`make integration` runs `integration/run_all.sh`, which builds the lab with
`setup_lab.sh` and then runs every `t*.sh`. Without root it reports the whole
suite as **BLOCKED** (exit 2) rather than passing it — "I could not test it" and
"it passed" must never look the same.

## The synchronisation contract

**A test never establishes a fact by waiting a fixed amount of time.** Every
positive claim ("the service is registered", "the queue is full", "the message
arrived", "the worker drained it") is established by one of:

* **`jwait <journal> <regex> <deadline>`** — block until a specific journal line
  appears, with a deadline. This is the normal case; there are several hundred
  call sites.
* **`wait_exit` / a `kill -0` loop** — block until a process is gone.
* **a counter poll** — repeatedly ask for stats and stop as soon as the
  **expression the assertion reads** has converged (e.g. `t10`'s
  `reply_unmatched`, `t15`'s drain and quiescence loops, and `t08`'s
  `invoked + dropped >= burst_ok`).

  The wording matters. A poll must wait on the *whole* expression the assertion
  reads, not on one convenient term of it: `t08` used to stop as soon as
  `cb_dropped` went non-zero, which happens while the last `--queue 2` datagrams
  are still in the queue — so it consistently woke two datagrams early and the
  accounting assertion failed `1850 accepted vs 1848 accounted for`. Polling a
  single counter is only correct when the assertion reads exactly that counter.

`mod_stats()` deserves a note: it blocks until a **new** `^STATS` line has
appeared. A caller that merely greps for `^STATS` would silently read the
previous dump and assert against stale counters, so the freshness wait is folded
into the helper and no call site has to know about it.

### Where a fixed `sleep` still appears — the complete list

This list exists because an earlier revision of `common.sh`, `README.md` and
`REPORT.md` claimed the suite contained "exactly one fixed sleep". That was
false: there were at least thirteen, several of them "wait a bit and it will
probably be done". The substantive ones have been replaced by counter polls;
what remains is listed here so the claim is checkable instead of aspirational.

| Site | Kind |
|---|---|
| `common.sh` `jwait`, `wait_exit`, `mod_stats` | **poll interval** (0.02 s) inside a deadline loop |
| `common.sh` `jwait_gone` | **observation window** — asserts that something *has not* happened yet after N seconds; a window is the only way to express that |
| `t08:141`, `t08:209` | poll interval (0.1 s) in a bounded retry / counter-poll loop |
| `t10:136`, `t10:152` | poll interval (0.1 s) in a bounded counter-poll loop |
| `t13:120` | poll interval (0.1 s) waiting for a child to exit |
| `t14:79` | poll interval (0.3 s) in a bounded counter-poll loop |
| `t15:171`, `t15:214` | poll interval (0.1 s) in bounded counter-poll loops |
| `t15:197` | poll interval (0.25 s) in a quiescence loop: it stops as soon as two consecutive `/proc` samples agree |
| `t09:55` | the one legitimate **fixed wait**: the subject of that test *is* "this call blocks forever", so a wait is the measurement. It is bounded by an external watchdog |
| `teardown_lab.sh:29` | 0.2 s settle after `pkill`, in the teardown script — not a test |

So: no fixed sleep establishes a positive fact anywhere in the suite, and the
only place where waiting *is* the subject is `t09`.

Two consequences worth keeping:

* `mod_cmd` only writes the command into the module's FIFO; the module may not
  have parsed it yet. **Always follow it with `jwait`, never with `assert_re`.**
  A lost race would record a permanent `FAIL`, because `fail()` counts and never
  retries. (This was the bug in `t01`.)
* `assert_no_re` requires the file to exist. `grep` on a missing file returns
  non-zero for a reason unrelated to the pattern, so without that check
  "the forbidden line never appeared" and "this journal was never created" are
  indistinguishable, and the assertion passes for the wrong reason.

## Preconditions

`require_root` and `require_tools` both report **BLOCKED** and exit 0 rather
than letting the tests fail with confusing messages. `require_lab` checks the
lab binaries (`ipc_testmod`, `forge_peer`) plus `setpriv` and `python3`.
`run_all.sh` classifies each test from the verdict line the test itself wrote,
so a `BLOCKED` test can never be counted as passed even if it exits 0.

## The lab

`setup_lab.sh` creates group `ipcmembers` (gid 1500) and users `ipca`/`ipcb`/
`ipcc` (1501–1503, members) plus `ipcx` (1504, deliberately not a member), and a
three-UID, nine-module tree under `$IPC_LAB` (default `/opt/ipc-lab`).
`teardown_lab.sh` reverses it, refusing to delete any account whose UID/GID does
not match.

Both scripts delete things as root, so `IPC_LAB` is validated by
`lab_guard.sh` before anything else happens: the path must be absolute, at least
two components deep, and its last component must contain `lab`. That rejects
`/`, `/usr`, `/etc`, `/home`, `/root`, `/opt`, `/tmp` and `$HOME`, and accepts
`/opt/ipc-lab` and `/opt/ipc-lab-asan`.

Everything must live on a **Linux-native filesystem**. `/mnt/c` is drvfs: no
real ownership, no real `flock`, so a run there proves nothing.
