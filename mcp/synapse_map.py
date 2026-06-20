#!/usr/bin/env python3
"""synapse_map.py -- the Hiss "synapse harmonizer" + ghost-in-the-machine inference layer.

ONE place that connects every signal the bot perceives, every knob/mode that steers it, and every
output it produces -- into a single graph of NODES (values/knobs/outputs) joined by SYNAPSES
(which knob influences which signal / which signal drives which decision). At EACH node the GHOST
IN THE MACHINE (Jasper) attaches a short inference -- a read of what that value means right now -- so
the system reasons at every point of the game, not just at the final action.

It harmonizes:
  signals   live symbols + /api/table-state + scrapes + HUD opponent stats + hero/board/pot/blinds
  knobs     OHF(autoplayer) / NN-driver / ULTRA / SUPERSTITION-omen / beastfavor + settings
  memory    pending voice feedback (the human feedback loop) + replay hand count
  outputs   the decision (fold/call/raise/betsize) the above produce

and EXPORTS:
  --json       synapse_state.json  : the live harmonized snapshot (nodes+inferences+synapses)
  --features   synapse_features.json: the NN feature/action/knob spec so neural-net training can
               build synapses to ALL the values, knobs and settings (one unified feature space)
  --watch      daemon: recompute every tick, store to postgres synapse_state, and emit a ghost
               inference at every game point (optionally --speak the headline read via Lilith)

  python synapse_map.py [--bot-url http://127.0.0.1:27654] [--json|--features|--watch] [--every 3] [--speak]

The AIL reads this each cycle (see Release/logs/AIL_PLAYBOOK.md): the knob catalog tells it every
lever it can pull; the synapse graph tells it what each lever moves; the ghost inferences and pending
voice feedback tell it what to improve. The feature spec connects it all to NN training.
"""
import sys, os, json, time, urllib.request

BOT = "http://127.0.0.1:27654"
DSN = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")
OUT_DIR = os.environ.get("HISS_SYNAPSE_DIR", r"C:\www\openholdembot_old\Release\logs")
LILITH = r"C:\www\openholdembot_old\Release\lilith.exe"

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

# ---- curated signal symbols the synapse map reads every tick --------------------------------
# NOTE: deliberately EXCLUDES prwin -- its Monte-Carlo evaluation runs synchronously on the HTTP
# thread in /api/symbols (no heartbeat lock), which is slow and can race the engine -> a heavy query
# coincided with a 0xc0000005 crash (2026-06-19). Keep this set cheap/fast to minimise the race
# window until /api/symbols serves a cached symbol snapshot instead of evaluating live.
SYMBOLS = ["betround", "nplayersdealt", "nplayersplaying", "nouts", "PotSize",
           "AmountToCall", "StackSize", "bblind", "sblind", "f$InPositionPost", "f$HeadsUpPot",
           "f$WasPreflopRaiser", "f$HaveStrongMade", "f$HaveOnePair", "f$HaveBigDraw",
           "f$Opp_IsStation", "f$Opp_IsAggro", "sb_beastfavor", "f$fold", "f$call", "f$raise",
           "f$betsize", "f$Style"]


def num(v, d=0.0):
    try:
        return float(v)
    except Exception:
        return d


def gather():
    """Pull the whole live system into one dict of raw inputs."""
    ts = _get("/api/table-state")
    syms = _get("/api/symbols?names=" + ",".join(SYMBOLS))
    nn = _get("/api/nn-driver")
    ultra = _get("/api/ultra")
    superstition = _get("/api/superstition")
    autoplayer = _get("/api/autoplayer")
    beast = _get("/api/beast")
    hero = ""
    hero_balance = None
    uc = ts.get("userchair", -1)
    for p in (ts.get("players") or []):
        if p.get("chair") == uc:
            hero = " ".join(c for c in (p.get("cards") or []) if c and c != "BACK")
            try:
                hero_balance = float(p.get("balance"))
            except Exception:
                hero_balance = None
    voice_pending, hud_rows = 0, []
    try:
        import psycopg2
        c = psycopg2.connect(DSN); cur = c.cursor()
        cur.execute("SELECT count(*) FROM voice_feedback WHERE NOT applied")
        voice_pending = cur.fetchone()[0]
        try:
            cur.execute("SELECT player, vpip, pfr, af, n FROM hud_player_stats ORDER BY n DESC LIMIT 6")
            hud_rows = [dict(zip(("player", "vpip", "pfr", "af", "n"), r)) for r in cur.fetchall()]
        except Exception:
            c.rollback()
        c.close()
    except Exception:
        pass
    return {"ts": ts, "syms": syms, "nn": nn, "ultra": ultra, "superstition": superstition,
            "autoplayer": autoplayer, "beast": beast, "hero": hero, "board":
            " ".join(c for c in (ts.get("commonCards") or []) if c),
            "handnumber": str(ts.get("handnumber") or ""), "hero_balance": hero_balance,
            "voice_pending": voice_pending, "hud": hud_rows}


# ---- per-hand result tracker: net = hero stack at start of next hand - start of this hand ----
# hiss_log_hands is win-biased (the ACR writer only logs hands the hero stayed in), so we derive
# EVERY hand's win/loss from the hero's between-hands stack delta. The AIL reads hand_results to
# synthesize voice feedback on LOSING hands (ail_feedback.py).
def record_hand_result(prev_hand, prev_start_bal, new_start_bal, ts_ms):
    if not prev_hand or prev_start_bal is None or new_start_bal is None:
        return None
    # Guard against garbage transitions (e.g. the off-table -> reconnect jump that produced a bogus
    # handnumber "8" with start_balance 1111). Require a REAL hand id and plausible stacks; otherwise
    # the spurious row would pollute the loss-weighted synthesis with a fake huge "loss".
    ph = str(prev_hand)
    if not (ph.isdigit() and len(ph) >= 6):
        return None
    if not (0 < prev_start_bal < 1e7 and 0 < new_start_bal < 1e7):
        return None
    net = round(new_start_bal - prev_start_bal, 2)
    try:
        import psycopg2
        c = psycopg2.connect(DSN); cur = c.cursor()
        cur.execute("CREATE TABLE IF NOT EXISTS hand_results (handnumber text primary key, "
                    "ts_ms bigint, net double precision, start_balance double precision, "
                    "end_balance double precision)")
        cur.execute("INSERT INTO hand_results (handnumber, ts_ms, net, start_balance, end_balance) "
                    "VALUES (%s,%s,%s,%s,%s) ON CONFLICT (handnumber) DO UPDATE SET "
                    "net=EXCLUDED.net, end_balance=EXCLUDED.end_balance, ts_ms=EXCLUDED.ts_ms",
                    (prev_hand, ts_ms, net, prev_start_bal, new_start_bal))
        c.commit(); c.close()
    except Exception as e:
        print("[synapse] hand_results error:", e, flush=True)
    return net


# ---- THE GHOST IN THE MACHINE: an inference at each node ------------------------------------
def ghost_node_inference(node_id, value, g):
    """Jasper's read of one node, given the live state g. Short, situational; the synapse layer
    calls this for every node so the system reasons at every point, not just at the final action."""
    s = g["syms"]
    bb = num(s.get("bblind"), 1.0) or 1.0
    pot = num(s.get("PotSize"))
    stack = num(s.get("StackSize"))
    spr = (stack / pot) if pot > 0 else None
    if node_id == "signal.pot":
        return "pot %.2f = %.0fbb; SPR %s" % (pot, pot / bb, ("%.1f" % spr) if spr else "-")
    if node_id == "signal.spr":
        if spr is None:
            return "no pot yet"
        return ("committed - get it in with a strong made hand" if spr < 3
                else ("medium SPR - one-pair pot-control" if spr < 8 else "deep - play small pots in position"))
    if node_id == "signal.prwin":
        if s.get("prwin") is None:
            return "equity n/a (live prwin eval disabled for stability)"
        pw = num(s.get("prwin"))
        return "prwin %.0f%% -> %s" % (pw * 100, "ahead, value" if pw > 0.6 else ("marginal" if pw > 0.4 else "behind"))
    if node_id == "signal.position":
        ip = num(s.get("f$InPositionPost")) > 0
        pfr = num(s.get("f$WasPreflopRaiser")) > 0
        if ip and not pfr:
            return "in position as the caller -> RHYTHM: check back to the raiser for information"
        return "in position, aggressor" if ip and pfr else "out of position - tighten up"
    if node_id == "knob.superstition":
        on = bool(g["superstition"].get("engaged"))
        return "ENGAGED - 666 Card Oracle feeding the OHF" if on else "off (display only / inert)"
    if node_id == "knob.beastfavor":
        bf = num(g["beast"].get("favor"))
        return ("THE BEAST FAVORS (%.2f) - chase draws" % bf if bf >= 0.66
                else ("a faint omen (%.2f)" % bf if bf > 0 else "quiet (0.00) - no omen influence"))
    if node_id == "knob.mode":
        if g["nn"].get("engaged"):
            return "NN driver is steering (the student plays)"
        if g["ultra"].get("engaged"):
            return "ULTRA is steering - flipping OHF<->NN by the music"
        return "OHF strategy is steering (the master plays)"
    if node_id == "memory.voice":
        n = g["voice_pending"]
        return "%d spoken note(s) await the AIL" % n if n else "voice feedback caught up"
    if node_id == "signal.opponents":
        if not g["hud"]:
            return "no HUD reads yet"
        tags = []
        for h in g["hud"][:3]:
            v = num(h.get("vpip")); tags.append("%s %.0f/%s" % (h.get("player", "?")[:6], v * 100 if v <= 1 else v,
                                                                 ("%.0f" % (num(h.get("pfr")) * 100)) if num(h.get("pfr")) <= 1 else "%.0f" % num(h.get("pfr"))))
        return "reads: " + ", ".join(tags)
    if node_id == "output.action":
        for a in ("raise", "call", "fold"):
            if num(s.get("f$" + a)) > 0:
                return "leaning %s%s" % (a.upper(), (" %.2f" % num(s.get("f$betsize"))) if a == "raise" else "")
        return "no decision at this instant"
    return ""


# ---- the synapse graph: knobs -> signals -> outputs (the connections) -----------------------
SYNAPSES = [
    ("knob.mode", "output.action", "the active engine (OHF/NN/ULTRA) produces the action"),
    ("knob.ultra", "knob.mode", "ULTRA flips OHF<->NN from the system-audio average"),
    ("knob.superstition", "knob.beastfavor", "superstition mode feeds the live omen resonance"),
    ("knob.beastfavor", "signal.beastfavor", "/api/beast -> the sb_beastfavor symbol"),
    ("signal.beastfavor", "output.action", "sb_beastfavor makes the OHF chase draws when the Beast favors"),
    ("signal.pot", "signal.spr", "pot + stack -> SPR (commitment)"),
    ("signal.spr", "output.action", "SPR gates get-it-in vs pot-control lines"),
    ("signal.prwin", "output.action", "equity drives value/bluff/fold"),
    ("signal.position", "output.action", "position gates the check-back rhythm + float lines"),
    ("signal.opponents", "output.action", "HUD reads classify opponents -> exploit lines"),
    ("memory.voice", "knob.mode", "the AIL turns spoken feedback into OHF/knob/code changes"),
]

NODES = ["knob.mode", "knob.nn_driver", "knob.ultra", "knob.superstition", "knob.beastfavor",
         "signal.pot", "signal.spr", "signal.prwin", "signal.position", "signal.beastfavor",
         "signal.opponents", "memory.voice", "output.action"]


def node_value(node_id, g):
    s = g["syms"]
    if node_id == "knob.mode":
        return "NN" if g["nn"].get("engaged") else ("ULTRA" if g["ultra"].get("engaged") else "OHF")
    if node_id == "knob.nn_driver":
        return bool(g["nn"].get("engaged"))
    if node_id == "knob.ultra":
        return bool(g["ultra"].get("engaged"))
    if node_id == "knob.superstition":
        return bool(g["superstition"].get("engaged"))
    if node_id in ("knob.beastfavor", "signal.beastfavor"):
        return num(g["beast"].get("favor"), num(s.get("sb_beastfavor")))
    if node_id == "signal.pot":
        return num(s.get("PotSize"))
    if node_id == "signal.spr":
        pot = num(s.get("PotSize")); stack = num(s.get("StackSize"))
        return round(stack / pot, 2) if pot > 0 else None
    if node_id == "signal.prwin":
        return num(s.get("prwin"))
    if node_id == "signal.position":
        return {"in_position": num(s.get("f$InPositionPost")) > 0, "pfr": num(s.get("f$WasPreflopRaiser")) > 0}
    if node_id == "signal.opponents":
        return g["hud"]
    if node_id == "memory.voice":
        return g["voice_pending"]
    if node_id == "output.action":
        return {"fold": num(s.get("f$fold")), "call": num(s.get("f$call")),
                "raise": num(s.get("f$raise")), "betsize": num(s.get("f$betsize"))}
    return None


def harmonize(g):
    nodes = []
    for nid in NODES:
        val = node_value(nid, g)
        nodes.append({"id": nid, "value": val, "ghost": ghost_node_inference(nid, val, g)})
    return {"ts_ms": int(time.time() * 1000), "hero": g["hero"], "board": g["board"],
            "betround": int(num(g["syms"].get("betround"))), "nodes": nodes,
            "synapses": [{"from": a, "to": b, "kind": k} for a, b, k in SYNAPSES]}


# ---- NN feature / action / knob spec --------------------------------------------------------
def feature_spec():
    """Unified feature/action space so NN training can build synapses to all values+knobs+settings."""
    features = [
        {"name": "betround", "type": "int", "range": [0, 4]},
        {"name": "prwin", "type": "float", "range": [0, 1]},
        {"name": "nouts", "type": "int", "range": [0, 21]},
        {"name": "pot_bb", "type": "float", "range": [0, 1000]},
        {"name": "spr", "type": "float", "range": [0, 100]},
        {"name": "stack_bb", "type": "float", "range": [0, 1000]},
        {"name": "in_position", "type": "bool"},
        {"name": "was_preflop_raiser", "type": "bool"},
        {"name": "heads_up", "type": "bool"},
        {"name": "have_strong_made", "type": "bool"},
        {"name": "have_one_pair", "type": "bool"},
        {"name": "have_big_draw", "type": "bool"},
        {"name": "opp_is_station", "type": "bool"},
        {"name": "opp_is_aggro", "type": "bool"},
        {"name": "sb_beastfavor", "type": "float", "range": [0, 1]},
        {"name": "opp_vpip", "type": "float", "range": [0, 1]},
        {"name": "opp_pfr", "type": "float", "range": [0, 1]},
        {"name": "opp_af", "type": "float", "range": [0, 10]},
    ]
    actions = [
        {"name": "action", "type": "categorical", "values": ["fold", "check", "call", "raise", "allin"]},
        {"name": "betsize_pct_pot", "type": "continuous", "range": [0, 2.0]},
    ]
    knobs = [  # the steering settings the AIL/NN may turn; each maps to a live endpoint
        {"name": "mode.autoplayer", "type": "binary", "endpoint": "/api/autoplayer"},
        {"name": "mode.nn_driver", "type": "binary", "endpoint": "/api/nn-driver"},
        {"name": "mode.ultra", "type": "binary", "endpoint": "/api/ultra"},
        {"name": "mode.superstition", "type": "binary", "endpoint": "/api/superstition"},
        {"name": "knob.beastfavor", "type": "continuous", "range": [0, 1], "endpoint": "/api/beast?favor="},
        {"name": "knob.cbet_freq", "type": "continuous", "range": [0, 1], "ohf": "f$CbetFreq"},
        {"name": "knob.style", "type": "categorical", "values": [0, 1, 2], "ohf": "f$Style"},
    ]
    return {"version": 1, "features": features, "actions": actions, "knobs": knobs,
            "synapses": [{"from": a, "to": b, "kind": k} for a, b, k in SYNAPSES]}


def store_state(state):
    try:
        import psycopg2
        c = psycopg2.connect(DSN); cur = c.cursor()
        cur.execute("CREATE TABLE IF NOT EXISTS synapse_state (id bigserial primary key, "
                    "ts_ms bigint, betround int, hero text, board text, state jsonb)")
        cur.execute("INSERT INTO synapse_state (ts_ms, betround, hero, board, state) VALUES (%s,%s,%s,%s,%s)",
                    (state["ts_ms"], state["betround"], state["hero"], state["board"], json.dumps(state)))
        cur.execute("DELETE FROM synapse_state WHERE id < (SELECT max(id)-5000 FROM synapse_state)")
        c.commit(); c.close()
    except Exception as e:
        print("[synapse] store error:", e, flush=True)


def speak(text):
    try:
        import subprocess
        subprocess.Popen([LILITH, text], creationflags=0x08000000)
    except Exception:
        pass


def render(state):
    lines = ["SYNAPSE MAP  hero[%s] board[%s] round %d" % (state["hero"] or "-", state["board"] or "-", state["betround"]), ""]
    for n in state["nodes"]:
        lines.append("  %-20s = %-22s | ghost: %s" % (n["id"], str(n["value"])[:22], n["ghost"]))
    lines.append("")
    lines.append("  synapses (%d connections):" % len(state["synapses"]))
    for sy in state["synapses"]:
        lines.append("    %s --%s--> %s" % (sy["from"], sy["kind"], sy["to"]))
    return "\n".join(lines)


def main():
    global BOT
    BOT = (_argval("--bot-url") or BOT).rstrip("/")
    if "--features" in sys.argv:
        spec = feature_spec()
        path = os.path.join(OUT_DIR, "synapse_features.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(spec, f, indent=2)
        print("wrote %s (%d features, %d actions, %d knobs, %d synapses)"
              % (path, len(spec["features"]), len(spec["actions"]), len(spec["knobs"]), len(spec["synapses"])))
        return
    if "--watch" in sys.argv:
        every = float(_argval("--every") or 3)
        do_speak = "--speak" in sys.argv
        print("[synapse] ghost online -> %s | inferring every %.0fs%s" % (BOT, every, "  [voiced]" if do_speak else ""), flush=True)
        last_spoke = 0.0
        cur_hand, cur_start_bal = "", None      # per-hand result tracking
        while True:
            g = gather()
            state = harmonize(g)
            store_state(state)
            # hand transition: bank the previous hand's net (stack delta between hand starts)
            hn, bal = g.get("handnumber") or "", g.get("hero_balance")
            if hn and hn != cur_hand:
                if cur_hand and cur_start_bal is not None and bal is not None:
                    net = record_hand_result(cur_hand, cur_start_bal, bal, state["ts_ms"])
                    if net is not None and net < 0:
                        msg = "you LOST %.2f on hand %s" % (-net, cur_hand)
                        print("[synapse] *** %s ***" % msg, flush=True)
                        if do_speak and time.time() - last_spoke > 15:
                            speak("The ghost notes: " + msg); last_spoke = time.time()
                cur_hand, cur_start_bal = hn, bal
            head = next((n["ghost"] for n in state["nodes"] if n["id"] == "output.action"), "")
            print("[synapse] r%d %s | %s" % (state["betround"], state["hero"] or "-", head), flush=True)
            if do_speak and head and head != "no decision at this instant" and time.time() - last_spoke > 20:
                speak("The ghost reads: " + head); last_spoke = time.time()
            time.sleep(every)
    state = harmonize(gather())
    if "--json" in sys.argv:
        path = os.path.join(OUT_DIR, "synapse_state.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(state, f, indent=2)
        print("wrote %s" % path)
        print(json.dumps(state, indent=2))
    else:
        print(render(state))


if __name__ == "__main__":
    main()
