#!/usr/bin/env python3
r"""automation_map.py -- build and inspect an AUTOMATION map (automation.* schema).

An automation map is the region map for a PROCESS the bot drives on a phone mirror
(logging in, joining a freeroll) rather than for the felt itself. It lives beside the
playing tablemaps but in its own schema:

    automation.tablemaps            one row per phone mirror   (automation_a17, automation_s10)
    automation.tm_regions           the regions, tagged with (process, step)
    automation.process_screenshots  one reference image per (process, step)

WHY A SEPARATE MODULE (and not inside Automation.exe): the region mapper is an MFC GUI
app; everything here is a headless capture -> place -> draw -> store pipeline that the MCP
server can drive. Automation.exe stays the human's editor for the same rows.

SESSION 0 PROBLEM
    A window can only be captured from the session that owns the desktop. SSH lands in
    session 0 and sees no windows at all, so `capture` run there finds nothing. When that
    happens we hand the job to a scheduled task registered "interactive only" (the same
    trick launch_hiss.py uses) and wait for the file to appear.

Usage:
    python automation_map.py capture   --window A17 --out C:\tmp\a17.png
    python automation_map.py decode    --map automation_s10 --process freeroll --step 1 --out C:\tmp\s10.png
    python automation_map.py ensure-map --map automation_a17 --window A17 --site wpn
    python automation_map.py set-region --map automation_a17 --process login --step 1 \
           --name acr_login_button --rect 120,880,360,940 --transform autoocr0
    python automation_map.py draw      --map automation_a17 --process login --step 1 \
           --shot C:\tmp\a17.png --label "acr login"
"""
import argparse, os, re, subprocess, sys, tempfile, time

PSQL = os.environ.get("HISS_PSQL", r"C:\Program Files\PostgreSQL\12\bin\psql.exe")
PGDB = os.environ.get("PGDATABASE", "hiss")
PGUSER = os.environ.get("PGUSER", "postgres")
PGPASS = os.environ.get("PGPASSWORD", "dbpass")
CAPTURE_TASK = os.environ.get("AUTOMATION_CAPTURE_TASK", "AutomationCapture")
NO_WINDOW = 0x08000000

# Pixels are stored the way the tablemap tables already store them: one row of text per
# scanline, 8 hex chars per pixel, 0x00RRGGBB. Kept in one place so encode/decode agree.
def pack(r, g, b):
    return "%08X" % ((r << 16) | (g << 8) | b)


def unpack(word):
    v = int(word, 16)
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


# --------------------------------------------------------------------------- postgres
def psql(sql, want_rows=True):
    env = dict(os.environ, PGPASSWORD=PGPASS)
    r = subprocess.run([PSQL, "-h", "127.0.0.1", "-U", PGUSER, "-d", PGDB, "-t", "-A", "-c", sql],
                       capture_output=True, text=True, env=env, timeout=180,
                       creationflags=NO_WINDOW if os.name == "nt" else 0)
    if r.returncode:
        raise SystemExit("psql: " + (r.stderr or "").strip())
    return r.stdout.strip() if want_rows else ""


def psql_file(sql_path):
    """Big statements (a screenshot is megabytes) go through a file, never argv."""
    env = dict(os.environ, PGPASSWORD=PGPASS)
    r = subprocess.run([PSQL, "-h", "127.0.0.1", "-U", PGUSER, "-d", PGDB, "-q", "-f", sql_path],
                       capture_output=True, text=True, env=env, timeout=600,
                       creationflags=NO_WINDOW if os.name == "nt" else 0)
    if r.returncode:
        raise SystemExit("psql -f: " + (r.stderr or "").strip())
    return r.stdout


def q(s):
    """Quote a literal for SQL."""
    return "'" + str(s).replace("'", "''") + "'"


# ---------------------------------------------------------------------------- capture
CAP_PS1 = r"""
Add-Type -TypeDefinition @'
using System;using System.Text;using System.Collections.Generic;using System.Runtime.InteropServices;
using System.Drawing;using System.Drawing.Imaging;
public class Cap {
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  delegate bool EnumProc(IntPtr h, IntPtr l);
  // EXACT title first, substring only as a fallback. A stale 144x20 window whose title
  // merely CONTAINED "A17" was being captured instead of the mirror -- every grab came
  // back EMPTY because its client rect is 0x0, while the real mirror sat there fine.
  public static IntPtr Find(string want) {
    IntPtr exact = IntPtr.Zero, loose = IntPtr.Zero;
    EnumWindows((h,l) => {
      if (!IsWindowVisible(h)) return true;
      var sb = new StringBuilder(300); GetWindowText(h, sb, 300);
      string t = sb.ToString();
      if (t == want) { exact = h; return false; }
      if (loose == IntPtr.Zero && t.IndexOf(want, StringComparison.OrdinalIgnoreCase) >= 0) loose = h;
      return true; }, IntPtr.Zero);
    return exact != IntPtr.Zero ? exact : loose; }
  public static string Grab(IntPtr h, string p) {
    RECT r; GetClientRect(h, out r); int w=r.R-r.L, ht=r.B-r.T;
    if (w<=0||ht<=0) return "EMPTY";
    using (Bitmap b=new Bitmap(w,ht,PixelFormat.Format32bppArgb))
    using (Graphics g=Graphics.FromImage(b)) {
      IntPtr dc=g.GetHdc(); PrintWindow(h,dc,3); g.ReleaseHdc(dc); b.Save(p,ImageFormat.Png); }
    return "OK " + w + "x" + ht; }
}
'@ -ReferencedAssemblies System.Drawing
$h = [Cap]::Find('%TITLE%')
if ($h -eq [IntPtr]::Zero) { Write-Output 'NOWINDOW'; exit 1 }
Write-Output ([Cap]::Grab($h, '%OUT%'))
"""


def capture(title, out_path, allow_task=True):
    """Capture a window's CLIENT area by title substring. Returns 'OK WxH'."""
    ps = CAP_PS1.replace("%TITLE%", title).replace("%OUT%", out_path.replace("\\", "\\\\"))
    script = os.path.join(tempfile.gettempdir(), "_automation_cap.ps1")
    with open(script, "w", encoding="utf-8") as f:
        f.write(ps)

    if os.path.exists(out_path):
        os.remove(out_path)
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script],
                       capture_output=True, text=True, timeout=120,
                       creationflags=NO_WINDOW if os.name == "nt" else 0)
    if "OK" in (r.stdout or ""):
        return r.stdout.strip()

    # No window here. If we are in session 0 (ssh/service) the desktop belongs to another
    # session -- hand it to the interactive task and wait for the file.
    if not allow_task:
        raise SystemExit("capture failed: " + ((r.stdout or "") + (r.stderr or "")).strip())
    return capture_via_task(title, out_path)


# The mirrors, exactly as DeveloperToolbar launches them (same serials, titles and
# flags) so a mirror opened from here is indistinguishable from one opened by the
# toolbar button -- same --window-title, therefore the same window the bot attaches to.
SCRCPY = os.environ.get("HISS_SCRCPY", r"C:\www\scrcpy-win64-v4.0\scrcpy.exe")
MIRRORS = {
    "S10":  {"serial": "R58M50TB3BY",   "field": "scrcpy_s10",  "extra": ["--max-fps=1"]},
    "A17":  {"serial": "R5GL205FT7Y",   "field": "scrcpy_a17",  "extra": ["--max-fps=1"]},
    "EMU":  {"serial": "emulator-5554", "field": "scrcpy_emu",
             "extra": ["--max-fps=1", "--no-audio", "--max-size=1080", "--no-clipboard-autosync"]},
    "EMU2": {"serial": "emulator-5556", "field": "scrcpy_emu2",
             "extra": ["--max-fps=1", "--no-audio", "--max-size=1080", "--no-clipboard-autosync"]},
}

POSITION_PS1 = r"""
Add-Type -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public class Pos {
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int ht,bool r);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int cmd);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  delegate bool EnumProc(IntPtr h, IntPtr l);
  public static IntPtr Find(string want) {
    IntPtr hit = IntPtr.Zero;
    EnumWindows((h,l) => { if (!IsWindowVisible(h)) return true;
      var sb = new StringBuilder(300); GetWindowText(h, sb, 300);
      if (sb.ToString() == want) { hit = h; return false; } return true; }, IntPtr.Zero);
    return hit; }
}
'@
$h = [Pos]::Find('%TITLE%')
if ($h -eq [IntPtr]::Zero) { Write-Output 'NOWINDOW'; exit 1 }
[void][Pos]::MoveWindow($h, %X%, %Y%, %W%, %H%, $true)
Write-Output 'POSITIONED'
"""


def saved_rect(field):
    """Where DeveloperToolbar last recorded this mirror: 'x,y,w,h' in devtoolbar_windows."""
    raw = psql("SELECT value->>%s FROM settings WHERE key='devtoolbar_windows'" % q(field)).strip()
    if not raw:
        return None
    try:
        x, y, w, h = (int(v) for v in raw.split(","))
    except ValueError:
        return None
    return (x, y, w, h) if w >= 50 and h >= 50 else None


def open_mirror(title, direct=False):
    """Launch the scrcpy mirror and put its window back where it was recorded.

    Must happen on the logged-on desktop -- a window created from session 0 lives on a
    desktop nobody can see or capture -- so from anywhere else this is handed to the
    interactive task, same as capture.
    """
    if title not in MIRRORS:
        raise SystemExit("unknown mirror %r (known: %s)" % (title, ", ".join(sorted(MIRRORS))))
    if not direct and not _has_desktop():
        return run_interactive("openmirror " + title)

    dev = MIRRORS[title]
    subprocess.Popen([SCRCPY, "-s", dev["serial"], "--window-title=" + title] + dev["extra"],
                     close_fds=True, creationflags=0x00000008 | 0x08000000 if os.name == "nt" else 0)

    # Wait for the MIRROR, not merely for something whose title contains "A17": a stale
    # 144x20 window matched instantly and had us position before scrcpy even existed.
    probe = os.path.join(tempfile.gettempdir(), "_mirror_probe.png")
    for _ in range(60):
        time.sleep(1)
        try:
            got = capture(title, probe, allow_task=False)
        except SystemExit:
            continue
        m = re.search(r"OK (\d+)x(\d+)", got or "")
        if m and int(m.group(1)) >= 300 and int(m.group(2)) >= 600:
            break

    rect = saved_rect(dev["field"])
    if rect:
        ps = (POSITION_PS1.replace("%TITLE%", title).replace("%X%", str(rect[0]))
              .replace("%Y%", str(rect[1])).replace("%W%", str(rect[2])).replace("%H%", str(rect[3])))
        script = os.path.join(tempfile.gettempdir(), "_automation_pos.ps1")
        with open(script, "w", encoding="utf-8") as f:
            f.write(ps)
        r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script],
                           capture_output=True, text=True, timeout=60,
                           creationflags=NO_WINDOW if os.name == "nt" else 0)
        return "%s opened, %s (%s)" % (title, (r.stdout or "").strip(), rect)
    return "%s opened (no saved position for %s)" % (title, dev["field"])


def position_mirror(title, direct=False):
    """Put an already-open mirror back on its recorded rect (client area must match the
    size the regions were mapped against, or every coordinate is wrong)."""
    if title not in MIRRORS:
        raise SystemExit("unknown mirror %r" % title)
    if not direct and not _has_desktop():
        return run_interactive("position " + title)
    rect = saved_rect(MIRRORS[title]["field"])
    if not rect:
        return "no saved position for %s" % title
    ps = (POSITION_PS1.replace("%TITLE%", title).replace("%X%", str(rect[0]))
          .replace("%Y%", str(rect[1])).replace("%W%", str(rect[2])).replace("%H%", str(rect[3])))
    script = os.path.join(tempfile.gettempdir(), "_automation_pos.ps1")
    with open(script, "w", encoding="utf-8") as f:
        f.write(ps)
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script],
                       capture_output=True, text=True, timeout=60,
                       creationflags=NO_WINDOW if os.name == "nt" else 0)
    return "%s -> %s : %s" % (title, rect, (r.stdout or r.stderr or "").strip())


def _has_desktop():
    """Can this session see any window at all? Session 0 cannot."""
    script = os.path.join(tempfile.gettempdir(), "_has_desktop.ps1")
    with open(script, "w", encoding="utf-8") as f:
        f.write("if ((Get-Process | Where-Object { $_.MainWindowTitle -ne '' }).Count -gt 0)"
                " { 'YES' } else { 'NO' }")
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script],
                       capture_output=True, text=True, timeout=60,
                       creationflags=NO_WINDOW if os.name == "nt" else 0)
    return "YES" in (r.stdout or "")


def run_interactive(request_line):
    """Ask the desktop-session task to do something and wait for its log to answer."""
    os.makedirs(r"C:\tmp", exist_ok=True)
    done = r"C:\tmp\automation_task.done"
    if os.path.exists(done):
        os.remove(done)
    with open(r"C:\tmp\automation_capture.req", "w", encoding="utf-8") as f:
        f.write(request_line + "\n")
    subprocess.run(["schtasks", "/run", "/tn", CAPTURE_TASK], capture_output=True, text=True,
                   timeout=60, creationflags=NO_WINDOW if os.name == "nt" else 0)
    for _ in range(180):
        time.sleep(1)
        if os.path.exists(done):
            with open(done, encoding="utf-8") as f:
                return f.read().strip()
    return "interactive task did not report back"


def capture_via_task(title, out_path):
    """Run the same capture inside the logged-on desktop session via a scheduled task."""
    req = os.path.join(r"C:\tmp", "automation_capture.req")
    os.makedirs(r"C:\tmp", exist_ok=True)
    with open(req, "w", encoding="utf-8") as f:
        f.write(title + "\n" + out_path + "\n")
    subprocess.run(["schtasks", "/run", "/tn", CAPTURE_TASK], capture_output=True, text=True,
                   timeout=60, creationflags=NO_WINDOW if os.name == "nt" else 0)
    for _ in range(120):
        time.sleep(0.5)
        if os.path.exists(out_path) and os.path.getsize(out_path) > 0:
            return "OK (via %s)" % CAPTURE_TASK
    raise SystemExit("capture via task produced nothing -- is the desktop logged on and is "
                     "the '%s' task registered? (run: automation_map.py install-task)" % CAPTURE_TASK)


# ------------------------------------------------------------------------------- maps
def map_id(name, create_with=None):
    got = psql("SELECT id FROM automation.tablemaps WHERE name = %s" % q(name))
    if got:
        return int(got.splitlines()[0])
    if not create_with:
        raise SystemExit("no automation map named %r (use ensure-map first)" % name)
    window_title, site = create_with
    # id is not a sequence here (the S10 row was inserted by hand) -- take max+1.
    psql("INSERT INTO automation.tablemaps (id, name, sitename, titletext, bits_per_pixel, updated_at) "
         "SELECT COALESCE(MAX(id),0)+1, %s, %s, %s, 32, now() FROM automation.tablemaps"
         % (q(name), q(site), q(window_title)), want_rows=False)
    return int(psql("SELECT id FROM automation.tablemaps WHERE name = %s" % q(name)).splitlines()[0])


def set_region(tm, name, rect, process, step, transform=None, radius=0, color=0):
    left, top, right, bottom = rect
    psql("DELETE FROM automation.tm_regions WHERE tablemap_id=%d AND name=%s" % (tm, q(name)),
         want_rows=False)
    psql(
        "INSERT INTO automation.tm_regions "
        "(tablemap_id, name, rgn_left, rgn_top, rgn_right, rgn_bottom, color, radius, transform, "
        " use_default, process, step) VALUES "
        "(%d, %s, %d, %d, %d, %d, %d, %d, %s, true, %s, %d)"
        % (tm, q(name), left, top, right, bottom, color, radius,
           q(transform) if transform else "NULL", q(process), step),
        want_rows=False)


def regions(tm, process=None, step=None):
    where = "tablemap_id=%d" % tm
    if process is not None:
        where += " AND process=%s" % q(process)
    if step is not None:
        where += " AND step=%d" % step
    out = psql("SELECT name||'|'||rgn_left||'|'||rgn_top||'|'||rgn_right||'|'||rgn_bottom||'|'||"
               "COALESCE(transform,'') FROM automation.tm_regions WHERE %s ORDER BY name" % where)
    rows = []
    for line in out.splitlines():
        if line.count("|") < 5:
            continue
        n, l, t, r, b, tr = line.split("|", 5)
        rows.append({"name": n, "rect": (int(l), int(t), int(r), int(b)), "transform": tr})
    return rows


# ------------------------------------------------------------------- screenshots (I/O)
def store_screenshot(tm, process, step, label, png_path):
    from PIL import Image
    im = Image.open(png_path).convert("RGB")
    w, h = im.size
    px = im.load()
    lines = ["".join(pack(*px[x, y]) for x in range(w)) for y in range(h)]
    body = "\n".join(lines)

    # newline="" is load-bearing: the scanline separators are DATA inside the SQL literal.
    # Left to Windows text mode they become \r\n, which comes back as a blank row between
    # every real one -- the image decodes stretched 2x with black gaps.
    sql_path = os.path.join(tempfile.gettempdir(), "_automation_shot.sql")
    with open(sql_path, "w", encoding="utf-8", newline="") as f:
        f.write("DELETE FROM automation.process_screenshots WHERE tablemap_id=%d AND process=%s AND step=%d;\n"
                % (tm, q(process), step))
        f.write("INSERT INTO automation.process_screenshots "
                "(tablemap_id, process, step, label, width, height, pixels, updated_at) VALUES (%d, %s, %d, %s, %d, %d, %s, now());\n"
                % (tm, q(process), step, q(label), w, h, q(body)))
    psql_file(sql_path)
    os.remove(sql_path)
    return w, h


def screenshot_size(tm, process, step):
    """Size of the reference capture the regions were placed on, or None."""
    meta = psql("SELECT width||'|'||height FROM automation.process_screenshots "
                "WHERE tablemap_id=%d AND process=%s AND step=%d" % (tm, q(process), step))
    if not meta.strip():
        return None
    w, h = meta.splitlines()[0].split("|")
    return int(w), int(h)


def load_screenshot(tm, process, step, out_path):
    from PIL import Image
    meta = psql("SELECT width||'|'||height FROM automation.process_screenshots "
                "WHERE tablemap_id=%d AND process=%s AND step=%d" % (tm, q(process), step))
    if not meta:
        raise SystemExit("no screenshot for that (map, process, step)")
    w, h = (int(v) for v in meta.splitlines()[0].split("|"))
    # The field itself contains one newline per scanline, so unaligned -t -A output IS
    # the pixel text, verbatim -- no escaping to undo.
    # A real scanline is width*8 chars, so blanks are never data -- drop them rather than
    # trusting whatever newline convention the row was written with.
    rows = [r.strip("\r") for r in
            psql("SELECT pixels FROM automation.process_screenshots "
                 "WHERE tablemap_id=%d AND process=%s AND step=%d"
                 % (tm, q(process), step)).splitlines() if r.strip()]
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(min(h, len(rows))):
        line = rows[y]
        for x in range(w):
            word = line[x * 8:(x + 1) * 8]
            if len(word) == 8:
                px[x, y] = unpack(word)
    im.save(out_path)
    return w, h


def draw_regions(src_png, out_png, rows):
    """Draw every region on a copy of the shot: box + name, so the map is self-documenting."""
    from PIL import Image, ImageDraw
    palette = ["#00e5ff", "#ffd400", "#ff4fa3", "#7cff6b", "#ff8a3d", "#b388ff"]
    im = Image.open(src_png).convert("RGB")
    d = ImageDraw.Draw(im)
    for i, r in enumerate(rows):
        l, t, rt, b = r["rect"]
        c = palette[i % len(palette)]
        d.rectangle([l, t, rt, b], outline=c, width=3)
        tag = r["name"] + (("  [" + r["transform"] + "]") if r.get("transform") else "")
        ty = max(0, t - 15)
        d.rectangle([l, ty, l + 7 * len(tag) + 6, ty + 14], fill="#000000")
        d.text((l + 3, ty + 2), tag, fill=c)
    im.save(out_png)
    return out_png


# ------------------------------------------------------------------------------- task
def install_task():
    """Register the interactive-session capture helper (mirrors HissLaunch's logon mode)."""
    runner = os.path.join(os.path.dirname(os.path.abspath(__file__)), "automation_capture_task.py")
    pyw = os.environ.get("HISS_PYTHONW",
                         r"C:\Users\scarl\AppData\Local\Programs\Python\Python310\pythonw.exe")
    cmd = ["schtasks", "/create", "/f", "/tn", CAPTURE_TASK, "/sc", "once", "/st", "00:00",
           "/tr", '"%s" "%s"' % (pyw, runner), "/it"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    return (r.stdout or "") + (r.stderr or "")


# ------------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("capture");     c.add_argument("--window", required=True); c.add_argument("--out", required=True)
    e = sub.add_parser("ensure-map");  e.add_argument("--map", required=True); e.add_argument("--window", required=True); e.add_argument("--site", default="wpn")
    s = sub.add_parser("set-region");  s.add_argument("--map", required=True); s.add_argument("--name", required=True)
    s.add_argument("--rect", required=True, help="left,top,right,bottom"); s.add_argument("--process", required=True)
    s.add_argument("--step", type=int, default=1); s.add_argument("--transform", default=None)
    l = sub.add_parser("list");        l.add_argument("--map", required=True); l.add_argument("--process", default=None)
    d = sub.add_parser("draw");        d.add_argument("--map", required=True); d.add_argument("--process", required=True)
    d.add_argument("--step", type=int, default=1); d.add_argument("--shot", required=True); d.add_argument("--label", default=None)
    d.add_argument("--out", default=None)
    de = sub.add_parser("decode");     de.add_argument("--map", required=True); de.add_argument("--process", required=True)
    de.add_argument("--step", type=int, default=1); de.add_argument("--out", required=True)
    pm = sub.add_parser("position-mirror"); pm.add_argument("--window", required=True)
    pm.add_argument("--direct", action="store_true")
    om = sub.add_parser("open-mirror"); om.add_argument("--window", required=True)
    om.add_argument("--direct", action="store_true", help="run here (already on the desktop)")
    sub.add_parser("install-task")

    a = ap.parse_args()

    if a.cmd == "capture":
        print(capture(a.window, a.out))
    elif a.cmd == "ensure-map":
        print(map_id(a.map, create_with=(a.window, a.site)))
    elif a.cmd == "set-region":
        rect = tuple(int(v) for v in a.rect.split(","))
        set_region(map_id(a.map), a.name, rect, a.process, a.step, a.transform)
        print("ok")
    elif a.cmd == "list":
        for r in regions(map_id(a.map), a.process):
            print(r)
    elif a.cmd == "draw":
        tm = map_id(a.map)
        rows = regions(tm, a.process, a.step)
        out = a.out or (os.path.splitext(a.shot)[0] + "_regions.png")
        draw_regions(a.shot, out, rows)
        w, h = store_screenshot(tm, a.process, a.step, a.label or (a.process + " step %d" % a.step), out)
        print("stored %dx%d with %d regions -> %s" % (w, h, len(rows), out))
    elif a.cmd == "decode":
        print(load_screenshot(map_id(a.map), a.process, a.step, a.out))
    elif a.cmd == "position-mirror":
        print(position_mirror(a.window, direct=a.direct))
    elif a.cmd == "open-mirror":
        print(open_mirror(a.window, direct=a.direct))
    elif a.cmd == "install-task":
        print(install_task())


if __name__ == "__main__":
    main()
