#!/usr/bin/env python3
"""Render captured ANSI terminal output to an SVG "CRT" screenshot (stdlib only).
Full-screen programs (the settings screen) are reduced to their last complete frame.
usage: ansi2svg.py in.ansi out.svg [title]"""
import re, sys, html
PAL = {30: "#1b1f1b", 31: "#c84848", 32: "#2fae4a", 33: "#c98a1c", 37: "#c8c8c8",
       91: "#ff6060", 92: "#6dff8c", 93: "#ffd75f", 97: "#ffffff"}
raw = open(sys.argv[1], encoding="utf-8").read().replace("\r", "")
if "\x1b[H" in raw:   # cursor-home redraws: keep the last complete frame (ends with erase-below)
    frames = [f for f in raw.split("\x1b[H") if "\x1b[J" in f] or raw.split("\x1b[H")[-1:]
    raw = frames[-1].split("\x1b[J")[0]
raw = re.sub(r"\x1b\[[0-9;?]*[A-Za-ln-z]", "", raw)   # drop non-colour control sequences
lines = raw.rstrip("\n").split("\n")
CW, LH, PAD = 9.6, 19, 18
cols = max(len(re.sub(r"\x1b\[[0-9;]*m", "", l)) for l in lines)
W, H = int(cols * CW + 2 * PAD), int(len(lines) * LH + 2 * PAD + 6)
out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
       '<defs><radialGradient id="g" cx="50%" cy="45%" r="75%"><stop offset="0" stop-color="#0d1a10"/>'
       '<stop offset="1" stop-color="#030604"/></radialGradient>'
       '<pattern id="s" width="4" height="3" patternUnits="userSpaceOnUse"><rect width="4" height="1" fill="#000" opacity="0.22"/></pattern></defs>',
       f'<rect width="{W}" height="{H}" rx="14" fill="url(#g)"/>',
       '<g font-family="Menlo, \'DejaVu Sans Mono\', Consolas, monospace" font-size="16" xml:space="preserve">']
for y, line in enumerate(lines):
    col, fg, bold, dim, rev = 0, "#2fae4a", False, False, False
    for tok in re.split(r"(\x1b\[[0-9;]*m)", line):
        m = re.fullmatch(r"\x1b\[([0-9;]*)m", tok)
        if m:
            for c in [int(x) for x in m.group(1).split(";") if x] or [0]:
                if c == 0: fg, bold, dim, rev = "#2fae4a", False, False, False
                elif c == 1: bold = True
                elif c == 2: dim = True
                elif c == 7: rev = True
                elif c == 27: rev = False
                elif c in PAL: fg = PAL[c]
            continue
        if not tok: continue
        x, yy = PAD + col * CW, PAD + (y + 1) * LH - 4
        if rev:   # reverse video (selected value): solid cell background, dark glyphs
            out.append(f'<rect x="{x:.1f}" y="{PAD + y * LH + 1}" width="{len(tok) * CW:.1f}" height="{LH}" fill="{fg}"/>')
        attrs = f'fill="{"#071009" if rev else fg}"' + (' font-weight="bold"' if bold else "") + (' opacity="0.55"' if dim and not rev else "")
        glow = f' style="filter:drop-shadow(0 0 3px {fg})"' if not dim and not rev else ""
        xs = " ".join(f"{x + i * CW:.1f}" for i in range(len(tok)))   # one x per glyph: exact grid
        out.append(f'<text x="{xs}" y="{yy}" {attrs}{glow}>{html.escape(tok)}</text>')
        col += len(tok)
out += ['</g>', f'<rect width="{W}" height="{H}" rx="14" fill="url(#s)"/>', '</svg>']
open(sys.argv[2], "w", encoding="utf-8").write("\n".join(out))
