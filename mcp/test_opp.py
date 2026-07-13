import nn_driver as D
D.DRY = True   # enable the diagnostic print

def show(tag, gs, sym):
    out = D._inject_opp_read(gs, dict(sym))
    opp = {k: out[k] for k in out if k.startswith("f$Opp_") and (out[k] not in (0, 0.0) or k in ("f$Opp_VPIP","f$Opp_Known"))}
    print("%-22s -> Known=%s VPIP=%s AF=%s flags=%s" % (
        tag, out.get("f$Opp_Known"), out.get("f$Opp_VPIP"), out.get("f$Opp_AF"),
        [k.replace("f$Opp_Is","") for k in out if k.startswith("f$Opp_Is") and out[k]]))

PLACE = {"aggressorchair": 2, "f$Opp_VPIP": 25, "f$Opp_PFR": 18, "f$Opp_AF": 2, "f$Opp_Known": 0, "f$Opp_IsNit": 0}

# pull a few real known villains spanning archetypes
import psycopg2, os
c = psycopg2.connect(os.environ.get("HISS_PG_DSN","host=127.0.0.1 dbname=hiss user=postgres password=dbpass"))
cur = c.cursor()
cur.execute("SELECT player FROM hud_player_stats WHERE vpip_d>=25 AND gametype='nlhe' ORDER BY vpip_d DESC LIMIT 8")
names = [r[0] for r in cur.fetchall()]
print("=== known villains (real DB lookup + classification) ===")
for nm in names:
    gs = {"userchair": 3, "players": [{"chair": 2, "name": nm, "seated": True}, {"chair": 3, "name": "me", "seated": True}]}
    show(nm[:20], gs, PLACE)

print("=== fail-safe paths (should keep placeholder VPIP=25, Known=0) ===")
show("Seat-6 placeholder", {"userchair": 3, "players": [{"chair": 2, "name": "Seat 6", "seated": True}]}, PLACE)
show("hero-is-aggressor", {"userchair": 3, "players": [{"chair": 3, "name": "me", "seated": True}]}, {"aggressorchair": 3, "f$Opp_VPIP": 25, "f$Opp_Known": 0})
show("no-aggressor(-1)", {"userchair": 3, "players": []}, {"aggressorchair": -1, "f$Opp_VPIP": 25, "f$Opp_Known": 0})
show("unknown-name", {"userchair": 3, "players": [{"chair": 2, "name": "NeverSeenBefore123", "seated": True}]}, PLACE)
