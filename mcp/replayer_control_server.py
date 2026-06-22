#!/usr/bin/env python3
"""replayer_control_server.py - bridge replay.html's "Play in Replayer.exe" to replayer.exe.

A tiny CORS HTTP server on the Windows bot box. replay.html calls:
  GET/POST /play?hand=<H>   start continuous play from hand H
  GET/POST /stop            stop
  GET      /status          {playing, hand}

Continuous play: each hand's PNG frames are fetched from the replay server, converted to the
BMP frames replayer.exe plays, and handed to replayer via a control file. When replayer finishes
a hand it writes a ".status" marker; we then advance to the next NEWER hand (by start_ts) and,
once at the newest, poll for new hands as they arrive (follows a live session) until /stop.

  python replayer_control_server.py      # listens on :8089 (REPLAYER_CTRL_PORT)
"""
import os, io, json, time, threading, urllib.request
from urllib.parse import urlparse, parse_qs
from http.server import BaseHTTPRequestHandler, HTTPServer
import subprocess

REPLAY_BASE  = os.environ.get("HISS_REPLAY_URL",  "http://192.168.1.39")
REPLAY_HOST  = os.environ.get("HISS_REPLAY_HOST", "hiss.scarletbeast.com")
REPLAYER_EXE = os.environ.get("HISS_REPLAYER_EXE", r"C:\www\openholdembot_old\Release\replayer.exe")
REPLAYER_DIR = os.environ.get("HISS_REPLAYER_DIR", r"C:\tmp\replayer")
CONTROL = os.path.join(REPLAYER_DIR, "control.txt")
STATUS  = CONTROL + ".status"
PORT    = int(os.environ.get("REPLAYER_CTRL_PORT", "8089"))
PLAY_MS = int(os.environ.get("REPLAYER_PLAY_MS", "600"))


def replay_get(path, raw=False, base=None, host=None):
    # base/host let each caller choose WHICH replay store to read from:
    #   - server UI (swiftsnake):  base=http://192.168.1.39  host=hiss.scarletbeast.com
    #   - local UI  (BEAST :8090): base=http://127.0.0.1:8090 host="" (no Host override)
    # host=None  -> use the legacy default (REPLAY_HOST); host=""  -> send no Host header.
    base = base or REPLAY_BASE
    headers = {}
    use_host = REPLAY_HOST if host is None else host
    if use_host:
        headers["Host"] = use_host
    req = urllib.request.Request(base + path, headers=headers)
    with urllib.request.urlopen(req, timeout=15) as r:
        data = r.read()
    return data if raw else json.loads(data.decode("utf-8", errors="replace"))


def hand_to_bmp(hand, dest_dir, base=None, host=None):
    """Fetch hand's PNG frames -> frame??????.bmp in dest_dir. Returns frame count."""
    from PIL import Image
    os.makedirs(dest_dir, exist_ok=True)
    for old in os.listdir(dest_dir):
        if old.startswith("frame") and old.endswith(".bmp"):
            try: os.remove(os.path.join(dest_dir, old))
            except Exception: pass
    frames = replay_get("/api/frames/%s" % urllib.request.quote(str(hand)), base=base, host=host)
    idx = 0
    for f in frames:
        ts = f.get("ts_ms")
        if ts is None:
            continue
        try:
            png = replay_get("/api/img/%s/%d" % (urllib.request.quote(str(hand)), int(ts)),
                             raw=True, base=base, host=host)
            Image.open(io.BytesIO(png)).convert("RGB").save(
                os.path.join(dest_dir, "frame%06d.bmp" % idx), "BMP")
            idx += 1
        except Exception:
            continue
    return idx


_state = {"playing": False, "hand": None, "seq": 0, "stop": False, "src": None}


def ensure_replayer():
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq replayer.exe"],
                             capture_output=True, text=True).stdout
        if "replayer.exe" in out:
            return
    except Exception:
        pass
    os.makedirs(REPLAYER_DIR, exist_ok=True)
    subprocess.Popen([REPLAYER_EXE, "--watch", CONTROL])
    time.sleep(1.2)


def write_control(folder_or_stop):
    _state["seq"] += 1
    seq = _state["seq"]
    with open(CONTROL, "w") as f:
        f.write("seq=%d\nfolder=%s\nms=%d\n" % (seq, folder_or_stop, PLAY_MS))
    return seq


def wait_done(seq, timeout=180):
    end = time.time() + timeout
    while time.time() < end:
        if _state["stop"]:
            return False
        try:
            with open(STATUS) as f:
                s = f.read().strip()
            if s == ("done %d" % seq) or s.startswith("error"):
                return True
        except Exception:
            pass
        time.sleep(0.4)
    return True   # timeout -> advance anyway


def hands_by_time(base=None, host=None):
    return sorted(replay_get("/api/hands", base=base, host=host), key=lambda h: h.get("start_ts", 0))


def play_loop(start_hand, base=None, host=None):
    ensure_replayer()
    cur = str(start_hand)
    while not _state["stop"]:
        _state["hand"] = cur
        folder = os.path.join(REPLAYER_DIR, "hand_%s" % cur)
        try:
            n = hand_to_bmp(cur, folder, base=base, host=host)
        except Exception:
            n = 0
        if n > 0:
            seq = write_control(folder)
            wait_done(seq)
        if _state["stop"]:
            break
        # advance to the next hand NEWER in time than the one just played
        try:
            hs = hands_by_time(base=base, host=host)
        except Exception:
            hs = []
        cur_ts = next((h.get("start_ts", 0) for h in hs if str(h.get("handnumber")) == cur), None)
        nxt = None
        if cur_ts is not None:
            newer = [h for h in hs if h.get("start_ts", 0) > cur_ts]
            if newer:
                nxt = str(newer[0].get("handnumber"))
        if nxt:
            cur = nxt
            continue
        # at the newest hand: poll for a newer one to arrive (live), up to ~10 min
        waited = 0
        got = False
        while not _state["stop"] and waited < 600:
            time.sleep(2); waited += 2
            try:
                hs = hands_by_time(base=base, host=host)
            except Exception:
                continue
            if cur_ts is not None:
                newer = [h for h in hs if h.get("start_ts", 0) > cur_ts]
                if newer:
                    cur = str(newer[0].get("handnumber")); got = True; break
        if not got:
            break
    _state["playing"] = False
    write_control("STOP")


class H(BaseHTTPRequestHandler):
    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code); self._cors()
        self.send_header("Content-Type", "application/json"); self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(204); self._cors(); self.end_headers()

    def log_message(self, *a):
        pass

    def do_GET(self):  self.route()
    def do_POST(self): self.route()

    def route(self):
        u = urlparse(self.path); q = parse_qs(u.query)
        if u.path == "/play":
            hand = (q.get("hand") or [""])[0]
            if not hand:
                return self._json({"error": "hand required"}, 400)
            # Which replay store to pull frames from. Each UI passes its own so the LOCAL
            # (:8090) and SERVER (swiftsnake) advanced-replay views both drive replayer.exe
            # from the SAME frames they display. Absent -> legacy swiftsnake default.
            src  = (q.get("src")  or [None])[0]
            host = (q.get("host") or [None])[0]   # "" -> no Host header; absent -> default
            _state["stop"] = True; time.sleep(0.6)      # stop any prior loop
            _state["stop"] = False; _state["playing"] = True
            _state["src"] = src or REPLAY_BASE
            threading.Thread(target=play_loop, args=(hand, src, host), daemon=True).start()
            return self._json({"ok": True, "playing": hand, "src": _state["src"]})
        if u.path == "/stop":
            _state["stop"] = True; _state["playing"] = False
            write_control("STOP")
            return self._json({"ok": True, "stopped": True})
        if u.path == "/status":
            return self._json({"playing": _state["playing"], "hand": _state["hand"]})
        return self._json({"error": "unknown"}, 404)


if __name__ == "__main__":
    os.makedirs(REPLAYER_DIR, exist_ok=True)
    print("replayer control server on :%d  control=%s" % (PORT, CONTROL))
    HTTPServer(("0.0.0.0", PORT), H).serve_forever()
