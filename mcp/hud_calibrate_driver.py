#!/usr/bin/env python3
r"""hud_calibrate_driver.py  (runs on windows-beast)

Makes the scrcpy HUD overlay's "Recalibrate all HUDs (Claude)" right-click item ACTUALLY recalibrate,
with no interactive Claude session needed. The overlay only sets g_hud_calibrate_request=true; this
driver watches for it and does the placement itself:

  poll each hiss.exe /api/hud-calibrate-status  ->  on {"pending":true}:
    1. /api/dump-scrapes  (refresh Release\logs\scrapes\_table.bmp = the scrcpy client-area capture)
    2. read that image, ask claude.exe (vision) for the best TOP-LEFT anchor per SEATED chair --
       just below each name-plate, clear of the balance/cards/board/pot/buttons
    3. POST /api/hud-positions?json={"c<chair>":{"x":fx,"y":fy}}  (fractions) -- Hiss repositions +
       persists the boxes AND clears the pending flag.

Chairs are identified by matching each seat's known username (from /api/table-state) to the plate in
the image, so the fractions land on the right chair index. Polls fast (default 8s) so a right-click
recalibrates within seconds; claude.exe only fires when a request is actually pending.
"""
import subprocess, json, urllib.request, urllib.parse, re, os, sys, time

CREATE_NO_WINDOW = 0x08000000
CLAUDE  = r"C:\Users\scarl\.local\bin\claude.exe"
SCRAPES = r"C:\www\openholdembot_old\Release\logs\scrapes"
PORTS   = range(27654, 27665)
LOG     = r"C:\tmp\hud_calibrate.log"
LOCK    = r"C:\tmp\hud_calibrate.lock"
ONCE    = "--once" in sys.argv
FORCE   = next((int(a.split("=")[1]) for a in sys.argv if a.startswith("--force=")), 0)


def log(msg):
    line = time.strftime("%Y-%m-%d %H:%M:%S  ") + msg
    print(line, flush=True)
    try:
        with open(LOG, "a") as f:
            f.write(line + "\n")
    except OSError:
        pass


def urlget(port, path, timeout=6):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def active_ports():
    out = {}
    for p in PORTS:
        try:
            out[p] = json.loads(urlget(p, "/api/table-state", 1.5))
        except Exception:
            pass
    return out


def calibrate(port, state):
    seated = {p["chair"]: (p.get("name") or "").strip()
              for p in state.get("players", []) if p.get("seated")}
    seated = {c: n for c, n in seated.items() if n}
    if not seated:
        log("port %d: no named seated players -- skip" % port); return
    try:
        urlget(port, "/api/dump-scrapes", 10)
    except Exception as e:
        log("port %d: dump-scrapes failed %s" % (port, e)); return
    time.sleep(1.5)
    bmp = os.path.join(SCRAPES, "_table.bmp")
    if not os.path.isfile(bmp):
        log("port %d: no _table.bmp" % port); return
    png = r"C:\tmp\hud_table.png"
    try:
        from PIL import Image
        with Image.open(bmp) as im:
            im.convert("RGB").save(png)
    except Exception:
        png = bmp                      # claude can read the bmp directly if PIL is unavailable
    roster = "; ".join("chair %d = \"%s\"" % (c, n) for c, n in sorted(seated.items()))
    prompt = (
        "Read the poker table screenshot at %s . Seated players and their chair indices: %s . "
        "For EACH listed chair, find that player's name-plate in the image and decide where a small HUD "
        "stat box (roughly 150 wide x 46 tall) should be placed: just BELOW that player's name-plate, near "
        "them, so it does NOT cover their name, balance/stack, or cards, and clear of the community cards, "
        "pot, dealer button and action buttons. Output ONLY one compact JSON object, no prose, mapping each "
        "listed chair index to the TOP-LEFT of its box as FRACTIONS of the image (x = fraction of width 0..1, "
        "y = fraction of height 0..1): {\"0\":{\"x\":0.12,\"y\":0.34}, ...}. Include only the listed chairs."
    ) % (png, roster)
    env = dict(os.environ); env.pop("ANTHROPIC_API_KEY", None)
    env["HISS_NO_CHIME"] = "1"   # no Stop-hook chime for headless calibration runs; see launch_hiss.py
    try:
        r = subprocess.run([CLAUDE, "-p", prompt], capture_output=True, text=True,
                           timeout=300, env=env, creationflags=CREATE_NO_WINDOW)
    except Exception as e:
        log("port %d: claude failed %s" % (port, e)); return
    m = re.search(r"\{.*\}", r.stdout, re.S)
    if not m:
        log("port %d: no JSON from claude: %r" % (port, (r.stdout or "")[:150])); return
    try:
        pos = json.loads(m.group(0))
    except ValueError:
        log("port %d: bad JSON from claude" % port); return
    parts = []
    for k, v in pos.items():
        try:
            ch = int(k)
            fx = max(0.0, min(1.0, float(v["x"])))
            fy = max(0.0, min(1.0, float(v["y"])))
            parts.append('"c%d":{"x":%.4f,"y":%.4f}' % (ch, fx, fy))
        except Exception:
            continue
    if not parts:
        log("port %d: claude returned no usable positions" % port); return
    obj = "{" + ",".join(parts) + "}"
    resp = urlget(port, "/api/hud-positions?json=" + urllib.parse.quote(obj), 8)
    log("port %d: applied %d HUD anchors -> %s" % (port, len(parts), resp.strip()[:60]))


def cycle():
    ports = active_ports()
    for port, state in ports.items():
        try:
            if FORCE and port != FORCE:
                continue
            pending = FORCE == port
            if not pending:
                pending = '"pending":true' in urlget(port, "/api/hud-calibrate-status", 2)
            if pending:
                log("port %d: recalibration %s -> running" % (port, "FORCED" if FORCE else "PENDING"))
                calibrate(port, state)
        except Exception as e:
            log("port %d: ERROR %s" % (port, e))


def acquire_lock():
    try:
        fd = os.open(LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY); os.write(fd, str(os.getpid()).encode()); os.close(fd)
        return True
    except FileExistsError:
        try:
            if time.time() - os.path.getmtime(LOCK) > 600:
                os.remove(LOCK); return acquire_lock()
        except OSError:
            pass
        return False


def main():
    if ONCE or FORCE:
        cycle(); return
    if not acquire_lock():
        log("another hud_calibrate_driver already running; exit"); return
    try:
        log("hud_calibrate_driver started (poll 8s)")
        while True:
            cycle(); time.sleep(8)
    finally:
        try: os.remove(LOCK)
        except OSError: pass


if __name__ == "__main__":
    main()
