#!/usr/bin/env python3
"""Health-check EVERY region of a tablemap against its live table.

Each transform type is verified the way Hiss actually evaluates it:

  I   image match  - replicate ITypeTransform: only same-size templates are eligible,
                     nearest by mean |dR|+|dG|+|dB|, blank if <40, accept if <90.
  C   colour probe - does the region's mean colour sit inside the key colour's cube
                     (radius)? radius 0 means an EXACT match and is inherently brittle.
  A*/T* text       - report what hiss.exe itself OCR'd (from /api/dump-scrapes).
  N                - nothing to scrape; skipped.

Usage:
  python verify_all_regions.py --tablemap 282 --window EMU --port 27655
"""
import argparse
import os
import subprocess
import tempfile
import time

import cv2
import numpy as np

PSQL = r"C:\Program Files\PostgreSQL\12\bin\psql.exe"
SCRAPES = r"C:\www\openholdembot_old\Release\logs\scrapes"
ACCEPT, BLANK = 90.0, 40.0
WORK = os.path.join(tempfile.gettempdir(), "verify_regions")
os.makedirs(WORK, exist_ok=True)

CAP = r"""
Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;using System.Drawing;using System.Drawing.Imaging;
public class Cap {
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  public static void Grab(IntPtr h, string p) {
    RECT r; GetClientRect(h, out r); int w=r.R-r.L, ht=r.B-r.T; if(w<=0||ht<=0) return;
    using (Bitmap b=new Bitmap(w,ht,PixelFormat.Format32bppArgb))
    using (Graphics g=Graphics.FromImage(b)) {
      IntPtr dc=g.GetHdc(); PrintWindow(h,dc,3); g.ReleaseHdc(dc); b.Save(p,ImageFormat.Png); }
  }
}
'@ -ReferencedAssemblies System.Drawing
$p = Get-Process scrcpy -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -eq '%T%' } | Select-Object -First 1
if (-not $p) { Write-Output "NOWINDOW"; exit }
[Cap]::Grab($p.MainWindowHandle, '%O%'); Write-Output "OK"
"""


def psql(sql):
    env = dict(os.environ, PGPASSWORD="dbpass")
    r = subprocess.run([PSQL, "-h", "127.0.0.1", "-U", "postgres", "-d", "hiss",
                        "-t", "-A", "-c", sql], capture_output=True, text=True, env=env, timeout=90)
    if r.returncode:
        raise SystemExit(r.stderr.strip())
    return r.stdout


def capture(title, path):
    s = CAP.replace("%T%", title).replace("%O%", path.replace("\\", "\\\\"))
    f = os.path.join(WORK, "_c.ps1")
    open(f, "w", encoding="utf-8").write(s)
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", f],
                       capture_output=True, text=True, timeout=90)
    return "OK" in r.stdout


def load_templates(tmid):
    out = psql(f"SELECT name||'~'||width||'~'||height||'~'||replace(pixels, chr(10), '@') "
               f"FROM tm_images WHERE tablemap_id={tmid}")
    by = {}
    for line in out.split("\n"):
        if "~" not in line:
            continue
        nm, w, h, pix = line.split("~", 3)
        w, h = int(w), int(h)
        rows = pix.split("@")
        if len(rows) != h:
            continue
        img = np.zeros((h, w, 3), np.int16)
        ok = True
        for y in range(h):
            if len(rows[y]) < w * 8:
                ok = False
                break
            v = [int(rows[y][x * 8:(x + 1) * 8], 16) for x in range(w)]
            img[y, :, 0] = [(p >> 0) & 0xFF for p in v]
            img[y, :, 1] = [(p >> 8) & 0xFF for p in v]
            img[y, :, 2] = [(p >> 16) & 0xFF for p in v]
        if ok:
            by.setdefault((w, h), []).append((nm, img))
    return by


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tablemap", type=int, required=True)
    ap.add_argument("--window", required=True)
    ap.add_argument("--port", type=int)
    a = ap.parse_args()

    if a.port:
        subprocess.run(["curl", "-s", "--max-time", "10",
                        f"http://127.0.0.1:{a.port}/api/dump-scrapes"], capture_output=True, timeout=30)
        time.sleep(4)

    shot = os.path.join(WORK, f"{a.window}.png")
    if not capture(a.window, shot):
        raise SystemExit(f"window '{a.window}' not found")
    img = cv2.imread(shot)
    tmpl = load_templates(a.tablemap)

    rows = psql(f"SELECT name||'~'||transform||'~'||rgn_left||'~'||rgn_top||'~'||rgn_right||'~'"
                f"||rgn_bottom||'~'||color||'~'||radius FROM tm_regions "
                f"WHERE tablemap_id={a.tablemap} ORDER BY name").strip().split("\n")

    tally = {}
    problems = []
    for line in rows:
        if "~" not in line:
            continue
        n, tr, l, t, r, b, col, rad = line.split("~")
        l, t, r, b, col, rad = int(l), int(t), int(r), int(b), int(col), int(rad)
        crop = img[t:b + 1, l:r + 1] if (0 <= t and 0 <= l and b < img.shape[0] and r < img.shape[1]) else None
        kind = tr[0]

        if kind == "N":
            verdict = "skip(N)"
        elif crop is None or crop.size == 0:
            verdict = "OFF-SCREEN"
            problems.append((n, tr, verdict, f"rect ({l},{t},{r},{b}) outside {img.shape[1]}x{img.shape[0]}"))
        elif kind == "I":
            h, w = crop.shape[:2]
            cands = tmpl.get((w, h))
            if not cands:
                verdict = "NO-TEMPLATES"
                problems.append((n, tr, verdict, f"{w}x{h}; sizes available: "
                                                 f"{sorted(f'{k[0]}x{k[1]}' for k in tmpl)}"))
            else:
                rgb = cv2.cvtColor(crop, cv2.COLOR_BGR2RGB).astype(np.int16)
                best, bd, blankd = None, 1e9, 1e9
                for nm, ti in cands:
                    d = np.abs(rgb - ti).sum() / (w * h * 3)
                    if d < bd:
                        best, bd = nm, d
                    if nm == "" and d < blankd:
                        blankd = d
                if blankd < BLANK:
                    verdict = "blank"
                elif bd < ACCEPT:
                    verdict = f"match '{best}' ({bd:.0f})"
                else:
                    verdict = "NO-MATCH"
                    problems.append((n, tr, verdict, f"closest '{best}' {bd:.0f} > {ACCEPT}"))
        elif kind == "C":
            mean = crop.reshape(-1, 3).mean(axis=0)          # BGR
            kr, kg, kb = col & 0xFF, (col >> 8) & 0xFF, (col >> 16) & 0xFF
            dist = max(abs(mean[2] - kr), abs(mean[1] - kg), abs(mean[0] - kb))
            if dist <= max(rad, 0):
                verdict = f"probe-hit ({dist:.0f}<={rad})"
            else:
                verdict = f"probe-miss ({dist:.0f}>{rad})"
                if rad == 0:
                    problems.append((n, tr, "BRITTLE radius=0", f"mean off by {dist:.0f}"))
        else:                                                 # A*, T*, CL ...
            p = os.path.join(SCRAPES, f"{n}.txt")
            val = open(p).read().strip() if os.path.exists(p) else "<no dump>"
            verdict = f"ocr='{val[:22]}'"
            if val == "":
                problems.append((n, tr, "EMPTY-OCR", "region scraped nothing"))
        tally[verdict.split("(")[0].split("'")[0].strip()] = \
            tally.get(verdict.split("(")[0].split("'")[0].strip(), 0) + 1
        print(f"  {n:<30}{tr:<4}{verdict}")

    print(f"\n=== summary for tablemap {a.tablemap} ({a.window}) ===")
    for k, v in sorted(tally.items(), key=lambda kv: -kv[1]):
        print(f"  {k:<18}{v}")
    print(f"\n{len(problems)} problem region(s):")
    for n, tr, v, why in problems:
        print(f"  {n:<30}{tr:<4}{v:<18}{why}")


if __name__ == "__main__":
    main()
