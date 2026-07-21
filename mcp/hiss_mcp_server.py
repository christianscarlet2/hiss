#!/usr/bin/env python3
"""
Hiss bot MCP server  (stdio JSON-RPC 2.0, Python stdlib only).

Exposes the running Hiss poker bot + its workspace to Claude:
  - LIVE (attaches to hiss.exe via its terminal HTTP server, port auto-discovered
    from Release\\logs\\terminal_port.txt): terminal panes, internal game state,
    OpenPPL/engine symbols, and a trigger to dump region scrapes / a table screenshot.
  - FILES (read-only, confined to the repo): source code, OHF strategy files, logs,
    debug files, the Release directory, tesseract trained models, and the per-region
    scrape images + their OCR results (the two files per region) + the /improve screenshot.
  - DATABASE (via psql): the tablemap dumped from postgres, and the settings table.

Launched by Claude per .mcp.json. No third-party packages required (Pillow is used
only if present, to convert BMP screenshots to PNG for nicer rendering).
"""

import sys, os, json, glob, base64, subprocess, urllib.request, urllib.parse, time
# NOTE: import urllib.parse HERE at module scope. Importing it locally inside call_tool() would make
# `urllib` a local of that whole function -> the replay_stream/replay_frame branches that use
# urllib.request.quote earlier in the same function then crash with
# "local variable 'urllib' referenced before assignment".

# --- fixed workspace layout -------------------------------------------------
REPO      = os.environ.get("HISS_REPO", r"C:\www\openholdembot_old")
RELEASE   = os.path.join(REPO, "Release")
LOGS      = os.path.join(RELEASE, "logs")
STRATEGY  = os.path.join(RELEASE, "bot_logic", "Strategy")
SCRAPES   = os.path.join(LOGS, "scrapes")
PORT_FILE = os.path.join(LOGS, "terminal_port.txt")
PSQL      = os.environ.get("HISS_PSQL", r"C:\Program Files\PostgreSQL\12\bin\psql.exe")
PGUSER    = os.environ.get("PGUSER", "postgres")
PGDB      = os.environ.get("PGDATABASE", "hiss")
PGPASS    = os.environ.get("PGPASSWORD", "dbpass")

def log(*a):
    print("[hiss-mcp]", *a, file=sys.stderr, flush=True)

# --- path safety: confine all file access to the repo ----------------------
def safe_path(rel):
    p = os.path.normpath(os.path.join(REPO, rel))
    if os.path.commonpath([os.path.abspath(p), os.path.abspath(REPO)]) != os.path.abspath(REPO):
        raise ValueError("path escapes repo: %s" % rel)
    return p

def read_text(path, max_bytes=200000, tail=0):
    with open(path, "rb") as f:
        data = f.read()
    if tail and len(data) > tail:
        data = data[-tail:]
    if len(data) > max_bytes:
        data = data[-max_bytes:]
    return data.decode("utf-8", errors="replace")

def list_files(root, patterns, rel_to=REPO):
    out = []
    for pat in patterns:
        for p in glob.glob(os.path.join(root, pat), recursive=True):
            if os.path.isfile(p):
                out.append(os.path.relpath(p, rel_to).replace("\\", "/"))
    return sorted(set(out))

# --- live Hiss HTTP ---------------------------------------------------------
_port_cache = [None]
def hiss_port():
    if _port_cache[0]:
        return _port_cache[0]
    try:
        with open(PORT_FILE) as f:
            _port_cache[0] = int(f.read().strip())
            return _port_cache[0]
    except Exception:
        pass
    for cand in range(27654, 27665):           # fall back to scanning
        try:
            urllib.request.urlopen("http://127.0.0.1:%d/api/terminal-state" % cand, timeout=0.5).read()
            _port_cache[0] = cand
            return cand
        except Exception:
            continue
    return None

def hiss_get(path):
    port = hiss_port()
    if not port:
        raise RuntimeError("hiss.exe terminal server not found (is Hiss running?)")
    url = "http://127.0.0.1:%d%s" % (port, path)
    with urllib.request.urlopen(url, timeout=5) as r:
        return r.read().decode("utf-8", errors="replace")

def hiss_get_on(port, path):
    """Same as hiss_get but against an EXPLICIT instance.

    hiss_port() caches ONE port, so plain hiss_get() always talks to whichever instance answered
    first. That is wrong for anything instance-specific: several Hiss instances run at once, one
    per scrcpy mirror, and 'which window am I capturing' is meaningless without saying which bot.
    Ports are assigned in bind order, NOT fixed per mirror, so re-check the window list after a
    restart rather than trusting a remembered port->mirror mapping.
    """
    port = int(port) if port else hiss_port()
    if not port:
        raise RuntimeError("hiss.exe terminal server not found (is Hiss running?)")
    url = "http://127.0.0.1:%d%s" % (port, path)
    with urllib.request.urlopen(url, timeout=5) as r:
        return r.read().decode("utf-8", errors="replace")

def live_hiss_ports():
    """Every instance currently answering, so a tool can report the fleet rather than guess."""
    out = []
    for cand in range(27654, 27665):
        try:
            urllib.request.urlopen("http://127.0.0.1:%d/api/terminal-state" % cand, timeout=0.4).read()
            out.append(cand)
        except Exception:
            continue
    return out

# --- AIL control server (mcp/ail_server.py on :7900) -----------------------
AIL_PORT = int(os.environ.get("AIL_SERVER_PORT", "7900"))
def _ail_url(path):
    return "http://127.0.0.1:%d%s" % (AIL_PORT, path)
def ensure_ail_server():
    """Reachable? If not, launch mcp/ail_server.py detached (no console) and wait for it to bind."""
    try:
        urllib.request.urlopen(_ail_url("/ail/ping"), timeout=0.6).read()
        return True
    except Exception:
        pass
    script = os.path.join(REPO, "mcp", "ail_server.py")
    if not os.path.isfile(script):
        return False
    DETACHED = 0x00000008 | 0x00000200 | 0x08000000   # DETACHED | NEW_GROUP | NO_WINDOW
    # pythonw.exe = no console at all, so neither the server nor its child scans flash a window. [Emrald]
    pyw = r"C:\Users\scarl\AppData\Local\Programs\Python\Python310\pythonw.exe"
    launcher = pyw if os.path.isfile(pyw) else sys.executable
    try:
        subprocess.Popen([launcher, script], cwd=os.path.join(REPO, "mcp"), close_fds=True,
                         creationflags=DETACHED if os.name == "nt" else 0)
    except Exception:
        return False
    for _ in range(20):
        time.sleep(0.3)
        try:
            urllib.request.urlopen(_ail_url("/ail/ping"), timeout=0.6).read()
            return True
        except Exception:
            continue
    return False
def ail_get(path):
    if not ensure_ail_server():
        raise RuntimeError("AIL control server (mcp/ail_server.py :%d) not reachable and could not be started" % AIL_PORT)
    with urllib.request.urlopen(_ail_url(path), timeout=10) as r:
        return r.read().decode("utf-8", errors="replace")

# --- replay server (hiss.scarletbeast.com) ----------------------------------
# Ingest/read over the LAN with a Host header (name-based vhost) so frame blobs
# never leave the LAN; the public Cloudflare hostname is only for the browser UI.
REPLAY_BASE = os.environ.get("HISS_REPLAY_URL", "http://192.168.1.39")
REPLAY_HOST = os.environ.get("HISS_REPLAY_HOST", "hiss.scarletbeast.com")

def replay_get(path, raw=False):
    req = urllib.request.Request(REPLAY_BASE + path, headers={"Host": REPLAY_HOST})
    with urllib.request.urlopen(req, timeout=15) as r:
        data = r.read()
    return data if raw else json.loads(data.decode("utf-8", errors="replace"))

def replay_latest_ts(hand):
    frames = replay_get("/api/frames/%s" % urllib.request.quote(str(hand)))
    return frames[-1]["ts_ms"] if frames else 0

# --- replayer.exe integration (load hands/sessions as BMP frames + auto-play) ---
REPLAYER_EXE = os.environ.get("HISS_REPLAYER_EXE", r"C:\www\openholdembot_old\Release\replayer.exe")
REPLAYER_DIR = os.environ.get("HISS_REPLAYER_DIR", r"C:\tmp\replayer")

def _hand_frames_to_bmp(hand, dest_dir, start_index=0, max_frames=0):
    """Fetch a hand's PNG frames from the replay server and write them as
    frame??????.bmp (replayer.exe loads BMP, not PNG). Returns the next free index."""
    import io
    from PIL import Image
    os.makedirs(dest_dir, exist_ok=True)
    frames = replay_get("/api/frames/%s" % urllib.request.quote(str(hand)))
    if max_frames and len(frames) > max_frames:
        step = len(frames) / float(max_frames)
        frames = [frames[int(i * step)] for i in range(max_frames)]
    idx = start_index
    for f in frames:
        ts = f.get("ts_ms")
        if ts is None:
            continue
        try:
            png = replay_get("/api/img/%s/%d" % (urllib.request.quote(str(hand)), int(ts)), raw=True)
            Image.open(io.BytesIO(png)).convert("RGB").save(
                os.path.join(dest_dir, "frame%06d.bmp" % idx), "BMP")
            idx += 1
        except Exception:
            continue
    return idx

# --- postgres via psql ------------------------------------------------------
def esc_sql(s):
    return ("" if s is None else str(s)).replace("'", "''")

def psql_query(sql, database=None, tuples_only=True):
    env = dict(os.environ); env["PGPASSWORD"] = PGPASS
    flags = ["-A"] + (["-t"] if tuples_only else [])   # unaligned; headers unless tuples_only
    cmd = [PSQL, "-U", PGUSER, "-d", database or PGDB] + flags + ["-c", sql]
    # CREATE_NO_WINDOW (0x08000000): never pop a psql console/terminal window. [Emrald]
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=60,
                          creationflags=(0x08000000 if os.name == "nt" else 0))
    if proc.returncode != 0:
        raise RuntimeError("psql failed: %s" % (proc.stderr.strip() or proc.stdout.strip()))
    return proc.stdout

# Statements that are safe in read-only mode (Claude can't DROP/DELETE without opt-in).
_READONLY_PREFIXES = ("select", "with", "explain", "show", "table", "values")
def is_readonly_sql(sql):
    s = sql.strip()
    # strip leading line/block comments
    while s.startswith("--") or s.startswith("/*"):
        if s.startswith("--"):
            nl = s.find("\n"); s = s[nl + 1:].strip() if nl >= 0 else ""
        else:
            end = s.find("*/"); s = s[end + 2:].strip() if end >= 0 else ""
    first = (s.lower().split(None, 1) or [""])[0]
    return first in _READONLY_PREFIXES

# --- image content ----------------------------------------------------------
def image_content(path):
    with open(path, "rb") as f:
        raw = f.read()
    mime = "image/bmp"
    try:                                        # convert BMP -> PNG if Pillow exists
        from PIL import Image
        import io
        im = Image.open(io.BytesIO(raw))
        buf = io.BytesIO(); im.save(buf, format="PNG")
        raw = buf.getvalue(); mime = "image/png"
    except Exception:
        pass
    return {"type": "image", "data": base64.b64encode(raw).decode("ascii"), "mimeType": mime}

# ===========================================================================
#  TOOLS
# ===========================================================================
TOOLS = [
    {"name": "hiss_status", "description": "Is hiss.exe running? Returns its terminal HTTP port and reachability.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "start_hiss", "description": "Launch Release\\Hiss.exe (cwd=Release). No-op if already reachable, unless force=true.",
     "inputSchema": {"type": "object", "properties": {"force": {"type": "boolean", "default": False}}}},
    {"name": "stop_hiss", "description": "Terminate all running Hiss.exe processes (and their OCR workers).",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "replay_screenshot", "description": "Take a FRESH replay screenshot of the connected table window (triggers a capture, then returns the image).",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "autoplayer_toggle", "description": "Turn the bot's autoplayer on or off.",
     "inputSchema": {"type": "object", "properties": {"on": {"type": "boolean"}}, "required": ["on"]}},
    {"name": "fckra_action", "description": "Manually act on the table (fold/check/call/bet/raise/allin), passing through the bot's button-finding (label/state/button regions). bet/raise use the two-successive-clicks + numpad path; pass amount (in big blinds) for a sized bet/raise. Applied on the next heartbeat when the button is live.",
     "inputSchema": {"type": "object", "properties": {"action": {"type": "string", "enum": ["fold", "check", "call", "bet", "raise", "allin"]}, "amount": {"type": "number", "description": "bet/raise size in big blinds"}}, "required": ["action"]}},
    {"name": "terminal_panes", "description": "Live contents of the 4 Terminal panes (Context / State / Decisions / Chat) + the pinned State block, from the running hiss.exe.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "hand_report", "description": "EVERYTHING about one hand, by hand number: the hand number, its gamestate/hand-history, every NN-driver decision, every OHF decision, the net result, and the list of captured frame screenshots (png paths you can Read). Use this FIRST when asked why the bot did or did not do something on a specific hand -- it joins the tables that otherwise have to be queried one by one. Pass no handnumber to get the most recent hand.",
     "inputSchema": {"type": "object", "properties": {
        "handnumber": {"type": "string", "description": "e.g. 2782511829; omit for the most recent hand"},
        "max_frames": {"type": "integer", "description": "cap on screenshot rows returned (default 40)"}}}},
    {"name": "set_table_game_info", "description": "Set table_game_info from what YOU (Claude) read in the table image (a heartbeat frame) - NOT OCR. Determine the real blinds/ante/level/tourney from the screenshot and pass them here; the bot uses these as authoritative (blinds drive the engine, fixing BB-denominated displays). For a big-blind display set sb=0.5 bb=1.0 and chips_per_bb to the real big blind (e.g. 400). All fields optional.",
     "inputSchema": {"type": "object", "properties": {
        "sb": {"type": "number", "description": "operating small blind (BB-display -> 0.5)"},
        "bb": {"type": "number", "description": "operating big blind (BB-display -> 1.0)"},
        "ante": {"type": "number"},
        "chips_per_bb": {"type": "number", "description": "real chips per big blind, e.g. 400"},
        "level": {"type": "integer"},
        "players": {"type": "integer", "description": "players remaining"},
        "tourney_name": {"type": "string"}, "tourney_id": {"type": "string"},
        "table_number": {"type": "string"}, "gametype": {"type": "string"}}}},
    {"name": "click_region", "description": "Click an arbitrary tablemap region by name (lobby navigation: goto_lobby_button, leave_lobby_button, return_to_tables_button, lobby_more_info_button, etc.). The bot clicks the region's center on its next heartbeat. Use to open the lobby, read tournament info from the frame, then return to the tables.",
     "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}}, "required": ["name"]}},
    {"name": "set_region_value", "description": "Claude/MCP TRANSFORM: you parsed a region from the table image (a heartbeat frame) - NOT OCR - and post its value here. The scraper returns this value for that region instead of OCR'ing it. Use for finicky regions OCR misreads. name = the tablemap region name (e.g. c0handnumber), value = what you read.",
     "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}, "value": {"type": "string"}}, "required": ["name", "value"]}},
    {"name": "set_table_game_info_2", "description": "table_game_info_2: set the CURRENT and PREVIOUS hand numbers you read from the table image (ACR shows 'Current: <n>  Previous: <n>'). Exposed as symbols table_game_info_2 / tgi2_handnumber / tgi2_prev_handnumber.",
     "inputSchema": {"type": "object", "properties": {"curr_hand": {"type": "number"}, "prev_hand": {"type": "number"}}}},
    {"name": "reload_ohf", "description": "Reload the OHF strategy in the running hiss.exe WITHOUT a restart. Re-parses bot_logic/Strategy + the master OHF on the next heartbeat. Use after editing/rebuilding the strategy so changes take effect live.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "validate", "description": "Run the scrape + game-state sanity heuristics (CSymbolEngineValidator) on the CURRENT table state and return the verdict: ok, confidence (0..1), error/warning counts, per-category flags (cards/pot/stacks/bets), and a human-readable report of every issue. Use to decide whether a scraped value (pot, stacks, bets, cards) is trustworthy before acting on it.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "log_settings", "description": "Read or toggle the Hiss advanced-logging settings (hiss_log_settings table: advanced_logging | reporting | replays), per daemon identity ('*' = global default; a daemon_id row overrides it, each flag falling back to '*'). action='list' (default) shows all rows; action='set' with kind + value (+ optional identity) flips one flag. This DB table is the single source of truth shared by the Linux headless daemons, hiss.exe, and the hiss.scarletbeast.com web control.",
     "inputSchema": {"type": "object", "properties": {"action": {"type": "string", "enum": ["list", "set"]}, "kind": {"type": "string"}, "value": {"type": "boolean"}, "identity": {"type": "string"}}}},
    {"name": "validate_ohf", "description": "VALIDATE THE OHF STRATEGY before deploying. Runs the hardened build_and_lint.py over .strategy_build/strategy/*.ohf: concatenates the master, checks structural rules (WHEN/FORCE/action, balanced parens), the symbol whitelist, AND the OpenPPL operator grammar (e.g. it rejects '<>' / '!=' -- OpenPPL has no not-equal operator; use NOT (a = b) -- which the real parser rejects but a text lint would miss). Also returns the latest real-parser errors logged by Hiss (logs/ohf_parse_errors.log) if present. ALWAYS call this after editing any .ohf and BEFORE restarting Hiss; fix every reported error first. Returns PASS or the full error list.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "game_state", "description": "Live internal-engine game state JSON (seats, cards, pot, blinds, button, hero, HUD) from the running hiss.exe.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "symbols", "description": "Evaluate OpenPPL / engine symbols live. Pass a comma-separated list of symbol names.",
     "inputSchema": {"type": "object", "properties": {"names": {"type": "string", "description": "comma-separated symbol names, e.g. 'prwin,Raises,PotSize,f$Style'"}}, "required": ["names"]}},
    {"name": "trigger_scrape_dump", "description": "Tell hiss.exe to dump the full-table screenshot + every region's raw scrape image and OCR result to logs/scrapes on its next heartbeat, then list what was written.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "table_screenshot", "description": "The most recent full-table screenshot the bot captured (logs/scrapes/_table.bmp). Run trigger_scrape_dump or /improve first to refresh it.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "list_scrapes", "description": "List the per-region scrape files (raw images and OCR-result .txt) in logs/scrapes.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "hud_calibrate_pending", "description": "Check whether the user requested a HUD recalibration (right-clicked 'Recalibrate all HUDs (Claude)' on the scrcpy overlay). If pending, ALSO returns the fresh table screenshot so you can locate each seated player's name-plate, then call post_hud_positions with one anchor per occupied seat. Anchor each box just BELOW the name-plate so it never covers the player's balance/stack or cards.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "post_hud_positions", "description": "Set per-seat HUD overlay box anchors. 'positions' maps chair index -> top-left pixel coords on the table screenshot, e.g. {\"0\":{\"x\":120,\"y\":300},\"3\":{\"x\":500,\"y\":80}}. Optional 'locked' bool. Hiss converts pixels to client-area fractions and repositions + persists the boxes.",
     "inputSchema": {"type": "object", "properties": {"positions": {"type": "object"}, "locked": {"type": "boolean"}}, "required": ["positions"]}},
    {"name": "open_md_viewer", "description": "Open a markdown file in the user's MarkdownViewer (C:\\\\www\\\\mdviewer\\\\dist\\\\MarkdownViewer.exe). Use this to show the user any plan or markdown meant for them to read (relative paths resolve against the repo).",
     "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}},
    {"name": "replay_hands", "description": "Time-travel debugger: list the most-recent hands captured on the replay server (hiss.scarletbeast.com) with frame counts + start/end timestamps. Use to pick a hand to investigate, then replay_stream/replay_frame at a ts_ms.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "replay_stream", "description": "Time-travel debugger: the full state AS OF a moment in a hand -- the hand-history text, the latest symbol/scrape values, and the OHF decision (action + f$fold/f$call/f$raise/f$betsize + the complete decision-tree trace) at/just-before ts_ms. The single best tool for understanding WHY the bot did what it did on a given hand.",
     "inputSchema": {"type": "object", "properties": {"hand": {"type": "string"}, "ts": {"type": "integer", "description": "ts_ms cutoff; omit for the latest"}}, "required": ["hand"]}},
    {"name": "replay_frame", "description": "Time-travel debugger: the actual PNG screen-grab Hiss captured for a hand at (or nearest before) a ts_ms. See exactly what the bot saw at that heartbeat.",
     "inputSchema": {"type": "object", "properties": {"hand": {"type": "string"}, "ts": {"type": "integer", "description": "ts_ms; omit for the latest frame in the hand"}}, "required": ["hand"]}},
    {"name": "replayer_load", "description": "Load a hand (or several hands / the N most-recent as a session) from the replay server into the desktop replayer.exe and auto-play it, so you can watch the frames and inspect with vision.exe. Frames are fetched as PNG and converted to the BMP frames replayer.exe plays. Provide hand, OR hands (list), OR recent (count of newest hands).",
     "inputSchema": {"type": "object", "properties": {
        "hand": {"type": "string", "description": "single handnumber to load"},
        "hands": {"type": "array", "items": {"type": "string"}, "description": "several handnumbers, concatenated in order"},
        "recent": {"type": "integer", "description": "load the N most-recent hands as one session (played oldest->newest)"},
        "max_frames": {"type": "integer", "description": "cap frames per hand (evenly sampled); 0 = all"}}}},
    {"name": "read_scrape", "description": "Get a region's raw scrape image and its OCR/recognition result text.",
     "inputSchema": {"type": "object", "properties": {"region": {"type": "string"}}, "required": ["region"]}},
    {"name": "list_logs", "description": "List log files in Release/logs.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "read_log", "description": "Read a log file (tails large files).",
     "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}, "tail_bytes": {"type": "integer", "default": 60000}}, "required": ["name"]}},
    {"name": "list_ohf", "description": "List the OHF strategy files (Release/bot_logic/Strategy + the master).",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "read_ohf", "description": "Read an OHF strategy file by name (e.g. 40_preflop.ohf).",
     "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}}, "required": ["name"]}},
    {"name": "search_source", "description": "Grep the bot source code (ripgrep-style regex). Returns matching file:line.",
     "inputSchema": {"type": "object", "properties": {"pattern": {"type": "string"}, "glob": {"type": "string", "default": "*.cpp"}}, "required": ["pattern"]}},
    {"name": "read_source", "description": "Read a source/debug file by repo-relative path (confined to the repo).",
     "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}},
    {"name": "write_source", "description": "WRITE (create or overwrite) a Hiss source / OHF / script file by repo-relative path (confined to the repo). The MCP can AUTHOR CODE for Hiss -- C++, OHF, Python. After C++ edits a rebuild is needed; after OHF edits call validate_ohf then reload_ohf or restart. Prefer edit_source for surgical changes to large files.",
     "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "content": {"type": "string"}}, "required": ["path", "content"]}},
    {"name": "edit_source", "description": "Exact-string replace in a Hiss repo file (confined to the repo) -- surgical edits without rewriting the whole file. 'old' must match exactly and be unique unless replace_all=true. After C++ edits rebuild; after OHF edits validate_ohf.",
     "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "old": {"type": "string"}, "new": {"type": "string"}, "replace_all": {"type": "boolean", "default": False}}, "required": ["path", "old", "new"]}},
    {"name": "list_release", "description": "List files in the Release directory (optionally a subpath/glob).",
     "inputSchema": {"type": "object", "properties": {"subpath": {"type": "string", "default": ""}}}},
    {"name": "list_tesseract_models", "description": "List tesseract trained models (.traineddata/.checkpoint) under the repo, plus the AutoOcr model settings.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "dump_tablemap", "description": "Dump the tablemap(s) from the postgres 'hiss' DB to logs/tablemap_dump.json and return them. Optional name filter.",
     "inputSchema": {"type": "object", "properties": {"name": {"type": "string", "description": "optional tablemap name filter (ILIKE)"}}}},
    {"name": "read_settings", "description": "Read the postgres settings table (key -> jsonb value). Optional key filter.",
     "inputSchema": {"type": "object", "properties": {"key": {"type": "string", "description": "optional key filter (ILIKE)"}}}},
    {"name": "pg_query", "description": "Run a SQL query against a postgres database (default 'hiss'). Read-only by default (SELECT/WITH/EXPLAIN/SHOW); set allow_write=true to permit data-modifying statements.",
     "inputSchema": {"type": "object", "properties": {
         "sql": {"type": "string"},
         "database": {"type": "string", "description": "database name (default hiss)"},
         "allow_write": {"type": "boolean", "default": False}},
      "required": ["sql"]}},
    {"name": "pg_databases", "description": "List the postgres databases on the server.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "pg_tables", "description": "List tables (schema.table) in a database (default 'hiss').",
     "inputSchema": {"type": "object", "properties": {"database": {"type": "string"}}}},
    {"name": "pg_describe", "description": "Describe a table's columns and types.",
     "inputSchema": {"type": "object", "properties": {"table": {"type": "string"}, "database": {"type": "string"}}, "required": ["table"]}},
    {"name": "learner_decisions", "description": "Read human decisions logged from learner.exe (action + reasoning + game-state snapshot). Compare these to the OHF to find improvements.",
     "inputSchema": {"type": "object", "properties": {
         "only_unreviewed": {"type": "boolean", "default": True},
         "limit": {"type": "integer", "default": 20}}}},
    {"name": "learner_ask", "description": "Post a question to the human in learner.exe. 'summary' is a SHORT, succinct version read aloud (ElevenLabs) and shown as the headline; 'question' is the full detail shown in the box. If summary is omitted it is auto-derived from the question. Optionally link to a decision id.",
     "inputSchema": {"type": "object", "properties": {
         "question": {"type": "string", "description": "full detail (shown in the box)"},
         "summary": {"type": "string", "description": "short succinct version (read aloud + headline)"},
         "decision_id": {"type": "integer"}},
      "required": ["question"]}},
    {"name": "learner_answers", "description": "Read the human's answers to questions you posted in learner.exe.",
     "inputSchema": {"type": "object", "properties": {"only_recent": {"type": "boolean", "default": True}}}},
    {"name": "speak", "description": "Read text aloud in the Lilith (ElevenLabs) voice via the shared lilith.exe (mutes other apps except scrcpy + ACR Poker, then restores). Works during bot play when learner.exe isn't open. Use for tilt alerts.",
     "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}},
    {"name": "card_scrapes", "description": "Vision link: trigger a fresh capture and show what every CARD region scraped -- each player's hole cards (p0..p8) and the community cards -- so the human can tell you which position/card is missing or wrong.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "card_image", "description": "Vision link: the raw scrape IMAGE + recognised value for one card region, to verify a misread. Specify a player chair (0-8) and slot (0/1), or position='community' with slot 0-4.",
     "inputSchema": {"type": "object", "properties": {
         "position": {"type": "string", "description": "player chair number 0-8, or 'community'/'board'"},
         "slot": {"type": "integer", "description": "card index: 0/1 for players, 0-4 for community"}},
      "required": ["position", "slot"]}},
    {"name": "log_card_correction", "description": "Vision link: record the human's correction of a misscraped/missing card (position, slot, what the bot read, what it actually is) for font/region fixing.",
     "inputSchema": {"type": "object", "properties": {
         "position": {"type": "string"}, "slot": {"type": "integer"},
         "scraped": {"type": "string"}, "correct": {"type": "string"}, "note": {"type": "string"}},
      "required": ["position", "slot", "correct"]}},
    {"name": "ail_list", "description": "List the Autonomous-Improvement-Loop / data daemons (synapse harmonizer, observational learning, voice feedback, replay shipper, coach hype, HUD aggregator) with their on/off + running state, via the AIL control server (mcp/ail_server.py :7900). These are the same switches shown on the browser Terminal's AIL tab. Each entry reports per_port: a per-port AIL (the brain / synapse) is switched PER HISS INSTANCE -- pass port to see that instance's switches (default 27654).",
     "inputSchema": {"type": "object", "properties": {"port": {"type": "integer", "default": 27654}}}},
    {"name": "ail_toggle", "description": "Switch one AIL daemon on or off via the AIL control server (same switches as the browser Terminal AIL tab). name = synapse|observe|voice|shipper|coach|hud; on = true/false. port = which Hiss instance (default 27654) -- it matters for per-port AILs like synapse (the brain), where each instance has its OWN switch; global AILs ignore it.",
     "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}, "on": {"type": "boolean"},
         "port": {"type": "integer", "default": 27654}}, "required": ["name", "on"]}},
    {"name": "ail_output", "description": "Recent merged feedback lines from all enabled AIL daemons (the big terminal on the AIL tab). Pass since=<cursor> to get only new lines; returns lines + the new cursor to pass next time.",
     "inputSchema": {"type": "object", "properties": {"since": {"type": "integer", "default": 0}}}},
    {"name": "lobby_fetch", "description": "Trigger the lobby-recon choreography (mcp/lobby_fetch.sh) for a Hiss instance via the AIL control server: navigate to the tournament lobby, capture the info pages to C:/tmp, and return to the felt. port = the instance's terminal port (default 27654). Parse C:/tmp/lobby_main.png + C:/tmp/lobby_moreinfo.png AFTER it returns.",
     "inputSchema": {"type": "object", "properties": {"port": {"type": "integer", "default": 27654}}}},
    {"name": "set_capture_window", "description": "Change (or inspect) which window a Hiss instance is capturing. Call with NO title/hwnd/clear to LIST every pickable window for that instance, marking which is attached and which are already served by another bot -- do this first to get the hwnd/title. Then pass title (exact match, e.g. 'S10'/'A17'/'EMU'/'EMU2') or hwnd to pin that window; the connect happens on the next heartbeat, so re-check to confirm. clear=true drops the pin and restores automatic table selection. Several instances run at once (one per scrcpy mirror) so ALWAYS pass port to say which bot you mean; ports are assigned in bind order and are NOT fixed per mirror, so re-list after any restart instead of reusing an old port->mirror mapping.",
     "inputSchema": {"type": "object", "properties": {
        "port": {"type": "integer", "description": "terminal port of the instance to act on (omit to use the first one found)"},
        "title": {"type": "string", "description": "exact window title to capture, e.g. 'S10'. Exact because 'EMU' is a prefix of 'EMU2'."},
        "hwnd": {"type": "integer", "description": "window handle, if you already have it from a listing"},
        "clear": {"type": "boolean", "description": "release the manual pin and go back to automatic table selection"}}}},

    # ---- automation maps: the region maps for PROCESSES the bot drives on a phone
    # mirror (logging in, joining a freeroll) rather than for the felt itself. They live
    # in the automation.* schema, separate from the playing tablemaps.
    {"name": "automation_enabled", "description": "Is the AUTOMATION button on for an instance? Reads the same automation_api.enabled_<port> setting the toolbar tile and the tournament-join poller use, straight from the running Hiss. Omit port to report every live instance.",
     "inputSchema": {"type": "object", "properties": {
        "port": {"type": "integer", "description": "terminal port of one instance; omit for the whole fleet"},
        "set": {"type": "boolean", "description": "turn automation on/off for that instance (port required)"}}}},
    {"name": "automation_map_capture", "description": "Screenshot a phone mirror's scrcpy window (client area) and return the image. Use before placing regions so you can SEE where they belong. Runs the capture in the logged-on desktop session automatically when called from a session that owns no windows (ssh/service).",
     "inputSchema": {"type": "object", "properties": {
        "window": {"type": "string", "description": "window title substring, e.g. 'A17' or 'S10'"},
        "out": {"type": "string", "description": "png path to write (default C:\\tmp\\<window>.png)"}},
        "required": ["window"]}},
    {"name": "automation_map_set_regions", "description": "Define the regions of an automation map for one process/step, draw them onto the reference screenshot, and store that annotated image in the map. Creates the map if it does not exist. Each region: {name, rect:[l,t,r,b], transform?} -- transform 'autoocr0' makes Hiss OCR it. Pass the screenshot you captured with automation_map_capture.",
     "inputSchema": {"type": "object", "properties": {
        "map": {"type": "string", "description": "automation map name, e.g. automation_a17"},
        "window": {"type": "string", "description": "scrcpy window title for this mirror (used when creating the map)"},
        "process": {"type": "string", "description": "the process these regions belong to, e.g. 'login'"},
        "step": {"type": "integer", "default": 1},
        "shot": {"type": "string", "description": "path of the reference screenshot to draw on"},
        "label": {"type": "string"},
        "regions": {"type": "array", "description": "[{name, rect:[l,t,r,b], transform}]", "items": {"type": "object"}}},
        "required": ["map", "process", "regions"]}},
    {"name": "automation_map_show", "description": "List an automation map's regions, and decode the stored reference screenshot for a process/step back out of the database so you can check the boxes really sit where you meant.",
     "inputSchema": {"type": "object", "properties": {
        "map": {"type": "string"}, "process": {"type": "string"}, "step": {"type": "integer", "default": 1},
        "image": {"type": "boolean", "description": "also return the stored screenshot (default true)"}},
        "required": ["map"]}},
]

def _card_region(position, slot):
    p = str(position).strip().lower()
    if p in ("community", "board", "c0", "c"):
        return "c0cardface%d" % int(slot)
    p = p.replace("p", "")
    return "p%dcardface%d" % (int(p), int(slot))

# --- automation maps --------------------------------------------------------
# The heavy lifting (capture, postgres, drawing) lives in automation_map.py so the
# same code serves the MCP tools, the command line, and the interactive-session
# capture task. This is just the adapter.
def _automation_map():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import automation_map
    return automation_map


def call_tool(name, args):
    if name == "automation_enabled":
        port = args.get("port")
        if args.get("set") is not None:
            if not port:
                return [{"type": "text", "text": "port is required when setting automation on/off."}]
            return [{"type": "text", "text": hiss_get_on(
                port, "/api/automation-enabled?on=" + ("1" if args["set"] else "0"))}]
        if port:
            return [{"type": "text", "text": hiss_get_on(port, "/api/automation-enabled")}]
        out = []
        for p in live_hiss_ports():
            try:
                out.append("%d: %s" % (p, hiss_get_on(p, "/api/automation-enabled").strip()))
            except Exception as e:
                out.append("%d: unreachable (%s)" % (p, e))
        return [{"type": "text", "text": "\n".join(out) or "no live Hiss instances"}]

    if name == "automation_map_capture":
        am = _automation_map()
        window = args["window"]
        out = args.get("out") or (r"C:\tmp\%s.png" % window)
        status = am.capture(window, out)
        return [{"type": "text", "text": "%s -> %s" % (status, out)}, image_content(out)]

    if name == "automation_map_set_regions":
        am = _automation_map()
        process, step = args["process"], int(args.get("step", 1))
        tm = am.map_id(args["map"], create_with=(args.get("window") or args["map"], "wpn"))
        for r in args["regions"]:
            am.set_region(tm, r["name"], tuple(int(v) for v in r["rect"]),
                          process, step, r.get("transform"))
        written = am.regions(tm, process, step)
        shot = args.get("shot")
        if not shot:
            return [{"type": "text", "text": "regions written (no shot given, nothing drawn):\n" +
                     "\n".join(str(r) for r in written)}]
        drawn = os.path.splitext(shot)[0] + "_regions.png"
        am.draw_regions(shot, drawn, written)
        w, h = am.store_screenshot(tm, process, step, args.get("label") or ("%s step %d" % (process, step)), drawn)
        return [{"type": "text", "text": "map %s (id %d): %d regions on %s step %d, reference image %dx%d stored" %
                 (args["map"], tm, len(written), process, step, w, h)}, image_content(drawn)]

    if name == "automation_map_show":
        am = _automation_map()
        tm = am.map_id(args["map"])
        process = args.get("process")
        step = int(args.get("step", 1))
        rows = am.regions(tm, process, step if process else None)
        text = "map %s (id %d)\n" % (args["map"], tm) + "\n".join(str(r) for r in rows)
        if process and args.get("image", True):
            out = r"C:\tmp\_show_%s_%s_%d.png" % (args["map"], process, step)
            try:
                am.load_screenshot(tm, process, step, out)
                return [{"type": "text", "text": text}, image_content(out)]
            except SystemExit as e:
                text += "\n(no stored screenshot: %s)" % e
        return [{"type": "text", "text": text}]

    if name == "set_capture_window":
        port = args.get("port")
        try:
            ports = live_hiss_ports()
            if port and int(port) not in ports:
                return [{"type": "text", "text":
                    "No Hiss instance is listening on port %s. Live instances: %s"
                    % (port, ", ".join(str(p) for p in ports) or "(none)")}]
            if not port:
                if not ports:
                    return [{"type": "text", "text": "No Hiss instance is running."}]
                port = ports[0]

            # clear the pin -> automatic table selection
            if args.get("clear"):
                st = json.loads(hiss_get_on(port, "/api/connect-window?clear=1"))
                return [{"type": "text", "text":
                    "port %d: manual window pin CLEARED -- automatic table selection restored.\n"
                    "attached_hwnd=%s status=%s" % (port, st.get("attached_hwnd"), st.get("status"))}]

            wins = json.loads(hiss_get_on(port, "/api/windows")).get("windows", [])
            title = args.get("title")
            hwnd = args.get("hwnd")

            # No target -> LIST. Callers need the hwnd/title and, just as importantly, to see
            # which windows another instance already holds.
            if not title and not hwnd:
                lines = ["port %d -- pickable windows (fleet: %s):"
                         % (port, ", ".join(str(p) for p in ports))]
                for w in wins:
                    flags = []
                    if w.get("attached"): flags.append("ATTACHED (this instance)")
                    elif w.get("served"): flags.append("served by another bot")
                    lines.append("  hwnd=%-10s %-24s %sx%s %s"
                                 % (w.get("hwnd"), "'%s'" % w.get("title", ""),
                                    w.get("w"), w.get("h"), " ".join(flags)))
                lines.append("Pass title= or hwnd= to capture one of these.")
                return [{"type": "text", "text": "\n".join(lines)}]

            if title and not hwnd:
                # EXACT match: 'EMU' is a prefix of 'EMU2', and a substring match would happily
                # pin the wrong emulator -- a collision that has already bitten this rig once.
                hits = [w for w in wins if w.get("title") == title]
                if not hits:
                    avail = ", ".join("'%s'" % w.get("title", "") for w in wins) or "(none)"
                    return [{"type": "text", "text":
                        "No window titled exactly '%s' on port %d. Available: %s" % (title, port, avail)}]
                if len(hits) > 1:
                    return [{"type": "text", "text":
                        "%d windows are titled exactly '%s' (hwnds: %s). Pass hwnd= to disambiguate."
                        % (len(hits), title, ", ".join(str(w.get("hwnd")) for w in hits))}]
                hwnd = hits[0].get("hwnd")
                if hits[0].get("served") and not hits[0].get("attached"):
                    note = " NOTE: another instance currently holds this window; the pin may not take."
                else:
                    note = ""
            else:
                note = ""

            want = int(hwnd)
            st = json.loads(hiss_get_on(port, "/api/connect-window?hwnd=%d" % want))
            # The connect runs on the HEARTBEAT thread, so the response above always still shows
            # the old window. Poll for the real outcome instead of reporting the request and
            # calling it done -- a pin can legitimately fail (no tablemap matches that window's
            # size/title/tablepoints), and Hiss puts that reason in `status`.
            outcome, status = None, st.get("status")
            for _ in range(12):                      # ~18s; a connect cycle is a few seconds
                time.sleep(1.5)
                cur = json.loads(hiss_get_on(port, "/api/connect-window"))
                status = cur.get("status") or status
                if int(cur.get("attached_hwnd") or 0) == want:
                    outcome = "ok"
                    break
                if not cur.get("override"):
                    # Hiss dropped the pin: it gave up on that window and fell back.
                    outcome = "failed"
                    break
            if outcome == "ok":
                return [{"type": "text", "text":
                    "port %d: NOW CAPTURING hwnd=%d ('%s').%s" % (port, want, title or "", note)}]
            attached = json.loads(hiss_get_on(port, "/api/windows")).get("windows", [])
            now = [w for w in attached if w.get("attached")]
            now_s = ("'%s' (hwnd=%s)" % (now[0].get("title"), now[0].get("hwnd"))) if now else "nothing"
            if outcome == "failed":
                return [{"type": "text", "text":
                    "port %d: could NOT capture hwnd=%d ('%s') -- Hiss dropped the pin and is on %s.\n"
                    "reason: %s\n"
                    "A window is only pickable if some tablemap matches its size/title/tablepoints, "
                    "so check the mirror's client size against the tablemap's clientsizemin/max."
                    % (port, want, title or "", now_s, status)}]
            return [{"type": "text", "text":
                "port %d: requested hwnd=%d ('%s') but it has not attached yet; still on %s.%s\n"
                "status: %s -- re-run with no arguments to check again."
                % (port, want, title or "", now_s, note, status)}]
        except Exception as e:
            return [{"type": "text", "text": "set_capture_window failed: %s" % e}]
    if name == "hiss_status":
        port = hiss_port()
        if not port:
            return [{"type": "text", "text": "hiss.exe terminal server NOT found (Hiss not running, or no port file)."}]
        try:
            hiss_get("/api/terminal-state")
            return [{"type": "text", "text": "hiss.exe reachable on port %d." % port}]
        except Exception as e:
            return [{"type": "text", "text": "port %d found but not responding: %s" % (port, e)}]
    if name == "start_hiss":
        if not args.get("force") and hiss_port():
            try:
                hiss_get("/api/terminal-state")
                return [{"type": "text", "text": "Hiss already running and reachable; not relaunching (pass force=true to start another)."}]
            except Exception:
                pass
        exe = os.path.join(RELEASE, "Hiss.exe")
        if not os.path.isfile(exe):
            return [{"type": "text", "text": "Hiss.exe not found at %s" % exe}]
        DETACHED = 0x00000008 | 0x00000200   # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
        subprocess.Popen([exe], cwd=RELEASE, close_fds=True,
                         creationflags=DETACHED if os.name == "nt" else 0)
        # LIFECYCLE COUPLING: the AIL control server + the brain (synapse) live and die WITH Hiss. Bring up
        # ail_server (windowless) and engage the brain for this instance's terminal port. [Emrald]
        coupled = []
        if ensure_ail_server():
            coupled.append("ail_server")
            port = 0
            for _ in range(20):          # wait for Hiss to bind its terminal port
                time.sleep(0.5)
                port = hiss_port() or 0
                if port:
                    break
            if port:
                try:
                    ail_get("/brain?port=%d&on=1" % port)   # starts synapse + the brain daemons
                    coupled.append("brain(synapse) :%d" % port)
                except Exception:
                    pass
        # Couple the vision driver to Hiss boot (the port is bound by now): one windowless cycle
        # corrects table name / dollar blinds / name-lint immediately; the 3-min HissVisionDriver
        # scheduled task continues it thereafter. Best-effort, never blocks start_hiss. [vision]
        try:
            _vd = os.path.join(REPO, "mcp", "vision_driver.py")
            _pyw = r"C:\Users\scarl\AppData\Local\Programs\Python\Python310\pythonw.exe"
            if os.path.isfile(_vd):
                subprocess.Popen([_pyw if os.path.isfile(_pyw) else sys.executable, _vd, "--once"],
                                 cwd=os.path.join(REPO, "mcp"), close_fds=True,
                                 creationflags=(0x00000008 | 0x08000000) if os.name == "nt" else 0)
        except Exception:
            pass
        return [{"type": "text", "text": "Launched Hiss.exe (cwd=Release). Coupled: %s. Give it a few seconds, then hiss_status."
                 % (", ".join(coupled) or "none (ail_server unreachable)")}]
    if name == "stop_hiss":
        if os.name != "nt":
            return [{"type": "text", "text": "stop_hiss is Windows-only."}]
        # LIFECYCLE COUPLING: disengage the brain + stop the AIL control server, then kill Hiss. (ail_get is
        # NOT used here so we never auto-start ail_server just to stop it.) [Emrald]
        notes = []
        try:
            port = hiss_port() or 0
            if port:
                urllib.request.urlopen(_ail_url("/brain?port=%d&on=0" % port), timeout=3).read()
                notes.append("brain disengaged")
        except Exception:
            pass
        proc = subprocess.run(["taskkill", "/IM", "Hiss.exe", "/F"],
                              capture_output=True, text=True)
        out = (proc.stdout or "") + (proc.stderr or "")
        try:   # stop the AIL control server (it re-adopts its daemons on the next start_hiss)
            ps = ("Get-CimInstance Win32_Process -Filter \"Name='python.exe' or Name='pythonw.exe'\" | "
                  "Where-Object { $_.CommandLine -like '*ail_server.py*' } | "
                  "ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force } catch {} }")
            subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                           capture_output=True, creationflags=0x08000000)
            notes.append("ail_server stopped")
        except Exception:
            pass
        return [{"type": "text", "text": (out.strip() or "taskkill issued.") + " | " + ", ".join(notes)}]
    if name == "replay_screenshot":
        try:
            hiss_get("/api/dump-scrapes")
        except Exception as e:
            return [{"type": "text", "text": "Could not reach Hiss to trigger a capture: %s" % e}]
        p = os.path.join(SCRAPES, "_table.bmp")
        deadline = time.time() + 4.0
        while time.time() < deadline:           # wait for a heartbeat to write it
            if os.path.isfile(p) and time.time() - os.path.getmtime(p) < 5:
                break
            time.sleep(0.4)
        if not os.path.isfile(p):
            return [{"type": "text", "text": "Capture triggered but no screenshot file appeared (is Hiss connected to a table?)."}]
        return [image_content(p)]
    if name == "autoplayer_toggle":
        on = "1" if args.get("on") else "0"
        return [{"type": "text", "text": hiss_get("/api/autoplayer?on=%s" % on)}]
    if name == "fckra_action":
        act = str(args.get("action", "")).lower()
        if act not in ("fold", "check", "call", "bet", "raise", "allin"):
            return [{"type": "text", "text": "action must be fold|check|call|bet|raise|allin"}]
        q = "do=%s" % act
        if act in ("bet", "raise") and args.get("amount") is not None:
            q += "&amount=%s" % args["amount"]
        return [{"type": "text", "text": hiss_get("/api/action?" + q)}]
    if name == "terminal_panes":
        return [{"type": "text", "text": hiss_get("/api/terminal-state")}]
    if name == "hand_report":
        # One hand, everything we know about it. Built because answering "why didn't the bot call
        # on hand X" otherwise means querying four tables by hand and knowing which one holds what:
        # the NN driver writes bot_nn_decision, the OHF autoplayer writes hiss_log_decisions, and a
        # hand driven by the NN therefore has an EMPTY hiss_log_decisions -- which reads as "the bot
        # never decided" if you only look there. Showing both side by side is the whole point.
        hand = str(args.get("handnumber") or "").strip()
        try:
            max_frames = int(args.get("max_frames") or 40)
        except Exception:
            max_frames = 40
        max_frames = max(1, min(max_frames, 500))
        if not hand:
            row = psql_query("SELECT handnumber FROM hiss_log_frames ORDER BY ts_ms DESC LIMIT 1").strip()
            hand = row.splitlines()[0].strip() if row.strip() else ""
            if not hand:
                return [{"type": "text", "text": "no hands found in hiss_log_frames"}]
        h = esc_sql(hand)
        out = ["=== HAND %s ===" % hand]

        def section(title, sql, empty="(none)"):
            try:
                txt = psql_query(sql, tuples_only=False).rstrip()
            except Exception as e:
                txt = "query failed: %s" % e
            out.append("\n--- %s ---" % title)
            out.append(txt if txt.strip() else empty)

        section("NN DRIVER decisions (bot_nn_decision)",
                "SELECT betround, action, amount, note, "
                "to_char(to_timestamp(ts_ms/1000) AT TIME ZONE 'UTC','HH24:MI:SS') AS t_utc "
                "FROM bot_nn_decision WHERE handnumber='%s' ORDER BY ts_ms" % h,
                "(none - the NN driver did not decide this hand)")
        section("OHF decisions (hiss_log_decisions)",
                "SELECT betround, action, amount, hero_cards, "
                "to_char(to_timestamp(ts_ms/1000) AT TIME ZONE 'UTC','HH24:MI:SS') AS t_utc "
                "FROM hiss_log_decisions WHERE handnumber='%s' ORDER BY ts_ms" % h,
                "(none - the OHF autoplayer did not decide this hand; normal when the NN is driving)")
        section("RESULT (hand_results)",
                "SELECT net, start_balance, end_balance FROM hand_results WHERE handnumber='%s'" % h)
        section("REVIEW (bot_hand_review)",
                "SELECT hero_cards, board, hero_net, classification, equity, action_taken, reason "
                "FROM bot_hand_review WHERE handnumber='%s' ORDER BY ts" % h)
        section("GAMESTATE / hand history (hiss_log_hands)",
                "SELECT complete, verified_players, identity, hh_text "
                "FROM hiss_log_hands WHERE handnumber='%s' ORDER BY ts_ms" % h)
        section("SCREENSHOTS (hiss_log_frames, newest last) - Read these png_path values",
                "SELECT betround, active_seat, hole, changed, png_path, "
                "to_char(to_timestamp(ts_ms/1000) AT TIME ZONE 'UTC','HH24:MI:SS') AS t_utc "
                "FROM hiss_log_frames WHERE handnumber='%s' ORDER BY ts_ms LIMIT %d" % (h, max_frames))
        try:
            n = psql_query("SELECT count(*) FROM hiss_log_frames WHERE handnumber='%s'" % h).strip()
            if n.isdigit() and int(n) > max_frames:
                out.append("\n(%s frames total; showing the first %d - raise max_frames for more)"
                           % (n, max_frames))
        except Exception:
            pass
        return [{"type": "text", "text": "\n".join(out)}]
    if name == "set_table_game_info":
        keys = ["sb", "bb", "ante", "chips_per_bb", "level", "players",
                "tourney_name", "tourney_id", "table_number", "gametype"]
        qs = "&".join("%s=%s" % (k, urllib.parse.quote(str(args[k])))
                      for k in keys if k in args and args[k] is not None)
        return [{"type": "text", "text": hiss_get("/api/table-game-info?" + qs)}]
    if name == "click_region":
        return [{"type": "text", "text": hiss_get("/api/click-region?name=%s" % urllib.parse.quote(str(args["name"])))}]
    if name == "set_region_value":
        qs = "name=%s&value=%s" % (urllib.parse.quote(str(args["name"])),
                                   urllib.parse.quote(str(args.get("value", ""))))
        return [{"type": "text", "text": hiss_get("/api/set-region-value?" + qs)}]
    if name == "set_table_game_info_2":
        qs = "&".join("%s=%s" % (k, urllib.parse.quote(str(args[k])))
                      for k in ("curr_hand", "prev_hand") if k in args and args[k] is not None)
        return [{"type": "text", "text": hiss_get("/api/table-game-info-2?" + qs)}]
    if name == "reload_ohf":
        return [{"type": "text", "text": hiss_get("/api/reload-ohf")}]
    if name == "ail_list":
        # port selects WHICH Hiss instance's switches to report -- per-port AILs (the brain / synapse)
        # are per instance, so without it you always see (and toggle) the default bot's.
        return [{"type": "text", "text": ail_get("/ail/list?port=%d" % int(args.get("port") or 27654))}]
    if name == "ail_toggle":
        on = "1" if args.get("on") else "0"
        return [{"type": "text", "text": ail_get("/ail/toggle?name=%s&port=%d&on=%s"
                 % (urllib.parse.quote(str(args["name"])), int(args.get("port") or 27654), on))}]
    if name == "ail_output":
        since = int(args.get("since") or 0)
        return [{"type": "text", "text": ail_get("/ail/output?since=%d" % since)}]
    if name == "lobby_fetch":
        port = int(args.get("port") or 27654)
        return [{"type": "text", "text": ail_get("/lobby-fetch?port=%d" % port)}]
    if name == "validate":
        return [{"type": "text", "text": hiss_get("/api/validate")}]
    if name == "log_settings":
        action = (args.get("action") or "list").lower()
        if action == "set":
            amap = {"advanced": "advanced_logging", "logging": "advanced_logging", "adv": "advanced_logging",
                    "advanced_logging": "advanced_logging", "report": "reporting", "reports": "reporting",
                    "reporting": "reporting", "replay": "replays", "replays": "replays"}
            col = amap.get(str(args.get("kind", "")).lower())
            if col is None:
                return [{"type": "text", "text": "unknown kind (use advanced_logging | reporting | replays)"}]
            ident = str(args.get("identity") or "*").replace("'", "")
            val = "true" if args.get("value") else "false"
            psql_query("INSERT INTO hiss_log_settings(identity,%s,updated_by) VALUES ('%s',%s,'mcp') "
                       "ON CONFLICT (identity) DO UPDATE SET %s=EXCLUDED.%s, updated_at=now(), updated_by='mcp'"
                       % (col, ident, val, col, col), tuples_only=False)
        out = psql_query("SELECT identity, advanced_logging, reporting, replays, "
                         "to_char(updated_at,'YYYY-MM-DD HH24:MI') AS updated_at, updated_by "
                         "FROM hiss_log_settings ORDER BY (identity='*') DESC, identity", tuples_only=False)
        return [{"type": "text", "text": out}]
    if name == "validate_ohf":
        out_parts = []
        lintdir = os.path.join(REPO, ".strategy_build")
        try:
            proc = subprocess.run([sys.executable, "build_and_lint.py"],
                                  cwd=lintdir, capture_output=True, text=True, timeout=120)
            verdict = "PASS" if proc.returncode == 0 else "FAIL"
            out_parts.append("OHF lint: %s\n%s" % (verdict, (proc.stdout or "") + (proc.stderr or "")))
        except Exception as e:
            out_parts.append("OHF lint could not run: %s" % e)
        # real-parser errors logged by Hiss at load time (if the C++ logger is present)
        perr = os.path.join(RELEASE, "logs", "ohf_parse_errors.log")
        if os.path.isfile(perr):
            try:
                tail = open(perr, encoding="utf-8", errors="replace").read()[-4000:]
                if tail.strip():
                    out_parts.append("--- Hiss real-parser errors (logs/ohf_parse_errors.log, tail) ---\n" + tail)
            except Exception as e:
                out_parts.append("could not read parse-error log: %s" % e)
        return [{"type": "text", "text": "\n\n".join(out_parts)}]
    if name == "game_state":
        return [{"type": "text", "text": hiss_get("/api/table-state")}]
    if name == "symbols":
        q = urllib.parse.quote(args["names"])
        return [{"type": "text", "text": hiss_get("/api/symbols?names=%s" % q)}]
    if name == "trigger_scrape_dump":
        hiss_get("/api/dump-scrapes")
        time.sleep(1.2)                          # let a heartbeat write the files
        files = list_files(SCRAPES, ["*"], rel_to=SCRAPES) if os.path.isdir(SCRAPES) else []
        return [{"type": "text", "text": "Dumped to logs/scrapes. Files:\n" + "\n".join(files)}]
    if name == "table_screenshot":
        p = os.path.join(SCRAPES, "_table.bmp")
        if not os.path.isfile(p):
            return [{"type": "text", "text": "No screenshot yet. Run trigger_scrape_dump first."}]
        return [image_content(p)]
    if name == "list_scrapes":
        if not os.path.isdir(SCRAPES):
            return [{"type": "text", "text": "logs/scrapes does not exist yet (run trigger_scrape_dump)."}]
        return [{"type": "text", "text": "\n".join(list_files(SCRAPES, ["*"], rel_to=SCRAPES))}]
    if name == "open_md_viewer":
        raw = str(args.get("path", "")).strip()
        if not raw:
            return [{"type": "text", "text": "No path given."}]
        path = raw if os.path.isabs(raw) else os.path.join(REPO, raw)
        if not os.path.isfile(path):
            return [{"type": "text", "text": "File not found: %s" % path}]
        viewer = r"C:\www\mdviewer\dist\MarkdownViewer.exe"
        if not os.path.isfile(viewer):
            return [{"type": "text", "text": "MarkdownViewer.exe not found at %s" % viewer}]
        try:
            subprocess.Popen([viewer, path])
            return [{"type": "text", "text": "Opened in MarkdownViewer: %s" % path}]
        except Exception as e:
            return [{"type": "text", "text": "Failed to open MarkdownViewer: %s" % e}]
    if name == "replay_hands":
        try:
            hands = replay_get("/api/hands")
        except Exception as e:
            return [{"type": "text", "text": "Replay server unreachable: %s" % e}]
        if not hands:
            return [{"type": "text", "text": "No hands captured on the replay server yet."}]
        lines = ["%-14s frames=%-4s span=%sms" %
                 (h.get("handnumber"), h.get("frames"),
                  int(h.get("end_ts", 0)) - int(h.get("start_ts", 0))) for h in hands]
        return [{"type": "text", "text": "%d hands on replay server:\n%s" % (len(hands), "\n".join(lines))}]
    if name == "replay_stream":
        hand = str(args.get("hand", "")).strip()
        if not hand:
            return [{"type": "text", "text": "hand required."}]
        ts = args.get("ts")
        try:
            if ts is None:
                ts = replay_latest_ts(hand)
            data = replay_get("/api/stream?hand=%s&ts=%d" % (urllib.request.quote(hand), int(ts)))
        except Exception as e:
            return [{"type": "text", "text": "Replay server error: %s" % e}]
        return [{"type": "text", "text": json.dumps(data, indent=2)[:60000]}]
    if name == "replay_frame":
        hand = str(args.get("hand", "")).strip()
        if not hand:
            return [{"type": "text", "text": "hand required."}]
        ts = args.get("ts")
        try:
            if ts is None:
                ts = replay_latest_ts(hand)
            png = replay_get("/api/img/%s/%d" % (urllib.request.quote(hand), int(ts)), raw=True)
        except Exception as e:
            return [{"type": "text", "text": "Frame fetch failed: %s" % e}]
        return [{"type": "image", "data": base64.b64encode(png).decode("ascii"), "mimeType": "image/png"},
                {"type": "text", "text": "hand %s @ ts %s" % (hand, ts)}]
    if name == "replayer_load":
        if os.name != "nt":
            return [{"type": "text", "text": "replayer_load is Windows-only."}]
        hand = str(args.get("hand", "")).strip()
        hands = args.get("hands") or []
        recent = args.get("recent")
        max_frames = int(args.get("max_frames") or 0)
        hand_list = []
        if hands:
            hand_list = [str(h).strip() for h in hands if str(h).strip()]
        elif hand:
            hand_list = [hand]
        elif recent:
            try:
                allh = replay_get("/api/hands")
            except Exception as e:
                return [{"type": "text", "text": "Could not list hands: %s" % e}]
            hand_list = [str(h.get("handnumber")) for h in allh[:int(recent)]]
            hand_list.reverse()   # play oldest -> newest
        if not hand_list:
            return [{"type": "text", "text": "Provide hand, hands, or recent."}]
        tag = hand_list[0] if len(hand_list) == 1 else ("session_%s_x%d" % (hand_list[0], len(hand_list)))
        dest = os.path.join(REPLAYER_DIR, str(tag))
        try:
            if os.path.isdir(dest):
                for old in os.listdir(dest):
                    if old.startswith("frame") and old.endswith(".bmp"):
                        os.remove(os.path.join(dest, old))
            idx = 0
            for h in hand_list:
                idx = _hand_frames_to_bmp(h, dest, idx, max_frames)
        except Exception as e:
            return [{"type": "text", "text": "Frame fetch/convert failed: %s" % e}]
        if idx == 0:
            return [{"type": "text", "text": "No frames fetched for: %s" % ", ".join(hand_list)}]
        if not os.path.isfile(REPLAYER_EXE):
            return [{"type": "text", "text": "Wrote %d frames to %s but replayer.exe not found at %s" % (idx, dest, REPLAYER_EXE)}]
        try:
            subprocess.Popen([REPLAYER_EXE, dest])
        except Exception as e:
            return [{"type": "text", "text": "Wrote %d frames to %s but could not launch replayer.exe: %s" % (idx, dest, e)}]
        return [{"type": "text", "text": "Loaded %d frames from %d hand(s) into replayer.exe and started playing.\nFolder: %s" % (idx, len(hand_list), dest)}]
    if name == "hud_calibrate_pending":
        status = hiss_get("/api/hud-calibrate-status").strip()
        out = [{"type": "text", "text": "HUD calibrate status: %s" % status}]
        if '"pending":true' in status:
            try:
                hiss_get("/api/dump-scrapes"); time.sleep(1.2)   # refresh _table.bmp
            except Exception:
                pass
            p = os.path.join(SCRAPES, "_table.bmp")
            if os.path.isfile(p):
                out.append({"type": "text", "text": "Locate each seated player's name-plate below and call post_hud_positions with one top-left pixel anchor per chair. PLACEMENT RULES: put each anchor just BELOW the player's name-plate so the box does NOT cover their balance/stack or their cards -- the on-table balance must stay fully visible. Each box renders the player name + balance on the top line and the stats as two pipe(|)-separated lines underneath, so leave a little vertical room below the name-plate."})
                out.append(image_content(p))
        return out
    if name == "post_hud_positions":
        positions = args.get("positions") or {}
        locked = args.get("locked")
        p = os.path.join(SCRAPES, "_table.bmp")
        W = H = 0
        try:
            from PIL import Image
            with Image.open(p) as im:
                W, H = im.size
        except Exception:
            pass
        if W <= 0 or H <= 0:
            return [{"type": "text", "text": "No table screenshot to size against; run hud_calibrate_pending or trigger_scrape_dump first."}]
        parts = []
        if locked is not None:
            parts.append('"locked":%d' % (1 if locked else 0))
        n = 0
        for k, v in positions.items():
            try:
                ch = int(k)
                fx = max(0.0, min(1.0, float(v["x"]) / W))
                fy = max(0.0, min(1.0, float(v["y"]) / H))
                parts.append('"c%d":{"x":%.4f,"y":%.4f}' % (ch, fx, fy))
                n += 1
            except Exception:
                continue
        obj = "{" + ",".join(parts) + "}"
        resp = hiss_get("/api/hud-positions?json=" + urllib.parse.quote(obj))
        return [{"type": "text", "text": "Posted %d HUD positions (capture %dx%d). Hiss: %s" % (n, W, H, resp.strip())}]
    if name == "read_scrape":
        region = os.path.basename(args["region"])
        img = os.path.join(SCRAPES, region + "_raw.bmp")
        txt = os.path.join(SCRAPES, region + ".txt")
        out = []
        if os.path.isfile(txt):
            out.append({"type": "text", "text": "OCR result for %s: %r" % (region, read_text(txt, 4000))})
        else:
            out.append({"type": "text", "text": "No OCR-result file for %s." % region})
        if os.path.isfile(img):
            out.append(image_content(img))
        return out
    if name == "list_logs":
        return [{"type": "text", "text": "\n".join(list_files(LOGS, ["*.log", "*.txt", "*.dmp"], rel_to=LOGS))}]
    if name == "read_log":
        p = safe_path(os.path.join("Release", "logs", os.path.basename(args["name"])))
        return [{"type": "text", "text": read_text(p, tail=args.get("tail_bytes", 60000))}]
    if name == "list_ohf":
        files = list_files(STRATEGY, ["*.ohf", "*.md"], rel_to=STRATEGY)
        files.append("(master) Release/ScarletBeast_PowerHoldem.ohf")
        return [{"type": "text", "text": "\n".join(files)}]
    if name == "read_ohf":
        n = os.path.basename(args["name"])
        cand = os.path.join(STRATEGY, n)
        if not os.path.isfile(cand):
            cand = os.path.join(RELEASE, n)
        return [{"type": "text", "text": read_text(cand)}]
    if name == "search_source":
        rg = "rg"
        try:
            proc = subprocess.run([rg, "-n", "--glob", args.get("glob", "*.cpp"), args["pattern"], REPO],
                                  capture_output=True, text=True, timeout=30)
            out = proc.stdout
        except FileNotFoundError:
            # fallback: python grep
            import re
            rx = re.compile(args["pattern"])
            out_lines = []
            for p in glob.glob(os.path.join(REPO, "Hiss", args.get("glob", "*.cpp"))):
                for i, line in enumerate(read_text(p, 2000000).splitlines(), 1):
                    if rx.search(line):
                        out_lines.append("%s:%d:%s" % (os.path.relpath(p, REPO), i, line))
            out = "\n".join(out_lines[:300])
        return [{"type": "text", "text": out[:60000] or "(no matches)"}]
    if name == "read_source":
        return [{"type": "text", "text": read_text(safe_path(args["path"]), max_bytes=400000)}]
    if name == "write_source":
        p = safe_path(args["path"])
        d = os.path.dirname(p)
        if d and not os.path.isdir(d):
            os.makedirs(d, exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(args["content"])
        return [{"type": "text", "text": "wrote %d bytes to %s" % (len(args["content"]), os.path.relpath(p, REPO))}]
    if name == "edit_source":
        p = safe_path(args["path"])
        s = read_text(p, max_bytes=8000000)
        old, new = args["old"], args["new"]
        cnt = s.count(old)
        rel = os.path.relpath(p, REPO)
        if cnt == 0:
            return [{"type": "text", "text": "ERROR: old_string not found in %s" % rel}]
        if cnt > 1 and not args.get("replace_all"):
            return [{"type": "text", "text": "ERROR: old_string appears %d times in %s -- make it unique or set replace_all" % (cnt, rel)}]
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(s.replace(old, new))
        return [{"type": "text", "text": "edited %s (%d replacement%s)" % (rel, cnt if args.get("replace_all") else 1, "s" if (args.get("replace_all") and cnt > 1) else "")}]
    if name == "list_release":
        sub = args.get("subpath", "") or ""
        root = safe_path(os.path.join("Release", sub))
        if os.path.isfile(root):
            return [{"type": "text", "text": "(file) " + sub}]
        pats = ["*"] if "*" not in sub else [os.path.basename(sub)]
        return [{"type": "text", "text": "\n".join(list_files(root, pats, rel_to=RELEASE))[:60000]}]
    if name == "list_tesseract_models":
        models = list_files(REPO, ["**/*.traineddata", "**/*.checkpoint"], rel_to=REPO)
        try:
            settings = psql_query("SELECT key||' = '||value::text FROM settings WHERE key ILIKE 'autoocr%';")
        except Exception as e:
            settings = "(settings query failed: %s)" % e
        return [{"type": "text", "text": "Trained models:\n" + "\n".join(models) + "\n\nAutoOcr settings:\n" + settings}]
    if name == "dump_tablemap":
        flt = args.get("name", "")
        where = ("WHERE name ILIKE '%%%s%%'" % flt.replace("'", "''")) if flt else ""
        try:
            out = psql_query("SELECT to_jsonb(t)::text FROM tablemaps t %s;" % where)
        except Exception as e:
            return [{"type": "text", "text": "tablemap dump failed: %s" % e}]
        dump_path = os.path.join(LOGS, "tablemap_dump.json")
        try:
            os.makedirs(LOGS, exist_ok=True)
            open(dump_path, "w", encoding="utf-8").write(out)
        except Exception:
            pass
        return [{"type": "text", "text": "Wrote %s\n\n%s" % (dump_path, out[:200000])}]
    if name == "read_settings":
        key = args.get("key", "")
        where = ("WHERE key ILIKE '%%%s%%'" % key.replace("'", "''")) if key else ""
        try:
            out = psql_query("SELECT key||E'\\t'||value::text FROM settings t %s ORDER BY key;" % where)
        except Exception as e:
            return [{"type": "text", "text": "settings query failed: %s" % e}]
        return [{"type": "text", "text": out[:200000] or "(no rows)"}]
    if name == "pg_query":
        sql = args["sql"]
        if not args.get("allow_write") and not is_readonly_sql(sql):
            return [{"type": "text", "text": "Refused: non-read-only SQL. Pass allow_write=true to run data-modifying statements."}]
        try:
            out = psql_query(sql, database=args.get("database"), tuples_only=False)
        except Exception as e:
            return [{"type": "text", "text": "query failed: %s" % e}]
        return [{"type": "text", "text": out[:200000] or "(no rows)"}]
    if name == "pg_databases":
        out = psql_query("SELECT datname FROM pg_database WHERE datistemplate=false ORDER BY datname;")
        return [{"type": "text", "text": out or "(none)"}]
    if name == "pg_tables":
        out = psql_query("SELECT schemaname||'.'||tablename FROM pg_tables "
                         "WHERE schemaname NOT IN ('pg_catalog','information_schema') "
                         "ORDER BY 1;", database=args.get("database"))
        return [{"type": "text", "text": out or "(none)"}]
    if name == "pg_describe":
        t = args["table"].split(".")[-1].replace("'", "''")
        out = psql_query("SELECT column_name||E'\\t'||data_type FROM information_schema.columns "
                         "WHERE table_name='%s' ORDER BY ordinal_position;" % t,
                         database=args.get("database"))
        return [{"type": "text", "text": out or "(no such table / no columns)"}]
    if name == "learner_decisions":
        where = "WHERE reviewed=false " if args.get("only_unreviewed", True) else ""
        lim = int(args.get("limit", 20))
        out = psql_query(
            "SELECT id, ts, handnumber, betround, hero_cards, board, pot, amount_to_call, "
            "action, amount, reasoning, self_liked, self_feedback FROM learner_decisions "
            "%s ORDER BY id DESC LIMIT %d;" % (where, lim), tuples_only=False)
        return [{"type": "text", "text": out or "(no decisions logged yet)"}]
    if name == "learner_ask":
        q = args["question"]
        summary = args.get("summary")
        if not summary:
            # Auto-derive a succinct summary: first sentence, capped ~120 chars.
            s = q.strip().replace("\n", " ")
            cut = s.find(". ")
            summary = (s[:cut + 1] if 0 < cut < 120 else s[:120])
        did = args.get("decision_id")
        did_sql = str(int(did)) if did is not None else "NULL"
        psql_query("INSERT INTO learner_questions (question, summary, decision_id) "
                   "VALUES ('%s', '%s', %s);" % (esc_sql(q), esc_sql(summary), did_sql))
        return [{"type": "text", "text": "Question posted to learner.exe (summary: %s)" % summary}]
    if name == "speak":
        text = str(args.get("text", "")).strip()
        if not text:
            return [{"type": "text", "text": "nothing to speak"}]
        exe = os.path.join(RELEASE, "lilith.exe")
        try:
            if os.path.isfile(exe):
                # fire-and-forget: lilith.exe mutes/synths/plays/unmutes on its own
                subprocess.Popen([exe, text], cwd=RELEASE, close_fds=True,
                                 creationflags=0x08000000 if os.name == "nt" else 0)  # CREATE_NO_WINDOW
            else:
                # fallback: speak in-process via the shared module
                import lilith_tts
                lilith_tts.speak(text)
        except Exception as e:
            return [{"type": "text", "text": "speak failed: %s" % e}]
        return [{"type": "text", "text": "Lilith: %s" % text[:200]}]
    if name == "card_scrapes":
        # fresh capture so the raw images on disk match what's shown
        try:
            hiss_get("/api/dump-scrapes")
        except Exception:
            pass
        time.sleep(1.2)
        try:
            st = json.loads(hiss_get("/api/table-state"))
        except Exception as e:
            return [{"type": "text", "text": "Hiss unreachable: %s" % e}]
        lines = []
        board = st.get("commonCards", [])
        lines.append("COMMUNITY (c0cardface0..4): " +
                     " ".join("[%d]%s" % (i, (board[i] if i < len(board) and board[i] else "--")) for i in range(5)))
        for pl in st.get("players", []):
            ch = pl.get("chair")
            cs = pl.get("cards", [])
            shown = " ".join("[%d]%s" % (i, (cs[i] if i < len(cs) and cs[i] else "--")) for i in range(2))
            seated = "" if pl.get("seated") else " (empty seat)"
            lines.append("p%s %-14s %s%s" % (ch, (pl.get("name") or "")[:14], shown, seated))
        lines.append("\nUse card_image(position, slot) to see a region's raw image; "
                     "log_card_correction(...) to record a fix. (BACK = face-down, -- = none/missing)")
        return [{"type": "text", "text": "\n".join(lines)}]
    if name == "card_image":
        region = _card_region(args["position"], args["slot"])
        try:
            hiss_get("/api/dump-scrapes"); time.sleep(1.2)
        except Exception:
            pass
        img = os.path.join(SCRAPES, region + "_raw.bmp")
        txt = os.path.join(SCRAPES, region + ".txt")
        out = [{"type": "text", "text": "region %s -> recognised: %r" %
                (region, read_text(txt, 200) if os.path.isfile(txt) else "(no result file)")}]
        if os.path.isfile(img):
            out.append(image_content(img))
        else:
            out.append({"type": "text", "text": "(no raw image for %s -- region may be undefined in the tablemap)" % region})
        return out
    if name == "log_card_correction":
        region = _card_region(args["position"], args["slot"])
        hn = ""
        try:
            hn = json.loads(hiss_get("/api/table-state")).get("handnumber", "")
        except Exception:
            pass
        psql_query(
            "INSERT INTO card_corrections (position, slot, region, scraped, correct, handnumber, note) "
            "VALUES ('%s', %d, '%s', '%s', '%s', '%s', '%s');" % (
                esc_sql(str(args["position"])), int(args["slot"]), esc_sql(region),
                esc_sql(args.get("scraped", "")), esc_sql(args["correct"]),
                esc_sql(hn), esc_sql(args.get("note", ""))))
        return [{"type": "text", "text": "Logged card correction: %s scraped=%r -> correct=%r" %
                 (region, args.get("scraped", ""), args["correct"])}]
    if name == "learner_answers":
        cond = "WHERE answered=true " + ("AND answered_ts > now() - interval '1 day' " if args.get("only_recent", True) else "")
        out = psql_query("SELECT id, question, answer, answered_ts FROM learner_questions "
                         "%s ORDER BY answered_ts DESC LIMIT 50;" % cond, tuples_only=False)
        return [{"type": "text", "text": out or "(no answers yet)"}]
    raise ValueError("unknown tool: %s" % name)

# ===========================================================================
#  JSON-RPC / MCP plumbing  (newline-delimited messages over stdio)
# ===========================================================================
def send(msg):
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()

def main():
    log("starting; repo=%s" % REPO)
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception as e:
            log("bad json:", e); continue
        mid = req.get("id")
        method = req.get("method")
        params = req.get("params") or {}
        try:
            if method == "initialize":
                send({"jsonrpc": "2.0", "id": mid, "result": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "hiss-bot", "version": "1.0.0"}}})
            elif method == "notifications/initialized":
                pass  # notification, no reply
            elif method == "ping":
                send({"jsonrpc": "2.0", "id": mid, "result": {}})
            elif method == "tools/list":
                send({"jsonrpc": "2.0", "id": mid, "result": {"tools": TOOLS}})
            elif method == "tools/call":
                tname = params.get("name")
                targs = params.get("arguments") or {}
                try:
                    content = call_tool(tname, targs)
                    send({"jsonrpc": "2.0", "id": mid, "result": {"content": content, "isError": False}})
                except Exception as e:
                    send({"jsonrpc": "2.0", "id": mid, "result": {
                        "content": [{"type": "text", "text": "ERROR: %s" % e}], "isError": True}})
            elif mid is not None:
                send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found: %s" % method}})
        except Exception as e:
            log("handler error:", e)
            if mid is not None:
                send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32603, "message": str(e)}})

if __name__ == "__main__":
    main()
