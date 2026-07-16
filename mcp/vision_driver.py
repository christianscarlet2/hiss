#!/usr/bin/env python3
r"""vision_driver.py  (runs on windows-beast)

Full-vision table reader / smart-tier OCR. hiss.exe's tesseract "Vision" region OCR is the FAST tier
(stacks, pot, per-heartbeat) but mangles the table title + blind level -> garbage headers -> hands
dropped from the PT4 export (drop_no_bigblind). This driver closes the gap:

  for each connected phone:
    1. adb screencap  -> a clean full-res PNG (bypasses OpenHoldem's fixed-region OCR)
    2. claude.exe -p  -> compact JSON {table_name, blinds(BB-frame)+chips_per_bb, tourney id, hand#}
    3. match the read to the owning hiss.exe instance (by table name vs /api/table-state)
    4. push it back as AUTHORITATIVE via the bot's HTTP hooks:
         /api/table-game-info      (sb,bb,chips_per_bb,tourney_name,tourney_id,table_number)
         /api/table-game-info-2    (curr_hand, prev_hand)
         /api/set-region-value     (c0table_name, c0tourney_title, c0tourney_id)

Blinds/table-name are stable per level, so a slow cadence (every few min) is plenty and cheap.
The bot's writer then emits correct headers -> higher PT4 yield + correct $-denominated stacks
(the phone shows BB; chips_per_bb converts to dollars, killing the 78.39->78398 garble).
"""
import subprocess, json, urllib.request, urllib.parse, re, sys, os, time, difflib

CREATE_NO_WINDOW = 0x08000000          # keep adb/claude.exe child processes from flashing a console
CLAUDE = r"C:\Users\scarl\.local\bin\claude.exe"
PYDIR  = r"C:\tmp"
PORTS  = range(27654, 27665)
ONCE   = "--once" in sys.argv
LOG    = r"C:\tmp\vision_driver.log"


def log(msg):
    line = time.strftime("%Y-%m-%d %H:%M:%S  ") + msg
    print(line, flush=True)
    try:
        with open(LOG, "a") as f:
            f.write(line + "\n")
    except OSError:
        pass


def urlget(port, path, timeout=6):
    url = "http://127.0.0.1:%d%s" % (port, path)
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def active_ports():
    out = {}
    for p in PORTS:
        try:
            out[p] = json.loads(urlget(p, "/api/table-state", timeout=1.5))
        except Exception:
            pass
    return out


def devices():
    r = subprocess.run(["adb", "devices"], capture_output=True, text=True, timeout=20,
                       creationflags=CREATE_NO_WINDOW)
    return [ln.split("\t")[0] for ln in r.stdout.splitlines()[1:]
            if ln.strip() and "\tdevice" in ln]


def screencap(serial, path):
    with open(path, "wb") as f:
        subprocess.run(["adb", "-s", serial, "exec-out", "screencap", "-p"],
                       stdout=f, timeout=30, creationflags=CREATE_NO_WINDOW)
    return os.path.getsize(path) > 1000


PROMPT = (
    "Read the image file at %s . It is an ACR Poker table screenshot. Output ONLY a compact single-line "
    "JSON object, no prose and no code fence, with keys: "
    "table_name (the table title, e.g. \"Matteson - No Limit\"), "
    "sb_bbframe (small-blind badge value in big blinds, e.g. 0.5 or 0.4), "
    "bb_bbframe (big-blind badge in big blinds, normally 1), "
    "sb_dollars (the small blind in DOLLARS from the stakes line, e.g. for \"$0.25 / $0.50 Hold'em\" this is 0.25), "
    "bb_dollars (the big blind in DOLLARS from the stakes line, e.g. 0.50), "
    "tourney_id (the id in parentheses on the stakes line, e.g. 35413971T477), "
    "curr_hand (the number after \"Current:\" as a string, or \"\" if absent), "
    "prev_hand (the number after \"Previous:\" as a string, or \"\"), "
    "seats (array, one per player plate that shows a username, of "
    "{name (exact username), sitting_out (true if the seat is greyed out / shows a JOIN button / "
    "has no chip stack, else false)}; DO skip fully empty seats). "
    "If the table is not clearly visible output {\"table_name\":\"\"}."
)


def vision_read(path):
    env = dict(os.environ)
    env.pop("ANTHROPIC_API_KEY", None)          # use the logged-in Claude Code subscription auth
    r = subprocess.run([CLAUDE, "-p", PROMPT % path],
                       capture_output=True, text=True, timeout=240, env=env,
                       creationflags=CREATE_NO_WINDOW)
    m = re.search(r"\{.*\}", r.stdout, re.S)
    if not m:
        raise ValueError("no JSON from claude.exe: %r" % (r.stdout[:200]))
    return json.loads(m.group(0))


def _norm(s):
    return re.sub(r"[^a-z0-9]", "", (s or "").lower())


def match_port(vision, ports_state):
    """Match the vision read to the owning instance by table-name overlap with /api/table-state.table."""
    key = _norm(vision.get("table_name"))
    if len(key) < 4:
        return None
    for p, st in ports_state.items():
        tf = _norm(st.get("table", ""))         # e.g. "35414155t4rozelnolimit"
        if key[:6] in tf or tf and _norm(vision.get("tourney_id", ""))[:6] and _norm(vision["tourney_id"])[:6] in tf:
            return p
        if key[:6] and key[:6] in tf:
            return p
    return None


def push(port, v):
    q = {}
    # These are OBSERVER tables: engine decisions are irrelevant, only the written HH text matters.
    # The writer formats "Level (sblind/bblind)" and "posts the big blind <bb>" straight from sb/bb.
    # Push DOLLAR blinds read off the stakes line and do NOT send chips_per_bb -- sending it makes the
    # bot re-normalise sblind/bblind back to the BB-frame (0.4/1.0), overriding the dollar values.
    sb = v.get("sb_dollars") or ((v.get("sb_bbframe") or 0) * (v.get("chips_per_bb") or 0) or None)
    bb = v.get("bb_dollars") or ((v.get("bb_bbframe") or 0) * (v.get("chips_per_bb") or 0) or None)
    if sb:  q["sb"] = round(sb, 4)
    if bb:  q["bb"] = round(bb, 4)
    if v.get("table_name"):    q["tourney_name"] = v["table_name"]
    if v.get("tourney_id"):    q["tourney_id"] = v["tourney_id"]
    if q:
        urlget(port, "/api/table-game-info?" + urllib.parse.urlencode(q))
    if v.get("curr_hand"):
        h = {"curr_hand": v["curr_hand"]}
        if v.get("prev_hand"): h["prev_hand"] = v["prev_hand"]
        urlget(port, "/api/table-game-info-2?" + urllib.parse.urlencode(h))
    if v.get("table_name"):
        tn = urllib.parse.quote(v["table_name"])
        urlget(port, "/api/set-region-value?name=c0table_name&value=" + tn)
        urlget(port, "/api/set-region-value?name=c0tourney_title&value=" + tn)
    if v.get("tourney_id"):
        urlget(port, "/api/set-region-value?name=c0tourney_id&value=" + urllib.parse.quote(v["tourney_id"]))


def lint_names(port, seats, state):
    """Push Claude-clean usernames onto the per-chair name regions (p<chair>name), snapping the
    fast-tier OCR read (e.g. 'Brokenphear.t') to the true name ('Brokenpheart'). Each vision name
    is matched to the chair whose OCR name it most resembles, so we never need to solve the visual
    position -> chair-index mapping. SITTING-OUT seats are skipped entirely -- per the rule 'don't
    detect a sitting-out player's name or stack', the writer keeps its last sitting-in _known_name.
    """
    ocr = {p["chair"]: (p.get("name") or "") for p in state.get("players", []) if p.get("seated")}
    n = 0
    for s in seats or []:
        name = (s.get("name") or "").strip()
        if not name or s.get("sitting_out"):
            continue
        best, br = None, 0.0
        for ch, onm in ocr.items():
            if not onm:
                continue
            r = difflib.SequenceMatcher(None, name.lower(), onm.lower()).ratio()
            if r > br:
                best, br = ch, r
        if best is not None and br >= 0.6 and name != ocr[best]:
            urlget(port, "/api/set-region-value?name=p%dname&value=%s" % (best, urllib.parse.quote(name)))
            ocr.pop(best, None)          # one vision name per chair
            n += 1
    return n


def cycle():
    ports_state = active_ports()
    if not ports_state:
        log("no live Hiss instances found"); return
    devs = devices()
    log("instances=%s devices=%s" % (sorted(ports_state), devs))
    for serial in devs:
        path = os.path.join(PYDIR, "vis_%s.png" % serial)
        try:
            if not screencap(serial, path):
                log("%s: empty screencap" % serial); continue
            v = vision_read(path)
            if not v.get("table_name"):
                log("%s: no table visible" % serial); continue
            port = match_port(v, ports_state)
            if not port:
                log("%s: read '%s' (%s) but no matching instance" %
                    (serial, v.get("table_name"), v.get("tourney_id"))); continue
            push(port, v)
            nlint = lint_names(port, v.get("seats"), ports_state[port])
            log("%s -> port %d: %s  blinds=%s/%s tid=%s hand=%s names_linted=%d" %
                (serial, port, v.get("table_name"), v.get("sb_dollars"), v.get("bb_dollars"),
                 v.get("tourney_id"), v.get("curr_hand"), nlint))
        except Exception as e:
            log("%s: ERROR %s" % (serial, e))


LOCK = r"C:\tmp\vision_driver.lock"


def acquire_lock():
    """Prevent overlapping runs (a slow claude.exe cycle must not collide with the next schtask)."""
    try:
        fd = os.open(LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        os.write(fd, str(os.getpid()).encode()); os.close(fd)
        return True
    except FileExistsError:
        try:                                  # stale lock (>10 min) -> steal it
            if time.time() - os.path.getmtime(LOCK) > 600:
                os.remove(LOCK); return acquire_lock()
        except OSError:
            pass
        return False


def release_lock():
    try: os.remove(LOCK)
    except OSError: pass


def main():
    if not acquire_lock():
        log("another vision_driver run holds the lock; skipping"); return
    try:
        if ONCE:
            cycle(); return
        interval = int(os.environ.get("VISION_INTERVAL", "180"))
        while True:
            cycle()
            time.sleep(interval)
    finally:
        release_lock()


if __name__ == "__main__":
    main()
