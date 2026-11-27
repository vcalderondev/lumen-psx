#!/usr/bin/env python3
"""Run a command on the PlayStation Classic over its USB serial console.
   psc.py [-t SECONDS] 'command'      prints the command's output, exits with its status"""
import glob, os, select, sys, termios, time, tty

def main():
    args = sys.argv[1:]; timeout = 20.0
    if args and args[0] == "-t": timeout = float(args[1]); args = args[2:]
    cmd = " ".join(args)
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports: sys.exit("no /dev/cu.usbmodem* (console not connected or serial switch off)")
    fd = os.open(ports[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd); tty.setraw(fd); a = termios.tcgetattr(fd)
    a[2] |= termios.CLOCAL | termios.CREAD; termios.tcsetattr(fd, termios.TCSANOW, a)
    def drain(t):
        buf = b""; end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try: buf += os.read(fd, 65536)
                except BlockingIOError: pass
        return buf
    drain(0.2)
    tag = "PSC%d" % (time.time() * 1000 % 1000000)
    # stty -echo keeps the command out of the output; markers frame what the command prints
    line = "stty -echo 2>/dev/null; echo %sA; %s; echo %sZ$?\n" % (tag, cmd, tag)
    os.write(fd, b"\n"); drain(0.2)
    for i in range(0, len(line), 256):
        os.write(fd, line[i:i + 256].encode()); time.sleep(0.01)
    buf = b""; end = time.time() + timeout; a_mark = (tag + "A").encode(); z_mark = (tag + "Z").encode()
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try: buf += os.read(fd, 65536)
            except BlockingIOError: continue
            # the end marker followed by the status and a newline (not the echoed command line)
            i = buf.rfind(z_mark)
            if i >= 0 and b"\n" in buf[i:] and buf[i + len(z_mark):i + len(z_mark) + 1].isdigit(): break
    else:
        sys.stdout.write(buf.decode(errors="replace")); sys.exit("\n[psc.py: no end marker in %ds]" % timeout)
    i = buf.rfind(z_mark); status = int(buf[i + len(z_mark):].split()[0])
    j = buf.rfind(a_mark, 0, i); out = buf[j + len(a_mark):i].lstrip(b"\r\n")
    sys.stdout.write(out.decode(errors="replace").replace("\r\n", "\n"))
    os.close(fd); sys.exit(status)
main()
