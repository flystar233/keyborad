#!/usr/bin/env python3
"""
tools/verify_icons.py -- offline icon verification (host only, not firmware)

WHY THIS EXISTS
    Icon bitmap bytes are practically impossible to proof-read by eye. This
    project got them wrong several times (bluetooth rune had a blank row,
    battery fill width off by 2px). So every icon byte in display_icons.h is
    rasterised here and checked against an independent model.

WHAT IT CHECKS
    1. battery 5 levels: frame outline, positive nub, contiguous fill
    2. battery 100% == official u8g2 "battery-full.png" glyph
    3. battery   0% == official u8g2 "battery-empty.png" glyph
    4. battery fill is monotonically increasing across levels
    5. bluetooth == official u8g2 "bluetooth.png" glyph

USAGE
    python tools/verify_icons.py      (exit 0 = ok, 1 = mismatch)

Re-run after touching include/display_icons.h.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "display_icons.h")

# official u8g2 BDF fonts, used as the reference
BDF_ICONIC = os.path.join(
    os.environ.get("TEMP", ""), "u8g2m", "u8g2-master",
    "tools", "font", "bdf", "open_iconic_all_1x.bdf")

fail = []


# --------------------------------------------------------------------------
# parse the project header
# --------------------------------------------------------------------------
def read_header_array(name, dims=None):
    txt = open(HEADER, encoding="utf-8").read()
    # note: array may be declared with more than one bracket pair, e.g.
    #   ICON_BAT[ICON_BAT_LEVELS][ICON_BAT_W] = { ... };
    # so do not try to match the brackets precisely -- just skip to the first '{'.
    m = re.search(re.escape(name) + r"\s*\[.*?=\s*\{(.*?)\};", txt, re.S)
    if not m:
        return None
    body = re.sub(r"//.*", "", m.group(1))
    vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    if dims:
        rows, cols = dims
        if len(vals) < rows * cols:
            return None
        return [vals[i * cols:(i + 1) * cols] for i in range(rows)]
    return vals


def render_colmajor(cols, w=8, h=8):
    """column-major page format -> ASCII rows"""
    out = []
    for y in range(h):
        out.append("".join("#" if (cols[x] >> y) & 1 else "." for x in range(w)))
    return out


# --------------------------------------------------------------------------
# parse official BDF for reference glyphs
# --------------------------------------------------------------------------
def parse_bdf(path):
    glyphs = {}
    name = None
    enc = None
    bbx = None
    inb = False
    bmp = []

    def flush():
        if name is not None and enc is not None and bbx:
            glyphs.setdefault(os.path.basename(name), (enc, bbx, list(bmp)))

    for l in open(path, encoding="utf-8", errors="replace").read().splitlines():
        s = l.strip()
        if s.startswith("STARTCHAR"):
            flush()
            name = s[9:].strip()
            bmp = []
            inb = False
            bbx = None
            enc = None
        elif s.startswith("ENCODING"):
            enc = int(s.split()[1])
        elif s.startswith("BBX"):
            bbx = [int(v) for v in s.split()[1:5]]
        elif s == "BITMAP":
            inb = True
        elif s == "ENDCHAR":
            pass
        elif inb:
            bmp.append(s)
    flush()
    return glyphs


def bdf_to_rows(bbx, bmp):
    w, h = bbx[0], bbx[1]
    nb = (w + 7) // 8
    rows = []
    for y in range(h):
        parts = [int(v, 16) for v in bmp[y].split()] if bmp[y].split() else []
        while len(parts) < nb:
            parts.insert(0, 0)
        rows.append("".join("#" if (parts[x // 8] >> (7 - (x % 8))) & 1 else "."
                            for x in range(w)))
    return rows


def bdf_to_colmajor(bbx, bmp):
    rows = bdf_to_rows(bbx, bmp)
    w, h = bbx[0], bbx[1]
    return [sum(1 << y for y in range(h) if rows[y][x] == "#") for x in range(w)]


# --------------------------------------------------------------------------
# load both sides
# --------------------------------------------------------------------------
BAT = read_header_array("ICON_BAT", dims=(5, 8))
BT = read_header_array("ICON_BT")
if BAT is None:
    fail.append("ICON_BAT not found in display_icons.h")
if BT is None:
    fail.append("ICON_BT not found in display_icons.h")

ref_ok = os.path.exists(BDF_ICONIC)
if not ref_ok:
    print("WARNING: official BDF not found at %s" % BDF_ICONIC)
    print("         cross-check against u8g2 data will be skipped.")
    print("         (extract u8g2-master.zip to %%TEMP%%\\u8g2m to enable it)")
glyphs = parse_bdf(BDF_ICONIC) if ref_ok else {}

# --------------------------------------------------------------------------
# 1) battery structure
# --------------------------------------------------------------------------
print("=" * 62)
print("BATTERY icon")
print("=" * 62)
for lvl in range(5):
    cols = BAT[lvl]
    rows = render_colmajor(cols)
    pct = lvl * 100 // 4
    print("--- level %d (%d%%) ---" % (lvl, pct))
    for r in rows:
        print("  " + r)
    print()

    # frame: top and bottom rails must span x0..x6
    if rows[1][:7] != "#######":
        fail.append("battery L%d: top rail is not ####### at row 1" % lvl)
    if rows[6][:7] != "#######":
        fail.append("battery L%d: bottom rail is not ####### at row 6" % lvl)
    # left post
    if any(rows[y][0] != "#" for y in range(1, 7)):
        fail.append("battery L%d: left post broken" % lvl)
    # positive nub present at (7,3) and (7,4)
    if rows[3][7] != "#" or rows[4][7] != "#":
        fail.append("battery L%d: positive nub missing at col7 rows3-4" % lvl)
    # interior fill must be contiguous from the left
    filled = [x for x in range(1, 6) if rows[2][x] == "#"]
    if filled and filled != list(range(1, len(filled) + 1)):
        fail.append("battery L%d: fill is not contiguous from the left" % lvl)

# monotonic fill
counts = [sum(1 for x in range(1, 6) if render_colmajor(BAT[lvl])[2][x] == "#")
          for lvl in range(5)]
if counts != sorted(counts):
    fail.append("battery: fill does not increase monotonically (%s)" % counts)
print("fill width per level:", counts)

# --------------------------------------------------------------------------
# 2/3) compare against official u8g2 glyphs
# --------------------------------------------------------------------------
if ref_ok:
    for fname, lvl, label in [("battery-empty.png", 0, "0%"),
                              ("battery-full.png", 4, "100%")]:
        g = glyphs.get(fname)
        if not g:
            fail.append("official %s not found in BDF" % fname)
            continue
        enc, bbx, bmp = g
        want = bdf_to_colmajor(bbx, bmp)
        got = BAT[lvl]
        print("\ncomparison with official %s (ENCODING=%d):" % (fname, enc))
        wrows = bdf_to_rows(bbx, bmp)
        for r in wrows:
            print("  official: " + r)
        for r in render_colmajor(got):
            print("  ours    : " + r)
        if got == want:
            print("  -> IDENTICAL to official %s" % label)
        else:
            fail.append("battery %s differs from official %s" % (label, fname))
            print("  -> MISMATCH")
            print("     official: %s" % ", ".join("0x%02X" % v for v in want))
            print("     ours    : %s" % ", ".join("0x%02X" % v for v in got))

# --------------------------------------------------------------------------
# 4) bluetooth vs official
# --------------------------------------------------------------------------
print()
print("=" * 62)
print("BLUETOOTH icon")
print("=" * 62)
if BT is not None:
    for r in render_colmajor(BT):
        print("  " + r)
    print()
if ref_ok and BT is not None:
    g = glyphs.get("bluetooth.png")
    if not g:
        fail.append("official bluetooth.png not found in BDF")
    else:
        enc, bbx, bmp = g
        want = bdf_to_colmajor(bbx, bmp)
        for r in bdf_to_rows(bbx, bmp):
            print("  official: " + r)
        for r in render_colmajor(BT):
            print("  ours    : " + r)
        if BT == want:
            print("  -> IDENTICAL to official bluetooth.png (ENCODING=%d)" % enc)
        else:
            fail.append("bluetooth differs from official bluetooth.png")
            print("  -> MISMATCH")
            print("     official: %s" % ", ".join("0x%02X" % v for v in want))
            print("     ours    : %s" % ", ".join("0x%02X" % v for v in BT))

print()
print("=" * 62)
if fail:
    print("FAILED: %d problem(s)" % len(fail))
    for f in fail:
        print("  !! " + f)
    sys.exit(1)
print("ALL CHECKS PASSED")
sys.exit(0)
