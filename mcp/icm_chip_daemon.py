#!/usr/bin/env python3
"""icm_chip_daemon.py - the always-on ICM / chip-value pulse for the icm-chip-value skill (Lilith's voice).

Every ~2-3 min (and on a blind-level change or near a pay jump) it reads the live table state + the
tournament structure and speaks a concise read: big blinds, M-ratio, players left, pay-jump proximity,
ICM $ equity and CHIP VALUE ($ per big blind). Spoken via Release\\lilith.exe, logged to coach_notes
(shows in learner.exe's "Poker coach" panel) and snapshotted to icm_snapshots. AIL-toggleable.

STRUCTURE COMES FROM THE LOBBY (settings.lobby_info), written by the icm-chip-value skill after
`lobby_fetch.sh` (Claude parses the captured lobby PNGs with vision). icm_config is only an override.
[Emrald: actually use the lobby info; get chip value and players-left right]

Three bugs this replaces:
  1. STALE / WRONG TOURNAMENT. It read icm_config blindly, so a structure captured weeks ago for a
     different tournament produced confident, fictional equity. Now the lobby row must MATCH the live
     table (name) and be fresh, or we do not speak an ICM number at all.
  2. UNIT MIX. ACR displays stacks in BIG BLINDS (bblind_fallback=1.0), but the old code built the
     field from `total_entrants * starting_stack` -- CHIPS. Hero (17 bb) was compared against a field
     of ~22,000 "chips", so hero looked like a 0.0001% stack and equity collapsed to ~0. ALL stack math
     is now in ONE unit: big blinds.
  3. INTRACTABLE ICM. The hand-rolled Malmuth-Harville recursion expanded over places_paid+2 stacks --
     factorial in the number of paid places. On a 593-runner freeroll paying ~89 that never returns.
     We now shell out to the skill's icm.py (Malmuth-Harville MONTE-CARLO), which scales to any field
     and already reports dollars_per_chip + bubble_factor.

Run:  python mcp/icm_chip_daemon.py        (the AIL 'icm' switch launches/kills it)
"""
import os, re, sys, json, time, urllib.request, subprocess

BOT     = os.environ.get("HISS_BOT_URL", "http://127.0.0.1:27654")
PSQL    = os.environ.get("HISS_PSQL", r"C:\Program Files\PostgreSQL\12\bin\psql.exe")
PGUSER  = os.environ.get("PGUSER", "postgres")
PGDB    = os.environ.get("PGDATABASE", "hiss")
PGPASS  = os.environ.get("PGPASSWORD", "dbpass")
RELEASE = os.environ.get("HISS_RELEASE", r"C:\www\openholdembot_old\Release")
REPO    = os.environ.get("HISS_REPO", r"C:\www\openholdembot_old")
LILITH  = os.path.join(RELEASE, "lilith.exe")
ICM_PY  = os.path.join(REPO, ".claude", "skills", "icm-chip-value", "icm.py")
PYEXE   = sys.executable or "python"

TICK_S    = 30
SAY_MIN_S = int(os.environ.get("ICM_SAY_MIN", "150"))    # speak at most ~every 2.5 min unless an event fires
# Players bust fast in a big field, so a lobby row goes stale quickly. Past this we still speak DEPTH
# (always true, straight off the felt) but we will NOT speak players-left or an ICM $ number.
LOBBY_MAX_AGE_S = int(os.environ.get("ICM_LOBBY_MAX_AGE", "1800"))   # 30 min
ICM_SIMS  = int(os.environ.get("ICM_SIMS", "20000"))

NOWIN = (0x08000000 if os.name == "nt" else 0)           # CREATE_NO_WINDOW: never pop a console
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
                           capture_output=True, text=True, env=env, timeout=30,
                           creationflags=NOWIN, startupinfo=SI)
        return p.stdout.strip() if p.returncode == 0 else ""
    except Exception:
        return ""


def esc(s): return str(s).replace("'", "''")


def num(v, d=0.0):
    try:
        return float(v)
    except Exception:
        return d


def speak(text, kind="strategy"):
    log("speak:", text[:100])
    psql("INSERT INTO coach_notes (kind,priority,message,spoken) VALUES ('%s',1,'%s',true);" % (kind, esc(text)))
    try:
        subprocess.run([LILITH, text], cwd=RELEASE, timeout=120, creationflags=NOWIN, startupinfo=SI)
    except Exception as e:
        log("lilith failed:", e)


# ---------------------------------------------------------------- structure (lobby)

def _slug(s):
    """Compare a lobby tournament name against the scraped table name. Both are OCR-noisy and
    formatted differently ("$50 GTD Freeroll" vs "50GTDFreerollTable1N"), so reduce to lowercase
    alphanumerics and ask for containment."""
    return re.sub(r"[^a-z0-9]", "", (s or "").lower())


# How long a CONFIRMED table sighting keeps counting. The table-name region OCRs to junk between
# hands ('0.a|FONFPENTRPAI'), and junk still looks alphabetic -- so you cannot tell "garbled" from
# "different tournament" by inspecting one read. What you CAN do is remember the last read that
# positively MATCHED the lobby: while that is recent we are still at that table and a junk read means
# nothing. Once it goes stale (we really did move tables), the match must be re-earned.
TABLE_CONFIRM_TTL_S = 300      # a sighting stops counting after 5 min
TABLE_MAX_MISSES    = 4        # ...and after 4 CONSECUTIVE non-matching reads (~2 min at TICK_S)
_confirm = {"name": "", "ts": 0.0, "misses": 0}


def table_matches(ts, lobby_name):
    """(ok, seen) -- ok: treat the live table as this tournament. seen: what we actually read.

    Junk reads are INTERMITTENT; a real table/tournament change is PERSISTENT. So a mismatch is
    forgiven only while the last positive sighting is recent AND the mismatches have not piled up.
    That tolerates OCR noise without letting a genuine move (bust out, re-register elsewhere) keep
    speaking the old tournament's equity."""
    seen = _slug(ts.get("table"))
    if not lobby_name:
        return True, seen
    if seen and lobby_name in seen:                       # positive sighting
        _confirm.update(name=lobby_name, ts=time.time(), misses=0)
        return True, seen
    _confirm["misses"] += 1
    fresh = (_confirm["name"] == lobby_name
             and (time.time() - _confirm["ts"]) < TABLE_CONFIRM_TTL_S)
    return (fresh and _confirm["misses"] <= TABLE_MAX_MISSES), seen


def lobby_info():
    """settings.lobby_info + its age in seconds. This is the tournament structure the icm-chip-value
    skill wrote after lobby_fetch.sh (Claude vision on the lobby screens)."""
    raw = psql("SELECT json_build_object('v', value, 'age', "
               "EXTRACT(EPOCH FROM (now() - updated_at)))::text "
               "FROM settings WHERE key = 'lobby_info';")
    try:
        d = json.loads(raw) if raw else {}
        return (d.get("v") or {}), num(d.get("age"), 1e9)
    except Exception:
        return {}, 1e9


def icm_config():
    raw = psql("SELECT row_to_json(c)::text FROM icm_config c ORDER BY id DESC LIMIT 1;")
    try:
        return json.loads(raw) if raw else {}
    except Exception:
        return {}


def payout_ladder(prize_pool, places_paid, first_place=0.0):
    """A prize ladder for a field whose exact payouts the lobby did not give us.

    ACR's info page shows the prize POOL (and often 1st place) but the full breakdown lives behind the
    PRIZE POOL button, so we model it: prize_k proportional to k^-alpha, normalised to the pool. If we
    know 1st place we solve alpha so the model reproduces it; otherwise assume 1st ~= 18% of the pool,
    which is typical for a large-field ACR freeroll. Returns [] when we cannot honestly build one."""
    prize_pool = num(prize_pool); places_paid = int(num(places_paid))
    if prize_pool <= 0 or places_paid <= 0:
        return []
    if places_paid == 1:
        return [prize_pool]
    target = (num(first_place) / prize_pool) if num(first_place) > 0 else 0.18
    target = min(max(target, 1.0 / places_paid + 1e-6), 0.95)   # must exceed a flat split

    def share_of_first(alpha):
        w = [k ** -alpha for k in range(1, places_paid + 1)]
        return w[0] / sum(w)

    lo, hi = 0.0, 6.0                      # alpha=0 -> flat; larger -> more top-heavy
    for _ in range(60):
        mid = (lo + hi) / 2.0
        if share_of_first(mid) < target:
            lo = mid
        else:
            hi = mid
    alpha = (lo + hi) / 2.0
    w = [k ** -alpha for k in range(1, places_paid + 1)]
    s = sum(w)
    return [prize_pool * x / s for x in w]


def structure(ts):
    """The tournament structure for the table we are ACTUALLY sitting at, in BIG BLINDS.

    Returns (info, why_not). info is None when we cannot build an honest one -- the caller then speaks
    depth only. Never guesses players-left or equity from a stale/foreign lobby row: that is exactly
    what made this daemon confidently wrong."""
    lob, age = lobby_info()
    if not lob:
        return None, "no lobby_info -- run lobby_fetch + the icm-chip-value skill"

    # OCR mangles the table string ("oldem354821|50GTDFreerollTable31N"), so match on the NAME, which
    # survives it, rather than the tourney id (whose digits get truncated).
    name = _slug(lob.get("tournament"))
    ok, seen = table_matches(ts, name)
    if not ok:
        return None, "lobby_info is for '%s' but we are at '%s'" % (lob.get("tournament"), seen)
    if age > LOBBY_MAX_AGE_S:
        return None, "lobby_info is %.0f min old" % (age / 60.0)

    # icm_config is a MANUAL override, and a long-lived one -- it happily survives into the next
    # tournament. Only honour it when it names the tournament we are actually sitting in, otherwise a
    # three-week-old row silently supplies the payouts and every $ figure is wrong (7 places / $4.05
    # instead of the lobby's $50 pool). Same gate as the lobby row.
    cfg = icm_config()
    if cfg and name and _slug(cfg.get("tournament_name")) != name:
        cfg = {}

    remaining = int(num(lob.get("remaining") or cfg.get("players_remaining"), 0))
    entrants  = int(num(lob.get("entrants") or cfg.get("total_entrants"), 0))
    pool      = num(lob.get("prize_pool") or cfg.get("prize_pool"), 0.0)
    if remaining <= 0:
        return None, "lobby_info has no players-remaining"

    # --- the field's average stack, in BIG BLINDS ---------------------------------------------
    # Preferred: the lobby prints it in bb already ("Avg Stack 68.48 BB").
    avg_bb = num(lob.get("avg_stack_bb"), 0.0)
    if avg_bb <= 0:
        # Otherwise convert the chip figures with the CURRENT big blind in chips. Without bb_chips
        # the chip numbers cannot be compared to a bb-denominated stack at all -- so we refuse
        # rather than mix units (the old bug).
        start_chips = num(lob.get("starting_chips") or cfg.get("starting_stack"), 0.0)
        bb_chips    = num(lob.get("bb_chips"), 0.0)
        if start_chips > 0 and bb_chips > 0 and entrants > 0:
            avg_bb = (entrants * start_chips / remaining) / bb_chips
    if avg_bb <= 0:
        return None, "lobby_info lacks avg_stack_bb (or bb_chips) -- cannot size the field in bb"

    # --- the payout ladder ---------------------------------------------------------------------
    payouts = lob.get("payouts") or cfg.get("payouts") or []
    if isinstance(payouts, str):
        try: payouts = json.loads(payouts)
        except Exception: payouts = []
    payouts = [num(x) for x in payouts if num(x) > 0]
    paid = int(num(lob.get("places_paid") or cfg.get("places_paid"), 0))
    if not payouts:
        if paid <= 0 and entrants > 0:
            paid = max(1, int(round(entrants * 0.15)))      # ACR pays ~15% of the field
        payouts = payout_ladder(pool, paid, lob.get("first_place"))
    if not payouts:
        return None, "no payouts and no prize pool -- cannot value chips"
    paid = paid or len(payouts)

    return {
        "remaining": remaining, "entrants": entrants, "paid": paid,
        "avg_bb": avg_bb, "payouts": payouts, "pool": pool,
        "bb_chips": num(lob.get("bb_chips"), 0.0),
        "age_min": age / 60.0,
    }, None


def run_icm(hero_bb, st):
    """Shell out to the skill's Monte-Carlo ICM. Stacks are in BIG BLINDS, payouts in dollars, so
    `dollars_per_chip` comes back as DOLLARS PER BIG BLIND -- the hero's chip value."""
    payload = {
        "hero_stack": hero_bb,
        "players_remaining": st["remaining"],
        "avg_stack": st["avg_bb"],
        "payouts": st["payouts"],
        "sims": ICM_SIMS,
    }
    try:
        p = subprocess.run([PYEXE, ICM_PY, json.dumps(payload)], capture_output=True, text=True,
                           timeout=120, creationflags=NOWIN, startupinfo=SI)
        if p.returncode != 0:
            log("icm.py failed:", (p.stderr or "")[:200]); return {}
        return json.loads(p.stdout.strip() or "{}")
    except Exception as e:
        log("icm.py error:", e)
        return {}


def main():
    log("icm chip daemon up. structure source = settings.lobby_info; math = icm.py "
        "(exact closed form mid-field, monte-carlo at the final table)")
    last_say = 0.0
    last_level = None
    warned = None

    while True:
        ts = _get("/api/table-state")
        uc = ts.get("userchair", -1)
        lim = ts.get("limits", {}) or {}
        bb = num(lim.get("bblind")); sb = num(lim.get("sblind")); ante = num(lim.get("ante"))
        players = ts.get("players") or []
        seated = [p for p in players if p.get("seated")]
        hero_stack = next((num(p.get("balance")) for p in players if p.get("chair") == uc), None)
        if hero_stack is None or bb <= 0 or uc < 0:
            time.sleep(TICK_S); continue

        nseat = max(1, len(seated))
        # Works whether the table reports CHIPS (bb=300) or BIG BLINDS (ACR: bb=1.0) -- one unit out.
        hero_bb = hero_stack / bb
        m_ratio = hero_stack / max(sb + bb + ante * nseat, 1e-9)

        gear = ("deep - play poker"   if hero_bb >= 40 else
                "maneuver"            if hero_bb >= 25 else
                "tighten, look for clean spots" if hero_bb >= 15 else
                "push-or-fold, first-in with fold equity")

        st, why = structure(ts)

        # DEPTH is always honest -- it comes straight off the felt. Everything below it needs the lobby.
        parts = ["%d big blinds, M of %d -- %s." % (round(hero_bb), round(m_ratio), gear)]
        equity = 0.0
        near_bubble = False
        level = None
        remaining = None

        if st is None:
            if why != warned:
                log("no ICM:", why)
                warned = why
        else:
            warned = None
            remaining = st["remaining"]; paid = st["paid"]
            near_bubble = bool(paid and paid < remaining <= paid + max(3, int(paid * 0.15)))
            r = run_icm(hero_bb, st)
            equity = num(r.get("hero_equity"))
            dpbb   = num(r.get("dollars_per_chip"))          # $ per BIG BLIND (stacks were in bb)
            bubble = r.get("bubble_factor")

            if equity > 0:
                parts.append("Worth about $%.2f by I-C-M." % equity)
                if dpbb > 0:
                    # Chip value: per big blind always; per chip too when the lobby gave us bb_chips.
                    if st["bb_chips"] > 0:
                        parts.append("Each big blind is worth %.0f cents (%.4f cents a chip)."
                                     % (dpbb * 100.0, dpbb * 100.0 / st["bb_chips"]))
                    else:
                        parts.append("Each big blind is worth %.0f cents." % (dpbb * 100.0))
            if near_bubble:
                parts.append("Bubble is near -- %d left, %d paid. Ladder up; let the short stacks bust."
                             % (remaining, paid))
            else:
                parts.append("%d left of %d, %d paid." % (remaining, st["entrants"], paid))
            if bubble and num(bubble) >= 1.3:
                parts.append("Risk premium %.1f -- your stack is worth more than its chips." % num(bubble))
            level = st.get("age_min")

        event = near_bubble or (last_level is not None and level != last_level)
        if event or (time.time() - last_say) >= SAY_MIN_S:
            speak(" ".join(parts), kind="strategy")
            last_say = time.time()
            psql("INSERT INTO icm_snapshots (handnumber,players_remaining,hero_stack,hero_equity,detail) "
                 "VALUES ('%s',%d,%.2f,%.2f,'%s');"
                 % (esc(ts.get("handnumber", "")), int(remaining or nseat), hero_bb, equity,
                    esc("bb=%.1f m=%.1f gear=%s%s" % (hero_bb, m_ratio, gear,
                        "" if st else " NO-STRUCTURE:" + str(why)))))
        last_level = level
        time.sleep(TICK_S)


if __name__ == "__main__":
    main()
