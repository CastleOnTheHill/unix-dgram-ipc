import os, shutil, socket, sys, tempfile

# A Linux-native filesystem, never /mnt/c: drvfs has no real ownership semantics,
# so the chown comparison below would be meaningless there.  tempfile honours
# $TMPDIR, which is /tmp on a normal Linux box.
D = tempfile.mkdtemp(prefix="ipc-sock-semantics-")


def t_fchown_fd_effect():
    p = os.path.join(D, "a.sock")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.bind(p)
    before = os.stat(p).st_gid
    other = [g for g in os.getgroups() if g != os.getegid()]
    if not other:
        print("fchown test: skipped (no other group)")
        s.close(); os.unlink(p); return
    g = other[0]
    try:
        os.fchown(s.fileno(), -1, g)
        r1 = os.stat(p).st_gid
        print("fchown(fd)   gid %s -> %s (requested %s) : %s" % (before, r1, g, "EFFECTIVE" if r1 == g else "NO-OP"))
    except OSError as e:
        print("fchown(fd) FAIL:", e)
    os.chown(p, -1, os.getegid())
    try:
        os.chown(p, -1, g)
        r2 = os.stat(p).st_gid
        print("chown(path)  gid %s -> %s (requested %s) : %s" % (before, r2, g, "EFFECTIVE" if r2 == g else "NO-OP"))
    except OSError as e:
        print("chown(path) FAIL:", e)
    s.close(); os.unlink(p)


def t_umask_on_bind():
    p = os.path.join(D, "b.sock")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    old = os.umask(0o077)
    s.bind(p)
    os.umask(old)
    print("bind under umask 0077 -> mode %s" % oct(os.stat(p).st_mode))
    s.close(); os.unlink(p)


def t_rcvbuf():
    for want in (0, 4096, 16384, 65536):
        rp = os.path.join(D, "r%d.sock" % want)
        r = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        r.bind(rp)
        if want:
            r.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, want)
        eff = r.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
        w = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        w.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
        w.setblocking(False)
        payload = b"x" * 1000
        n = 0
        err = "none"
        try:
            for _ in range(20000):
                w.sendto(payload, rp)
                n += 1
        except BlockingIOError:
            err = "EAGAIN"
        except OSError as e:
            err = "errno=%d" % e.errno
        print("SO_RCVBUF req=%-7s eff=%-8s queued=%-7s err=%s" % (want or "default", eff, n, err))
        w.close(); r.close(); os.unlink(rp)


def t_max_dgram():
    rp = os.path.join(D, "m.sock")
    r = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    r.bind(rp)
    r.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16 * 1024 * 1024)
    w = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    w.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 16 * 1024 * 1024)
    w.setblocking(False)
    snd_eff = w.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF)
    last, reason = 0, "?"
    for size in (1024, 65536, 100000, 130000, 200000, 212000, 212900, 250000, 500000, 1000000):
        try:
            w.sendto(b"y" * size, rp)
            last = size
        except BlockingIOError as e:
            reason = "EAGAIN/ENOBUFS at %d" % size
            break
        except OSError as e:
            reason = "errno=%d at %d" % (e.errno, size)
            break
    print("max datagram payload accepted = %s (SO_SNDBUF effective=%s) reason=%s" % (last, snd_eff, reason))
    # drain
    while True:
        try:
            r.recv(1 << 20, socket.MSG_DONTWAIT)
        except BlockingIOError:
            break
    w.close(); r.close(); os.unlink(rp)


t_fchown_fd_effect()
t_umask_on_bind()
t_rcvbuf()
t_max_dgram()

# Clean up after ourselves.  Every socket in D is already unlinked by its own
# case; this only removes the directory, and it must not take the interpreter
# down with it if a case bailed out early.
try:
    shutil.rmtree(D)
except OSError as exc:
    print("NOTE: could not remove %s: %s" % (D, exc), file=sys.stderr)
