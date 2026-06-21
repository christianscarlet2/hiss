#!/usr/bin/env python3
"""introspect_aggregator.py -- per-opponent INTROSPECTION model (gametype-separated).

Companion to hud_aggregator.py. Where the HUD gives base-rate frequencies, introspection
remembers *how a player actually plays the streets* over a rolling window of their last 100
hands, PER GAME TYPE (nlhe / plo / plo8 never bleed into each other), and derives concrete
EXPLOIT flags + a "rhythm" (likelihood to keep firing) + a timing tell (does he have it when
he bets fast).

Pipeline (same incremental watermark trick as hud_aggregator, because hiss_log_hands is a
small shipping outbox that gets pruned -- we must capture each hand as it flows through):
  1. read new hiss_log_hands (id > watermark)
  2. parse each -> per-player per-hand features (rhythm/aggression/fold-to-pressure/showdown)
  3. upsert into opponent_hand  (durable rolling source, keyed player+gametype+handnumber)
  4. trim opponent_hand to the last 100 hands per (player, gametype)
  5. recompute opponent_profile from that window, blending hud_player_stats + opponent_timing
     (latency rows emitted by the Hiss engine) -> rhythm, fold-to-pressure, fast-bet tell,
     a profile label, and the EXPLOIT flag set the OHF / NN / synapse consume.

  python3 introspect_aggregator.py            # one pass
  python3 introspect_aggregator.py --watch    # loop
  python3 introspect_aggregator.py --reset     # wipe + re-key watermark
  python3 introspect_aggregator.py --dump NAME # print a player's profile across gametypes
"""
import os, re, sys, time, json
import psycopg2
from hud_aggregator import gametype_from_hh, SEAT_RE, BUTTON_RE, ACT_RE

DSN = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")

WINDOW = 100          # rolling hands per (player, gametype)
AMT_RE = re.compile(r"\b(?:bets|raises|calls)\b.*?([\d]+(?:\.[\d]+)?)")
SHOWS_RE = re.compile(r"^(.*\S) shows \[[^\]]+\]\s*\(([^)]*)\)")
COLLECT_RE = re.compile(r"^(.*\S) collected")
# showdown made-hand strength keywords (two pair or better == "had it")
STRONG_KW = ("two pair", "three of a kind", "a straight", "a flush", "a full house",
             "four of a kind", "straight flush", "trips", "set")


def _marker(lines, m):
    for i, l in enumerate(lines):
        if m in l:
            return i
    return None


def _street_actions(lines, a, b):
    """Ordered (actor, action) for the action lines in lines[a:b]."""
    out = []
    for ln in lines[a:b]:
        mm = ACT_RE.match(ln)
        if mm:
            out.append((mm.group(1).strip(), mm.group(2)))
    return out


def parse_hand_features(hh):
    """Return (gametype, handnumber, {name: feat}) for one hand.

    feat = per-player, per-hand rhythm/aggression signals:
      saw_flop, was_aggr            -- did they bet/raise at all
      agg_opps, agg_cont            -- continuation: aggressor on street S and kept firing on S+1
      facing_d, facing_fold         -- fold-to-pressure (folded when there was a bet to call)
      aggr, passive                 -- bet+raise count vs call count (aggression factor numerator/denom)
      sd_seen, sd_strong            -- reached showdown / showed two-pair+ or won at showdown
    """
    lines = [l.rstrip() for l in (hh or "").splitlines()]
    gametype = gametype_from_hh(hh)
    handnumber = ""
    if lines:
        hm = re.search(r"Hand #(\S+)", lines[0])
        if hm:
            handnumber = hm.group(1)

    seats = {}
    for ln in lines:
        if ln.startswith("***"):
            break
        m = SEAT_RE.match(ln)
        if m:
            seats[int(m.group(1))] = m.group(2).strip()
    if not seats:
        return gametype, handnumber, {}
    names = set(seats.values())

    i_hole = _marker(lines, "*** HOLE CARDS ***")
    i_flop = _marker(lines, "*** FLOP ***")
    i_turn = _marker(lines, "*** TURN ***")
    i_river = _marker(lines, "*** RIVER ***")
    i_show = _marker(lines, "*** SHOW DOWN ***")
    i_summ = _marker(lines, "*** SUMMARY ***")
    end = i_summ if i_summ is not None else len(lines)

    # street boundaries: (street_index, start, stop)
    bounds = []
    if i_hole is not None:
        bounds.append((1, i_hole + 1, i_flop if i_flop is not None else end))
    if i_flop is not None:
        bounds.append((2, i_flop + 1, i_turn if i_turn is not None else (i_show if i_show is not None else end)))
    if i_turn is not None:
        bounds.append((3, i_turn + 1, i_river if i_river is not None else (i_show if i_show is not None else end)))
    if i_river is not None:
        bounds.append((4, i_river + 1, i_show if i_show is not None else end))

    feat = {n: dict(saw_flop=0, was_aggr=0, agg_opps=0, agg_cont=0, facing_d=0, facing_fold=0,
                    aggr=0, passive=0, sd_seen=0, sd_strong=0) for n in names}
    saw_flop = i_flop is not None
    if saw_flop:
        for n in names:
            feat[n]["saw_flop"] = 1

    # per-player: streets where they were the (first) aggressor, and their first action per street
    aggr_streets = {n: set() for n in names}
    first_action = {n: {} for n in names}      # street -> first action token
    for street, a, b in bounds:
        acts = _street_actions(lines, a, b)
        bet_outstanding = False
        for actor, act in acts:
            if actor not in feat:
                continue
            if street not in first_action[actor]:
                first_action[actor][street] = act
            # fold-to-pressure: there is a live bet/raise in front of them
            if bet_outstanding:
                feat[actor]["facing_d"] += 1
                if act == "folds":
                    feat[actor]["facing_fold"] += 1
            if act in ("bets", "raises"):
                feat[actor]["aggr"] += 1
                feat[actor]["was_aggr"] = 1
                aggr_streets[actor].add(street)
                bet_outstanding = True
            elif act == "calls":
                feat[actor]["passive"] += 1

    # rhythm: aggressor on street S, still acted on S+1 -> opportunity; first action S+1 aggressive -> continued
    acted_streets = {n: set(first_action[n].keys()) for n in names}
    for n in names:
        for s in aggr_streets[n]:
            ns = s + 1
            if ns in acted_streets[n]:
                feat[n]["agg_opps"] += 1
                if first_action[n].get(ns) in ("bets", "raises"):
                    feat[n]["agg_cont"] += 1

    # showdown strength
    if i_show is not None:
        for ln in lines[i_show + 1:end]:
            sm = SHOWS_RE.match(ln)
            if sm and sm.group(1).strip() in feat:
                feat[sm.group(1).strip()]["sd_seen"] = 1
                desc = sm.group(2).lower()
                if any(k in desc for k in STRONG_KW):
                    feat[sm.group(1).strip()]["sd_strong"] = 1
            cm = COLLECT_RE.match(ln)
            if cm and cm.group(1).strip() in feat:
                feat[cm.group(1).strip()]["sd_seen"] = 1
                feat[cm.group(1).strip()]["sd_strong"] = 1   # won at showdown -> "had it"
    return gametype, handnumber, feat


# ---------------------------------------------------------------- schema
def ensure_schema(conn):
    cur = conn.cursor()
    cur.execute("""
    CREATE TABLE IF NOT EXISTS opponent_hand (
      player TEXT NOT NULL, gametype TEXT NOT NULL, handnumber TEXT NOT NULL,
      ts_ms BIGINT, hid BIGINT,
      saw_flop INT, was_aggr INT, agg_opps INT, agg_cont INT,
      facing_d INT, facing_fold INT, aggr INT, passive INT,
      sd_seen INT, sd_strong INT,
      PRIMARY KEY (player, gametype, handnumber));
    CREATE INDEX IF NOT EXISTS idx_opphand_pg ON opponent_hand(player, gametype, hid DESC);

    CREATE TABLE IF NOT EXISTS opponent_timing (
      id BIGSERIAL PRIMARY KEY, ts_ms BIGINT, handnumber TEXT, player TEXT, gametype TEXT,
      street INT, action TEXT, latency_ms INT, shipped BOOL DEFAULT false);
    CREATE INDEX IF NOT EXISTS idx_opptiming_pg ON opponent_timing(player, gametype, handnumber);

    CREATE TABLE IF NOT EXISTS opponent_profile (
      player TEXT NOT NULL, gametype TEXT NOT NULL,
      window_hands INT DEFAULT 0,
      cont_freq REAL DEFAULT 0,        -- agg_cont/agg_opps : likelihood to KEEP firing (rhythm)
      aggr_index REAL DEFAULT 0,       -- aggr/(aggr+passive)
      fold_to_pressure REAL DEFAULT 0, -- facing_fold/facing_d : over-fold tendency
      sd_strong_rate REAL DEFAULT 0,   -- of showdowns, how often they actually had it
      fastbet_tell REAL DEFAULT -1,    -- P(strong | bet fast) from opponent_timing; -1 = unknown
      fast_n INT DEFAULT 0,
      profile TEXT DEFAULT 'unknown',
      exploits JSONB DEFAULT '{}'::jsonb,
      updated_at TIMESTAMP DEFAULT now(),
      PRIMARY KEY (player, gametype));

    CREATE TABLE IF NOT EXISTS introspect_state (id INT PRIMARY KEY, last_hand_id BIGINT);
    INSERT INTO introspect_state (id, last_hand_id) VALUES (1, 0) ON CONFLICT (id) DO NOTHING;
    """)
    conn.commit()


# ---------------------------------------------------------------- profile compute
def _hud(cur, player, gametype):
    """Pull the gametype-matched HUD base rates (or None)."""
    cur.execute("""SELECT hands, vpip_n,vpip_d, pfr_n,pfr_d, threeb_n,threeb_d, f3b_n,f3b_d,
                          cbet_n,cbet_d, ftc_n,ftc_d, wtsd_n,wtsd_d, aggr_actions, call_actions
                   FROM hud_player_stats WHERE player=%s AND gametype=%s""", (player, gametype))
    r = cur.fetchone()
    if not r:
        return None
    def pct(n, d):
        return (100.0 * n / d) if d else -1.0
    return dict(hands=r[0], vpip=pct(r[1], r[2]), pfr=pct(r[3], r[4]), threeb=pct(r[5], r[6]),
                f3b=pct(r[7], r[8]), cbet=pct(r[9], r[10]), ftc=pct(r[11], r[12]), wtsd=pct(r[13], r[14]),
                af=(r[15] / r[16] if r[16] else (r[15] if r[15] else -1.0)))


def _timing_tell(cur, player, gametype):
    """P(showed strong | bet fast) over the window. Joins opponent_timing fast bets/raises
    to opponent_hand.sd_strong on the same hand. Returns (rate or -1, fast_n)."""
    cur.execute("""
      SELECT count(*) FILTER (WHERE t.latency_ms <= 1500 AND t.action IN ('bets','raises')),
             count(*) FILTER (WHERE t.latency_ms <= 1500 AND t.action IN ('bets','raises') AND h.sd_strong=1)
      FROM opponent_timing t
      JOIN opponent_hand h ON h.player=t.player AND h.gametype=t.gametype AND h.handnumber=t.handnumber
      WHERE t.player=%s AND t.gametype=%s AND h.sd_seen=1""", (player, gametype))
    fast_d, fast_strong = cur.fetchone()
    if not fast_d:
        return -1.0, 0
    return float(fast_strong) / fast_d, int(fast_d)


def _classify(hud, cont_freq, aggr_index, fold_to_pressure):
    """Profile label from HUD base rates refined by introspection rhythm."""
    if hud and hud["hands"] >= 20:
        v, p, af, wtsd = hud["vpip"], hud["pfr"], hud["af"], hud["wtsd"]
        if v > 45 and p > 30 and af > 3.5: return "maniac"
        if v > 40 and p < 8: return "fish"
        if (wtsd > 34) or (af >= 0 and af < 1.0 and v > 40): return "station"
        if v > 28 and p > 20 and af > 2.5: return "lag"
        if 18 <= v <= 26 and (v - p) < 6 and af >= 2: return "tag"
        if v < 17: return "nit"
    # introspection-only fallback (no/low HUD sample)
    if aggr_index > 0.7 and cont_freq > 0.6: return "lag"
    if aggr_index < 0.25: return "station"
    if fold_to_pressure > 0.6: return "nit"
    return "unknown"


def _exploits(hud, cont_freq, aggr_index, fold_to_pressure, sd_strong_rate, fastbet_tell, label):
    """Concrete, OHF-consumable exploit flags. Each is a 0/1 the strategy deep-wires."""
    e = {}
    ftc = hud["ftc"] if hud else -1
    f3b = hud["f3b"] if hud else -1
    # over-folds -> attack relentlessly
    e["overfold"] = 1 if (fold_to_pressure > 0.62 or (ftc >= 0 and ftc > 60)) else 0
    e["folds_to_3bet"] = 1 if (f3b >= 0 and f3b > 65) else 0
    # gives up the lead -> float / stab when he checks
    e["gives_up"] = 1 if (cont_freq >= 0 and cont_freq < 0.35 and aggr_index > 0.3) else 0
    # keeps firing / sticky aggressor -> let him bluff, bluff-catch wider, don't bluff-raise
    e["keeps_firing"] = 1 if (cont_freq > 0.62) else 0
    # never folds -> value only, never bluff
    e["never_folds"] = 1 if (label in ("station", "fish") or fold_to_pressure < 0.15) else 0
    # honest -> his bets are real (passive / high showdown strength)
    e["honest"] = 1 if (aggr_index < 0.3 or sd_strong_rate > 0.7) else 0
    # timing tell: fast bet is WEAK (low strong-rate when fast) -> attack fast bets; or STRONG -> respect
    e["fast_is_weak"] = 1 if (fastbet_tell >= 0 and fastbet_tell < 0.35) else 0
    e["fast_is_strong"] = 1 if (fastbet_tell >= 0 and fastbet_tell > 0.7) else 0
    return e


def recompute_profile(cur, player, gametype):
    cur.execute("""SELECT saw_flop, was_aggr, agg_opps, agg_cont, facing_d, facing_fold,
                          aggr, passive, sd_seen, sd_strong
                   FROM opponent_hand WHERE player=%s AND gametype=%s
                   ORDER BY hid DESC LIMIT %s""", (player, gametype, WINDOW))
    w = cur.fetchall()
    if not w:
        return
    agg_opps = sum(r[2] for r in w); agg_cont = sum(r[3] for r in w)
    facing_d = sum(r[4] for r in w); facing_fold = sum(r[5] for r in w)
    aggr = sum(r[6] for r in w); passive = sum(r[7] for r in w)
    sd_seen = sum(r[8] for r in w); sd_strong = sum(r[9] for r in w)
    cont_freq = (agg_cont / agg_opps) if agg_opps else -1.0
    aggr_index = (aggr / (aggr + passive)) if (aggr + passive) else -1.0
    fold_to_pressure = (facing_fold / facing_d) if facing_d else -1.0
    sd_strong_rate = (sd_strong / sd_seen) if sd_seen else -1.0
    hud = _hud(cur, player, gametype)
    fastbet_tell, fast_n = _timing_tell(cur, player, gametype)
    label = _classify(hud, cont_freq, aggr_index, fold_to_pressure)
    exploits = _exploits(hud, cont_freq, aggr_index, fold_to_pressure, sd_strong_rate, fastbet_tell, label)
    cur.execute("""
      INSERT INTO opponent_profile (player, gametype, window_hands, cont_freq, aggr_index,
        fold_to_pressure, sd_strong_rate, fastbet_tell, fast_n, profile, exploits, updated_at)
      VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,now())
      ON CONFLICT (player, gametype) DO UPDATE SET window_hands=EXCLUDED.window_hands,
        cont_freq=EXCLUDED.cont_freq, aggr_index=EXCLUDED.aggr_index,
        fold_to_pressure=EXCLUDED.fold_to_pressure, sd_strong_rate=EXCLUDED.sd_strong_rate,
        fastbet_tell=EXCLUDED.fastbet_tell, fast_n=EXCLUDED.fast_n, profile=EXCLUDED.profile,
        exploits=EXCLUDED.exploits, updated_at=now()""",
      (player, gametype, len(w), cont_freq, aggr_index, fold_to_pressure, sd_strong_rate,
       fastbet_tell, fast_n, label, json.dumps(exploits)))


def trim_window(cur, player, gametype):
    cur.execute("""DELETE FROM opponent_hand WHERE player=%s AND gametype=%s AND handnumber NOT IN
                   (SELECT handnumber FROM opponent_hand WHERE player=%s AND gametype=%s
                    ORDER BY hid DESC LIMIT %s)""",
                (player, gametype, player, gametype, WINDOW))


# ---------------------------------------------------------------- run loop
def run_once(conn):
    cur = conn.cursor()
    cur.execute("SELECT last_hand_id FROM introspect_state WHERE id=1")
    wm = cur.fetchone()[0]
    cur.execute("SELECT id, ts_ms, hh_text, verified_players FROM hiss_log_hands WHERE id > %s ORDER BY id", (wm,))
    rows = cur.fetchall()
    touched = set()
    n_hands = 0
    for hid, ts_ms, hh, verified in rows:
        try:
            gt, handnumber, feat = parse_hand_features(hh or "")
        except Exception as e:
            print("parse error hand id %s: %s" % (hid, e)); feat = {}; gt = "nlhe"; handnumber = ""
        if not handnumber:
            wm = hid; continue
        vset = set(verified) if verified else None
        for name, f in feat.items():
            if vset is not None and name not in vset:
                continue
            cur.execute("""INSERT INTO opponent_hand (player,gametype,handnumber,ts_ms,hid,
                saw_flop,was_aggr,agg_opps,agg_cont,facing_d,facing_fold,aggr,passive,sd_seen,sd_strong)
                VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
                ON CONFLICT (player,gametype,handnumber) DO UPDATE SET
                  saw_flop=EXCLUDED.saw_flop, was_aggr=EXCLUDED.was_aggr, agg_opps=EXCLUDED.agg_opps,
                  agg_cont=EXCLUDED.agg_cont, facing_d=EXCLUDED.facing_d, facing_fold=EXCLUDED.facing_fold,
                  aggr=EXCLUDED.aggr, passive=EXCLUDED.passive, sd_seen=EXCLUDED.sd_seen,
                  sd_strong=EXCLUDED.sd_strong, hid=EXCLUDED.hid, ts_ms=EXCLUDED.ts_ms""",
                (name, gt, handnumber, ts_ms, hid, f["saw_flop"], f["was_aggr"], f["agg_opps"],
                 f["agg_cont"], f["facing_d"], f["facing_fold"], f["aggr"], f["passive"],
                 f["sd_seen"], f["sd_strong"]))
            touched.add((name, gt))
        wm = hid; n_hands += 1
    for player, gt in touched:
        trim_window(cur, player, gt)
        recompute_profile(cur, player, gt)
    cur.execute("UPDATE introspect_state SET last_hand_id=%s WHERE id=1", (wm,))
    conn.commit()
    return n_hands, len(touched)


def main():
    conn = psycopg2.connect(DSN)
    ensure_schema(conn)
    if "--reset" in sys.argv:
        cur = conn.cursor()
        cur.execute("TRUNCATE opponent_hand; TRUNCATE opponent_profile; "
                    "UPDATE introspect_state SET last_hand_id=0 WHERE id=1")
        conn.commit(); print("reset: introspection cleared, watermark=0")
    if "--dump" in sys.argv:
        name = sys.argv[sys.argv.index("--dump") + 1]
        cur = conn.cursor()
        cur.execute("""SELECT gametype, window_hands, cont_freq, aggr_index, fold_to_pressure,
                       sd_strong_rate, fastbet_tell, profile, exploits FROM opponent_profile
                       WHERE player=%s ORDER BY gametype""", (name,))
        for r in cur.fetchall():
            print(json.dumps(dict(gametype=r[0], window=r[1], cont_freq=round(r[2], 3),
                  aggr_index=round(r[3], 3), fold_to_pressure=round(r[4], 3),
                  sd_strong_rate=round(r[5], 3), fastbet_tell=round(r[6], 3),
                  profile=r[7], exploits=r[8]), indent=0))
        return
    if "--watch" in sys.argv:
        while True:
            n, t = run_once(conn)
            if n: print("processed %d hands, %d (player,gametype) profiles" % (n, t))
            time.sleep(5)
    else:
        n, t = run_once(conn)
        print("processed %d hands; updated %d profiles" % (n, t))


if __name__ == "__main__":
    main()
