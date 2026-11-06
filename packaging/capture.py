#!/usr/bin/env python3
"""Run a command in a pty (100x40) and save its raw ANSI output.  usage: capture.py out.ansi cmd [args...]"""
import os, pty, select, fcntl, termios, struct, sys
pid, fd = pty.fork()
if pid == 0:
    os.environ.setdefault("LANG", "en_US.UTF-8"); os.environ["TERM"] = "xterm-256color"
    os.execvp(sys.argv[2], sys.argv[2:])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 100, 0, 0))
out = b""
while True:
    try:
        r, _, _ = select.select([fd], [], [], 3)
        if not r: break
        d = os.read(fd, 65536)
        if not d: break
        out += d
    except OSError: break
os.waitpid(pid, 0)
open(sys.argv[1], "wb").write(out)
