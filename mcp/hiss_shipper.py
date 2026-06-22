#!/usr/bin/env python3
"""hiss_shipper.py - ship the local postgres hiss_log_* outbox to hiss.scarletbeast.com.

The local postgres `hiss_log_*` tables are a durable outbox: Hiss writes rows there
every heartbeat; this daemon drains them to the server and marks them shipped.

  * Frames are DEDUPED on ship: only rows with changed=true upload their PNG blob
    (POST /api/frames, multipart). Unchanged frames ship metadata only (png_path=NULL)
    so the replay UI holds the last real PNG across the gap.
  * Structured rows (decisions/hands/scrapes/symbols/debug) go up via POST /api/ingest.
  * Rows flip shipped=true only after the server acknowledges -> no data loss, safe retry.

Ingest goes to the LAN IP with a Host header (name-based vhost) so PNG blobs never
leave the LAN; the public Cloudflare hostname is only for the replay UI.

Run:  python hiss_shipper.py            # loop, default 15s interval
      python hiss_shipper.py --once     # single drain pass
      python hiss_shipper.py --interval 30
"""
import os, sys, json, time, subprocess, argparse
import requests

PSQL    = os.environ.get("HISS_PSQL", r"C:\Program Files\PostgreSQL\12\bin\psql.exe")
PGUSER  = os.environ.get("PGUSER", "postgres")
PGDB    = os.environ.get("PGDATABASE", "hiss")
PGPASS  = os.environ.get("PGPASSWORD", "dbpass")
RELEASE = os.environ.get("HISS_RELEASE", r"C:\www\openholdembot_old\Release")

SERVER  = os.environ.get("HISS_SHIP_URL", "http://192.168.1.39")
HOSTHDR = os.environ.get("HISS_SHIP_HOST", "hiss.scarletbeast.com")
TOKEN   = os.environ.get("INGEST_TOKEN", "hisslog_b6ad55973b1cdd42bf238b8a")
BATCH   = int(os.environ.get("HISS_SHIP_BATCH", "300"))

HDRS = {"Authorization": "Bearer %s" % TOKEN, "Host": HOSTHDR}

# Each psql call is a separate process; without this, every invocation flashes a console window
# on Windows when the daemon itself has no visible console. Suppress it.
CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0

_singleton_handle = None
def ensure_single_instance():
    """Prevent double-shippers. The AIL's 'restart shipper if dead' check can otherwise spawn a 2nd
    instance that double-ships rows (wasted work + duplicate frames/decisions on the replay server).
    Hold a global named mutex; a 2nd instance sees ERROR_ALREADY_EXISTS and exits cleanly."""
    global _singleton_handle
    if os.name != "nt":
        return
    import ctypes
    _singleton_handle = ctypes.windll.kernel32.CreateMutexW(None, False, "Global\\hiss_shipper_singleton")
    if ctypes.windll.kernel32.GetLastError() == 183:   # ERROR_ALREADY_EXISTS
        log("another hiss_shipper instance already running -> exiting (single-instance guard).")
        sys.exit(0)

# table -> columns shipped in the /api/ingest payload (id is fetched too, for marking)
STREAMS = {
    "hands":     ["ts_ms", "handnumber", "complete", "hh_text"],
    "decisions": ["ts_ms", "handnumber", "betround", "hero_cards", "action", "amount",
                  "f_fold", "f_call", "f_check", "f_raise", "f_allin", "f_betsize", "trace"],
    "scrapes":   ["ts_ms", "handnumber", "betround", "region_name", "ocr_text", "is_crop"],
    "symbols":   ["ts_ms", "handnumber", "betround", "name", "value"],
    "debug":     ["ts_ms", "handnumber", "category", "line"],
}
FRAME_COLS = ["ts_ms", "handnumber", "betround", "png_path", "sha256", "changed", "active_seat", "hole"]

# voice_feedback is its own table (written directly by voice_feedback.py, not a hiss_log_* outbox),
# so it ships via a dedicated drain. Columns mirror the server's `voice` ingest stream / `voice` table.
VOICE_COLS = ["ts_ms", "handnumber", "betround", "board", "hero_cards", "pot", "mode",
              "transcript", "category", "sentiment"]


def log(*a):
    print("[shipper]", *a, file=sys.stderr, flush=True)


def psql(sql):
    env = dict(os.environ); env["PGPASSWORD"] = PGPASS
    cmd = [PSQL, "-U", PGUSER, "-d", PGDB, "-t", "-A", "-c", sql]
    p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=120,
                       creationflags=CREATE_NO_WINDOW)
    if p.returncode != 0:
        raise RuntimeError("psql: " + (p.stderr.strip() or p.stdout.strip()))
    return p.stdout


def fetch(table, cols):
    sel = ",".join("'%s',%s" % (c, c) for c in cols)
    sql = ("SELECT coalesce(json_agg(row_to_json(s)),'[]') FROM "
           "(SELECT id, json_build_object(%s) AS rec FROM hiss_log_%s "
           "WHERE shipped=false ORDER BY id LIMIT %d) s;" % (sel, table, BATCH))
    out = psql(sql).strip()
    return json.loads(out) if out else []


def mark_shipped(table, ids):
    if ids:
        psql("UPDATE hiss_log_%s SET shipped=true WHERE id IN (%s);"
             % (table, ",".join(str(int(i)) for i in ids)))


def ship_structured():
    """Drain decisions/hands/scrapes/symbols/debug via one bulk /api/ingest call."""
    payload, idmap = {}, {}
    for table, cols in STREAMS.items():
        rows = fetch(table, cols)
        if not rows:
            continue
        payload[table] = [r["rec"] for r in rows]
        idmap[table] = [r["id"] for r in rows]
    if not payload:
        return 0
    resp = requests.post(SERVER + "/api/ingest", headers=HDRS, json=payload, timeout=60)
    resp.raise_for_status()
    body = resp.json()
    if not body.get("ok"):
        raise RuntimeError("ingest rejected: %s" % body)
    n = 0
    for table, ids in idmap.items():
        mark_shipped(table, ids)
        n += len(ids)
    log("structured shipped:", body.get("counts"))
    return n


def ship_frames():
    """Upload changed PNGs (multipart), then bulk-insert frame metadata, then mark shipped."""
    rows = fetch("frames", FRAME_COLS)
    if not rows:
        return 0
    meta, ids, uploaded, skipped = [], [], 0, 0
    for r in rows:
        rec = r["rec"]; ids.append(r["id"])
        png = rec.get("png_path")
        server_path = None
        if rec.get("changed") and png:
            full = png if os.path.isabs(png) else os.path.join(RELEASE, png)
            if os.path.exists(full):
                with open(full, "rb") as fh:
                    files = {"file": (os.path.basename(full), fh, "image/png")}
                    data = {"hand": str(rec.get("handnumber") or "unknown"),
                            "ts": str(rec.get("ts_ms") or 0)}
                    fr = requests.post(SERVER + "/api/frames", headers=HDRS,
                                       files=files, data=data, timeout=120)
                fr.raise_for_status()
                server_path = fr.json().get("path")
                uploaded += 1
            else:
                skipped += 1
        meta.append({"ts_ms": rec.get("ts_ms"), "handnumber": rec.get("handnumber"),
                     "betround": rec.get("betround"), "png_path": server_path,
                     "sha256": rec.get("sha256"), "changed": rec.get("changed")})
    resp = requests.post(SERVER + "/api/ingest", headers=HDRS,
                         json={"frames": meta}, timeout=60)
    resp.raise_for_status()
    if not resp.json().get("ok"):
        raise RuntimeError("frame metadata ingest rejected")
    mark_shipped("frames", ids)
    log("frames shipped: %d (PNG uploaded=%d, missing=%d)" % (len(ids), uploaded, skipped))
    return len(ids)


def ship_voice():
    """Drain spoken-feedback markers (voice_feedback) -> server `voice` stream.
    These are replay-aligned indicators (ts_ms + handnumber) the replay UI renders on the
    timeline and the AIL reads to improve the bot. shipped=true only after the server acks."""
    sel = ",".join("'%s',%s" % (c, c) for c in VOICE_COLS)
    sql = ("SELECT coalesce(json_agg(row_to_json(s)),'[]') FROM "
           "(SELECT id, json_build_object(%s) AS rec FROM voice_feedback "
           "WHERE shipped=false ORDER BY id LIMIT %d) s;" % (sel, BATCH))
    out = psql(sql).strip()
    rows = json.loads(out) if out else []
    if not rows:
        return 0
    resp = requests.post(SERVER + "/api/ingest", headers=HDRS,
                         json={"voice": [r["rec"] for r in rows]}, timeout=60)
    resp.raise_for_status()
    if not resp.json().get("ok"):
        raise RuntimeError("voice ingest rejected")
    ids = [r["id"] for r in rows]
    psql("UPDATE voice_feedback SET shipped=true WHERE id IN (%s);"
         % ",".join(str(int(i)) for i in ids))
    log("voice shipped: %d" % len(ids))
    return len(ids)


def drain():
    total = 0
    total += ship_structured()
    total += ship_frames()
    total += ship_voice()
    return total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--once", action="store_true", help="single pass then exit")
    ap.add_argument("--interval", type=int, default=15, help="seconds between passes")
    args = ap.parse_args()
    ensure_single_instance()
    log("target=%s (Host: %s) batch=%d" % (SERVER, HOSTHDR, BATCH))
    backoff = args.interval
    while True:
        try:
            n = drain()
            backoff = args.interval
            if n:
                log("pass shipped %d rows" % n)
        except Exception as e:
            log("ERROR:", e)
            backoff = min(backoff * 2, 300)
        if args.once:
            return
        time.sleep(backoff)


if __name__ == "__main__":
    main()
