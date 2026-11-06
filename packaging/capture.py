#!/usr/bin/env python3
"""Run a command in a pty and save its raw ANSI output (for packaging/ansi2svg.py).

usage: capture.py [-s COLSxROWS] [-k KEYS] [-d SECONDS] out.ansi cmd [args...]
  -s  pty size (default 100x40)
  -k  keys to type after the delay, e.g. q to quit the settings screen (\\x1b etc. allowed)
  -d  seconds to wait before typing the keys (default 3)
out.ansi may be "-" for stdout, e.g. over ssh:
  ssh host python3 - -s 80x32 -k q - macbat < packaging/capture.py > tui.ansi"""
import os, pty, select, fcntl, termios, struct, sys, time, signal

args, cols, rows, keys, delay = sys.argv[1:], 100, 40, b"", 3.0
while args and args[0] in ("-s", "-k", "-d"):
    opt, val, args = args[0], args[1], args[2:]
    if opt == "-s": cols, rows = (int(x) for x in val.lower().split("x"))
    elif opt == "-k": keys = val.encode().decode("unicode_escape").encode("latin-1")
    else: delay = float(val)
if len(args) < 2: sys.exit(__doc__)
pid, fd = pty.fork()
if pid == 0:
    fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))  # before exec: no race
    os.environ.setdefault("LANG", "en_US.UTF-8"); os.environ["TERM"] = "xterm-256color"
    os.execvp(args[1], args[1:])
out, start, sent = b"", time.time(), not keys
while True:
    if not sent and time.time() - start >= delay:
        os.write(fd, keys); sent = True
    if time.time() - start > delay + 30: break     # safety net: never leave the command running (see below)
    try:
        r, _, _ = select.select([fd], [], [], 0.2 if not sent else 3)
        if not r:
            if sent: break
            continue
        d = os.read(fd, 65536)
        if not d: break
        out += d
    except OSError: break
if os.waitpid(pid, os.WNOHANG)[0] == 0:            # still running: stop it, but keep reading the pty -
    os.kill(pid, signal.SIGTERM)                   # a TUI restoring the terminal blocks until its output is read
    end = time.time() + 5
    while time.time() < end and os.waitpid(pid, os.WNOHANG)[0] == 0:
        try:
            r, _, _ = select.select([fd], [], [], 0.2)
            if r: out += os.read(fd, 65536)
        except OSError: break
    else:
        if time.time() >= end: os.kill(pid, signal.SIGKILL)
    try: os.waitpid(pid, 0)
    except ChildProcessError: pass
(sys.stdout.buffer if args[0] == "-" else open(args[0], "wb")).write(out)
