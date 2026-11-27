#!/usr/bin/env python3
"""Copy a file to the PlayStation Classic over its USB serial console, checked by MD5.
   psc-put.py LOCAL REMOTE      (gzip + base64 through `cat > file`, then decoded there)"""
import base64, glob, gzip, hashlib, os, select, sys, termios, time, tty
local, remote = sys.argv[1], sys.argv[2]
data = open(local, "rb").read()
b64 = base64.encodebytes(gzip.compress(data, 6))
fd = os.open(sorted(glob.glob("/dev/cu.usbmodem*"))[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
def read_until(mark, timeout):
    buf = b""; end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try: buf += os.read(fd, 65536)
            except BlockingIOError: pass
            if mark in buf: return buf
    sys.exit("timeout waiting for %r; got: %r" % (mark, buf[-300:]))
def send(b, drain=True):
    for i in range(0, len(b), 4096):
        chunk = b[i:i + 4096]
        while chunk:
            _, w, _ = select.select([], [fd], [], 5)
            if not w: sys.exit("port not writable")
            try: n = os.write(fd, chunk); chunk = chunk[n:]
            except BlockingIOError: time.sleep(0.005)
        # drop whatever the shell echoes back
        r, _, _ = select.select([fd], [], [], 0) if drain else ([], [], [])
        if r:
            try: os.read(fd, 65536)
            except BlockingIOError: pass
tmp = "/tmp/.psc-put.b64"
send(b"\nstty -echo 2>/dev/null; cat > %s <<'PSC_EOF_7'\n" % tmp.encode())
send(b64)
send(b"PSC_EOF_7\n")
send(("base64 -d %s | gzip -d > '%s.new' && md5sum '%s.new' | cut -d' ' -f1 | sed 's/^/PSCMD5=/'; rm -f %s; echo PSC''DONE\n" % (tmp, remote, remote, tmp)).encode(), drain=False)
out = read_until(b"PSCDONE\r\n", 120).decode(errors="replace")
want = hashlib.md5(data).hexdigest()
import re
got = re.findall(r"PSCMD5=([0-9a-f]{32})", out)   # the prompt may precede it on the line
if not got or got[-1] != want: sys.exit("MD5 mismatch: want %s, got %s" % (want, got))
send(("mv -f '%s.new' '%s'; echo PSC''MOVED\n" % (remote, remote)).encode(), drain=False)
read_until(b"PSCMOVED\r\n", 30)
print("ok  %s (%d bytes, md5 %s)" % (remote, len(data), want))
