#!/usr/bin/env python3
"""icm_chip_daemon.py - the always-on ICM / chip-value pulse for the icm-chip-value skill (Lilith's voice).

Every ~2-3 min (and on a blind-level change or when near a pay jump) it reads the live table state +
the tournament structure (icm_config) and speaks a concise read: big blinds, M-ratio, the blind-level
clock, pay-jump proximity, and an approximate ICM $ equity (Malmuth-Harville over hero + field-average
stacks). Each read is spoken via Release\\lilith.exe AND logged to coach_notes (so it shows in
learner.exe's "Poker coach (Lilith)" panel) AND snapshotted to icm_snapshots. AIL-toggleable. [Emrald]

Run:  python mcp/icm_chip_daemon.py        (the AIL 'icm' switch launches/kills it)
"""
import os, sys, json, time, urllib.request, subprocess

BOT     = os.environ.get("HISS_BOT_URL", "http://127.0.0.1:27654")
PSQL    = os.environ.get("HISS_PSQL", r"C:\Program Files\PostgreSQL\12\bin\psql.exe")
PGUSER  = os.environ.get("PGUSER", "postgres")
PGDB    = os.environ.get("PGDATABASE", "hiss")
PGPASS  = os.environ.get("PGPASSWORD", "dbpass")
RELEASE = os.environ.get("HISS_RELEASE", r"C:\www\openholdembot_old\Release")
LILITH  = os.path.join(RELEASE, "lilith.exe")
TICK_S  = 30
SAY_MIN_S = int(os.environ.get("ICM_SAY_MIN", "150"))   # speak at most ~every 2.5 min unless an event fires
NOWIN = (0x08000000 if os.name == "nt" else 0)          # CREATE_NO_WINDOW: never pop a console
SI = subprocess.STARTUPINFO() if os.name == 'nt' else None
if SI is not None:
    SI.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    SI.wShowWindow = 0


def log(*a): print("[icm]", *a, file=sys.stderr, flush=True)


def _get(path):
    try:
        with urllib.request.urlopen(BOT + path, timeout=5) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception:
        return {}


def psql(sql):
    env = dict(os.environ); env["PGPASSWORD"] = PGPASS
    try:
        p = subprocess.run([PSQL, "-U", PGUSER, "-d", PGDB, "-t", "-A", "-c", sql],
                           capture_output=True, text=True, env=env, timeout=30, creationflags=NOWIN, startupinfo=SI)
        return p.stdout.strip() if p.returncode == 0 else ""
    except Exception:
        return ""


def esc(s): return str(s).replace("'", "''")


def speak(text, kind="strategy"):
    log("speak:", text[:80])
    psql("INSERT INTO coach_notes (kind,priority,message,spoken) VALUES ('%s',1,'%s',true);" % (kind, esc(text)))
    try:
        subprocess.run([LILITH, text], cwd=RELEASE, timeout=120, creationflags=NOWIN, startupinfo=SI)
    except Exception as e:
        log("lilith failed:", e)


def num(v, d=0.0):
    try: return float(v)
    except Exception: return d


def icm_config():
    """Latest tournament structure row (or {})."""
    raw = psql("SELECT row_to_json(c) FROM icm_config c ORDER BY id DESC LIMIT 1;")
    try: return json.loads(raw) if raw else {}
    except Exception: return {}


def icm_equity(hero, stacks, payouts):
    """Malmuth-Harville ICM equity (in payout units) for `hero` given the full `stacks` list + the
    `payouts` list (descending prize for 1st, 2nd, ...). Recursion is capped to the paid places."""
    total = sum(stacks)
    if total <= 0 or not payouts: return 0.0
    payouts = list(payouts)

    def finish(remaining_idx, place):
        # remaining_idx: indices still in; place: prize index being awarded (0=1st)
        if place >= len(payouts) or not remaining_idx:
            return 0.0
        eq = 0.0
        tot = sum(stacks[i] for i in remaining_idx)
        if tot <= 0: return 0.0
        for i in remaining_idx:
            p_first = stacks[i] / tot                       # prob i takes THIS place
            if i == hero:
                eq += p_first * payouts[place]
            elif place + 1 < len(payouts):
                rest = [j for j in remaining_idx if j != i]
                eq += p_first * finish(rest, place + 1)
        return eq
    # Cap the field to keep the recursion cheap: hero + the top (places_paid+2) stacks.
    keep = sorted(range(len(stacks)), key=lambda i: -stacks[i])[:max(len(payouts) + 2, 4)]
    if hero not in keep: keep.append(hero)
    return finish(keep, 0)


def main():
    log("icm chip daemon up.")
    last_say = 0.0
    last_level = None
    while True:
        ts = _get("/api/table-state")
        cfg = icm_config()
        uc = ts.get("userchair", -1)
        lim = ts.get("limits", {}) or {}
        bb = num(lim.get("bblind")); sb = num(lim.get("sblind")); ante = num(lim.get("ante"))
        players = ts.get("players") or []
        seated = [p for p in players if p.get("seated")]
        hero_stack = None
        for p in players:
            if p.get("chair") == uc:
                hero_stack = num(p.get("balance"))
        if hero_stack is None or bb <= 0 or uc < 0:
            time.sleep(TICK_S); continue

        nseat = max(1, len(seated))
        bb_depth = hero_stack / bb
        m_ratio = hero_stack / max(sb + bb + ante * nseat, 0.01)
        remaining = int(num(cfg.get("players_remaining"), len(seated)))
        paid = int(num(cfg.get("places_paid"), 0))
        level = cfg.get("current_level")
        # ICM equity (approx): hero + (remaining-1) field-average stacks vs the payout ladder.
        payouts = cfg.get("payouts") or []
        if isinstance(payouts, str):
            try: payouts = json.loads(payouts)
            except Exception: payouts = []
        payouts = [num(x) for x in payouts if num(x) > 0]
        equity = 0.0
        if payouts and remaining > 0:
            total_chips = num(cfg.get("total_entrants")) * num(cfg.get("starting_stack"))
            avg = (total_chips - hero_stack) / max(remaining - 1, 1) if total_chips > hero_stack else hero_stack
            field = [hero_stack] + [avg] * max(remaining - 1, 0)
            equity = icm_equity(0, field, payouts[:max(paid, 1)] if paid else payouts)

        # gear label
        gear = "deep - play poker" if bb_depth >= 40 else \
               "maneuver" if bb_depth >= 25 else \
               "tighten, look for clean spots" if bb_depth >= 15 else \
               "push-or-fold, first-in with fold equity"
        near_bubble = paid and remaining and (paid < remaining <= paid + max(3, int(paid * 0.15)))

        event = (level != last_level and last_level is not None) or near_bubble
        if event or (time.time() - last_say) >= SAY_MIN_S:
            parts = ["%d big blinds, M of %d -- %s." % (round(bb_depth), round(m_ratio), gear)]
            if equity > 0:
                parts.append("ICM equity about $%.0f." % equity)
            if near_bubble:
                parts.append("Bubble is near -- %d left, %d paid. Ladder up; let the short stacks bust." % (remaining, paid))
            elif paid and remaining:
                parts.append("%d left, %d paid." % (remaining, paid))
            speak(" ".join(parts), kind="strategy")
            last_say = time.time()
            psql("INSERT INTO icm_snapshots (handnumber,players_remaining,hero_stack,hero_equity,detail) "
                 "VALUES ('%s',%d,%.2f,%.2f,'%s');" % (esc(ts.get("handnumber", "")), remaining or nseat,
                 hero_stack, equity, esc("bb=%.1f m=%.1f gear=%s" % (bb_depth, m_ratio, gear))))
        last_level = level
        time.sleep(TICK_S)


if __name__ == "__main__":
    main()
