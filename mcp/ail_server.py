#!/usr/bin/env python3
"""
AIL control server -- one HTTP endpoint (default 0.0.0.0:7900) that the browser Terminal's
"AIL" tab AND the Hiss MCP use to switch the Autonomous-Improvement-Loop / data daemons on and
off and to stream their combined feedback into one big terminal window. [Emrald request: "build
all the AILs with on and off switches into the browser terminal on a second tab ... show feedback
from the MCP in a big terminal window on the AIL tab".]

Stdlib only. CORS-enabled (so the React/terminal pages served by Hiss on another port can call it).

Endpoints (all GET, CORS *):
  /ail/ping                         -> {"ok":true}
  /ail/list                         -> {"ails":[{name,label,desc,icon,enabled,running}]}
  /ail/toggle?name=<n>&on=1|0       -> switch one AIL; returns its new {enabled,running}
  /ail/output?since=<seq>&max=<n>   -> {"lines":[{seq,name,text}], "cursor":<maxseq>}  (merged feed)
  /lobby-fetch?port=<P>             -> kick off mcp/lobby_fetch.sh for that Hiss instance (async)

Each AIL is a python daemon; its stdout+stderr are appended to Release/logs/ail_<name>.out.log and a
background tailer merges all enabled logs into one ring buffer the /ail/output cursor reads. Enabled
state persists in Release/logs/ail_state.json so the switches survive a restart (re-launching any AIL
that was on). Launched with CREATE_NO_WINDOW so no console ever pops up. Adopts an already-running
daemon (PowerShell cmdline scan) instead of starting a duplicate.
"""
import os, sys, json, glob, re, time, threading, subprocess, traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

# pythonw.exe (windowless) sets sys.stdout/stderr to None; back them with devnull so any write
# (e.g. the startup banner) can't crash the server. Same gotcha as lilith_tts.
for _nm in ("stdout", "stderr"):
    if getattr(sys, _nm, None) is None:
        try:
            setattr(sys, _nm, open(os.devnull, "w"))
        except Exception:
            pass

REPO       = os.environ.get("HISS_REPO", r"C:\www\openholdembot_old")
MCPDIR     = os.path.join(REPO, "mcp")
RELEASE    = os.path.join(REPO, "Release")
LOGS       = os.path.join(RELEASE, "logs")
STATE_FILE = os.path.join(LOGS, "ail_state.json")
PORT       = int(os.environ.get("AIL_SERVER_PORT", "7900"))

CREATE_NO_WINDOW          = 0x08000000
DETACHED_PROCESS          = 0x00000008
CREATE_NEW_PROCESS_GROUP  = 0x00000200
LAUNCH_FLAGS = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW

def no_window_si():
    """STARTUPINFO with SW_HIDE -- belt-and-suspenders with CREATE_NO_WINDOW so a spawned helper
    (powershell/taskkill) NEVER flashes a console window. [Emrald: "this terminal should never open"]"""
    if os.name != "nt":
        return None
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 0   # SW_HIDE
    return si

# Python that runs the daemons. Prefer pythonw.exe (NO console at all) so neither the daemons nor any
# child they spawn can pop a window. [Emrald]
PY_CANDIDATES = [
    r"C:\Users\scarl\AppData\Local\Programs\Python\Python310\pythonw.exe",
    r"C:\Users\scarl\AppData\Local\Programs\Python\Python310\python.exe",
    sys.executable,
    "python",
]
def daemon_python():
    for p in PY_CANDIDATES:
        if p == "python" or os.path.isfile(p):
            return p
    return sys.executable

def capture_python():
    """A console python.exe (NOT pythonw) for subprocesses whose stdout we capture (windowless via
    CREATE_NO_WINDOW). pythonw's stdout pipe can be flaky; python.exe + CREATE_NO_WINDOW is reliable."""
    for p in (r"C:\Users\scarl\AppData\Local\Programs\Python\Python310\python.exe", sys.executable, "python"):
        if p == "python" or os.path.isfile(p):
            return p
    return sys.executable

# --- audio input devices (for the Voice Feedback AIL mic picker) ------------
def list_audio_devices():
    """Reuse `voice_feedback.py --list` so the device indices match exactly what the daemon will use.
    --list returns before importing whisper/psycopg2, so it's quick (just sounddevice)."""
    try:
        out = subprocess.run([capture_python(), os.path.join(MCPDIR, "voice_feedback.py"), "--list"],
                             capture_output=True, text=True, timeout=25,
                             creationflags=CREATE_NO_WINDOW, startupinfo=no_window_si()).stdout
    except Exception:
        return []
    devs = []
    for ln in out.splitlines():
        m = re.match(r"^\s*in\[(\d+)\]\s+(.*\S)\s*$", ln)
        if m:
            devs.append({"index": int(m.group(1)), "name": m.group(2)})
    return devs

# --- the AIL registry -------------------------------------------------------
# Each entry is a python daemon launched detached; args[0] is its (unique) script filename, also used
# to adopt an already-running instance. Add new AILs here and they appear in the tab automatically.
# "logs" = glob patterns (under Release/logs) of the files the daemon actually writes -- they differ by
# who launched it (the loop runs one synapse per phone, e.g. synapse_s10/a17), so the tailer merges all
# matches PLUS ail_<name>.out.log (used when this server starts the daemon). All tagged with the name.
AILS = [
    {"name": "synapse", "label": "Synapse Harmonizer", "icon": "\U0001F9EC",
     "desc": "Unifies every signal/knob/mode/output into one node+synapse graph; writes synapse_state + per-hand hand_results (AIL step 1b).",
     "args": ["synapse_map.py", "--watch"], "logs": ["synapse_*.out.log"]},
    {"name": "observe", "label": "Observational Learning", "icon": "\U0001F441",
     "desc": "While observing, mines villain exploits / table reads / session trend into observations; drives the HUD aggregator (AIL step 1c).",
     "args": ["observe_learn.py", "--watch"], "logs": ["observe_learn.out.log"]},
    {"name": "voice", "label": "Voice Feedback", "icon": "\U0001F399",
     "desc": "Mic -> whisper -> pins your spoken feedback to the live hand in postgres; the AIL improves the bot from it.",
     "args": ["voice_feedback.py", "--bot-url", "http://127.0.0.1:27654"], "logs": ["voice_feedback.log"]},
    {"name": "shipper", "label": "Replay Shipper", "icon": "\U0001F4E6",
     "desc": "Ships the postgres replay/telemetry outbox to hiss.scarletbeast.com for the replay UI + time-travel debugger.",
     "args": ["hiss_shipper.py"], "logs": ["hiss_shipper.out.log", "hiss_shipper.err.log"]},
    {"name": "coach", "label": "Coach Hype (Lilith)", "icon": "\U0001F5E3",
     "desc": "Speaks hype/wisdom every 5-10 min + a ~5-min elite prep speech before each scheduled tournament.",
     "args": ["poker_coach_hype.py"], "logs": ["poker_coach.log", "poker_coach.err.log"]},
    {"name": "hud", "label": "HUD Aggregator", "icon": "\U0001F4CA",
     "desc": "Aggregates per-opponent HUD stats (VPIP/PFR/AF) from hand history for the overlay + exploit reads.",
     "args": ["hud_aggregator.py", "--watch"], "logs": ["hud_aggregator*.log", "hud*.out.log"]},
    {"name": "manic", "label": "Manic Burst", "icon": "\U0001F525",
     "desc": "When a table is very passive (low opp AF), fires a short maniac burst (aggro/bluff/open knobs maxed + power style) to wake people up, then reverts to small ball after ~a song. Auto-discovers the live seated Hiss.",
     "args": ["manic_burst.py"], "logs": ["manic_burst.log"]},
]
AIL_BY_NAME = {a["name"]: a for a in AILS}

def logfile(name):
    return os.path.join(LOGS, "ail_%s.out.log" % name)

# --- persistent enabled-state ----------------------------------------------
_state_lock = threading.Lock()
_state = {"enabled": {}, "pid": {}}

def load_state():
    global _state
    try:
        with open(STATE_FILE) as f:
            _state = json.load(f)
    except Exception:
        _state = {"enabled": {}, "pid": {}}
    _state.setdefault("enabled", {})
    _state.setdefault("pid", {})

def save_state():
    try:
        with open(STATE_FILE, "w") as f:
            json.dump(_state, f)
    except Exception:
        pass

# --- pid-alive (ctypes, fast) ----------------------------------------------
def pid_alive(pid):
    if not pid:
        return False
    try:
        import ctypes
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        k = ctypes.windll.kernel32
        h = k.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
        if not h:
            return False
        code = ctypes.c_ulong()
        k.GetExitCodeProcess(h, ctypes.byref(code))
        k.CloseHandle(h)
        return code.value == STILL_ACTIVE
    except Exception:
        return False

def is_running(name):
    return pid_alive(_state["pid"].get(name))

def find_existing_pid(script):
    """One-shot PowerShell scan for a python.exe whose command line contains <script>, so we adopt an
    already-running daemon instead of starting a duplicate. Only called on toggle-ON (rare)."""
    try:
        ps = ("Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | "
              "Where-Object { $_.CommandLine -like '*%s*' } | "
              "Select-Object -First 1 -ExpandProperty ProcessId" % script)
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                             capture_output=True, text=True, timeout=12,
                             creationflags=CREATE_NO_WINDOW, startupinfo=no_window_si()).stdout.strip()
        return int(out) if out.isdigit() else 0
    except Exception:
        return 0

# --- start / stop -----------------------------------------------------------
def start_daemon(name):
    a = AIL_BY_NAME.get(name)
    if not a:
        return False, "unknown AIL"
    if is_running(name):
        _state["enabled"][name] = True
        save_state()
        return True, "already running"
    ext = find_existing_pid(a["args"][0])
    if ext:
        _state["pid"][name] = ext
        _state["enabled"][name] = True
        save_state()
        return True, "adopted existing pid %d" % ext
    try:
        lf = open(logfile(name), "ab", buffering=0)
        lf.write(("\n==== %s started %s ====\n"
                  % (name, time.strftime("%Y-%m-%d %H:%M:%S"))).encode())
        cmd = [daemon_python(), "-u"] + a["args"]
        if name == "voice":   # apply the AIL-tab-selected mic device
            dev = _state.get("voice_device")
            if dev not in (None, "", "default"):
                cmd += ["--device", str(dev)]
        p = subprocess.Popen(cmd, cwd=MCPDIR, stdout=lf, stderr=lf, stdin=subprocess.DEVNULL,
                             close_fds=True, creationflags=LAUNCH_FLAGS)
        _state["pid"][name] = p.pid
        _state["enabled"][name] = True
        save_state()
        return True, "started pid %d" % p.pid
    except Exception as e:
        return False, "start failed: %s" % e

def stop_daemon(name):
    pid = _state["pid"].get(name)
    _state["enabled"][name] = False
    if pid and pid_alive(pid):
        try:
            subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"],
                           capture_output=True, creationflags=CREATE_NO_WINDOW, startupinfo=no_window_si())
        except Exception:
            pass
    _state["pid"][name] = 0
    save_state()
    return True, "stopped"

# --- merged output ring buffer + log tailer --------------------------------
_ring_lock = threading.Lock()
_ring = []          # [{seq, name, text}]
_ring_seq = [0]
RING_MAX = 1500
_offsets = {}       # name -> last byte offset read from its log

def push_line(name, text):
    with _ring_lock:
        _ring_seq[0] += 1
        _ring.append({"seq": _ring_seq[0], "name": name, "text": text})
        if len(_ring) > RING_MAX:
            del _ring[:len(_ring) - RING_MAX]

def resolve_logs(a):
    """All log files (full paths) for an AIL: ail_<name>.out.log + everything its glob patterns match."""
    paths = [logfile(a["name"])]
    for pat in a.get("logs", []):
        paths += glob.glob(os.path.join(LOGS, pat))
    return paths

def tail_loop():
    while True:
        try:
            for a in AILS:
                if not _state["enabled"].get(a["name"]):
                    continue
                for path in resolve_logs(a):
                    if not os.path.isfile(path):
                        continue
                    size = os.path.getsize(path)
                    off = _offsets.get(path)
                    if off is None:
                        off = max(0, size - 4096)   # first sight: just the recent tail, not whole history
                    if size < off:                  # truncated / rotated
                        off = 0
                    if size > off:
                        with open(path, "rb") as f:
                            f.seek(off)
                            chunk = f.read(size - off)
                        _offsets[path] = size
                        tag = "[%s]" % a["name"]
                        for ln in chunk.decode("utf-8", errors="replace").split("\n"):
                            ln = ln.rstrip("\r")
                            if ln.startswith(tag):          # don't double up the source tag the UI adds
                                ln = ln[len(tag):].lstrip()
                            if ln.strip():
                                push_line(a["name"], ln)
        except Exception:
            pass
        time.sleep(0.6)

# --- lobby fetch (task C trigger) ------------------------------------------
BASH_CANDIDATES = [
    r"C:\Program Files\Git\bin\bash.exe",
    r"C:\Program Files\Git\usr\bin\bash.exe",
    r"C:\Program Files (x86)\Git\bin\bash.exe",
    "bash",
]
def find_bash():
    for b in BASH_CANDIDATES:
        if b == "bash" or os.path.isfile(b):
            return b
    return None

def run_lobby_fetch(port):
    b = find_bash()
    if not b:
        return False, "git bash not found"
    script = "/c/www/openholdembot_old/mcp/lobby_fetch.sh"
    try:
        subprocess.Popen([b, script, str(port), "3.5", "6"],
                         creationflags=CREATE_NO_WINDOW, close_fds=True)
        return True, "lobby_fetch started for port %s" % port
    except Exception as e:
        return False, str(e)

# --- HTTP -------------------------------------------------------------------
class Handler(BaseHTTPRequestHandler):
    def _send(self, code, obj, ctype="application/json"):
        body = (json.dumps(obj) if ctype.startswith("application/json") else obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype + "; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
        except Exception:
            pass

    def log_message(self, *a):
        pass  # quiet

    def do_OPTIONS(self):
        self._send(204, "", ctype="text/plain")

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        path = u.path
        try:
            if path == "/ail/ping":
                return self._send(200, {"ok": True})
            if path == "/ail/list":
                items = []
                for a in AILS:
                    n = a["name"]
                    items.append({"name": n, "label": a["label"], "desc": a["desc"], "icon": a["icon"],
                                  "enabled": bool(_state["enabled"].get(n)), "running": is_running(n)})
                return self._send(200, {"ails": items})
            if path == "/ail/toggle":
                n = (q.get("name") or [""])[0]
                on = (q.get("on") or ["1"])[0] in ("1", "true", "on", "yes")
                if n not in AIL_BY_NAME:
                    return self._send(400, {"error": "unknown AIL: %s" % n})
                ok, msg = (start_daemon(n) if on else stop_daemon(n))
                return self._send(200, {"name": n, "enabled": bool(_state["enabled"].get(n)),
                                        "running": is_running(n), "ok": ok, "msg": msg})
            if path == "/ail/audio-devices":
                return self._send(200, {"devices": list_audio_devices(),
                                        "selected": _state.get("voice_device")})
            if path == "/ail/audio-device":
                idx = (q.get("index") or q.get("device") or [""])[0]
                _state["voice_device"] = idx if idx not in ("", "default") else None
                save_state()
                restarted = False
                if is_running("voice"):          # apply the new mic right away
                    stop_daemon("voice")
                    start_daemon("voice")
                    restarted = True
                return self._send(200, {"selected": _state.get("voice_device"), "restarted": restarted})
            if path == "/ail/output":
                since = int((q.get("since") or ["0"])[0] or 0)
                mx = int((q.get("max") or ["400"])[0] or 400)
                with _ring_lock:
                    lines = [x for x in _ring if x["seq"] > since][-mx:]
                    cursor = _ring_seq[0]
                if since == 0:
                    lines = lines[-120:]   # fresh client: only the recent tail, not a flood
                return self._send(200, {"lines": lines, "cursor": cursor})
            if path == "/lobby-fetch":
                port = (q.get("port") or ["27654"])[0]
                ok, msg = run_lobby_fetch(port)
                return self._send(200, {"ok": ok, "msg": msg, "port": port})
            return self._send(404, {"error": "not found"})
        except Exception as e:
            return self._send(500, {"error": str(e), "trace": traceback.format_exc()})

def reconcile_loop():
    """Keep the switches honest: adopt a daemon that's already running (started by the loop / a .bat /
    a previous server) so it shows ON, without resurrecting one the user explicitly switched OFF."""
    while True:
        time.sleep(12)
        try:
            for a in AILS:
                n = a["name"]
                if is_running(n):
                    continue
                if _state["enabled"].get(n) is False:   # user turned it off -> respect that
                    continue
                pid = find_existing_pid(a["args"][0])
                if pid:
                    _state["pid"][n] = pid
                    _state["enabled"][n] = True
                    save_state()
        except Exception:
            pass

def main():
    load_state()
    # restore: re-launch any AIL that was switched ON before this server (re)started
    for a in AILS:
        if _state["enabled"].get(a["name"]) and not is_running(a["name"]):
            start_daemon(a["name"])
    threading.Thread(target=tail_loop, daemon=True).start()
    threading.Thread(target=reconcile_loop, daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    sys.stderr.write("[ail_server] listening on 0.0.0.0:%d\n" % PORT)
    sys.stderr.flush()
    srv.serve_forever()

if __name__ == "__main__":
    main()
