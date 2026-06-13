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
def psql_query(sql):
    env = dict(os.environ); env["PGPASSWORD"] = PGPASS
    proc = subprocess.run([PSQL, "-U", PGUSER, "-d", PGDB, "-t", "-A", "-c", sql],
                          capture_output=True, text=True, env=env, timeout=30)
    if proc.returncode != 0:
        raise RuntimeError("psql failed: %s" % (proc.stderr.strip() or proc.stdout.strip()))
    return proc.stdout

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
