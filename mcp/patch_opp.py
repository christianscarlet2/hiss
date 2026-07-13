#!/usr/bin/env python3
"""patch_opp.py -- insert the opponent-read injection into nn_driver.py (roadmap item 4).
Idempotent, backs up, syntax-checks. Run in C:\\www\\openholdembot_old\\mcp."""
import io, os, sys, py_compile

TARGET = "nn_driver.py"
BACKUP = "nn_driver.py.bak_preopp"

FUNCS = r'''# --- OPPONENT READ INJECTION (roadmap item 4) --------------------------------------------------
# The OHF sends f$Opp_* as PLACEHOLDERS (Known=0, VPIP/PFR/AF defaults ~25/18/2), so the net gets no
# real read on live tables. Here we look up the AGGRESSOR's VERIFIED HUD stats (hud_player_stats,
# populated by hud_aggregator.py) and overwrite the placeholders with a REAL read -- a Python port of
# the server BotBrain::oppFeatures (KEEP IN SYNC) so the read->adjust mapping the net learned
# server-side applies here too. FAIL-SAFE: any problem leaves the placeholder and the decision goes
# on. Only KNOWN villains (>=20 hands) are overridden; unknown villains keep the OHF placeholder.
_OPP_COLS = ("vpip_n", "vpip_d", "pfr_n", "pfr_d", "threeb_n", "threeb_d", "f3b_n", "f3b_d",
             "ftc_n", "ftc_d", "aggr_actions", "call_actions", "wtsd_n", "wtsd_d")
_OPP_WARNED = [False]


def _pg_query(sql, params):
    """Read-only query reusing the _PG connection (runs in DRY too -- reads never mutate)."""
    try:
        import psycopg2
        if _PG["conn"] is None or _PG["conn"].closed:
            dsn = os.environ.get("HISS_PG_DSN",
                                 "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")
            _PG["conn"] = psycopg2.connect(dsn); _PG["conn"].autocommit = True
        with _PG["conn"].cursor() as cur:
            cur.execute(sql, params)
            return cur.fetchall()
    except Exception as e:
        if not _OPP_WARNED[0]:
            print("[nn_driver] opp-read DB unavailable (%s) -- serving OHF placeholders" % e, flush=True)
            _OPP_WARNED[0] = True
        try:
            if _PG["conn"] is not None:
                _PG["conn"].close()
        except Exception:
            pass
        _PG["conn"] = None
        return None


def _villain_name(gs, sym):
    try:
        ac = int(float(sym.get("aggressorchair", -1)))
    except (TypeError, ValueError):
        return None
    if ac < 0 or ac == gs.get("userchair", -1):
        return None                                    # no aggressor, or WE are the aggressor
    for p in (gs.get("players") or []):
        if p.get("chair") == ac and p.get("seated"):
            name = (p.get("name") or "").strip()
            if not name or name.lower().startswith("seat "):
                return None                            # anonymized seat placeholder -> unknown
            return name
    return None


def _opp_features(row):
    st = dict(zip(_OPP_COLS, row))
    def pct(n, d):
        return (100.0 * st[n] / st[d]) if st[d] else 0.0
    hands = st["vpip_d"] or 0
    vpip = pct("vpip_n", "vpip_d"); pfr = pct("pfr_n", "pfr_d")
    ca = st["call_actions"] or 0
    af = (st["aggr_actions"] / ca) if ca else float(st["aggr_actions"] or 0)
    af = max(0.0, min(af, 10.0))                        # cap outliers (tiny call counts blow AF up)
    wtsd = pct("wtsd_n", "wtsd_d"); tb = pct("threeb_n", "threeb_d")
    f3b = pct("f3b_n", "f3b_d"); fcb = pct("ftc_n", "ftc_d")
    known = 1 if hands >= 20 else 0
    o = {"f$Opp_VPIP": round(vpip, 1), "f$Opp_PFR": round(pfr, 1), "f$Opp_AF": round(af, 2),
         "f$Opp_WTSD": round(wtsd, 1), "f$Opp_Hands": min(int(hands), 100000), "f$Opp_Known": known,
         "f$Opp_ThreeBetsLight": 1 if tb > 9 else 0,
         "f$Opp_Foldy": 1 if (fcb > 55 or f3b > 65) else 0,
         "f$Opp_IsNit": 0, "f$Opp_IsLoose": 0, "f$Opp_IsStation": 0, "f$Opp_IsPassive": 0,
         "f$Opp_IsAggro": 0, "f$Opp_IsTAG": 0, "f$Opp_IsLAG": 0, "f$Opp_IsFish": 0, "f$Opp_IsManiac": 0}
    if known:
        o["f$Opp_IsNit"] = 1 if vpip < 15 else 0
        o["f$Opp_IsLoose"] = 1 if vpip > 32 else 0
        o["f$Opp_IsPassive"] = 1 if af < 1.0 else 0
        o["f$Opp_IsAggro"] = 1 if af > 2.5 else 0
        o["f$Opp_IsStation"] = 1 if (vpip > 30 and pfr < 12 and af < 1.2) else 0
        o["f$Opp_IsTAG"] = 1 if (15 <= vpip <= 26 and pfr >= vpip * 0.6 and af >= 1.5) else 0
        o["f$Opp_IsLAG"] = 1 if (vpip > 26 and pfr > 18 and af > 2.0) else 0
        o["f$Opp_IsFish"] = 1 if (vpip > 35 and pfr < 15) else 0
        o["f$Opp_IsManiac"] = 1 if (vpip > 45 and af > 3.5) else 0
    return o, vpip, af


def _inject_opp_read(gs, sym):
    """Overwrite placeholder f$Opp_* with the aggressor's REAL verified HUD read. FAIL-SAFE."""
    try:
        if not isinstance(sym, dict):
            return sym
        name = _villain_name(gs, sym)
        if not name:
            return sym                                 # no identifiable villain -> keep placeholder
        rows = _pg_query("SELECT " + ",".join(_OPP_COLS) +
                         " FROM hud_player_stats WHERE player=%s AND gametype='nlhe'", (name,))
        if not rows:
            return sym                                 # unknown villain -> keep placeholder
        feats, vpip, af = _opp_features(rows[0])
        if not feats.get("f$Opp_Known"):
            return sym                                 # <20 hands -> unreliable, keep placeholder
        sym.update(feats)
        if DRY:
            arche = [k.replace("f$Opp_Is", "") for k in feats if k.startswith("f$Opp_Is") and feats[k]]
            print("[nn_driver] opp-read %-14s VPIP=%.0f AF=%.2f -> %s%s" %
                  (name[:14], vpip, af, arche or ["(no archetype)"],
                   " +Foldy" if feats["f$Opp_Foldy"] else ""), flush=True)
    except Exception as e:
        print("[nn_driver] opp-read skipped (%s) -- OHF placeholder kept" % e, flush=True)
    return sym


'''

CALL = '    sym = _inject_opp_read(gs, sym)   # roadmap 4: real villain HUD read -> f$Opp_* (fail-safe)\n'


def main():
    src = io.open(TARGET, encoding="utf-8").read()
    if "_inject_opp_read" in src:
        print("ALREADY PATCHED -- no change."); return
    if not os.path.exists(BACKUP):
        io.open(BACKUP, "w", encoding="utf-8").write(src)
        print("backup ->", BACKUP)
    lines = src.splitlines(keepends=True)
    out = []
    inserted_funcs = inserted_call = False
    for ln in lines:
        if not inserted_funcs and ln.startswith("def decide_and_act(gs):"):
            out.append(FUNCS); inserted_funcs = True
        if not inserted_call and "nn = _post(NN, {\"sym\": sym," in ln:
            out.append(CALL); inserted_call = True
        out.append(ln)
    if not (inserted_funcs and inserted_call):
        print("ANCHOR MISS funcs=%s call=%s -- ABORT, no write" % (inserted_funcs, inserted_call)); sys.exit(2)
    io.open(TARGET, "w", encoding="utf-8").write("".join(out))
    py_compile.compile(TARGET, doraise=True)
    print("PATCHED + syntax OK (funcs+call inserted)")


if __name__ == "__main__":
    main()
