#!/usr/bin/env python3
"""BEAST advanced replay server -- serves the ALL-FRAMES local store.

On BEAST (the Windows bot box, 192.168.1.137) Hiss's CLogWriter saves a PNG for EVERY heartbeat frame
into hiss_log_frames (png_path is set for ALL frames; the `changed` flag is only metadata, and
hiss_shipper.py uploads just the CHANGED ones to swiftsnake -> swiftsnake stays changed-only). This
server exposes the same API the swiftsnake replay UI uses, but reads the LOCAL all-frames store, so
every frame -- including brief showdown reveals that get deduped away on swiftsnake -- is navigable.

Endpoints (same contract as the swiftsnake replay UI in beast_replay.html):
  GET /                       -> the replay UI
  GET /api/hands              -> [{handnumber, hero_cards, start_ts, end_ts, frames}] newest first
  GET /api/frames/<hand>      -> [{ts_ms, betround, changed}] ascending (ALL frames)
  GET /api/img/<hand>/<ts>    -> PNG bytes for that exact frame (any frame, not just changed)
  GET /api/stream?hand=&ts=   -> {hand:{hh_text,complete}, symbols:[{name,value}],
                                  scrapes:[{region_name,ocr_text}], decision:{...}, gametype:{label}}
  GET /api/voice/<hand>       -> [{ts_ms, transcript, category, ...}]
  GET /api/gametype?hand=&ts= -> {label: "NLH"|"PLO"|"PLO8"}

Run:  python mcp/beast_replay_server.py [--port 8090]
"""
import sys, os, json, http.server, socketserver
from urllib.parse import urlparse, parse_qs, unquote
import psycopg2

PORT = 8090
for i, a in enumerate(sys.argv):
    if a == "--port" and i + 1 < len(sys.argv):
        PORT = int(sys.argv[i + 1])

DSN = "host=%s port=%s dbname=%s user=%s password=%s" % (
    os.environ.get("PGHOST", "127.0.0.1"), os.environ.get("PGPORT", "5432"),
    os.environ.get("PGDATABASE", "hiss"), os.environ.get("PGUSER", "postgres"),
    os.environ.get("PGPASSWORD", "dbpass"))
HERE = os.path.dirname(os.path.abspath(__file__))
HTML = os.path.join(HERE, "beast_replay.html")


def q(sql, args=()):
    c = psycopg2.connect(DSN)
    try:
        cur = c.cursor()
        cur.execute(sql, args)
        cols = [d[0] for d in cur.description]
        return [dict(zip(cols, r)) for r in cur.fetchall()]
    finally:
        c.close()


def _truthy(v):
    try:
        return float(v) != 0
    except Exception:
        return str(v).strip().lower() in ("true", "1", "yes")


def gametype(hand, ts):
    rows = q("SELECT DISTINCT ON (name) name, value FROM hiss_log_symbols "
             "WHERE handnumber=%s AND ts_ms<=%s AND name IN ('isomaha','isplo8') "
             "ORDER BY name, ts_ms DESC", (hand, ts))
    s = {r["name"]: r["value"] for r in rows}
    if _truthy(s.get("isplo8")):
        return {"label": "PLO8"}
    if _truthy(s.get("isomaha")):
        return {"label": "PLO"}
    return {"label": "NLH"}


def hands():
    rows = q("SELECT handnumber, min(ts_ms) start_ts, max(ts_ms) end_ts, count(*) frames "
             "FROM hiss_log_frames WHERE handnumber ~ '^[0-9]{9,}$' GROUP BY handnumber "
             "ORDER BY max(ts_ms) DESC LIMIT 100")
    hc = {r["handnumber"]: r["hero_cards"] for r in q(
        "SELECT DISTINCT ON (handnumber) handnumber, hero_cards FROM hiss_log_decisions "
        "WHERE handnumber<>'' ORDER BY handnumber, ts_ms DESC")}
    for r in rows:
        r["hero_cards"] = hc.get(r["handnumber"]) or ""
    return rows


def frames(hand):
    return q("SELECT ts_ms, betround, changed FROM hiss_log_frames "
             "WHERE handnumber=%s ORDER BY ts_ms ASC", (hand,))


def stream(hand, ts):
    syms = q("SELECT DISTINCT ON (name) name, value FROM hiss_log_symbols "
             "WHERE handnumber=%s AND ts_ms<=%s ORDER BY name, ts_ms DESC", (hand, ts))
    scr = q("SELECT DISTINCT ON (region_name) region_name, ocr_text FROM hiss_log_scrapes "
            "WHERE handnumber=%s AND ts_ms<=%s ORDER BY region_name, ts_ms DESC", (hand, ts))
    dec = q("SELECT hero_cards, action, amount, f_fold, f_call, f_check, f_raise, f_allin, "
            "f_betsize, trace FROM hiss_log_decisions WHERE handnumber=%s AND ts_ms<=%s "
            "ORDER BY ts_ms DESC LIMIT 1", (hand, ts))
    hh = q("SELECT hh_text, complete FROM hiss_log_hands WHERE handnumber=%s "
           "ORDER BY ts_ms DESC LIMIT 1", (hand,))
    return {"hand": (hh[0] if hh else None), "symbols": syms, "scrapes": scr,
            "decision": (dec[0] if dec else None), "gametype": gametype(hand, ts)}


def voice(hand):
    return q("SELECT ts_ms, transcript, category, sentiment, betround, board, hero_cards, pot "
             "FROM voice_feedback WHERE handnumber=%s ORDER BY ts_ms ASC", (hand,))


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _json(self, obj, code=200):
        b = json.dumps(obj, default=str).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def _bytes(self, b, ctype, code=200):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        try:
            u = urlparse(self.path)
            p = u.path
            qs = parse_qs(u.query)
            if p in ("/", "/replay.html", "/index.html"):
                with open(HTML, "rb") as f:
                    return self._bytes(f.read(), "text/html; charset=utf-8")
            if p == "/api/hands":
                return self._json(hands())
            if p.startswith("/api/frames/"):
                return self._json(frames(unquote(p[len("/api/frames/"):])))
            if p.startswith("/api/voice/"):
                return self._json(voice(unquote(p[len("/api/voice/"):])))
            if p.startswith("/api/img/"):
                rest = p[len("/api/img/"):].split("/")
                hand, ts = unquote(rest[0]), int(rest[1])
                r = q("SELECT png_path FROM hiss_log_frames WHERE handnumber=%s AND ts_ms=%s LIMIT 1", (hand, ts))
                path = r[0]["png_path"] if r else None
                if path and os.path.isfile(path):
                    with open(path, "rb") as f:
                        return self._bytes(f.read(), "image/png")
                return self._bytes(b"", "image/png", 404)
            if p == "/api/stream":
                return self._json(stream(qs.get("hand", [""])[0], int(qs.get("ts", ["0"])[0])))
            if p == "/api/gametype":
                return self._json(gametype(qs.get("hand", [""])[0], int(qs.get("ts", ["0"])[0])))
            return self._json({"error": "not found", "path": p}, 404)
        except Exception as e:
            return self._json({"error": str(e)}, 500)


if __name__ == "__main__":
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    srv = socketserver.ThreadingTCPServer(("0.0.0.0", PORT), H)
    print("[beast-replay] ALL-FRAMES replay server on http://0.0.0.0:%d  (UI: /  API: /api/*)" % PORT, flush=True)
    srv.serve_forever()
