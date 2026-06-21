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

try:
    import deep_thought                      # async LLM "deep thought" for any synapse point (over the bus)
except Exception:
    deep_thought = None
_dt_last_key = None                          # dedup: one deep thought per (hand, street, action)

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
           "f$betsize", "f$Style", "raischair", "nsuitedcommon", "f$Committed", "Raises"]


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
            "nchairs": ts.get("nchairs"), "table": ts.get("table") or "",
            "observer": bool(ts.get("observer")),
            "voice_pending": voice_pending, "hud": hud_rows}


# ---- per-hand result tracker: net = hero stack at start of next hand - start of this hand ----
# hiss_log_hands is win-biased (the ACR writer only logs hands the hero stayed in), so we derive
# EVERY hand's win/loss from the hero's between-hands stack delta. The AIL reads hand_results to
# synthesize voice feedback on LOSING hands (ail_feedback.py).
def record_hand_result(prev_hand, prev_start_bal, new_start_bal, ts_ms, bblind=1.0):
    if not prev_hand or prev_start_bal is None or new_start_bal is None:
        return None
    # Guard against garbage transitions. Require a REAL hand id and STAKE-PLAUSIBLE stacks/deltas.
    # Caps are relative to the big blind so they hold at any stake. These were 100k/20k BB which is
    # absurdly loose for BB-denominated tables (real stacks ~10-300 BB, real swings <60 BB); an OCR
    # misread of a 3733-BB stack on a 1bb table slipped through and logged a fake -3693 "loss" that
    # dominated the loss-weighting. Tightened to 2000-BB stacks / 1000-BB single-hand swings -- still
    # ~4-10x the deepest realistic stack, so legit deep/cooler hands pass while gross misreads don't.
    ph = str(prev_hand)
    if not (ph.isdigit() and len(ph) >= 6):
        return None
    bb = bblind if (bblind and bblind > 0) else 1.0
    bal_cap = bb * 2000.0
    if not (0 < prev_start_bal < bal_cap and 0 < new_start_bal < bal_cap):
        return None
    net = round(new_start_bal - prev_start_bal, 2)
    if abs(net) > bb * 1000.0:         # no single hand realistically swings >1000 BB -> garbage
        return None
    # You can't LOSE more than you sat down with this hand: a loss deeper than the starting stack
    # (+a blind of slack) is a rebuy/table-switch artifact, not a real result.
    if net < -(prev_start_bal + bb * 2.0):
        return None
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


# ================= THE BRAIN: INTUITION + DECISION PLAN + DECISION -> CURRENT DECIDED ACTION ===
# The single easy-API result. The introspection inputs (opponent_profile, gametype-matched) are
# harmonized into INTUITION; the multi-street PLAN forms over it; the engine DECISION is folded in;
# they resolve into ONE current_decided_action. Reads opponent_profile directly, so it works now and
# stays consistent once the OHF deep-rewire makes the engine decision itself intuition-driven.
PLANS = {0: "none", 1: "pot_control", 2: "value_three_streets", 3: "flat_then_bluff_scare",
         4: "check_raise_barrel", 5: "delayed_cbet", 6: "give_up", 7: "show_of_force"}

# the grown synapses that harmonize the WHOLE system -> intuition -> plan -> decided action.
INTRO_SYNAPSES = [
    # perception of them
    ("signal.introspection", "intuition.read", "per-villain rhythm/exploits/range -> the harmonized read"),
    ("signal.tilt", "signal.introspection", "recent-vs-baseline steam = the strongest exploit"),
    ("signal.timing", "signal.introspection", "bet-speed tell: does he have it when he's fast"),
    ("signal.range", "intuition.read", "card/holdings guess refines the read"),
    ("signal.donkfest", "intuition.persona", "a donk-heavy table -> cheap-in + heavy-value persona"),
    # perception of us
    ("signal.perception", "intuition.read", "how THEY perceive US (our table image) shapes our exploit"),
    ("signal.perception", "intuition.persona", "respect -> bluff more; they-think-we-bluff -> value thin"),
    ("signal.opponents", "intuition.read", "HUD base rates anchor the introspection confirmation"),
    # CONSIDERATIONS: the three temporal lenses the brain weighs
    ("consider.backward", "intuition.read", "BACKWARD: villain history + recent steam + our table image"),
    ("consider.present", "intuition.read", "PRESENT: the spot right now drives the exploit read"),
    ("consider.forward", "output.decided", "FORWARD: the plan + predicted-response lookahead confirm the pathway"),
    # the pineal gland: superstition + ultra's music harmonized as a bounded intuitive lean
    ("knob.superstition", "pineal.third_eye", "the Beast's favor / 666 omen feeds the third eye"),
    ("knob.ultra", "pineal.third_eye", "ULTRA's music (system-audio) feeds the third eye"),
    ("pineal.third_eye", "intuition.read", "aligned omens + music lean the read into the moment (bounded)"),
    # live advisor + recall
    ("knob.advice", "intuition.read", "the live claude advisor weights into the read"),
    ("memory.decisions", "intuition.read", "recall of similar past situations (decision_memory) sharpens the read"),
    ("compute.swiftsnake", "knob.advice", "swiftsnake's 32 cores evaluate pathways in parallel -> advice"),
    # intuition -> plan -> decided
    ("intuition.read", "plan.line", "intuition forms the multi-street plan over villain exploit-pathway buckets"),
    ("intuition.read", "intuition.persona", "the read picks the bot's counter-persona"),
    ("intuition.persona", "output.decided", "the adopted persona swings the decided action"),
    ("intuition.read", "signal.prediction", "we predict the villain's response to our decided action"),
    ("signal.prediction", "output.decided", "confirm the most profitable pathway before acting"),
    ("intuition.read", "output.decided", "intuition drives the decided action"),
    ("plan.line", "output.decided", "the plan steers the street's action toward the line"),
    ("output.action", "output.decided", "the OHF/NN decision is harmonized in -- exploit takes PRECEDENCE"),
    ("intelligence.gate", "output.decided", "the WISDOM gate vetoes an unwise/spewy line -- the final check"),
    ("output.decided", "memory.decisions", "every decided action is remembered to learn from later"),
    ("output.decided", "knob.advice", "the decided action is pushed to the live engine via the advice knobs"),
]


def _profile_for(cur, name, gt):
    cur.execute("""SELECT window_hands,cont_freq,aggr_index,fold_to_pressure,sd_strong_rate,
                   fastbet_tell,tilt,profile,exploits FROM opponent_profile
                   WHERE player=%s AND gametype=%s""", (name, gt))
    r = cur.fetchone()
    if not r:
        return {}
    return dict(window_hands=r[0], cont_freq=r[1], aggr_index=r[2], fold_to_pressure=r[3],
               sd_strong_rate=r[4], fastbet_tell=r[5], tilt=r[6], profile=r[7], exploits=(r[8] or {}))


def villain_and_table(g):
    """raischair villain profile + the donk-fest table read, in one DB pass."""
    s = g["syms"]; ts = g["ts"]
    gt = "plo" if ts.get("isomaha") else "nlhe"
    rc = int(num(s.get("raischair"), -1))
    vname = ""
    for p in (ts.get("players") or []):
        if p.get("chair") == rc:
            vname = (p.get("name") or "").strip(); break
    prof, ndonks = {}, 0
    try:
        import psycopg2
        c = psycopg2.connect(DSN); cur = c.cursor()
        if vname:
            prof = _profile_for(cur, vname, gt)
        for p in (ts.get("players") or []):
            nm = (p.get("name") or "").strip()
            if not p.get("seated") or not nm:
                continue
            pr = _profile_for(cur, nm, gt)
            ex = pr.get("exploits") or {}
            if pr and (pr.get("profile") in ("station", "fish")
                       or (pr.get("aggr_index") is not None and pr["aggr_index"] >= 0 and pr["aggr_index"] < 0.25)
                       or ex.get("never_folds")):
                ndonks += 1
        c.close()
    except Exception:
        pass
    return vname, gt, prof, (ndonks >= 3)


def compute_perception(g):
    """PERCEPTION: how the villain perceives US -- our table image -- so we can exploit their read of
    us. Our OWN profile IS our image (the aggregator profiles our username like any player): a tight/
    strong-showdown image -> they FOLD to us (we can bluff/steal more); a loose/wild or caught-light
    image -> they CALL us (value-bet thin, stop bluffing)."""
    ts = g["ts"]; gt = "plo" if ts.get("isomaha") else "nlhe"
    uc = ts.get("userchair", -1)
    name = ""
    for p in (ts.get("players") or []):
        if p.get("chair") == uc:
            name = (p.get("name") or "").strip(); break
    sp = {}
    if name:
        try:
            import psycopg2
            c = psycopg2.connect(DSN); cur = c.cursor()
            sp = _profile_for(cur, name, gt); c.close()
        except Exception:
            pass
    if not sp or num(sp.get("window_hands")) < 12:
        return {"known": False, "image": "unknown", "they_respect_us": False,
                "they_think_we_bluff": False, "exploit": "none"}
    img = sp.get("profile") or "unknown"
    aggr = num(sp.get("aggr_index"), -1); sd = num(sp.get("sd_strong_rate"), -1)
    respect = (img in ("nit", "tag")) or (sd >= 0 and sd > 0.65)
    bluffy = (img in ("lag", "maniac")) or (aggr >= 0 and aggr > 0.7 and sd >= 0 and sd < 0.45)
    exploit = "none"
    if respect and not bluffy:
        exploit = "leverage_respect_bluff"     # they fold to us -> bluff / steal / barrel more
    elif bluffy:
        exploit = "leverage_image_value"       # they call us light -> value-bet thin, stop bluffing
    return {"known": True, "image": img, "they_respect_us": bool(respect),
            "they_think_we_bluff": bool(bluffy), "exploit": exploit}


def compute_intuition(g, prof, donkfest, perception):
    s = g["syms"]; ex = (prof.get("exploits") or {})
    bb = num(s.get("bblind"), 1.0) or 1.0
    pot_bb = num(s.get("PotSize")) / bb
    known = bool(prof) and num(prof.get("window_hands")) >= 12
    exploit = "none"
    if ex.get("tilting"): exploit = "tilt_value"
    elif ex.get("never_folds"): exploit = "value_only"
    elif ex.get("keeps_firing"): exploit = "bluffcatch"
    elif ex.get("overfold") or ex.get("gives_up") or ex.get("folds_to_3bet"): exploit = "attack_fold"
    elif ex.get("fast_is_weak"): exploit = "attack_fast"
    elif ex.get("fast_is_strong") or ex.get("honest"): exploit = "respect"
    vs = -1.0
    if known and num(prof.get("aggr_index"), -1) >= 0:
        vs = 0.5 + (0.2 if ex.get("honest") else 0.0)
        if num(prof.get("sd_strong_rate"), -1) >= 0: vs += (num(prof.get("sd_strong_rate")) - 0.5) * 0.3
        if ex.get("keeps_firing"): vs -= 0.15
        vs = max(0.0, min(1.0, vs))
    aggr = 0.5
    if exploit == "attack_fold": aggr = 0.75
    elif exploit == "tilt_value": aggr = 0.72
    elif exploit == "value_only" and vs >= 0.6: aggr = 0.2
    committed = num(s.get("f$Committed")) > 0
    foldable = bool(ex.get("overfold") or ex.get("folds_to_3bet") or num(prof.get("fold_to_pressure"), -1) > 0.5)
    show_of_force = bool(pot_bb >= 12 and (foldable or committed))
    persona = "normal"
    if ex.get("never_folds"): persona = "tag_value"
    elif ex.get("overfold") or ex.get("folds_to_3bet"): persona = "maniac"
    elif ex.get("keeps_firing"): persona = "station"
    elif donkfest: persona = "fish_hunter"
    # PERCEPTION nudge: exploit how THEY see US. Respect -> we can bluff/steal more; if they think we
    # bluff -> value-bet thin and stop bluffing (they won't fold).
    pex = (perception or {}).get("exploit")
    if pex == "leverage_respect_bluff" and exploit == "none":
        exploit = "image_bluff"; aggr = max(aggr, 0.68)
    elif pex == "leverage_image_value":
        aggr = min(aggr, 0.6)
        if exploit == "none": exploit = "image_value"
    return {"known": known, "exploit": exploit, "villain_strength": round(vs, 3),
            "aggression": round(aggr, 3), "tilt": round(num(prof.get("tilt"), 0.0), 3),
            "show_of_force": show_of_force, "donkfest": donkfest, "persona": persona,
            "perception_exploit": pex, "confidence": 0.6 if known else 0.25}


def compute_plan(g, intuition):
    s = g["syms"]; br = int(num(s.get("betround")))
    strong = num(s.get("f$HaveStrongMade")) > 0
    draw = num(s.get("f$HaveBigDraw")) > 0
    suited_board = num(s.get("nsuitedcommon")) >= 3
    if intuition["show_of_force"]: code = 7
    elif strong and intuition["exploit"] != "bluffcatch": code = 2
    elif draw and suited_board: code = 3                       # flat then represent/bluff the flush scare
    elif intuition["exploit"] in ("attack_fold", "attack_fast"): code = 4
    elif intuition["exploit"] == "bluffcatch": code = 1
    elif intuition["exploit"] == "value_only" and not strong: code = 6
    else: code = 0
    return {"code": code, "label": PLANS[code], "street": br}


def predict_response(prof, action, intuition):
    """Predict the villain's RESPONSE distribution to OUR action, from their profile -- so we confirm
    the most profitable pathway before acting. (Fold equity is the whole game vs a bluff.)"""
    if action not in ("raise", "bet"):
        return {"note": "no villain decision vs %s" % action}
    ex = (prof.get("exploits") or {}) if prof else {}
    fp = num(prof.get("fold_to_pressure"), -1) if prof else -1
    pf = fp if fp >= 0 else 0.45
    if ex.get("overfold") or ex.get("folds_to_3bet"): pf = max(pf, 0.70)
    if ex.get("never_folds") or ex.get("tilting"): pf = min(pf, 0.15)
    cf = num(prof.get("cont_freq"), -1) if prof else -1
    pr = 0.30 if (ex.get("keeps_firing") or (cf >= 0 and cf > 0.6)) else 0.12
    if ex.get("never_folds"): pr = min(pr + 0.10, 0.40)
    pf = min(pf, 1.0 - pr)
    return {"fold": round(pf, 2), "call": round(max(0.0, 1.0 - pf - pr), 2), "raise": round(pr, 2)}


def resolve_action(g, intuition, plan, prof):
    """INTUITION + PLAN + DECISION + CONTEXT -> the CURRENT DECIDED ACTION.

    EXPLOITABLE PATTERNS TAKE PRECEDENCE: the OHF/NN engine only SUGGESTS the base action; a confident
    introspection exploit OVERRIDES it. Then we PREDICT the villain's response to the decided action and
    confirm it is the most profitable pathway -- a bluff the villain won't fold to is downgraded."""
    s = g["syms"]; bb = num(s.get("bblind"), 1.0) or 1.0
    f, cl, rz, bs = (num(s.get("f$fold")), num(s.get("f$call")), num(s.get("f$raise")), num(s.get("f$betsize")))
    pot = num(s.get("PotSize")); a2c = num(s.get("AmountToCall")); strong = num(s.get("f$HaveStrongMade")) > 0
    if rz > 0: action, size = "raise", bs
    elif cl > 0: action, size = "call", 0.0
    elif f > 0: action, size = "fold", 0.0
    else: action, size = "none", 0.0
    source = "engine"
    ex = intuition["exploit"]; conf = intuition["confidence"]; vstr = intuition["villain_strength"]
    if intuition["known"] and conf >= 0.5:
        if intuition["show_of_force"]:
            action, size, source = "raise", max(bs, pot), "exploit:show_of_force"
        elif ex in ("attack_fold", "attack_fast", "tilt_value", "image_bluff") and action in ("fold", "call", "none"):
            action, size, source = "raise", (bs or pot * 0.66), "exploit:" + ex
        elif ex == "bluffcatch" and action == "fold" and a2c > 0:
            action, size, source = "call", 0.0, "exploit:bluffcatch"
        elif ex in ("value_only", "image_value") and action == "raise" and vstr is not None and vstr >= 0.6:
            action, size, source = ("call" if a2c > 0 else "check"), 0.0, "exploit:no_bluff_vs_station"
    # PREDICT the villain's response and confirm the pathway is the most profitable one.
    predicted = predict_response(prof, action, intuition)
    pathway_profitable = True
    if action == "raise" and not strong and not intuition["show_of_force"] and predicted.get("fold", 1.0) < 0.20:
        # the exploit raise was a BLUFF, but he won't fold -> not profitable; fall back to the cheap line.
        action, size = ("call" if a2c > 0 else "check"), 0.0
        source += "->no_fold_predicted"
        predicted = predict_response(prof, action, intuition)
        pathway_profitable = False
    return {"action": action, "size_bb": round(size / bb, 2) if size else 0.0, "raw_betsize": round(size, 2),
            "exploit": ex, "plan": plan["label"], "persona": intuition["persona"],
            "predicted_response": predicted, "pathway_profitable": pathway_profitable,
            "source": source, "overridden": source != "engine", "confidence": conf}


def compute_considerations(g, prof, intuition, plan, current, perception):
    """CONSIDERATIONS: the brain weighs three TEMPORAL lenses before deciding.
       BACKWARD (what has happened: villain history, his recent steam, our table image),
       PRESENT  (the spot right now: intuition, exploit, range, confidence),
       FORWARD  (what comes next: the multi-street plan, our predicted-response lookahead,
                 the show-of-force / future-pot picture)."""
    backward = {
        "villain_history": (prof.get("profile") if prof else None),
        "villain_baseline_aggr": (round(num(prof.get("aggr_index"), -1), 3) if prof else -1),
        "recent_tilt": intuition.get("tilt", 0.0),
        "our_table_image": perception.get("image"),
        "they_perceive_us_as": perception.get("exploit"),
    }
    present = {
        "exploit": intuition.get("exploit"),
        "villain_strength_now": intuition.get("villain_strength"),
        "aggression_now": intuition.get("aggression"),
        "confidence": intuition.get("confidence"),
    }
    forward = {
        "plan": plan.get("label"),
        "predicted_response": current.get("predicted_response"),
        "pathway_profitable": current.get("pathway_profitable"),
        "show_of_force": intuition.get("show_of_force"),
    }
    return {"backward": backward, "present": present, "forward": forward}


def compute_pineal(g):
    """THE PINEAL GLAND (the third eye): harmonizes the decision with SUPERSTITION -- the Beast's favor /
    the 666 omen resonance -- and ULTRA MODE's MUSIC (the system-audio that drives ultra). A BOUNDED
    intuitive lean: when the omens + the music swell together, the brain leans into the moment (chase,
    press); when they're quiet, no influence. It colors the read, it never overrides it."""
    favor = num(g["beast"].get("favor"), num(g["syms"].get("sb_beastfavor")))
    superstition_on = bool(g["superstition"].get("engaged"))
    ultra = g["ultra"]; ultra_on = bool(ultra.get("engaged"))
    music = num(ultra.get("level", ultra.get("audio", 0.0)))     # ultra's system-audio average 0..1
    omen = favor if superstition_on else favor * 0.5             # the Beast/666 omen
    resonance = max(0.0, min(1.0, omen * 0.7 + (music * 0.3 if ultra_on else 0.0)))
    lean = 0.15 if resonance >= 0.66 else (0.07 if resonance >= 0.40 else 0.0)
    return {"beastfavor": round(favor, 3), "superstition": superstition_on, "ultra": ultra_on,
            "music": round(music, 3), "resonance": round(resonance, 3), "lean": lean,
            "read": ("the Beast favors -- seize it" if resonance >= 0.66
                     else ("a faint omen stirs" if resonance >= 0.40 else "the omens are quiet"))}


def _attach_deep_thought(g, gt, vname, intuition, plan, current, perception):
    """Read the latest async DEEP THOUGHT for this spot (non-blocking) and, on a KEY spot (an exploit
    override / show-of-force / low confidence), spawn a fresh one over the queue. Any synapse point can
    do this; here it's the decision node."""
    if deep_thought is None:
        return None
    global _dt_last_key
    s = g["syms"]; node = "decision." + gt
    thought = None
    try:
        r = deep_thought.recent(node, max_age_ms=25000)
        thought = r.get("thought") if r else None
        keyspot = bool(current.get("overridden") or intuition.get("show_of_force")
                       or num(intuition.get("confidence"), 1.0) < 0.4)
        dkey = (g.get("handnumber"), int(num(s.get("betround"))), current.get("action"), current.get("source"))
        if keyspot and dkey != _dt_last_key:
            _dt_last_key = dkey
            deep_thought.think(node, {"villain": vname, "intuition": intuition, "plan": plan,
                                      "perception": perception, "decided": current},
                               "Sharpest exploit of this villain here -- is our decided action the most profitable line?",
                               depth=("deep" if intuition.get("show_of_force") else "fast"))
    except Exception:
        pass
    return thought


def compute_intelligence(g, prof, intuition, plan, current, considerations):
    """The INTELLIGENCE layer -- the final WISDOM check that the brain's decided action is INTELLIGENT
    and WISE before it is committed. Catches spew, bad odds, inconsistency with the reads, and survival
    recklessness. Bounded veto: it can DOWNGRADE an unwise aggressive line to a safer one, but it never
    invents recklessness. Clever is not the same as wise; this layer is the conscience over the cleverness."""
    s = g["syms"]; bb = num(s.get("bblind"), 1.0) or 1.0
    action = current.get("action"); size_bb = num(current.get("size_bb"))
    strong = num(s.get("f$HaveStrongMade")) > 0
    draw = num(s.get("f$HaveBigDraw")) > 0
    a2c = num(s.get("AmountToCall")); pot = num(s.get("PotSize")); stack = num(s.get("StackSize"))
    pr = current.get("predicted_response") or {}
    concerns = []; wise = True
    # 1. don't commit big without equity OR fold equity (spew)
    if action == "raise" and size_bb * bb >= 0.66 * pot and not strong and not draw:
        fe = num(pr.get("fold"))
        if fe < 0.40:
            concerns.append("big raise, weak hand, only %.0f%% fold equity -- spewy" % (fe * 100)); wise = False
    # 2. don't call a big bet without a made hand or a draw (bad odds)
    if action == "call" and pot > 0 and a2c >= 0.5 * pot and not strong and not draw:
        concerns.append("calling %.1fx pot w/o made hand or draw -- bad odds" % (a2c / pot)); wise = False
    # 3. consistency: a confident ATTACK read but a passive action
    if intuition.get("exploit") in ("attack_fold", "attack_fast") and num(intuition.get("confidence")) >= 0.6 \
       and action in ("fold", "call"):
        concerns.append("read says ATTACK but the action is passive -- inconsistent")
    # 4. survival / ICM: a short stack committing big without a strong made hand
    if action == "raise" and stack > 0 and stack <= 12 * bb and not strong and size_bb * bb >= 0.5 * stack:
        concerns.append("short-stack big commit w/o a strong made hand -- survival risk"); wise = False
    adjusted = None
    if not wise and action == "raise":
        adjusted = {"action": ("call" if a2c > 0 else "check"),
                    "reason": "intelligence veto -- " + "; ".join(concerns)}
    return {"wise": wise, "score": round(max(0.0, 1.0 - 0.34 * len(concerns)), 2),
            "concerns": concerns, "adjusted": adjusted}


def brain(g):
    vname, gt, prof, donkfest = villain_and_table(g)
    perception = compute_perception(g)
    intuition = compute_intuition(g, prof, donkfest, perception)
    pineal = compute_pineal(g)
    if pineal["lean"]:                                           # the third-eye nudge (bounded)
        intuition["aggression"] = round(min(1.0, num(intuition.get("aggression"), 0.5) + pineal["lean"]), 3)
        intuition["pineal_lean"] = pineal["lean"]
    plan = compute_plan(g, intuition)
    s = g["syms"]
    decision = {"fold": num(s.get("f$fold")), "call": num(s.get("f$call")),
                "raise": num(s.get("f$raise")), "betsize": num(s.get("f$betsize"))}
    current = resolve_action(g, intuition, plan, prof)
    considerations = compute_considerations(g, prof, intuition, plan, current, perception)
    intelligence = compute_intelligence(g, prof, intuition, plan, current, considerations)
    if intelligence.get("adjusted"):                         # the wisdom gate vetoes an unwise line
        adj = intelligence["adjusted"]
        current["action"] = adj["action"]; current["size_bb"] = 0.0; current["raw_betsize"] = 0.0
        current["source"] = current.get("source", "") + " -> intelligence_veto"
        current["intelligence_veto"] = adj["reason"]
    dt = _attach_deep_thought(g, gt, vname, intuition, plan, current, perception)
    return {"ts_ms": int(time.time() * 1000), "handnumber": g.get("handnumber"),
            "betround": int(num(s.get("betround"))), "villain": vname, "gametype": gt,
            "villain_profile": (prof.get("profile") if prof else None),
            "perception": perception, "considerations": considerations, "pineal": pineal,
            "intelligence": intelligence, "deep_thought": dt, "intuition": intuition,
            "decision_plan": plan, "decision": decision, "current_decided_action": current}


def store_brain(b, history=True):
    """brain_state = the latest row (easy API). brain_log = a DEBUG HISTORY of every decided action
    (one row per real decision: action change or new betround) so we can replay/analyze later and
    improve. Bounded to ~20k rows."""
    try:
        import psycopg2
        c = psycopg2.connect(DSN); cur = c.cursor()
        cur.execute("CREATE TABLE IF NOT EXISTS brain_state (id int primary key, ts_ms bigint, "
                    "handnumber text, betround int, villain text, brain jsonb)")
        cur.execute("INSERT INTO brain_state (id,ts_ms,handnumber,betround,villain,brain) "
                    "VALUES (1,%s,%s,%s,%s,%s) ON CONFLICT (id) DO UPDATE SET ts_ms=EXCLUDED.ts_ms,"
                    "handnumber=EXCLUDED.handnumber,betround=EXCLUDED.betround,villain=EXCLUDED.villain,"
                    "brain=EXCLUDED.brain",
                    (b["ts_ms"], b["handnumber"], b["betround"], b["villain"], json.dumps(b)))
        if history:
            cur.execute("CREATE TABLE IF NOT EXISTS brain_log (id bigserial primary key, ts_ms bigint, "
                        "handnumber text, betround int, villain text, action text, exploit text, "
                        "plan text, source text, brain jsonb)")
            cda = b.get("current_decided_action", {})
            # dedup: only log when the decided action / betround / villain changed (a real new decision)
            cur.execute("SELECT handnumber, betround, action, source FROM brain_log ORDER BY id DESC LIMIT 1")
            prev = cur.fetchone()
            keynow = (b["handnumber"], b["betround"], cda.get("action"), cda.get("source"))
            if not prev or tuple(prev) != keynow:
                cur.execute("INSERT INTO brain_log (ts_ms,handnumber,betround,villain,action,exploit,plan,source,brain) "
                            "VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s)",
                            (b["ts_ms"], b["handnumber"], b["betround"], b["villain"], cda.get("action"),
                             cda.get("exploit"), cda.get("plan"), cda.get("source"), json.dumps(b)))
                cur.execute("DELETE FROM brain_log WHERE id < (SELECT max(id)-20000 FROM brain_log)")
        c.commit(); c.close()
    except Exception as e:
        print("[synapse] brain store error:", e, flush=True)


def harmonize(g):
    nodes = []
    for nid in NODES:
        val = node_value(nid, g)
        nodes.append({"id": nid, "value": val, "ghost": ghost_node_inference(nid, val, g)})
    # harmonize the introspection layer into INTUITION -> PLAN -> DECIDED ACTION (grown synapses)
    bn = brain(g)
    intu = bn["intuition"]; cda = bn["current_decided_action"]
    nodes.append({"id": "signal.introspection",
                  "value": {"villain": bn["villain"], "profile": bn["villain_profile"],
                            "tilt": intu["tilt"], "exploit": intu["exploit"]},
                  "ghost": (("%s: %s%s" % (bn["villain"] or "villain", intu["exploit"],
                            " (TILTING)" if intu["tilt"] >= 0.35 else "")) if intu["known"] else "no read yet")})
    per = bn["perception"]
    nodes.append({"id": "signal.perception", "value": per,
                  "ghost": (("they see us as %s -> %s" % (per["image"], per["exploit"]))
                            if per["known"] else "no table image yet")})
    con = bn["considerations"]
    nodes.append({"id": "consider.backward", "value": con["backward"],
                  "ghost": "looking back: %s%s" % (con["backward"]["villain_history"] or "no history",
                           " (STEAMING)" if con["backward"]["recent_tilt"] >= 0.35 else "")})
    nodes.append({"id": "consider.present", "value": con["present"],
                  "ghost": "right now: %s @ conf %s" % (con["present"]["exploit"], con["present"]["confidence"])})
    nodes.append({"id": "consider.forward", "value": con["forward"],
                  "ghost": "looking ahead: plan %s%s" % (con["forward"]["plan"],
                           " | SHOW OF FORCE" if con["forward"]["show_of_force"] else "")})
    pin = bn["pineal"]
    nodes.append({"id": "pineal.third_eye", "value": pin,
                  "ghost": "%s (resonance %.2f%s)" % (pin["read"], pin["resonance"],
                           ", music swelling" if pin["ultra"] and pin["music"] >= 0.5 else "")})
    nodes.append({"id": "intuition.read", "value": intu,
                  "ghost": "exploit %s | aggr %.2f | villain-strength %s%s" % (
                      intu["exploit"], intu["aggression"], intu["villain_strength"],
                      " | SHOW OF FORCE" if intu["show_of_force"] else "")})
    nodes.append({"id": "plan.line", "value": bn["decision_plan"],
                  "ghost": "plan: %s" % bn["decision_plan"]["label"]})
    intel = bn["intelligence"]
    nodes.append({"id": "intelligence.gate", "value": intel,
                  "ghost": ("wise (%.2f)" % intel["score"]) if intel["wise"]
                           else ("VETO: %s" % "; ".join(intel["concerns"])[:80])})
    nodes.append({"id": "output.decided", "value": cda,
                  "ghost": "DECIDED: %s%s via %s / %s%s" % (cda["action"].upper(),
                           (" %.2fbb" % cda["size_bb"]) if cda["size_bb"] else "", cda["exploit"], cda["plan"],
                           " [vetoed]" if cda.get("intelligence_veto") else "")})
    return {"ts_ms": int(time.time() * 1000), "hero": g["hero"], "board": g["board"],
            "betround": int(num(g["syms"].get("betround"))), "nodes": nodes, "brain": bn,
            "synapses": [{"from": a, "to": b2, "kind": k} for a, b2, k in (SYNAPSES + INTRO_SYNAPSES)]}


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
    if "--brain" in sys.argv:
        # the easy API: the harmonized INTUITION + DECISION PLAN + DECISION -> CURRENT DECIDED ACTION
        b = brain(gather()); store_brain(b)
        print(json.dumps(b, indent=2))
        return
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
        # per-hand result tracking. We only bank a hand's net when we actually OBSERVED the hero
        # playing it -- hero had hole cards, on the SAME table (blinds + seat count unchanged) across
        # the hand boundary. Otherwise the stack delta spans a table switch / observer view / a
        # flipped seat-mapping and is a phantom (the kind that logged a fake -172 then +170 next hand).
        cur_hand, cur_start_bal = "", None
        cur_had_cards = False                    # did we see hero's hole cards during cur_hand?
        cur_observer = False                     # was the bot OBSERVING during cur_hand? (no real hero)
        cur_bb, cur_nchairs = None, None         # table config captured at cur_hand's start
        cur_table = ""                           # table identity (tourney|name) at cur_hand's start
        while True:
            g = gather()
            state = harmonize(g)
            store_state(state)
            store_brain(state["brain"])   # the easy-API row: INTUITION + PLAN + DECISION + DECIDED ACTION
            hn, bal = g.get("handnumber") or "", g.get("hero_balance")
            bb_now = num(g["syms"].get("bblind"), 0) or None
            nch_now = g.get("nchairs")
            table_now = g.get("table") or ""
            observer_now = bool(g.get("observer"))
            hero_has_cards = len((g.get("hero") or "").split()) >= 2
            if hn and hn == cur_hand and hero_has_cards:
                cur_had_cards = True             # latch: hero was dealt in this hand
            if hn and hn == cur_hand and observer_now:
                cur_observer = True              # latch: we were OBSERVING -> the "hero" is some villain
            # hand transition: bank the previous hand's net (stack delta between hand starts)
            if hn and hn != cur_hand:
                if cur_hand and cur_start_bal is not None and bal is not None:
                    same_table = (cur_bb is not None and bb_now is not None
                                  and abs(cur_bb - bb_now) < 1e-9 and cur_nchairs == nch_now)
                    # Table identity closes the last gap: a switch between two SAME-config tables is
                    # invisible to the bb/nchairs check but shows here. Only blocks when BOTH ids are
                    # known (graceful: a missing id falls back to the config check, never over-blocks).
                    table_switched = bool(cur_table and table_now and cur_table != table_now)
                    if cur_had_cards and same_table and not table_switched and not cur_observer:
                        net = record_hand_result(cur_hand, cur_start_bal, bal, state["ts_ms"],
                                                 bblind=num(g["syms"].get("bblind"), 1.0))
                        if net is not None and net < 0:
                            msg = "you LOST %.2f on hand %s" % (-net, cur_hand)
                            print("[synapse] *** %s ***" % msg, flush=True)
                            if do_speak and time.time() - last_spoke > 15:
                                speak("The ghost notes: " + msg); last_spoke = time.time()
                    else:
                        why = ("observer mode (no real hero)" if cur_observer
                               else "no hero cards" if not cur_had_cards
                               else "table switch (%s->%s)" % (cur_table, table_now) if table_switched
                               else "table changed")
                        print("[synapse] skip hand %s result (%s) -- phantom guard" % (cur_hand, why), flush=True)
                cur_hand, cur_start_bal = hn, bal
                cur_had_cards = hero_has_cards    # seed the new hand
                cur_observer = observer_now       # seed the new hand
                cur_bb, cur_nchairs = bb_now, nch_now
                cur_table = table_now
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
