#!/usr/bin/env python3
"""Render captured ANSI terminal output to an SVG screenshot (stdlib only).
Full-screen programs (the settings screen) are reduced to their last complete frame.

usage: ansi2svg.py [--style crt|amber|dark|light|vga] [--title TEXT] in.ansi out.svg
  crt / amber   green / amber CRT with scanlines and glow (retro themes)
  dark / light  plain terminal window (the other themes)
  vga           black Linux-console window with the 16-colour VGA palette (to check TERM=linux output)
Colours: 16 (30-37, 90-97), 256 (38;5;n) and truecolor (38;2;r;g;b); bold, dim, reverse."""
import re, sys, html

args, style, title = sys.argv[1:], "crt", ""
while args and args[0] in ("--style", "--title"):
    if args[0] == "--style": style = args[1]
    else: title = args[1]
    args = args[2:]
if len(args) < 2: sys.exit(__doc__)
src, dst = args[0], args[1]
if len(args) > 2 and not title: title = args[2]

CRT16 = {30: "#1b1f1b", 31: "#c84848", 32: "#2fae4a", 33: "#c98a1c", 34: "#3b6fd0", 35: "#b05bc8", 36: "#2aa6a6", 37: "#c8c8c8",
         90: "#5f6b5f", 91: "#ff6060", 92: "#6dff8c", 93: "#ffd75f", 94: "#7aa2ff", 95: "#f08cff", 96: "#6ff0f0", 97: "#ffffff"}
DARK16 = {30: "#3b4048", 31: "#e06c75", 32: "#98c379", 33: "#e5c07b", 34: "#61afef", 35: "#c678dd", 36: "#56b6c2", 37: "#abb2bf",
          90: "#7f848e", 91: "#ff7b86", 92: "#b5e890", 93: "#ffd68a", 94: "#8cc8ff", 95: "#e2a3f2", 96: "#7fd3dd", 97: "#ffffff"}
LIGHT16 = {30: "#24292f", 31: "#cf222e", 32: "#116329", 33: "#9a6700", 34: "#0969da", 35: "#8250df", 36: "#1b7c83", 37: "#6e7781",
           90: "#57606a", 91: "#a40e26", 92: "#1a7f37", 93: "#7d4e00", 94: "#218bff", 95: "#a475f9", 96: "#3192aa", 97: "#8c959f"}
VGA16 = {30: "#000000", 31: "#aa0000", 32: "#00aa00", 33: "#aa5500", 34: "#0000aa", 35: "#aa00aa", 36: "#00aaaa", 37: "#aaaaaa",
         90: "#555555", 91: "#ff5555", 92: "#55ff55", 93: "#ffff55", 94: "#5555ff", 95: "#ff55ff", 96: "#55ffff", 97: "#ffffff"}
S = {  # palette, default fg, background stops, crt effects
    "crt":   (CRT16, "#2fae4a", ("#0d1a10", "#030604"), True),
    "amber": (CRT16, "#ffb000", ("#1c1306", "#070401"), True),
    "dark":  (DARK16, "#d4d7dd", ("#1b1e24", "#1b1e24"), False),
    "light": (LIGHT16, "#24292f", ("#ffffff", "#ffffff"), False),
    "vga":   (VGA16, "#aaaaaa", ("#000000", "#000000"), False),
}
if style not in S: sys.exit(f"unknown style {style}")
PAL, DEFAULT, BG, CRT = S[style]

def c256(n):
    if n < 16: return PAL[(30 + n) if n < 8 else (90 + n - 8)]
    if n >= 232: v = 8 + 10 * (n - 232); return f"#{v:02x}{v:02x}{v:02x}"
    n -= 16; lv = [0, 95, 135, 175, 215, 255]
    return "#%02x%02x%02x" % (lv[n // 36], lv[n // 6 % 6], lv[n % 6])

raw = open(src, encoding="utf-8").read().replace("\r", "")
if "\x1b[H" in raw:   # cursor-home redraws: keep the last complete frame (ends with erase-below)
    frames = [f for f in raw.split("\x1b[H") if "\x1b[J" in f] or raw.split("\x1b[H")[-1:]
    raw = frames[-1].split("\x1b[J")[0]
raw = re.sub(r"\x1b\[[0-9;?]*[A-Za-ln-z]", "", raw)   # drop non-colour control sequences
lines = raw.rstrip("\n").split("\n")
CW, LH, PAD = 9.6, 19, 18
TOP = 30 if not CRT else 0                               # window title bar for the plain styles
cols = max(len(re.sub(r"\x1b\[[0-9;]*m", "", l)) for l in lines)
W, H = int(cols * CW + 2 * PAD), int(len(lines) * LH + 2 * PAD + 6 + TOP)
out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
       f'<defs><radialGradient id="g" cx="50%" cy="45%" r="75%"><stop offset="0" stop-color="{BG[0]}"/>'
       f'<stop offset="1" stop-color="{BG[1]}"/></radialGradient>'
       '<pattern id="s" width="4" height="3" patternUnits="userSpaceOnUse"><rect width="4" height="1" fill="#000" opacity="0.22"/></pattern></defs>',
       f'<rect width="{W}" height="{H}" rx="{14 if CRT else 10}" fill="url(#g)"' + ('' if CRT or style != "light" else ' stroke="#d0d7de"') + '/>']
if TOP:
    bar = "#2a2e36" if style == "dark" else "#f0f2f5" if style == "light" else "#1a1a1a"
    out.append(f'<path d="M0 10 a10 10 0 0 1 10 -10 h{W - 20} a10 10 0 0 1 10 10 v{TOP - 10} h-{W} z" fill="{bar}"/>')
    for i, c in enumerate(("#ff5f57", "#febc2e", "#28c840")):
        out.append(f'<circle cx="{20 + i * 18}" cy="{TOP / 2}" r="6" fill="{c}"/>')
    if title:
        out.append(f'<text x="{W / 2}" y="{TOP / 2 + 4.5}" text-anchor="middle" font-family="-apple-system, \'Segoe UI\', Helvetica, Arial, sans-serif" '
                   f'font-size="12.5" fill="{"#9da5b4" if style != "light" else "#57606a"}">{html.escape(title)}</text>')
out.append('<g font-family="Menlo, \'DejaVu Sans Mono\', Consolas, monospace" font-size="16" xml:space="preserve">')
bgcolor = BG[1]
for y, line in enumerate(lines):
    col, fg, bold, dim, rev = 0, DEFAULT, False, False, False
    for tok in re.split(r"(\x1b\[[0-9;]*m)", line):
        m = re.fullmatch(r"\x1b\[([0-9;]*)m", tok)
        if m:
            codes = [int(x) for x in m.group(1).split(";") if x] or [0]
            i = 0
            while i < len(codes):
                c = codes[i]
                if c == 0: fg, bold, dim, rev = DEFAULT, False, False, False
                elif c == 1: bold = True
                elif c == 2: dim = True
                elif c == 22: bold = dim = False
                elif c == 7: rev = True
                elif c == 27: rev = False
                elif c == 39: fg = DEFAULT
                elif c in PAL: fg = PAL[c]
                elif c == 38 and i + 2 < len(codes) and codes[i + 1] == 5: fg = c256(codes[i + 2]); i += 2
                elif c == 38 and i + 4 < len(codes) and codes[i + 1] == 2:
                    fg = "#%02x%02x%02x" % tuple(codes[i + 2:i + 5]); i += 4
                i += 1
            continue
        if not tok: continue
        x, yy = PAD + col * CW, TOP + PAD + (y + 1) * LH - 4
        if rev:   # reverse video (selected value): solid cell background, glyphs in the background colour
            out.append(f'<rect x="{x:.1f}" y="{TOP + PAD + y * LH + 1}" width="{len(tok) * CW:.1f}" height="{LH}" fill="{fg}"/>')
        attrs = f'fill="{bgcolor if rev else fg}"' + (' font-weight="bold"' if bold else "") + (' opacity="0.55"' if dim and not rev else "")
        glow = f' style="filter:drop-shadow(0 0 3px {fg})"' if CRT and not dim and not rev else ""
        xs = " ".join(f"{x + i * CW:.1f}" for i in range(len(tok)))   # one x per glyph: exact grid
        out.append(f'<text x="{xs}" y="{yy}" {attrs}{glow}>{html.escape(tok)}</text>')
        col += len(tok)
out.append('</g>')
if CRT: out.append(f'<rect width="{W}" height="{H}" rx="14" fill="url(#s)"/>')
out.append('</svg>')
open(dst, "w", encoding="utf-8").write("\n".join(out))
