#!/usr/bin/env python3
"""Recalibrate OCR (A-transform) region boxes for a tablemap against the live table.

Tesseract on this app reads best when the region's BOTTOM sits exactly 1px below the
bottom of the glyphs. Cloning android_s10 -> android_EMU scaled every box by ~4.5%, which
left several regions with the wrong bottom padding, so balances/bets/names come back
mangled ('112.8 BB' -> '112.6', 'FOLD' -> 'DONT540M') even though the crop is legible.

For each region this finds the glyph bounding box using the region's OWN key colour and
radius (stored ABGR, same packing as tm_images), samples several frames so a single
mid-animation capture can't skew it, then proposes:
    rgn_bottom = text_bottom + 1
    rgn_top    = text_top - TOP_PAD      (only if the glyphs would otherwise be clipped)
    rgn_left/right widened just enough to contain the glyphs plus a small margin

Usage:
  python calibrate_ocr_regions.py --dry-run          # propose only
  python calibrate_ocr_regions.py --apply            # write to the DB (backs up first)
"""
import argparse
import os
import subprocess
import tempfile
import time

import cv2
import numpy as np

PSQL = r"C:\Program Files\PostgreSQL\12\bin\psql.exe"
TMID = 282
WINDOW_TITLE = "EMU"
PAD = 14          # how far outside the region to look for glyphs
TOP_PAD = 2       # breathing room above the glyphs
SIDE_PAD = 2      # breathing room left/right
BOTTOM_PAD = 1    # the rule: exactly 1px under the glyphs
MAX_GROW_H = 6    # reject implausible box changes -- they mean detection went wrong
MAX_GROW_W = 20

WORK = os.path.join(tempfile.gettempdir(), "ocr_calib")
os.makedirs(WORK, exist_ok=True)

CAPTURE_PS1 = r"""
Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;using System.Drawing;using System.Drawing.Imaging;
public class Cap {
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  public static void Grab(IntPtr h, string path) {
    RECT r; GetClientRect(h, out r);
    int w=r.R-r.L, ht=r.B-r.T; if (w<=0||ht<=0) return;
    using (Bitmap b=new Bitmap(w,ht,PixelFormat.Format32bppArgb))
    using (Graphics g=Graphics.FromImage(b)) {
      IntPtr dc=g.GetHdc(); PrintWindow(h,dc,3); g.ReleaseHdc(dc); b.Save(path,ImageFormat.Png);
    }
  }
}
'@ -ReferencedAssemblies System.Drawing
$p = Get-Process scrcpy -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowTitle -eq '%TITLE%' } | Select-Object -First 1
if (-not $p) { Write-Output "NOWINDOW"; exit }
[Cap]::Grab($p.MainWindowHandle, '%OUT%')
Write-Output "OK"
"""


def psql(sql, rows=True):
    env = dict(os.environ, PGPASSWORD="dbpass")
    r = subprocess.run([PSQL, "-h", "127.0.0.1", "-U", "postgres", "-d", "hiss",
                        "-t", "-A", "-F", "|", "-c", sql],
                       capture_output=True, text=True, env=env, timeout=60)
    if r.returncode:
        raise RuntimeError(r.stderr.strip())
    return [l for l in r.stdout.strip().splitlines() if l] if rows else None


def capture(path):
    s = CAPTURE_PS1.replace("%TITLE%", WINDOW_TITLE).replace("%OUT%", path.replace("\\", "\\\\"))
    f = os.path.join(WORK, "_cap.ps1")
    open(f, "w", encoding="utf-8").write(s)
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", f],
                       capture_output=True, text=True, timeout=90)
    return "OK" in r.stdout


def unpack_abgr(v):
    """tm_regions.color is packed like the images: A<<24 | B<<16 | G<<8 | R."""
    v = int(v) & 0xFFFFFFFF
    return (v >> 0) & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF     # R, G, B


def glyph_bbox(img, l, t, r, b, rgb, radius):
    """Bounding box of the glyphs belonging to this region.

    Detection is by LOCAL CONTRAST, not by the region's key colour: several regions carry a
    radius of 180-200, and matching within that tolerance selects essentially every pixel,
    which silently turns the "glyph bbox" into the whole search window.

    Only connected components that actually overlap the declared region are kept, so a
    neighbouring seat's text drifting into the padded window can't hijack the result.
    """
    H, W = img.shape[:2]
    x0, x1 = max(0, l - PAD), min(W, r + PAD + 1)
    y0, y1 = max(0, t - PAD), min(H, b + PAD + 1)
    win = img[y0:y1, x0:x1]
    if win.size == 0:
        return None
    gray = cv2.cvtColor(win, cv2.COLOR_BGR2GRAY).astype(np.int16)
    bg = int(np.median(gray))
    mask = (np.abs(gray - bg) > 40).astype(np.uint8)
    if mask.sum() < 6:
        return None

    n, lab, stats, _ = cv2.connectedComponentsWithStats(mask, 8)
    keep = None
    for i in range(1, n):
        cx, cy = stats[i, cv2.CC_STAT_LEFT], stats[i, cv2.CC_STAT_TOP]
        cw, ch = stats[i, cv2.CC_STAT_WIDTH], stats[i, cv2.CC_STAT_HEIGHT]
        if stats[i, cv2.CC_STAT_AREA] < 3:
            continue
        ax0, ay0, ax1, ay1 = x0 + cx, y0 + cy, x0 + cx + cw - 1, y0 + cy + ch - 1
        if ax1 < l or ax0 > r or ay1 < t or ay0 > b:      # no overlap with the region
            continue
        box = (ax0, ay0, ax1, ay1)
        keep = box if keep is None else (min(keep[0], box[0]), min(keep[1], box[1]),
                                         max(keep[2], box[2]), max(keep[3], box[3]))
    return keep


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--tablemap", type=int)
    ap.add_argument("--window")
    ap.add_argument("--samples", type=int, default=5)
    ap.add_argument("--delay", type=float, default=2.0)
    a = ap.parse_args()
    global TMID, WINDOW_TITLE
    if a.tablemap: TMID = a.tablemap
    if a.window:   WINDOW_TITLE = a.window
    if not a.apply:
        a.dry_run = True

    rows = psql(f"SELECT name,transform,rgn_left,rgn_top,rgn_right,rgn_bottom,color,radius "
                f"FROM tm_regions WHERE tablemap_id={TMID} AND transform LIKE 'A%' ORDER BY name")
    regions = []
    for line in rows:
        n, tr, l, t, r, b, col, rad = line.split("|")
        regions.append((n, tr, int(l), int(t), int(r), int(b), int(col), int(rad)))
    print(f"{len(regions)} OCR regions on tablemap {TMID}")

    shots = []
    for i in range(a.samples):
        p = os.path.join(WORK, f"s{i}.png")
        if capture(p):
            im = cv2.imread(p)
            if im is not None:
                shots.append(im)
        if i < a.samples - 1:
            time.sleep(a.delay)
    if not shots:
        raise SystemExit(f"could not capture the '{WINDOW_TITLE}' window")
    print(f"captured {len(shots)} frames\n")

    changes = []
    for n, tr, l, t, r, b, col, rad in regions:
        rgb = unpack_abgr(col)
        boxes = [bb for bb in (glyph_bbox(im, l, t, r, b, rgb, rad) for im in shots) if bb]
        if len(boxes) < max(2, len(shots) // 2):
            print(f"  {n:<26} {tr}  no stable glyphs ({len(boxes)}/{len(shots)}) -- skipped")
            continue
        # median is robust to a frame caught mid-animation
        gx0 = int(np.median([x[0] for x in boxes]))
        gy0 = int(np.median([x[1] for x in boxes]))
        gx1 = int(np.median([x[2] for x in boxes]))
        gy1 = int(np.median([x[3] for x in boxes]))

        nb = gy1 + BOTTOM_PAD
        nt = min(t, gy0 - TOP_PAD)
        nl = min(l, gx0 - SIDE_PAD)
        nr = max(r, gx1 + SIDE_PAD)
        if (nl, nt, nr, nb) == (l, t, r, b):
            print(f"  {n:<26} {tr}  already ok (bottom gap {b-gy1})")
            continue
        # Sanity clamp. A correct calibration nudges a box by a pixel or two; a large jump
        # means detection latched onto something else (e.g. a bet pill merging with the
        # avatar below it), and blindly applying that would wreck a working region.
        dh, dw = (nb - nt) - (b - t), (nr - nl) - (r - l)
        if abs(dh) > MAX_GROW_H or abs(dw) > MAX_GROW_W:
            print(f"  {n:<26} {tr}  IMPLAUSIBLE (dh={dh:+d} dw={dw:+d}) -- skipped, needs a look")
            continue
        print(f"  {n:<26} {tr}  bottom gap {b-gy1} -> {BOTTOM_PAD}   "
              f"({l},{t},{r},{b}) -> ({nl},{nt},{nr},{nb})")
        changes.append((n, nl, nt, nr, nb))

    print(f"\n{len(changes)} region(s) need adjustment")
    if not changes:
        return
    if a.dry_run:
        print("dry run -- nothing written. Re-run with --apply to commit.")
        return

    psql("DROP TABLE IF EXISTS tm_regions_backup_ocr", rows=False)
    psql(f"CREATE TABLE tm_regions_backup_ocr AS SELECT * FROM tm_regions WHERE tablemap_id={TMID}",
         rows=False)
    sets = ";".join(
        f"UPDATE tm_regions SET rgn_left={l},rgn_top={t},rgn_right={r},rgn_bottom={b} "
        f"WHERE tablemap_id={TMID} AND name='{n}'" for n, l, t, r, b in changes)
    psql(sets + f";UPDATE tablemaps SET updated_at=now() WHERE id={TMID}", rows=False)
    print(f"applied {len(changes)} change(s); previous geometry saved in tm_regions_backup_ocr")


if __name__ == "__main__":
    main()
