#!/usr/bin/env python3
"""hud_aggregator.py -- parse logged hands into per-player VERIFIED HUD stats.

Polls hiss_log_hands (id > watermark), parses each PokerStars-style hh_text, attributes each
verified player's preflop/postflop actions, and upserts numerator/denominator counters into
hud_player_stats. Idempotent via hud_aggregator_state.last_hand_id.

verified_players (JSONB array on the hand) gates which seats count: present -> only those names;
absent (legacy hands) -> count all seats (best-effort). The Hiss HUD reads hud_player_stats.

  python3 hud_aggregator.py            # process new hands once (backfills on first run)
  python3 hud_aggregator.py --watch    # loop every few seconds
  python3 hud_aggregator.py --reset    # recompute from scratch (clears stats + watermark)
"""
import os, re, sys, time, json
import psycopg2

DSN = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")

SEAT_RE   = re.compile(r"^Seat (\d+): (.+) \(([\d.]+)\)\s*$")
BUTTON_RE = re.compile(r"Seat #(\d+) is the button")
SB_RE     = re.compile(r"^(.*\S) posts the small blind")
BB_RE     = re.compile(r"^(.*\S) posts the big blind")
ACT_RE    = re.compile(r"^(.*\S) (folds|checks|calls|bets|raises)\b")

STAT_COLS = ["vpip", "pfr", "threeb", "fourb", "fiveb", "f3b", "f4b", "cbet", "ftc", "steal", "fts", "wtsd"]


def gametype_from_hh(hh):
    """Classify a hand-history into nlhe | plo | plo8 from its first line
    ('... - Holdem (No Limit) - ...' / '... - Omaha (Pot Limit) - ...' /
    '... - Omaha Hi/Lo - ...'). Per-gametype stats keep a player's PLO read
    from bleeding into their NLH read."""
    head = (hh or "")[:200].lower()
    if "omaha" in head:
        if any(t in head for t in ("hi/lo", "hi-lo", "h/l", "8 or b", "hilo", "8orb")):
            return "plo8"
        return "plo"
    return "nlhe"


def ensure_schema(conn):
    """Idempotently make hud_player_stats keyed by (player, gametype)."""
    cur = conn.cursor()
    cur.execute("ALTER TABLE hud_player_stats ADD COLUMN IF NOT EXISTS gametype TEXT NOT NULL DEFAULT 'nlhe'")
    cur.execute("""SELECT pg_get_constraintdef(oid) FROM pg_constraint
                   WHERE conname='hud_player_stats_pkey' AND conrelid='hud_player_stats'::regclass""")
    row = cur.fetchone()
    if row and "gametype" not in row[0]:
        cur.execute("ALTER TABLE hud_player_stats DROP CONSTRAINT hud_player_stats_pkey")
        cur.execute("ALTER TABLE hud_player_stats ADD PRIMARY KEY (player, gametype)")
    conn.commit()


def _marker(lines, m):
    for i, l in enumerate(lines):
        if m in l:
            return i
    return None


def parse_hand(hh):
    """Return {name: {col_n:int, col_d:int, 'hands':1, 'aggr':int, 'call':int}} for one hand."""
    lines = [l.rstrip() for l in hh.splitlines()]
    seats, order, button = {}, [], None
    sb_name = bb_name = None
    for ln in lines:
        # The seat list, button line and blind posts all precede "*** HOLE CARDS ***".
        # Stop here so the SUMMARY's "Seat N: name (button) collected (x)" lines -- which
        # also end in "(<number>)" -- are never mis-parsed as seats (that created phantom
        # players like "christianbeast (button) collected").
        if ln.startswith("***"):
            break
        m = SEAT_RE.match(ln)
        if m:
            sn = int(m.group(1)); seats[sn] = m.group(2).strip(); order.append(sn)
        b = BUTTON_RE.search(ln)
        if b: button = int(b.group(1))
        s = SB_RE.match(ln)
        if s and sb_name is None: sb_name = s.group(1).strip()
        bb = BB_RE.match(ln)
        if bb and bb_name is None: bb_name = bb.group(1).strip()
    if not seats:
        return {}
    names = set(seats.values())
    res = {n: {"hands": 1, "aggr": 0, "call": 0} for n in names}
    for n in names:
        for c in STAT_COLS:
            res[n][c + "_n"] = 0
            res[n][c + "_d"] = 0

    i_hole = _marker(lines, "*** HOLE CARDS ***")
    i_flop = _marker(lines, "*** FLOP ***")
    i_summ = _marker(lines, "*** SUMMARY ***")
    i_show = _marker(lines, "*** SHOW DOWN ***")
    end_pre = i_flop if i_flop is not None else (i_summ if i_summ is not None else len(lines))
    saw_flop = (i_flop is not None)

    # ---- ring positions (for steal/fts): order seated players starting after the button ----
    pos = {}   # name -> position label among {SB,BB,...,CO,BTN}
    if button is not None and button in seats:
        ring = sorted(seats.keys())
        bi = ring.index(button)
        ordered = ring[bi + 1:] + ring[:bi + 1]   # SB, BB, ..., BTN(last)
        labels = [None] * len(ordered)
        if len(ordered) >= 1: labels[0] = "SB"
        if len(ordered) >= 2: labels[1] = "BB"
        if len(ordered) >= 1: labels[-1] = "BTN"
        if len(ordered) >= 4: labels[-2] = "CO"
        for s, lab in zip(ordered, labels):
            if lab: pos[seats[s]] = lab

    # ---- preflop walk ----
    raises = 0           # number of raises standing so far (open=1 -> 3bet=2 -> 4bet=3 -> 5bet=4)
    opener = None        # 1st raiser (the open)
    threebettor = None   # 2nd raiser (the 3bet)
    fourbettor = None    # 3rd raiser (the 4bet)
    pf_aggressor = None  # last raiser preflop
    folded = set()
    first_voluntary = None   # first player to call/raise preflop (for steal "folded to")
    steal_resolved = False
    for ln in lines[(i_hole + 1) if i_hole is not None else 0:end_pre]:
        a = ACT_RE.match(ln)
        if not a:
            continue
        actor, act = a.group(1).strip(), a.group(2)
        if actor not in res:
            continue
        # Raise-tier opportunity = the player acts facing exactly N raises and is not the
        # one who made the standing top raise. raising into it = a (N+1)bet; otherwise the
        # spot still counts in the denominator. 3bet faces 1 raise, 4bet faces 2, 5bet faces 3.
        if raises == 1 and actor != opener:
            res[actor]["threeb_d"] += 1
            if act == "raises":
                res[actor]["threeb_n"] += 1
        elif raises == 2 and actor != threebettor:
            res[actor]["fourb_d"] += 1
            if act == "raises":
                res[actor]["fourb_n"] += 1
        elif raises == 3 and actor != fourbettor:
            res[actor]["fiveb_d"] += 1
            if act == "raises":
                res[actor]["fiveb_n"] += 1
        # fold-to-3bet: the opener faces a 3bet (exactly 2 raises stand on their turn).
        if actor == opener and raises == 2:
            res[actor]["f3b_d"] += 1
            if act == "folds":
                res[actor]["f3b_n"] += 1
        # fold-to-4bet: the 3bettor faces a 4bet (exactly 3 raises stand on their turn).
        if actor == threebettor and raises == 3:
            res[actor]["f4b_d"] += 1
            if act == "folds":
                res[actor]["f4b_n"] += 1
        # steal: first voluntary entrant, folded to, in CO/BTN/SB
        if first_voluntary is None and act in ("calls", "raises"):
            first_voluntary = actor
            if pos.get(actor) in ("CO", "BTN", "SB"):
                res[actor]["steal_d"] += 1
                if act == "raises":
                    res[actor]["steal_n"] += 1
                    steal_resolved = "raise"
        # VPIP / PFR (denominator = dealt, counted once below)
        if act in ("calls", "raises"):
            res[actor]["vpip_n"] = 1
        if act == "raises":
            res[actor]["pfr_n"] = 1
            raises += 1
            if   raises == 1: opener = actor
            elif raises == 2: threebettor = actor
            elif raises == 3: fourbettor = actor
            pf_aggressor = actor
        if act in ("bets", "raises"):
            res[actor]["aggr"] += 1
        if act == "calls":
            res[actor]["call"] += 1
        if act == "folds":
            folded.add(actor)
    # VPIP/PFR denom = dealt (1 per hand)
    for n in names:
        res[n]["vpip_d"] = 1
        res[n]["pfr_d"] = 1
    # fold-to-steal: blinds facing a steal raise
    if steal_resolved == "raise":
        for blind in (sb_name, bb_name):
            if blind and blind in res and blind != first_voluntary:
                res[blind]["fts_d"] += 1
                # did the blind fold preflop?
                if blind in folded:
                    res[blind]["fts_n"] += 1

    # ---- flop continuation-bet (pf aggressor) + fold-to-cbet ----
    if saw_flop and pf_aggressor in res:
        # flop action lines until next street/summary
        i_turn = _marker(lines, "*** TURN ***")
        end_flop = i_turn if i_turn is not None else (i_summ if i_summ is not None else len(lines))
        cbet_made = False
        res[pf_aggressor]["cbet_d"] += 1
        faced = []
        for ln in lines[i_flop + 1:end_flop]:
            a = ACT_RE.match(ln)
            if not a:
                continue
            actor, act = a.group(1).strip(), a.group(2)
            if actor == pf_aggressor and act == "bets" and not cbet_made:
                cbet_made = True
                res[pf_aggressor]["cbet_n"] += 1
            elif cbet_made and actor in res and actor != pf_aggressor:
                res[actor]["ftc_d"] += 1
                if act == "folds":
                    res[actor]["ftc_n"] += 1

    # ---- WTSD (denom = saw flop) ----
    if saw_flop:
        shown = set()
        if i_show is not None:
            for ln in lines[i_show + 1:(i_summ if i_summ is not None else len(lines))]:
                m = re.match(r"^(.*\S) (shows|mucks|collected)", ln)
                if m and m.group(1).strip() in res:
                    shown.add(m.group(1).strip())
        for n in names:
            if n not in folded:
                res[n]["wtsd_d"] += 1
                if n in shown:
                    res[n]["wtsd_n"] += 1
    return res


def upsert(cur, name, c):
    cols = ["hands"] + [s + "_n" for s in STAT_COLS] + [s + "_d" for s in STAT_COLS] + ["aggr_actions", "call_actions"]
    vals = [c.get("hands", 0)] + [c.get(s + "_n", 0) for s in STAT_COLS] + [c.get(s + "_d", 0) for s in STAT_COLS] + [c.get("aggr", 0), c.get("call", 0)]
    sets = ", ".join("%s = hud_player_stats.%s + EXCLUDED.%s" % (col, col, col) for col in cols)
    cur.execute(
        "INSERT INTO hud_player_stats (player, %s, updated_at) VALUES (%%s, %s, now()) "
        "ON CONFLICT (player) DO UPDATE SET %s, updated_at=now()"
        % (", ".join(cols), ", ".join(["%s"] * len(cols)), sets),
        [name] + vals)


def run_once(conn):
    cur = conn.cursor()
    cur.execute("SELECT last_hand_id FROM hud_aggregator_state WHERE id=1")
    wm = cur.fetchone()[0]
    cur.execute("SELECT id, hh_text, verified_players FROM hiss_log_hands WHERE id > %s ORDER BY id", (wm,))
    rows = cur.fetchall()
    n_hands = 0
    for hid, hh, verified in rows:
        try:
            contrib = parse_hand(hh or "")
        except Exception as e:
            print("parse error hand id %s: %s" % (hid, e)); contrib = {}
        vset = set(verified) if verified else None   # None -> count all (legacy)
        for name, c in contrib.items():
            if vset is not None and name not in vset:
                continue
            upsert(cur, name, c)
        wm = hid; n_hands += 1
    cur.execute("UPDATE hud_aggregator_state SET last_hand_id=%s WHERE id=1", (wm,))
    conn.commit()
    return n_hands


def main():
    conn = psycopg2.connect(DSN)
    if "--reset" in sys.argv:
        cur = conn.cursor()
        cur.execute("TRUNCATE hud_player_stats; UPDATE hud_aggregator_state SET last_hand_id=0 WHERE id=1")
        conn.commit()
        print("reset: stats cleared, watermark=0")
    if "--watch" in sys.argv:
        while True:
            n = run_once(conn)
            if n: print("processed %d hands" % n)
            time.sleep(5)
    else:
        n = run_once(conn)
        cur = conn.cursor()
        cur.execute("SELECT count(*) FROM hud_player_stats")
        print("processed %d hands; %d players in hud_player_stats" % (n, cur.fetchone()[0]))


if __name__ == "__main__":
    main()
