#!/usr/bin/env python3
"""observe_learn.py -- OBSERVATIONAL LEARNING for Hiss.

While the bot is OBSERVING a table (not necessarily playing), turn the live signals it can already
see -- scrape / symbols / game-state + the HUD opponent stats (mined from observed hand-histories by
hud_aggregator) + recent per-hand results -- into ACTIONABLE inferences that improve the whole system:
the opponent model, OHF exploit lines, strategy notes, and NN training signal.

It does NOT re-derive opponent stats (hud_aggregator already does that from hand-histories); it
SYNTHESISES them + the live table into exploits the AIL can act on, and stores them in `observations`.

  python observe_learn.py            # synthesize + store observations from the current table
  python observe_learn.py --digest   # print recent stored observations (for the AIL)
  python observe_learn.py --watch [--every 60]   # keep observing on an interval

The AIL reads these each cycle (AIL_PLAYBOOK step 1c) and folds them into the bot.
"""
import sys, os, json, time, urllib.request

BOT = "http://127.0.0.1:27654"
DSN = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def _argval(flag, default=None):
    if flag in sys.argv:
        i = sys.argv.index(flag)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return default


def _get(path):
    try:
        with urllib.request.urlopen(BOT + path, timeout=5) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception:
        return {}


def now_ms():
    return int(time.time() * 1000)


def run_hud_aggregator():
    """hud_aggregator.py is run-once (mines observed hand-histories -> hud_player_stats). Drive it each
    watch cycle so opponent stats keep improving from what we OBSERVE. CREATE_NO_WINDOW = no console flash."""
    try:
        import subprocess
        here = os.path.dirname(os.path.abspath(__file__))
        subprocess.run([sys.executable, os.path.join(here, "hud_aggregator.py")],
                       cwd=here, timeout=90, capture_output=True,
                       creationflags=(0x08000000 if os.name == "nt" else 0))
    except Exception as e:
        print("[observe] hud_aggregator error:", e, flush=True)


def hud_map(p):
    """Parse a player's live game-state HUD array -> {abbr: float|None}. VP/PF/FTS are %s, AF a ratio."""
    d = {}
    for h in (p.get("hud") or []):
        try:
            d[h.get("abbr")] = float(h.get("value"))
        except (TypeError, ValueError):
            d[h.get("abbr")] = None
    return d


def villain_exploit(vpip, pfr, af, fts, samples):
    """Reads from the LIVE per-seat HUD (vpip/pfr/fts fractions, af ratio). Returns (profile, exploit)
    or (None, None). Uses real seated opponents only -- no cross-table phantoms."""
    if samples is None or samples < 12 or vpip is None:
        return None, None
    v = vpip * 100
    p = (pfr * 100) if pfr is not None else None
    prof, ex = None, None
    if vpip >= 0.40 and af is not None and af <= 1.2:
        prof, ex = "calling station (VPIP %.0f, AF %.1f)" % (v, af), "value-bet bigger & thinner; do NOT bluff him"
    elif vpip >= 0.40 and af is not None and af >= 3.0:
        prof, ex = "loose maniac (VPIP %.0f, AF %.1f)" % (v, af), "trap big hands, widen call-downs, 3bet for value not as a bluff"
    elif vpip <= 0.15:
        prof, ex = "nit (VPIP %.0f)" % v, "steal his blinds relentlessly; fold to his big aggression"
    elif p is not None and (vpip - pfr) >= 0.18:
        prof, ex = "passive/loose (VPIP %.0f / PFR %.0f)" % (v, p), "value-bet relentlessly, rarely bluff"
    if ex is None and fts is not None and fts >= 0.60:
        prof, ex = "folds-to-steal %.0f%% (VPIP %.0f)" % (fts * 100, v), "raise/steal his blinds relentlessly"
    return prof, ex


def gather(conn):
    ts = _get("/api/table-state")
    obs = []
    bb = 0.0
    try:
        bb = float((ts.get("limits") or {}).get("bblind") or 0)
    except Exception:
        bb = 0.0
    uc = ts.get("userchair", -1)

    # --- opponent profiles from the LIVE per-seat HUD (real seats only; never the hero; no phantoms) ---
    vpips = []
    for p in (ts.get("players") or []):
        nm = (p.get("name") or "").strip()
        if not p.get("seated") or not nm or p.get("chair") == uc:
            continue                                   # skip empty seats + the hero himself
        hud = hud_map(p)
        samples = p.get("samples")
        vpip = (hud.get("VP") / 100.0) if hud.get("VP") is not None else None
        pfr = (hud.get("PF") / 100.0) if hud.get("PF") is not None else None
        af = hud.get("AF")
        fts = (hud.get("FTS") / 100.0) if hud.get("FTS") is not None else None
        if vpip is not None and samples and samples >= 12:
            vpips.append(vpip)
            profile, exploit = villain_exploit(vpip, pfr, af, fts, samples)
            if exploit:
                obs.append(("villain", nm, profile + " [n=%d]" % samples, exploit))
        # short-stack read from the live scrape (independent of history)
        try:
            depth_bb = float(p.get("balance") or 0) / bb if bb > 0 else None
        except Exception:
            depth_bb = None
        if depth_bb is not None and 0 < depth_bb <= 20:
            obs.append(("villain", nm, "short stack %.0fbb" % depth_bb,
                        "expect jam/fold from him; flat less, isolate wider"))

    # --- table profile (population tendency of the seen opponents) ---
    if len(vpips) >= 2:
        avg = sum(vpips) / len(vpips)
        if avg >= 0.38:
            obs.append(("table", "table", "LOOSE table (avg VPIP %.0f, n=%d)" % (avg * 100, len(vpips)),
                        "tighten opens, value-bet thinner, bluff less; let them pay off"))
        elif avg <= 0.20:
            obs.append(("table", "table", "TIGHT table (avg VPIP %.0f, n=%d)" % (avg * 100, len(vpips)),
                        "steal more, c-bet more, attack the blinds & weak-tight folds"))

    # --- recent result trend (from the hand_results tracker) ---
    try:
        cur = conn.cursor()
        cur.execute("SELECT to_regclass('hand_results')")
        if cur.fetchone()[0] is not None:
            cur.execute("SELECT coalesce(sum(net),0), count(*) FROM hand_results "
                        "WHERE ts_ms > %s", (now_ms() - 3600000,))
            net, n = cur.fetchone()
            if n and n >= 5:
                obs.append(("session", "hero", "last-hour net %+.2f over %d hands" % (float(net), n),
                            "down trend -> tighten/avoid marginal spots" if net < 0
                            else "up trend -> keep doing what works, don't get fancy"))
    except Exception:
        conn.rollback()

    return ts, obs


def store(conn, obs):
    cur = conn.cursor()
    cur.execute("CREATE TABLE IF NOT EXISTS observations (id bigserial primary key, ts_ms bigint, "
                "scope text, subject text, inference text, suggestion text, applied boolean default false)")
    n = 0
    for scope, subject, inference, suggestion in obs:
        # de-dupe: skip if the same subject+inference was stored in the last 30 min
        cur.execute("SELECT 1 FROM observations WHERE subject=%s AND inference=%s AND ts_ms > %s LIMIT 1",
                    (subject, inference, now_ms() - 1800000))
        if cur.fetchone():
            continue
        cur.execute("INSERT INTO observations (ts_ms, scope, subject, inference, suggestion) "
                    "VALUES (%s,%s,%s,%s,%s)", (now_ms(), scope, subject, inference, suggestion))
        n += 1
    cur.execute("DELETE FROM observations WHERE id < (SELECT max(id)-3000 FROM observations)")
    conn.commit()
    return n


def digest(conn, limit=40):
    cur = conn.cursor()
    cur.execute("SELECT to_regclass('observations')")
    if cur.fetchone()[0] is None:
        return "No observations yet (run observe_learn while a table is up)."
    cur.execute("SELECT ts_ms, scope, subject, inference, suggestion, applied FROM observations "
                "ORDER BY id DESC LIMIT %s", (limit,))
    rows = cur.fetchall()
    if not rows:
        return "No observations stored."
    out = ["%d recent observation(s) [newest first]:\n" % len(rows)]
    for ts, scope, subject, inference, suggestion, applied in rows:
        flag = " (APPLIED)" if applied else ""
        out.append("  [%s] %-14s %s%s" % (scope, subject, inference, flag))
        if suggestion:
            out.append("        -> %s" % suggestion)
    return "\n".join(out)


def main():
    global BOT
    BOT = (_argval("--bot-url") or BOT).rstrip("/")
    import psycopg2
    conn = psycopg2.connect(DSN)

    if "--digest" in sys.argv:
        print(digest(conn))
        return

    def one():
        ts, obs = gather(conn)
        obsv = ts.get("observer")
        n = store(conn, obs)
        print("[observe] observer=%s seated-villains=%d -> %d new observation(s)"
              % (obsv, sum(1 for p in (ts.get("players") or []) if p.get("seated") and p.get("name")), n),
              flush=True)
        for scope, subject, inference, suggestion in obs:
            print("   [%s] %s: %s%s" % (scope, subject, inference, (" -> " + suggestion) if suggestion else ""),
                  flush=True)

    if "--watch" in sys.argv:
        every = float(_argval("--every") or 60)
        print("[observe] learning every %.0fs -> observations table (drives hud_aggregator too)" % every, flush=True)
        while True:
            run_hud_aggregator()    # observed hands -> updated opponent stats, THEN synthesize
            one()
            time.sleep(every)
    else:
        one()


if __name__ == "__main__":
    main()
