"""observer_strategy.py -- turn the OBSERVER's reads into concrete STRATEGY BRANCHES.

observe_learn.py writes `observations` (villain reads, table reads, session trend). This layer
distills the freshest reads for the CURRENT villain + table + session into ONE strategy BRANCH with
a GOTO label (the OHF dispatches f$ObsStrategy on it), knob targets (aggro / bluff / openrange that
steer the live OHF + NN driver with no rebuild), and a MISCHIEF AFFINITY (how hard the mischief layer
should antagonize under this branch). The branch is the bridge: observation -> GOTO -> knobs -> OHF /
NN / mischief, all from one decision.

  observe_strategy(villain, table, dsn) -> {branch, goto, label, aggro, bluff, openrange,
                                            mischief_affinity, note, sources}

Branches (== OHF GOTO labels):
  ISOLATE_SHORTSTACK  attack/iso short jam-or-fold stacks, flat less
  ATTACK_FOLDERS      relentless pressure vs overfolders / give-ups
  PUNISH_TILT         widen + barrel a steaming villain
  VALUE_STATION       value thick, stop bluffing a calling station
  DONKFEST_CHEAP      lots of limpers/donks -> in cheap, bet heavy when we connect
  STEADY              winning / "don't get fancy" -> dial variance + mischief DOWN
  PRESS_DOWNTREND     losing session -> pick better spots, controlled aggression
  NORMAL              no strong read -> play the base strategy
"""
import os, time

DSN = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")
FRESH_MS = 6 * 60 * 1000      # an observation counts as "live" for 6 minutes

# branch -> (aggro, bluff, openrange, mischief_affinity, note). knobs are 0..1 targets; affinity scales
# the mischief rate (1.0 = normal, >1 = prank harder, <1 = behave).
_BRANCH = {
    "ISOLATE_SHORTSTACK": (0.72, 0.45, 0.70, 1.2, "iso the jam/fold short stack, flat less"),
    "ATTACK_FOLDERS":     (0.78, 0.72, 0.66, 1.6, "relentless pressure vs overfolders"),
    "PUNISH_TILT":        (0.75, 0.62, 0.60, 1.5, "widen + barrel the steamer"),
    "VALUE_STATION":      (0.55, 0.20, 0.50, 0.6, "value thick, stop bluffing the station"),
    "DONKFEST_CHEAP":     (0.60, 0.40, 0.80, 1.3, "in cheap vs donks, bet heavy when we hit"),
    "STEADY":             (0.50, 0.45, 0.50, 0.5, "up trend -> don't get fancy, dial it down"),
    "PRESS_DOWNTREND":    (0.62, 0.50, 0.55, 0.9, "down trend -> better spots, controlled aggression"),
    "NORMAL":             (0.50, 0.50, 0.50, 1.0, "no strong read -> base strategy"),
}


def _kw(text, *words):
    t = (text or "").lower()
    return any(w in t for w in words)


def _classify_villain(infer, sugg):
    """Map a villain observation's text to a branch (keyword-robust to observe_learn's wording)."""
    if _kw(infer, "tilt", "steam", "spew") or _kw(sugg, "tilt", "steam"):
        return "PUNISH_TILT"
    if _kw(infer, "station", "never folds", "calls too", "sticky") or _kw(sugg, "value thick", "stop bluff", "thin value"):
        return "VALUE_STATION"
    if _kw(infer, "short stack", "jam", "shove") or _kw(sugg, "isolate", "iso ", "flat less"):
        return "ISOLATE_SHORTSTACK"
    if _kw(infer, "overfold", "gives up", "folds too", "weak", "nit") or _kw(sugg, "attack", "pressure", "barrel", "bluff more"):
        return "ATTACK_FOLDERS"
    return None


def observe_strategy(villain=None, table=None, dsn=None):
    out = {"branch": "NORMAL", "goto": "NORMAL", "label": "NORMAL", "aggro": 0.5, "bluff": 0.5,
           "openrange": 0.5, "mischief_affinity": 1.0, "note": "no strong read", "sources": []}
    try:
        import psycopg2
    except Exception:
        return out
    now = int(time.time() * 1000)
    try:
        c = psycopg2.connect(dsn or DSN); cur = c.cursor()
        cur.execute(
            "SELECT scope, subject, inference, suggestion, ts_ms FROM observations "
            "WHERE ts_ms > %s ORDER BY ts_ms DESC LIMIT 40", (now - FRESH_MS,))
        rows = cur.fetchall(); c.close()
    except Exception:
        return out

    branch = None; srcs = []
    # 1) the CURRENT villain's read takes precedence (most specific)
    if villain:
        for scope, subj, infer, sugg, ts in rows:
            if scope == "villain" and subj and subj.lower() == str(villain).lower():
                b = _classify_villain(infer, sugg)
                if b:
                    branch = b; srcs.append("villain:%s:%s" % (subj, infer)); break
    # 2) table-wide read (donkfest / lots of limpers)
    if branch is None:
        for scope, subj, infer, sugg, ts in rows:
            if scope == "table" and (_kw(infer, "donk", "limp", "passive", "freeroll", "loose-passive")
                                     or _kw(sugg, "in cheap", "cheap", "bet heavy")):
                branch = "DONKFEST_CHEAP"; srcs.append("table:%s" % infer); break
    # 3) any FRESH villain read (not necessarily the one in the pot) as a table-texture hint
    if branch is None:
        for scope, subj, infer, sugg, ts in rows:
            if scope == "villain":
                b = _classify_villain(infer, sugg)
                if b:
                    branch = b; srcs.append("villain*:%s:%s" % (subj, infer)); break
    # 4) session trend modulates -- only if no villain/table override (it's the weakest signal)
    if branch is None:
        for scope, subj, infer, sugg, ts in rows:
            if scope == "session":
                if _kw(infer, "up trend", "up ", "+") or _kw(sugg, "don't get fancy", "keep doing", "working"):
                    branch = "STEADY"; srcs.append("session:%s" % infer); break
                if _kw(infer, "down", "losing", "-") or _kw(sugg, "better spots", "tighten"):
                    branch = "PRESS_DOWNTREND"; srcs.append("session:%s" % infer); break
    if branch is None:
        branch = "NORMAL"

    aggro, bluff, openrange, affinity, note = _BRANCH[branch]
    out.update({"branch": branch, "goto": branch, "label": branch, "aggro": aggro, "bluff": bluff,
                "openrange": openrange, "mischief_affinity": affinity, "note": note, "sources": srcs})
    return out


if __name__ == "__main__":
    import sys, json
    v = sys.argv[1] if len(sys.argv) > 1 else None
    print(json.dumps(observe_strategy(v), indent=2))
