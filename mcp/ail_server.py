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

# The /proposals API (served below for the browser-view Proposals tab) reuses the learning daemon's DB
# helpers + the SAFE apply flow (lint-to-staging -> deploy+reload on pass -> revert on fail). Guarded so
# a missing/broken module can never stop the AIL server from starting.
if MCPDIR not in sys.path:
    sys.path.insert(0, MCPDIR)
try:
    import learn_from_decisions as lfd
except Exception:
    lfd = None

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
    # The odometer is FIRST because nothing else means anything without it. It records what every hand
    # won, and it is deliberately its OWN daemon: hand_results used to be written by synapse_map, but
    # synapse_map is also the brain, and "brain disengage" switches the synapse AIL off -- so every
    # Hiss restart silently killed the measurement while the bot kept playing (102 hands seen in six
    # hours, 2 recorded). Nothing may switch this off as a side effect of switching off something else.
    {"name": "odometer", "label": "Odometer (hand results)", "icon": "\U0001F4CF",
     "desc": "Records EVERY hand's win/loss into hand_results (hero stack delta across the hand boundary). Independent of the brain -- measure_live.py, the A/B gate and the AIL all read this. Leave it ON.",
     "args": ["odometer.py"], "logs": ["odometer*.out.log"]},
    # PER-PORT: the brain. Each Hiss instance runs its own synapse steering its own bot, so this switch is
    # per instance -- "{port}" is substituted with that bot's port and the state is keyed "synapse@<port>".
    # The bot-url is EXPLICIT (it used to default to 27654 implicitly): every brain process must carry its
    # port on the command line, because that is how we tell one instance's brain from another's -- an
    # untagged synapse could not be killed, adopted or reported per-instance. [Emrald: per-instance brain]
    {"name": "synapse", "label": "Synapse Harmonizer", "icon": "\U0001F9EC", "per_port": True,
     "desc": "Unifies every signal/knob/mode/output into one node+synapse graph; writes synapse_state + per-hand hand_results (AIL step 1b). Per Hiss instance.",
     "args": ["synapse_map.py", "--bot-url", "http://127.0.0.1:{port}", "--watch"], "logs": ["synapse_*.out.log"]},
    {"name": "observe", "label": "Observational Learning", "icon": "\U0001F441",
     "desc": "While observing, mines villain exploits / table reads / session trend into observations; drives the HUD aggregator (AIL step 1c).",
     "args": ["observe_learn.py", "--watch"], "logs": ["observe_learn.out.log"]},
    {"name": "learn", "label": "Learn from decisions (EV+)", "icon": "\U0001F393",
     "desc": "Reconciles your MANUAL plays (learner_decisions) vs the bot's own pick; EV+-gates each divergence via claude -p (decision-time quality, NOT results); after >=2 same-pattern EV+ hits, proposes a minimal OHF improvement into ohf_proposals for you to approve. Never edits the live strategy on its own. Machine-wide (reads all instances).",
     "args": ["learn_from_decisions.py"], "logs": ["learn*.out.log"]},
    {"name": "voice", "label": "Voice Feedback", "icon": "\U0001F399",
     "desc": "Mic -> whisper -> pins your spoken feedback to the live hand in postgres; the AIL improves the bot from it.",
     "args": ["voice_feedback.py", "--bot-url", "http://127.0.0.1:27654"], "logs": ["voice_feedback.log"]},
    {"name": "shipper", "label": "Replay Shipper", "icon": "\U0001F4E6",
     "desc": "Ships the postgres replay/telemetry outbox to hiss.scarletbeast.com for the replay UI + time-travel debugger.",
     "args": ["hiss_shipper.py"], "logs": ["hiss_shipper.out.log", "hiss_shipper.err.log"]},
    {"name": "coach", "label": "Coach Hype (Lilith)", "icon": "\U0001F5E3",
     "desc": "Speaks hype/wisdom every 5-10 min + a ~5-min elite prep speech before each scheduled tournament.",
     "args": ["poker_coach_hype.py"], "logs": ["poker_coach.log", "poker_coach.err.log"]},
    {"name": "tilt", "label": "Tilt Detector (Lilith)", "icon": "\U0001F9EF",
     "desc": "Scores HERO tilt (stack drawdown / losing streak / fresh bad beat / your negative voice feedback -> protect) + OPPONENT tilt (recent big loss + over-aggression -> exploit). Speaks a de-bounced warning via Lilith + writes coach_notes + tilt_events.",
     "args": ["tilt_detector_daemon.py"], "logs": ["tilt_detector*.log", "tilt*.out.log"]},
    {"name": "icm", "label": "ICM / Chip Value (Lilith)", "icon": "\U0001F4B0",
     "desc": "Every ~2.5 min (and on a blind-level change / near a pay jump) speaks big blinds, M-ratio, blind gear, pay-jump proximity + approx ICM $ equity (Malmuth-Harville). Reads icm_config; snapshots to icm_snapshots.",
     "args": ["icm_chip_daemon.py"], "logs": ["icm_chip*.log", "icm*.out.log"]},
    {"name": "hud", "label": "HUD Aggregator", "icon": "\U0001F4CA",
     "desc": "Aggregates per-opponent HUD stats (VPIP/PFR/AF) from hand history for the overlay + exploit reads.",
     "args": ["hud_aggregator.py", "--watch"], "logs": ["hud_aggregator*.log", "hud*.out.log"]},
    {"name": "manic", "label": "Manic Burst", "icon": "\U0001F525",
     "desc": "When a table is very passive (low opp AF), fires a short maniac burst (aggro/bluff/open knobs maxed + power style) to wake people up, then reverts to small ball after ~a song. Auto-discovers the live seated Hiss.",
     "args": ["manic_burst.py"], "logs": ["manic_burst.log"]},
    {"name": "trainrand", "label": "Train Randomizer", "icon": "\U0001F3B2",
     "desc": "DOMAIN RANDOMIZATION for self-play data generation: re-rolls each bot's game style (smallball/power/hybrid) + synapse knobs PER HAND so retraining sees the whole strategy space. Turn ON only while generating PPO/CFR training data, OFF for real-money play.",
     "args": ["train_randomize.py"], "logs": ["train_randomize.log"]},
    {"name": "parseguard", "label": "Parse Guard", "icon": "\U0001F6E1",
     "desc": "Watches for OHF parse-error modals and AUTO-REPAIRS the strategy (via Claude, windowless) so a bad edit never leaves the bot folding every hand. Autonomous self-heal.",
     "args": ["parse_guard.py", "--watch"], "logs": ["parse_guard*.log"]},
]
AIL_BY_NAME = {a["name"]: a for a in AILS}

# --- the PORT dimension -----------------------------------------------------
# Most AILs are machine-wide: one replay shipper, one mic, one HUD aggregator -- a single switch is right.
# But some steer ONE Hiss instance, and the brain (synapse) above all: two bots each need their own, and
# switching one off must not switch the other off. Those carry "per_port": True, and everything about them
# is keyed by the bot's port:
#     state key   "synapse@27655"   (global AILs stay plain: "shipper")
#     args        "{port}" -> the bot's port, so the process is identifiable on its command line
#     log file    ail_synapse_27655.out.log
# A global AIL ignores `port` entirely, so callers can pass it unconditionally. [Emrald: per-instance brain]
DEFAULT_PORT = "27654"


def is_per_port(name):
    return bool((AIL_BY_NAME.get(name) or {}).get("per_port"))


def ail_key(name, port=None):
    """The state key for this AIL. Per-port AILs get one key per Hiss instance; global AILs keep the
    bare name, so existing state and switches are untouched."""
    if not is_per_port(name):
        return name
    return "%s@%s" % (name, str(port or DEFAULT_PORT))


def ail_args(a, port=None):
    return [x.replace("{port}", str(port or DEFAULT_PORT)) for x in a["args"]]


def key_port(key):
    """The port a state key belongs to ('synapse@27655' -> '27655'); DEFAULT_PORT for a global key."""
    return key.split("@", 1)[1] if "@" in key else DEFAULT_PORT


def known_ports():
    """Every Hiss instance we hold per-port state for, plus the default. This is how the reconcile loop
    and the startup restore know which instances' per-port AILs to look after."""
    ports = {DEFAULT_PORT}
    for k in list(_state.get("enabled", {})) + list(_state.get("pid", {})):
        if "@" in k:
            ports.add(key_port(k))
    return sorted(ports)


def logfile(key):
    # keyed by state key, so each instance's brain writes its own log ('@' is legal on NTFS but reads
    # badly in a glob, so it becomes '_': ail_synapse_27655.out.log)
    return os.path.join(LOGS, "ail_%s.out.log" % str(key).replace("@", "_"))

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
    # MIGRATE: an AIL that became per-port still has its old bare key on disk ("synapse": true). Move it
    # onto the default instance so a brain that was ON before this upgrade stays ON, and doesn't silently
    # come back as a second, portless daemon.
    #
    # This MUST be persisted, not just done in memory: save_state() re-adopts the file after every write,
    # so an unpersisted migration would be wiped by the very next switch change -- and the retired bare key
    # would come back from disk with it. Write the new key and retire the old one in the same atomic save.
    for a in AILS:
        n = a["name"]
        if not a.get("per_port") or n not in _state["enabled"] and n not in _state["pid"]:
            continue
        key = ail_key(n, DEFAULT_PORT)
        for bucket, default in (("enabled", False), ("pid", 0)):
            if n in _state[bucket]:
                _state[bucket].setdefault(key, _state[bucket].pop(n, default))
        save_state(key, drop=[n])

def save_state(name=None, drop=()):
    """MERGE-write, never whole-dict overwrite.

    Dumping the entire in-memory dict loses updates. Each Hiss restart spawns a fresh ail_server, and
    for a moment two of them are alive, each holding its own copy of the state it loaded at startup.
    Whichever writes LAST wins -- and it writes ALL the keys, silently reverting every switch the
    other one had changed. That is how the Synapse Harmonizer -- the daemon that records every hand's
    P&L -- kept switching itself back off: never by anyone stopping it (stop_daemon zeroes the pid,
    and the pid was still there), but by a stale copy of the state file overwriting the flag.

    A measurement pipeline that turns itself off without saying so is worse than one that was never
    built, because you go on trusting it. So: re-read the file, change ONLY the switch we just
    touched, and write atomically.

    `drop` retires keys from the file outright (used when an AIL becomes per-port and its old bare key
    is migrated to "<name>@<port>"). Without it a retired key would live on in the file and, because we
    re-adopt the file below, keep coming back into memory on every write.
    """
    with _state_lock:
        try:
            with open(STATE_FILE) as f:
                disk = json.load(f)
        except Exception:
            disk = {}
        disk.setdefault("enabled", {})
        disk.setdefault("pid", {})
        if name is None:                       # full save (first boot / voice_device etc.)
            disk["enabled"].update(_state.get("enabled", {}))
            disk["pid"].update(_state.get("pid", {}))
        else:                                  # only the switch that actually changed
            disk["enabled"][name] = _state["enabled"].get(name, False)
            disk["pid"][name] = _state["pid"].get(name, 0)
        for k in (drop or ()):
            disk["enabled"].pop(k, None)
            disk["pid"].pop(k, None)
        for k, v in _state.items():            # non-switch keys (voice_device, ...)
            if k not in ("enabled", "pid"):
                disk[k] = v
        try:
            tmp = STATE_FILE + ".tmp"
            with open(tmp, "w") as f:
                json.dump(disk, f)
            os.replace(tmp, STATE_FILE)        # atomic: a torn file cannot disable a daemon either
        except Exception:
            return
        # adopt what is now on disk, so this server stops carrying a stale view of the others
        _state["enabled"] = disk["enabled"]
        _state["pid"] = disk["pid"]

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

def is_running(name, port=None):
    return pid_alive(_state["pid"].get(ail_key(name, port)))

def find_existing_pid(script, port=None):
    """One-shot PowerShell scan for a python.exe whose command line contains <script>, so we adopt an
    already-running daemon instead of starting a duplicate. Only called on toggle-ON (rare).
    `port` narrows the match to the daemon steering THAT bot (its --bot-url carries ':<port>') -- without
    it, one instance's brain would be adopted as another's and the two switches would fuse together."""
    try:
        cond = "$_.CommandLine -like '*%s*'" % script
        if port:
            cond += " -and $_.CommandLine -like '*:%s*'" % port
        ps = ("Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | "
              "Where-Object { %s } | "
              "Select-Object -First 1 -ExpandProperty ProcessId" % cond)
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                             capture_output=True, text=True, timeout=12,
                             creationflags=CREATE_NO_WINDOW, startupinfo=no_window_si()).stdout.strip()
        return int(out) if out.isdigit() else 0
    except Exception:
        return 0

# --- start / stop -----------------------------------------------------------
def start_daemon(name, port=None):
    a = AIL_BY_NAME.get(name)
    if not a:
        return False, "unknown AIL"
    key = ail_key(name, port)
    scope = key_port(key) if is_per_port(name) else None   # per-port: only ever match/act on THIS bot
    if is_running(name, port):
        _state["enabled"][key] = True
        save_state(key)
        return True, "already running"
    ext = find_existing_pid(a["args"][0], scope)
    if ext:
        _state["pid"][key] = ext
        _state["enabled"][key] = True
        save_state(key)
        return True, "adopted existing pid %d" % ext
    try:
        lf = open(logfile(key), "ab", buffering=0)
        lf.write(("\n==== %s started %s ====\n"
                  % (key, time.strftime("%Y-%m-%d %H:%M:%S"))).encode())
        cmd = [daemon_python(), "-u"] + ail_args(a, port)
        if name == "voice":   # apply the AIL-tab-selected mic device
            dev = _state.get("voice_device")
            if dev not in (None, "", "default"):
                cmd += ["--device", str(dev)]
        p = subprocess.Popen(cmd, cwd=MCPDIR, stdout=lf, stderr=lf, stdin=subprocess.DEVNULL,
                             close_fds=True, creationflags=LAUNCH_FLAGS)
        _state["pid"][key] = p.pid
        _state["enabled"][key] = True
        save_state(key)
        return True, "started pid %d" % p.pid
    except Exception as e:
        return False, "start failed: %s" % e

def stop_daemon(name, port=None):
    key = ail_key(name, port)
    pid = _state["pid"].get(key)
    _state["enabled"][key] = False
    if pid and pid_alive(pid):
        try:
            subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"],
                           capture_output=True, creationflags=CREATE_NO_WINDOW, startupinfo=no_window_si())
        except Exception:
            pass
    _state["pid"][key] = 0
    save_state(key)
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
    """All log files (full paths) for an AIL: ail_<key>.out.log + everything its glob patterns match.
    A per-port AIL has one log per instance, so collect them for every known port."""
    if a.get("per_port"):
        paths = [logfile(ail_key(a["name"], p)) for p in known_ports()]
    else:
        paths = [logfile(a["name"])]
    for pat in a.get("logs", []):
        paths += glob.glob(os.path.join(LOGS, pat))
    return paths

def ail_enabled_anywhere(a):
    """Global AIL: is it on? Per-port AIL: is it on for ANY instance? (the tab merges every bot's output)"""
    if not a.get("per_port"):
        return bool(_state["enabled"].get(a["name"]))
    return any(_state["enabled"].get(ail_key(a["name"], p)) for p in known_ports())

def tail_loop():
    while True:
        try:
            for a in AILS:
                if not ail_enabled_anywhere(a):
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
                # ?port= selects WHICH Hiss instance's switches to report. Global AILs ignore it; per-port
                # AILs (the brain) report that instance's own state, so two tabs don't mirror each other.
                port = (q.get("port") or [DEFAULT_PORT])[0]
                items = []
                for a in AILS:
                    n = a["name"]
                    k = ail_key(n, port)
                    items.append({"name": n, "label": a["label"], "desc": a["desc"], "icon": a["icon"],
                                  "enabled": bool(_state["enabled"].get(k)), "running": is_running(n, port),
                                  "per_port": bool(a.get("per_port")),
                                  "port": key_port(k) if a.get("per_port") else None})
                return self._send(200, {"ails": items, "port": port})
            if path == "/ail/toggle":
                n = (q.get("name") or [""])[0]
                port = (q.get("port") or [DEFAULT_PORT])[0]
                on = (q.get("on") or ["1"])[0] in ("1", "true", "on", "yes")
                if n not in AIL_BY_NAME:
                    return self._send(400, {"error": "unknown AIL: %s" % n})
                ok, msg = (start_daemon(n, port) if on else stop_daemon(n, port))
                return self._send(200, {"name": n, "port": port,
                                        "enabled": bool(_state["enabled"].get(ail_key(n, port))),
                                        "running": is_running(n, port), "ok": ok, "msg": msg})
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
            # ---- OHF improvement proposals (browser-view Proposals tab) -----------------------------
            # Backed by the learn daemon's own functions so the SAFE apply flow (lint-to-staging ->
            # deploy+reload on pass -> revert on fail, backup kept) is the single source of truth.
            if path == "/proposals/list":
                if lfd is None:
                    return self._send(200, {"ok": False, "error": "learn module unavailable", "proposals": []})
                try:
                    rows = lfd.list_proposals("pending")
                    items = [{"id": int(r[0]), "created": r[1], "pattern": r[2], "target": r[3],
                              "validated": (r[4] == "t"), "supporting": int(r[5] or 0), "status": r[6]}
                             for r in rows]
                    return self._send(200, {"ok": True, "proposals": items})
                except Exception as e:
                    return self._send(200, {"ok": False, "error": str(e), "proposals": []})
            if path == "/proposals/show":
                if lfd is None:
                    return self._send(200, {"ok": False, "error": "learn module unavailable"})
                pid = (q.get("id") or ["0"])[0]
                try:
                    r = lfd.show_proposal(pid)
                    if not r:
                        return self._send(200, {"ok": False, "error": "not found"})
                    keys = ["id", "pattern", "target", "old", "new", "rationale", "validated", "validation", "status"]
                    d = dict(zip(keys, r)); d["id"] = int(d["id"]); d["validated"] = (d["validated"] == "t"); d["ok"] = True
                    return self._send(200, d)
                except Exception as e:
                    return self._send(200, {"ok": False, "error": str(e)})
            if path == "/proposals/nn-list":
                # NN retraining candidates: EV+ divergences vs the NN (routed here instead of OHF
                # proposals). Read-only in the UI -- they feed the offline retrain, not a live apply.
                if lfd is None:
                    return self._send(200, {"ok": False, "error": "learn module unavailable", "examples": []})
                try:
                    rows = lfd.list_nn_examples(60)
                    items = [{"id": int(r[0]), "created": r[1], "hero": r[2], "board": r[3],
                              "preferred": r[4], "nn": r[5], "pattern": r[6], "status": r[7]} for r in rows]
                    return self._send(200, {"ok": True, "examples": items})
                except Exception as e:
                    return self._send(200, {"ok": False, "error": str(e), "examples": []})
            if path == "/proposals/apply":
                if lfd is None:
                    return self._send(200, {"ok": False, "msg": "learn module unavailable"})
                try:
                    ok, msg = lfd.apply_proposal((q.get("id") or ["0"])[0])
                    return self._send(200, {"ok": ok, "msg": msg})
                except Exception as e:
                    return self._send(200, {"ok": False, "msg": str(e)})
            if path == "/proposals/reject":
                if lfd is None:
                    return self._send(200, {"ok": False, "msg": "learn module unavailable"})
                try:
                    ok, msg = lfd.reject_proposal((q.get("id") or ["0"])[0])
                    return self._send(200, {"ok": ok, "msg": msg})
                except Exception as e:
                    return self._send(200, {"ok": False, "msg": str(e)})
            if path == "/lobby-fetch":
                port = (q.get("port") or ["27654"])[0]
                ok, msg = run_lobby_fetch(port)
                return self._send(200, {"ok": ok, "msg": msg, "port": port})
            if path == "/brain":
                port = (q.get("port") or ["27654"])[0]
                if "on" in q:
                    on = (q.get("on") or ["1"])[0] in ("1", "true", "on", "yes")
                    ok, msg = (brain_launch(port) if on else brain_kill(port))
                    return self._send(200, {"engaged": brain_running(port), "ok": ok, "msg": msg, "port": port})
                return self._send(200, {"engaged": brain_running(port), "port": port})
            if path == "/decision":
                # The brain's CURRENT DECIDED ACTION, crash-safe (a plain DB read of brain_state -- never
                # re-evaluates the OHF / prwin, so the React overlay can poll it freely). Drives the
                # RED DECISION "on fire" overlay on the table view. Per-instance: ?port= selects which
                # bot's brain we report, so two tables don't show each other's decision.
                return self._send(200, latest_decision((q.get("port") or ["27654"])[0]))
            if path == "/brain-source":
                # Local vs server brain. React's Synapse-tab "Brain: Local/Server" toggle sets v=local|server;
                # synapse_map reads this and, when "server", offloads the observer/introspection/mischief
                # computation to the hiss-brain service on swiftsnake (replica DB is local THERE). [Emrald]
                if "v" in q:
                    v = (q.get("v") or ["local"])[0]
                    _state["brain_source"] = "server" if v == "server" else "local"
                    save_state()
                return self._send(200, {"source": _state.get("brain_source", "local"),
                                        "server_url": _state.get("brain_server_url", "http://192.168.1.39:8092/brain")})
            return self._send(404, {"error": "not found"})
        except Exception as e:
            return self._send(500, {"error": str(e), "trace": traceback.format_exc()})

# ---- BRAIN stack control (the introspection/intuition harmonizer) -------------------------------
# The 🧠💭 Brain button (React table view) toggles this. Global daemons (aggregators + nervous system)
# launch once; the port-specific pair (synapse_map + decision_advisor) is keyed by the Hiss port so each
# instance gets its own brain steering its own bot. Windowless + tracked so it's clean to stop.
_brain = {}          # port(str) -> [Popen]   (synapse + advisor for that bot)
_brain_global = []   # [Popen]                (aggregators + brain_service + deep_thought + growth)
_brain_lock = threading.Lock()
BRAIN_PG = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")

_dec_cache = {}      # port(str) -> {"ts": float, "val": dict}


def latest_decision(port):
    """The brain's CURRENT DECIDED ACTION from brain_state (DB read only -- never touches the OHF/prwin
    on the HTTP thread, so the React overlay can poll it safely). Cached ~0.4s to shield postgres.
    Keyed by the Hiss PORT: each instance shows ITS OWN brain's decision, not the other table's."""
    port = str(port)
    now = time.time()
    cc = _dec_cache.get(port)
    if cc and cc["val"] is not None and now - cc["ts"] < 0.4:
        return cc["val"]
    out = {"ok": False, "action": None}
    try:
        import psycopg2
        c = psycopg2.connect(BRAIN_PG); cur = c.cursor()
        cur.execute("SELECT ts_ms, handnumber, betround, villain, brain FROM brain_state WHERE id=%s",
                    (int(port),))
        r = cur.fetchone(); c.close()
        if r:
            b = r[4] or {}
            cda = b.get("current_decided_action", {}) or {}
            intu = b.get("intuition", {}) or {}
            obs = b.get("observer_strategy", {}) or {}
            mis = b.get("mischief", {}) or {}
            out = {"ok": True, "ts_ms": r[0], "handnumber": r[1], "betround": r[2], "villain": r[3],
                   "action": cda.get("action"), "size_bb": cda.get("size_bb"),
                   "source": cda.get("source"), "exploit": intu.get("exploit"),
                   "branch": obs.get("branch"), "mischief": (mis.get("kind") if isinstance(mis, dict) and mis.get("fired") else None),
                   "confidence": intu.get("confidence"),
                   "energy": (b.get("pineal", {}) or {}).get("energy")}
    except Exception as e:
        out = {"ok": False, "action": None, "error": str(e)[:120]}
    _dec_cache[port] = {"ts": now, "val": out}
    return out


def _brain_spawn(args):
    env = dict(os.environ); env["HISS_PG_DSN"] = BRAIN_PG
    log = open(os.path.join(LOGS, "brain_%s.out.log" % args[0].replace(".py", "")), "a")
    return subprocess.Popen([capture_python()] + args, cwd=MCPDIR, env=env, stdout=log,
                            stderr=subprocess.STDOUT, creationflags=LAUNCH_FLAGS, startupinfo=no_window_si())


_brain_run_cache = {}   # port(str) -> {"ts": float, "val": bool}


def brain_running(port):
    # Report the REAL brain state so the React button shows correct status however the brain was started
    # (ail_toggle / engage_brain.bat / manual): engaged if we launched the per-port daemons here OR
    # brain_state is being written fresh (the synapse ticks every few seconds). CACHED ~2s and we HOLD the
    # last-known state on a transient DB hiccup, so the button doesn't flicker under postgres load. [Emrald]
    #
    # Both the freshness row AND the cache are keyed by PORT. They were global: brain_state was read at the
    # shared id=1 and the cache had no port key, so a live brain on ONE instance made EVERY instance's
    # button light up "engaged" -- and a status read for port A could be served from a cached read of B.
    port = str(port)
    with _brain_lock:
        pp = [p for p in _brain.get(port, []) if p.poll() is None]
    if pp:
        _brain_run_cache[port] = {"ts": time.time(), "val": True}
        return True
    now = time.time()
    cc = _brain_run_cache.get(port)
    if cc and now - cc["ts"] < 2.0:
        return cc["val"]
    val = cc["val"] if cc else False
    try:
        import psycopg2
        c = psycopg2.connect(BRAIN_PG, connect_timeout=2); cur = c.cursor()
        cur.execute("SELECT ts_ms FROM brain_state WHERE id=%s", (int(port),))
        r = cur.fetchone(); c.close()
        val = bool(r and (time.time() * 1000 - r[0]) < 10000)   # fresh < 10s -> the harmonizer is live
    except Exception:
        pass                                                     # transient DB hiccup -> keep last-known state
    _brain_run_cache[port] = {"ts": now, "val": val}
    return val


def brain_launch(port):
    port = str(port); bot = "http://127.0.0.1:%s" % port
    with _brain_lock:
        if not any(p.poll() is None for p in _brain_global):       # global daemons: launch once
            _brain_global[:] = []
            for a in (["hud_aggregator.py", "--watch"], ["introspect_aggregator.py", "--watch"],
                      ["brain_service.py"], ["deep_thought.py", "--serve"], ["growth.py", "--watch"]):
                try:
                    _brain_global.append(_brain_spawn(a))
                except Exception:
                    pass
        procs = [p for p in _brain.get(port, []) if p.poll() is None]   # port-specific: synapse + advisor
        if not procs:
            for a in (["synapse_map.py", "--bot-url", bot, "--watch"], ["decision_advisor.py", "--bot-url", bot]):
                try:
                    procs.append(_brain_spawn(a))
                except Exception:
                    pass
        _brain[port] = procs
    # The 🧠 button and the AIL tab's Synapse switch drive the SAME daemon, so keep them in agreement:
    # mark THIS instance's synapse switch on (and record its pid, so the tab shows it running and the
    # reconcile loop doesn't try to adopt it as somebody else's).
    key = ail_key("synapse", port)
    _state["enabled"][key] = True
    if procs:
        _state["pid"][key] = procs[0].pid
    save_state(key)
    _brain_run_cache.pop(port, None)
    return True, "brain engaged for %s" % bot


# PER-PORT: one brain per Hiss instance. These carry --bot-url http://127.0.0.1:<port>, so they can be
# matched (and killed) for one instance without touching the other's.
_BRAIN_PORT_SCRIPTS   = ["synapse_map.py", "decision_advisor.py"]
# SHARED: one set for the whole machine (they serve every instance). Only the LAST instance to disengage
# may stop these -- killing them while another bot still has its brain engaged would lobotomise it.
_BRAIN_GLOBAL_SCRIPTS = ["deep_thought.py", "growth.py", "brain_service.py", "progression_bard.py"]


def _kill_brain_processes(scripts, port=None):
    """Terminate brain daemons by COMMAND LINE, however they were launched (ail_toggle / engage_brain.bat /
    manual). When `port` is given, only processes bound to THAT bot are killed -- this used to sweep every
    matching python process regardless of port, so disengaging the brain on one Hiss instance killed the
    brain of every other instance too. [Emrald: per-instance brain]
    Data feeds (hud / introspect aggregators) are AIL-managed and left alone."""
    if not scripts:
        return
    like = " -or ".join("$_.CommandLine -like '*%s*'" % sc for sc in scripts)
    cond = "(%s)" % like
    if port:
        # A brain is bound to its bot by --bot-url http://127.0.0.1:<port>. Match the ":<port>" so we only
        # ever kill the daemons steering THIS instance.
        cond += " -and $_.CommandLine -like '*:%s*'" % port
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='python.exe' or Name='pythonw.exe'\" | "
          "Where-Object { %s } | ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force } catch {} }" % cond)
    try:
        subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                       creationflags=CREATE_NO_WINDOW, startupinfo=no_window_si(), timeout=15)
    except Exception:
        pass


def brain_kill(port):
    port = str(port)
    with _brain_lock:
        for p in _brain.get(port, []):
            try:
                p.terminate()
            except Exception:
                pass
        _brain[port] = []
        last_one_out = not any(any(x.poll() is None for x in v) for v in _brain.values())
        if last_one_out:
            for p in _brain_global:
                try:
                    p.terminate()
                except Exception:
                    pass
            _brain_global[:] = []
    # DISENGAGE must truly STOP this bot's brain, however it was launched -- but ONLY this bot's. The
    # per-port sweep is scoped to :<port>; the shared daemons go only when no instance has a brain left.
    _kill_brain_processes(_BRAIN_PORT_SCRIPTS, port=port)
    if last_one_out:
        _kill_brain_processes(_BRAIN_GLOBAL_SCRIPTS)
    # Flip THIS instance's synapse switch off, so the reconcile loop can't revive its daemon and the button
    # sticks. 'synapse' is a per-port AIL, so this touches only this bot -- the other instance's brain
    # switch is a different key and is left exactly as it was.
    key = ail_key("synapse", port)
    _state["enabled"][key] = False
    _state["pid"][key] = 0
    save_state(key)   # ONLY this brain switch. The odometer is a separate AIL precisely so that
                      # disengaging the brain -- which every Hiss restart does -- can no longer take the
                      # hand-result recording down with it. Never disable it here.
    _brain_run_cache.pop(port, None)   # don't serve a stale "engaged" for ~2s after we just killed it
    return True, "brain disengaged for %s" % port


def reconcile_loop():
    """Keep the switches honest: adopt a daemon that's already running (started by the loop / a .bat /
    a previous server) so it shows ON, without resurrecting one the user explicitly switched OFF.
    Per-port AILs are reconciled PER INSTANCE -- adoption is scoped to the bot's port, so one instance's
    brain can never be adopted as another's (which would fuse the two switches back together)."""
    while True:
        time.sleep(12)
        try:
            for a in AILS:
                n = a["name"]
                for port in (known_ports() if a.get("per_port") else [None]):
                    k = ail_key(n, port)
                    if is_running(n, port):
                        continue
                    if _state["enabled"].get(k) is False:   # user turned it off -> respect that
                        continue
                    pid = find_existing_pid(a["args"][0], key_port(k) if a.get("per_port") else None)
                    if pid:
                        _state["pid"][k] = pid
                        _state["enabled"][k] = True
                        save_state()
        except Exception:
            pass

def main():
    load_state()
    # BIND THE PORT FIRST [Emrald]. Restoring the enabled AILs shells out several daemons and can take a
    # few seconds; if that runs BEFORE the bind, hiss's "ensure ail_server" pings :7900 during the gap,
    # sees nothing, and spawns a DUPLICATE ail_server -- and each duplicate re-launches every enabled AIL,
    # cascading into a pile-up of ail_servers + daemons (learn/synapse/...). Binding first also makes any
    # duplicate FAIL FAST (address in use) instead of running long enough to spawn more daemons.
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    sys.stderr.write("[ail_server] listening on 0.0.0.0:%d\n" % PORT)
    sys.stderr.flush()
    threading.Thread(target=tail_loop, daemon=True).start()
    threading.Thread(target=reconcile_loop, daemon=True).start()
    # Restore any AIL that was switched ON before this (re)start -- in the BACKGROUND so it never delays
    # the bind. Per-port AILs restore once per instance that had it on.
    def _restore():
        for a in AILS:
            for port in (known_ports() if a.get("per_port") else [None]):
                if _state["enabled"].get(ail_key(a["name"], port)) and not is_running(a["name"], port):
                    start_daemon(a["name"], port)
    threading.Thread(target=_restore, daemon=True).start()
    srv.serve_forever()

if __name__ == "__main__":
    main()
