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

import sys, os, json, glob, base64, subprocess, urllib.request, time

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

# --- postgres via psql ------------------------------------------------------
def psql_query(sql, database=None, tuples_only=True):
    env = dict(os.environ); env["PGPASSWORD"] = PGPASS
    flags = ["-A"] + (["-t"] if tuples_only else [])   # unaligned; headers unless tuples_only
    cmd = [PSQL, "-U", PGUSER, "-d", database or PGDB] + flags + ["-c", sql]
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=60)
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
    {"name": "learner_ask", "description": "Post a question to the human in learner.exe's 'Questions from Claude' box (e.g. when their play differs from the OHF, or you need clarification). Optionally link it to a decision id.",
     "inputSchema": {"type": "object", "properties": {
         "question": {"type": "string"},
         "decision_id": {"type": "integer"}},
      "required": ["question"]}},
    {"name": "learner_answers", "description": "Read the human's answers to questions you posted in learner.exe.",
     "inputSchema": {"type": "object", "properties": {"only_recent": {"type": "boolean", "default": True}}}},
]

def call_tool(name, args):
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
        return [{"type": "text", "text": "Launched Hiss.exe (cwd=Release). Give it a few seconds to bind its terminal port, then call hiss_status."}]
    if name == "stop_hiss":
        if os.name != "nt":
            return [{"type": "text", "text": "stop_hiss is Windows-only."}]
        proc = subprocess.run(["taskkill", "/IM", "Hiss.exe", "/F"],
                              capture_output=True, text=True)
        out = (proc.stdout or "") + (proc.stderr or "")
        return [{"type": "text", "text": out.strip() or "taskkill issued."}]
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
    if name == "game_state":
        return [{"type": "text", "text": hiss_get("/api/table-state")}]
    if name == "symbols":
        import urllib.parse
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
        q = args["question"].replace("'", "''")
        did = args.get("decision_id")
        did_sql = str(int(did)) if did is not None else "NULL"
        psql_query("INSERT INTO learner_questions (question, decision_id) VALUES ('%s', %s);"
                   % (q, did_sql))
        return [{"type": "text", "text": "Question posted to learner.exe."}]
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
