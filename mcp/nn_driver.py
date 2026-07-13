#!/usr/bin/env python3
"""nn_driver.py - drive the LIVE hiss.exe with the trained neural net, no rebuild.

The "special setting" is simply running this script: start it = the NN plays; stop it = the
OHF plays. It uses only the bot's existing HTTP API, so it can't touch the live binary.

Flow (turn-gated on ismyturn; the sym comes from the LIVE bot, not swiftsnake's /decide):
  1. poll http://127.0.0.1:27654/api/table-state  -> ismyturn (CACHED) + hero hole, board, bblind
  2. GET  http://127.0.0.1:27654/api/symbols?names=<86 NUMERIC_SYMBOLS> -> the full sym (local, DB-free)
  3. POST http://192.168.1.39:8088/nn-decide ({sym,hole,board,bblind}) -> {action, f$betsize, f$allin}
  4. GET  http://127.0.0.1:27654/api/action?do=<a>[&amount=<bb>]&force=1   -> the bot clicks it

The GATE polls /api/table-state, never /api/symbols. /api/symbols EVALUATES symbols on Hiss's HTTP
thread (racing the heartbeat), so polling it at 0.6s wedged that thread -- and a wedged endpoint made
the old gate silently read "not my turn" forever, with the autoplayer disengaged and nobody driving.
The heavy 86-symbol pull (incl. prwin) now happens once per TURN, not ~100x/min. See ismyturn().

Only /nn-decide is remote (pure inference, ~10ms); everything else is local, so a swamped
swiftsnake DB can't stall the NN. Run the bot with the AUTOPLAYER OFF so the NN drives.
  python nn_driver.py            # live: NN plays
  python nn_driver.py --dry-run  # read + decide + print, but DON'T click (safe test)
"""
import os, sys, json, math, time, subprocess, urllib.parse

def _argval(flag, default):
    if flag in sys.argv:
        i = sys.argv.index(flag)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return default
# Bot URL precedence: --bot-url <url>  >  $NN_BOT_URL  >  default. Hiss launches the driver with
# --bot-url http://127.0.0.1:<its own terminal port> when you engage it from the NN-driver button.
BOT    = _argval("--bot-url", os.environ.get("NN_BOT_URL", "http://127.0.0.1:27654"))
NN     = os.environ.get("NN_URL",        "http://192.168.1.39:8088/nn-decide")
POLL   = float(os.environ.get("NN_POLL_S", "0.6"))
DRY    = "--dry-run" in sys.argv

# The infoset the NN was trained on (features.py NUMERIC_SYMBOLS). The LIVE bot resolves all of
# these locally via /api/symbols, so we read the sym straight from the bot -- fast and reliable --
# instead of swiftsnake's /decide (which hangs under postgres load).
#
# Two corrections, both measured:
#
#  * The 92 -> 115 feature retrain happened; THIS LIST WAS NEVER UPDATED. The driver was sending 86
#    symbols to a features.py that expects 109, so 23 arrived as 0.0 on every live decision. Most are
#    dead in training anyway (harmless), but `obsbranch` (sd 2.29) and `aggressorchair` (sd 2.79) are
#    alive -- so the net was being handed z-scores of -1.54 and -1.13, values it never saw in
#    training, every single hand. All 23 are appended below (verified resolvable on the live bot).
#
#  * `prwin` is NOT sent any more. features.py overwrites it unconditionally with its own eval7
#    equity (features.py:288-294), so the bot's Monte-Carlo result was computed and then thrown away
#    -- and computing it means running the Monte-Carlo on Hiss's HTTP thread, the documented crash
#    vector. Dropping it costs nothing and removes the risk: identical decisions, less latency.
NUMERIC_SYMBOLS = ("handrank169,f$BoardWet,f$BoardDry,f$ScaryBoard,f$BoardHighCardFoldy,"
    "f$BoardHighCardSticky,f$BoardParched,HaveTopPair,HaveOverPair,HaveSet,HaveTwoPair,HaveFlush,"
    "HaveStraight,FlushPossible,StraightPossible,f$HaveStrongMade,f$HaveBigMade,f$HaveOnePair,"
    "f$HaveComboDraw,f$HaveBigDraw,f$HaveWeakDraw,betround,nplayersdealt,nopponentsplaying,"
    "f$InPositionPre,f$InPositionPost,f$HeadsUpPot,f$Is6Max,StackSize,f$EffStack,PotSize,"
    "AmountToCall,f$SPR,f$Committed,f$M,f$Mzone,f$DeepStack,f$ShortStack,f$PushFoldStack,"
    "f$ReshoveSpot,bblind,sblind,f$AnteTotal,f$Stage,tgi_players_remaining,f$NearBubble,f$InMoney,"
    "f$BubbleTighten,icm_fold,icm_callwin,icm_calllose,f$Opp_VPIP,f$Opp_PFR,f$Opp_AF,f$Opp_WTSD,"
    "f$Opp_Hands,f$Opp_Known,f$Opp_IsNit,f$Opp_IsLoose,f$Opp_IsStation,f$Opp_IsPassive,"
    "f$Opp_IsAggro,f$Opp_IsTAG,f$Opp_IsLAG,f$Opp_IsFish,f$Opp_IsManiac,f$Opp_Foldy,"
    "f$Opp_ThreeBetsLight,f$Opp_PotCommitted,lastraiseractiontime,f$TimingSaysWeak,"
    "f$TimingSaysStrong,Raises,Calls,Bets,BotRaisedBeforeFlop,BotRaisedOnFlop,BotRaisedOnTurn,"
    "f$Style,f$OpenBase,f$CbetFreq,f$ThreeBetBluffFreq,f$DoubleBarrel,"
    # --- the 23 the driver was silently omitting (brain / observer / exploit / introspection / knobs)
    "obsbranch,obsbranch_aggro,obsbranch_bluff,obsbranch_openrange,mischief_fire,aggressorchair,"
    "iamaggressor,isplo8,isomaha,exploit_overfold_raischair,exploit_neverfolds_raischair,"
    "exploit_keepsfiring_raischair,exploit_tilting_raischair,exploit_folds3bet_raischair,"
    "exploit_honest_raischair,intro_known_raischair,intro_foldpress_raischair,intro_tilt_raischair,"
    "intro_contfreq_raischair,intro_rangestrength_raischair,openai_knob_openrange,"
    "openai_knob_aggro,openai_knob_bluff,"
    # NOTE: do NOT ask for "nopponents". It was REMOVED from this fork's code-base, and requesting a
    # removed symbol pops a BLOCKING modal ("ERROR: outdated symbol") that freezes the heartbeat --
    # which is exactly what wedged /api/symbols. The live opponent count is carried by
    # `nopponentsplaying` (already in this list) and is aliased to `nopponents` for the NN below.
    "validator_ok,validator_confidence,"
    # Not a model feature -- the driver's own ICM sizing multiplier. Pulled in the SAME request rather
    # than a second mid-decision /api/symbols call on Hiss's symbol-evaluating HTTP thread.
    # features.py indexes by name, so an extra key it doesn't know about is simply ignored.
    "f$ICM_SizeMult")

# Use curl, not urllib: on this box something (Defender/firewall app-filtering) resets python's
# outbound LAN POSTs mid-body, but curl is reliable on every leg. Keeps the driver dependency-free.
def _get(url):
    out = subprocess.run(["curl", "-s", "--max-time", "6", url],
                         capture_output=True, text=True).stdout
    return json.loads(out)


def _post(url, payload):
    out = subprocess.run(["curl", "-s", "--max-time", "6", url, "-X", "POST",
                          "-H", "Content-Type: application/json", "-d", "@-"],
                         input=json.dumps(payload), capture_output=True, text=True).stdout
    return json.loads(out)


def _cards(arr):
    """keep only real 2-char cards (e.g. 'As'); drop '', '??', 'BACK', trailing blanks."""
    out = ""
    for c in (arr or []):
        if isinstance(c, str) and len(c) == 2 and c[0] in "23456789TJQKA" and c[1] in "cdhs":
            out += c
    return out


def seat_view(gs):
    """live /api/table-state -> the /decide seat-view. The CALLER gates on ismyturn (the bot's
    real turn signal); `toact` in table-state is unreliable (often -1) so we do NOT gate on it."""
    hero = gs.get("userchair", -1)
    players = gs.get("players", []) or []
    nchairs = gs.get("nchairs", len(players)) or len(players)
    if hero is None or hero < 0:
        return None                       # observer / not seated -> nothing to drive
    by = {p.get("chair"): p for p in players}
    h = by.get(hero, {})
    hole = _cards(h.get("cards"))
    if len(hole) < 4:
        return None                       # hole not readable yet
    board = _cards(gs.get("commonCards"))
    dealer = next((p["chair"] for p in players if p.get("dealer")), 0)
    occ = "".join("1" if (by.get(i, {}).get("seated")) else "0" for i in range(nchairs))
    act = "".join("1" if (by.get(i, {}).get("active")) else "0" for i in range(nchairs))
    lim = gs.get("limits", {}) or {}
    return {
        "hole": hole, "board": board, "userchair": hero, "dealer": int(dealer),
        "nchairs": nchairs, "occupied": occ, "active": act,
        "stack": float(h.get("balance", 0) or 0), "bet": float(h.get("bet", 0) or 0),
        "sblind": float(lim.get("sblind", 0) or 0), "bblind": float(lim.get("bblind", 1) or 1),
        "_handnumber": gs.get("handnumber", ""), "_board": board,
    }


def click(action, amount, hand=None, betround=None):
    """Send the action, STAMPED with the spot it was decided for.

    force=1 bypasses Hiss's ismyturn gate and the request then stays PENDING for up to 25 s. Hands
    finish in seconds, so without the stamp a fold decided for hand N could still be queued when hand
    N+1 deals -- and fire there, into a spot nobody decided anything about, as soon as a matching
    button appeared. (Seen live: a fold sat pending ~5 s against a Check|Raise bar that has no Fold
    button.) Hiss now drops any pending action whose hand or street is no longer the one on screen.
    """
    q = {"do": action, "force": "1"}
    if action in ("raise", "bet") and amount and amount > 0:
        q["amount"] = "%.2f" % amount
    if hand:
        q["hand"] = str(hand)
    if betround is not None:
        q["betround"] = str(int(betround))
    return _get(BOT + "/api/action?" + urllib.parse.urlencode(q))


def table_state():
    """The bot's published state. Raises on an unreachable/wedged bot -- callers must NOT swallow it.

    Everything the driver GATES on is served from here. /api/table-state is built from the values the
    heartbeat already cached, so it never evaluates symbols on Hiss's HTTP thread. /api/symbols DOES
    evaluate there, so it is reserved for the once-per-turn feature pull (below) -- never for polling.
    """
    return _get(BOT + "/api/table-state")


def ismyturn(gs):
    """The bot's real turn signal, read CACHED from /api/table-state (`toact` is unreliable).

    NEVER gate on /api/symbols?names=ismyturn (what this used to do). That endpoint evaluates on
    Hiss's HTTP thread; when it wedged, this function swallowed the exception and returned False --
    i.e. "not my turn", forever, with no log line. And because NN-driver mode disengages the
    autoplayer, NOBODY was driving: the bot sat on AT facing CHECK/RAISE and did nothing for a whole
    session (hand 2776921615). A dead gate must SCREAM, not quietly report "nothing to do".
    """
    return bool(gs.get("ismyturn", False))


def table_is_omaha(gs):
    """PLO/PLO8 guard [Emrald]: the NN is Hold'em-only, so on an Omaha table the autoplayer/OHF must
    drive -- the NN driver DEFERS. Cached booleans from /api/table-state (never prwin / pt_)."""
    return bool(gs.get("isomaha") or gs.get("isplo8") or gs.get("ispl"))


def brain_override(do, amount, sv):
    """PLAYER / EXPLOIT PRECEDENCE over the NN. The NN reads CARDS + GAMESTATE; the brain (synapse_map)
    reads the PLAYER -- introspection, HUD profiling, the observer STRATEGY BRANCH, the exploit, and
    mischief. Per Emrald the bot is PLAYER/EXPLOIT-focused, NOT card/gamestate-focused: so whenever the
    brain has ANY player-driven signal for this spot, it commands the NN's action (and odd mischief
    sizing); only when the brain is quiet does the NN's card-based action stand. Reads brain_state
    (synapse_map --watch); fresh (<4s) + same-hand only; graceful."""
    try:
        import psycopg2
        dsn = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")
        c = psycopg2.connect(dsn); cur = c.cursor()
        cur.execute("SELECT ts_ms, handnumber, brain FROM brain_state WHERE id=1")
        r = cur.fetchone(); c.close()
        if not r:
            return do, amount, ""
        ts, hn, b = r
        if (int(time.time() * 1000) - ts) > 4000:
            return do, amount, ""                                  # stale brain -> NN stands
        if sv.get("_handnumber") and hn and str(hn) != str(sv["_handnumber"]):
            return do, amount, ""                                  # different hand -> NN stands
        b = b or {}
        cda = b.get("current_decided_action") or {}
        obs = b.get("observer_strategy") or {}
        mis = b.get("mischief") or {}
        src = cda.get("source", "") or ""
        branch = obs.get("branch")
        mis_fired = isinstance(mis, dict) and mis.get("fired")
        # ANY player-read steers the NN: a confident exploit, a feeler, a wisdom veto, an observer
        # strategy branch, or a mischief prank -- this is the player/exploit focus over the card model.
        player_signal = (src.startswith("exploit:") or src.startswith("feeler")
                         or "intelligence_veto" in src or cda.get("overridden")
                         or mis_fired or (branch and branch != "NORMAL"))
        if not player_signal:
            return do, amount, ""                                  # no player read -> the NN's card action stands
        ba = cda.get("action")
        if ba == "bet":
            ba = "raise"
        if ba in ("raise", "call", "check", "fold", "allin"):
            namt = float(cda.get("size_bb") or 0) if ba == "raise" else 0
            tag = src if (src and src != "engine") else (("obs:" + branch) if branch and branch != "NORMAL" else "brain")
            if mis_fired:
                tag += "/mischief:" + str(mis.get("kind"))
            if ba != do or (ba == "raise" and namt and abs(namt - (amount or 0)) > 0.01):
                return ba, namt, "  [brain %s -> %s]" % (cda.get("exploit") or branch or "player", tag)
    except Exception:
        pass
    return do, amount, ""


### ---------------------------------------------------------------------------------------------
### Engine attribution. hand_results records what each hand WON; nothing recorded WHO PLAYED IT,
### so "does the NN beat the OHF on real tables?" was unanswerable -- the production question had
### never been asked. These two writes are the missing half:
###
###   bot_engine_beat   a heartbeat every ~10s while this driver is alive  -> the NN was in charge
###                     during this window (covers hands the NN was dealt but never had to act in)
###   bot_nn_decision   one row per actual decision                        -> per-hand ground truth
###
### measure_live.py joins these against hand_results to split bb/100 by engine. Best-effort only:
### a dead DB must never stop the bot from playing, so every failure here is swallowed after one
### loud line. DRY runs write nothing -- they don't move money, so they must not pollute the sample.
_PG = {"conn": None, "warned": False}

def _pg_exec(sql, params):
    if DRY:
        return
    try:
        import psycopg2
        if _PG["conn"] is None or _PG["conn"].closed:
            dsn = os.environ.get("HISS_PG_DSN",
                                 "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")
            _PG["conn"] = psycopg2.connect(dsn)
            _PG["conn"].autocommit = True
        with _PG["conn"].cursor() as cur:
            cur.execute(sql, params)
    except Exception as e:
        if not _PG["warned"]:
            print("[nn_driver] WARNING: cannot record engine attribution (%s) -- the bot plays on, "
                  "but these hands will not be measurable" % e, flush=True)
            _PG["warned"] = True
        try:
            if _PG["conn"] is not None:
                _PG["conn"].close()
        except Exception:
            pass
        _PG["conn"] = None


def record_beat():
    _pg_exec("INSERT INTO bot_engine_beat (ts_ms, engine, pid) VALUES (%s, 'nn', %s)",
             (int(time.time() * 1000), os.getpid()))


def record_decision(handnumber, betround, action, amount, note):
    _pg_exec("INSERT INTO bot_nn_decision (handnumber, ts_ms, betround, action, amount, note) "
             "VALUES (%s, %s, %s, %s, %s, %s)",
             (str(handnumber), int(time.time() * 1000), betround, action,
              float(amount or 0), (note or "").strip() or None))


def decide_and_act(gs):
    """Returns True if we read the seat and clicked (or decided, in DRY); False if the hole
    isn't readable yet so the CALLER should retry within the same turn (do NOT skip the turn --
    that silently drops the action and looks like a phantom 'sit out / between hands').

    Takes the table-state the caller already gated on -- one read per poll, and the decision is made
    against the exact same snapshot we saw the turn in."""
    if table_is_omaha(gs):
        print("[nn_driver] PLO/PLO8 table -> NN gated off; deferring to the autoplayer/OHF", flush=True)
        return True            # not the NN's job on Omaha; the autoplayer runs the OHF this turn
    sv = seat_view(gs)
    if not sv:
        return False                       # hole not scraped yet -> retry, don't consume the turn
    # Sym straight from the live bot (local, DB-free) -- not swiftsnake's /decide.
    sym = _get(BOT + "/api/symbols?names=" + NUMERIC_SYMBOLS)

    # features.py computes hand equity as `hole vs sym["nopponents"]`, defaulting to 1 when absent --
    # so the bot has been acting on HEADS-UP equity in a nine-handed game (AJo reads ~0.55 when its
    # true five-way equity is ~0.25). The symbol literally named `nopponents` was REMOVED from this
    # fork and asking for it pops a blocking modal that freezes the heartbeat, so alias the live
    # count from `nopponentsplaying`, which is the same thing: opponents still in the hand.
    #
    # Safe to send today: nn_decide clamps it back to 1 unless the champion was actually TRAINED on a
    # real opponent count (the SIGHTED marker). So the current heads-up-trained champion is
    # unaffected, and multiway equity switches on by itself the day a sighted champion is promoted.
    if isinstance(sym, dict) and "nopponentsplaying" in sym:
        try:
            sym["nopponents"] = int(float(sym["nopponentsplaying"]))
        except (TypeError, ValueError):
            pass
    nn = _post(NN, {"sym": sym, "hole": sv["hole"], "board": sv["board"], "bblind": sv["bblind"]})

    # NEVER DEFAULT TO FOLD.
    #
    # This was `a = nn.get("action", "fold")`. nn_decide.py returns {"error": ...} on ANY exception --
    # no champion loaded, a feature-dimension mismatch, a torch fault -- so a single bad promotion
    # turned the bot into a fold-bot that folded 100% of hands facing a bet, while the log line
    # printed a perfectly normal-looking "-> NN: fold". Silent, and catastrophic.
    #
    # A brain that cannot answer must not get to act. Hand the turn back to the OHF autoplayer, which
    # is a competent player in its own right, and say so loudly.
    if not isinstance(nn, dict) or "action" not in nn:
        print("[nn_driver] !! NN GAVE NO ACTION (%s) -- NOT acting. Handing this turn back to the "
              "OHF autoplayer. The NN is not driving until this is fixed."
              % (nn.get("error") if isinstance(nn, dict) else type(nn).__name__), flush=True)
        _get(BOT + "/api/autoplayer?on=1")
        return True                        # turn consumed -- by the OHF, not by a blind fold
    a = nn["action"]
    bb = float(nn.get("f$betsize", 0) or 0)
    allin = ((float(nn.get("f$allin", 0) or 0)) > 0) or a == "allin"
    do = "allin" if allin else ("raise" if a == "raise" else a)
    amount = bb if (do == "raise") else 0
    # The NN's "f$betsize" is a POT-FRACTION BET, not a raise-TO, despite the field name: nn_decide.py
    # returns (bucket_fraction * PotSize) / bb. Consuming it as a raise-TO is why the bot NEVER RAISED.
    # Preflop the pot is only ~1.5bb, so even a 1/3-pot bucket yields ~0.5bb -- always below the 2bb
    # legal minimum -- and the guard further down turned EVERY raise into a call (observed 100% of the
    # time: "raise 0.54<min 2.00 -> call"). With no raise ever sent, the two-successive-clicks raise
    # progression could never fire either. Convert the fraction to a real raise-TO first:
    #     raise_to = highest_bet + frac * (pot + amount_to_call)
    # i.e. the standard pot-fraction raise. Everything downstream (depth x ICM sizing, the legal-min
    # clamp, the allin check) then operates on an actual raise-TO, as it always assumed it did.
    if do == "raise" and amount > 0:
        _bbl0   = float(sv.get("bblind", 1) or 1) or 1.0
        _pot    = float(sym.get("PotSize", 0) or 0) / _bbl0        # money -> bb
        _tocall = float(sym.get("AmountToCall", 0) or 0) / _bbl0
        _mybet  = float(sv.get("bet", 0) or 0) / _bbl0
        if _pot > 0:
            _frac  = amount / _pot                                  # recover the NN's pot fraction
            amount = (_mybet + _tocall) + _frac * (_pot + _tocall)  # -> raise-TO, in bb
    do, amount, note = brain_override(do, amount, sv)   # exploit precedence steers the NN too
    # STACK-DEPTH-PROPORTIONAL sizing (leverage) [Emrald]: a fixed bb raise is nothing to a deep stack
    # and everything to a short one. Scale the NN's raise-TO by the EFFECTIVE stack depth; when SHORT
    # (<=12bb eff) a raise only commits us, so JAM instead. Mirrors the OHF f$DepthSizeMult. The legal
    # min-raise / allin(>=stack) clamps below still apply to the scaled amount.
    if do == "raise" and amount > 0:
        _bbl = float(sv.get("bblind", 1) or 1) or 1.0
        _eff_bb = (float(sv.get("bet", 0) or 0) + float(sv.get("stack", 0) or 0)) / _bbl
        if _eff_bb <= 12:
            do, amount, note = "allin", 0, "  (<=12bb eff -> jam, not a small raise)"
        else:
            _dm = 1.50 if _eff_bb >= 250 else 1.35 if _eff_bb >= 150 else 1.20 if _eff_bb >= 100 else 1.10 if _eff_bb >= 60 else 1.0
            # ICM chip-value multiplier: the SAME f$ICM_SizeMult the OHF uses (light symbol, no prwin/pt_)
            # -> the NN now sizes by depth x ICM, matching the OHF. 1.0 in cash/freeroll/early.
            # Comes from the once-per-turn sym pull now, not a second HTTP round trip.
            try:
                _icm = float(sym.get("f$ICM_SizeMult", 1.0) or 1.0)
            except Exception:
                _icm = 1.0
            if _icm <= 0:
                _icm = 1.0
            _mult = _dm * _icm
            if abs(_mult - 1.0) > 0.01:
                amount *= _mult
                note = "  (size x%.2f: depth %.2f@%.0fbb, icm %.2f)" % (_mult, _dm, _eff_bb, _icm)
    if do == "raise":
        # EVERYTHING IN THIS BLOCK IS IN BIG BLINDS.
        #
        # It used to mix units: `amount` is a raise-TO in bb (see the pot-fraction conversion above),
        # but min_raise_to / eff_max were built from sv.bet, sym.AmountToCall and sv.stack, which are
        # in TABLE MONEY. Those two only coincide while bblind == 1.0 -- which is exactly the current
        # ACR override, so the bug sat there armed and invisible. (The quarter-pot line divided by bbl
        # while its neighbours did not: the units were confused, not chosen.)
        #
        # The moment bblind isn't 1 -- a real-money cash table, a tourney whose tablemap reads true
        # chips, or your own documented gotcha where a restart didn't re-POST the bb=1.0 override --
        # EVERY raise would have become an all-in shove (e.g. at bb=0.02: a 3.5bb raise-to compared
        # against an eff_max of 2.00 in money -> 3.5 >= 2.0 -> jam). Convert once, compare in bb.
        bbl     = float(sv.get("bblind", 1) or 1) or 1.0
        my_bet  = float(sv.get("bet", 0) or 0) / bbl                 # money -> bb
        to_call = float(sym.get("AmountToCall", 0) or 0) / bbl        # money -> bb
        stack   = float(sv.get("stack", 0) or 0) / bbl                # money -> bb
        pot     = float(sym.get("PotSize", 0) or 0) / bbl             # money -> bb

        highest = my_bet + to_call                    # current highest bet, relative to hero (bb)
        min_raise_to = highest + max(1.0, to_call)    # last increment ~ to_call; never < one bb
        if highest <= 1.0 + 1e-6:                     # only the blinds are in -> opening raise
            min_raise_to = max(min_raise_to, 2.0)
        eff_max = my_bet + stack                      # a full shove, in bb

        # A raise with NO SIZE is a silent no-op: click("raise", 0) sends /api/action?do=raise with no
        # amount, and Hiss then only pops the Raise Options panel open and never confirms it. That
        # happens whenever PotSize scrapes as 0, because the NN's f$betsize is frac * PotSize / bb.
        # The NN's ACTION is the signal; if its SIZE is missing, give it the legal minimum rather than
        # throwing the decision away.
        if amount <= 0:
            amount = min_raise_to
            note = "  (no size from the NN -> min-raise %.2fbb)" % amount

        # Postflop OPENING bet (nothing to call -> a bet, not a raise): size it at least a QUARTER
        # of the pot (Emrald's rule). Only bumps the size up; raises facing a bet keep their sizing.
        betround = float(sym.get("betround", 0) or 0)
        if betround >= 2 and to_call <= 0.001:
            quarter_pot = 0.25 * pot
            if quarter_pot > amount:
                amount = quarter_pot
                note = "  (postflop bet -> >=1/4 pot %.2fbb)" % amount

        # Too small to be legal -> MIN-RAISE, don't abandon the raise. The NN's ACTION (raise) is the
        # signal; its SIZE is the part that's unreliable. The old rule ("below the minimum -> fall back
        # to call/check") silently converted the NN's aggression into passivity -- and because the NN's
        # size was ALWAYS below the minimum preflop (see the pot-fraction fix above), the bot never
        # raised a single hand. A min-raise is always legal, so honour "open >= 2bb / re-raise >= last
        # increment" by raising TO that minimum. If we cannot even afford it, the allin check below
        # turns it into the jam it effectively is. [Emrald: raise>=stack = all-in]
        if amount < min_raise_to - 1e-6:
            note = "  (NN size %.2f < min %.2f -> min-raise)" % (amount, min_raise_to)
            amount = min_raise_to

        # The phone keypad only accepts 0.5 increments (6.5 or 7, not 6.6). CEIL, never round: this
        # snap used to run AFTER the min-raise clamp and round to NEAREST, so a min_raise_to of 2.2bb
        # became 2.0bb -- an illegal under-raise, rejected by the table or landing as a call. Rounding
        # a legal minimum DOWN is never safe; rounding up always is.
        amount = math.ceil(amount * 2 - 1e-9) / 2.0

        # Check the shove AFTER the snap, so ceiling can't push us over our own stack unnoticed.
        if amount >= eff_max - 1e-6:
            do, amount, note = "allin", 0, "  (raise>=stack -> allin)"
    # Reconcile call/check with the actual spot, using AmountToCall (the bet state), NOT fckra.
    #
    # fckra is now published every heartbeat and is printed below purely as DIAGNOSTICS -- do NOT gate
    # decisions on it. Measured live, it is not trustworthy: it reports "FCKA", i.e. Call AND Check
    # simultaneously (mutually exclusive), and it NEVER reports R even in spots the bot can and does
    # raise (raises execute through the two-successive-clicks "RaiseOptions" label path, which needs no
    # R button at all). Gating on it therefore (a) silently disabled this call/check reconciliation
    # whenever both C and K were claimed, and (b) turned every intended raise into an ALL-IN SHOVE.
    # Both were worse than the bug they were meant to fix. Until the button label/rect detection is
    # recalibrated (the known i6/Raise mis-calibration), AmountToCall stays the authority here.
    fckra = (gs.get("fckra") or "").upper()          # diagnostics only -- see above
    amt_to_call = float(sym.get("AmountToCall", 0) or 0)
    if do == "call" and amt_to_call <= 0.001:
        do, note = "check", note + "  (call->check: nothing to call)"
    elif do == "check" and amt_to_call > 0.001:
        do, note = "call", note + "  (check->call: facing a bet)"
    elif do == "fold" and amt_to_call <= 0.001:
        # Folding a free option is strictly dominated -- and worse, the bar in that spot
        # (Check | Raise Options) has no Fold button at all, so the request never lands: Hiss
        # answers "button not clickable yet; keeping pending" every heartbeat and the bot just
        # sits there until the human clicks (hand 2777793688, 7h Jd in the BB).
        do, note = "check", note + "  (fold->check: nothing to call)"
    print("[nn_driver] %s hole=%s board=%s -> NN: %s%s%s  [btns=%s]  (val=%s)" %
          (sv["_handnumber"], sv["hole"], sv["board"] or "-", do,
           (" to %.1fbb" % amount) if amount else "", note, fckra or "-", nn.get("value")), flush=True)
    record_decision(sv["_handnumber"], gs.get("betround"), do, amount, note)
    if not DRY:
        click(do, amount)
    return True


def main():
    print("[nn_driver] %s  bot=%s nn=%s" % ("DRY-RUN" if DRY else "LIVE", BOT, NN), flush=True)
    print("[nn_driver] gating on ismyturn (per-turn latch; retries until the hole scrapes). Waiting for your turn...", flush=True)
    acted_this_turn = False     # acted on THIS turn already? re-armed on the falling edge of ismyturn
    last_act = 0.0
    wait_start = 0.0            # when the current "my turn but hole unreadable" wait began
    warned = False
    unreachable = 0             # consecutive failed state reads -- a dead gate must never look idle
    last_beat = 0.0             # engine-attribution heartbeat (see record_beat)
    while True:
        try:
            if (time.monotonic() - last_beat) > 10.0:
                record_beat()
                last_beat = time.monotonic()
            gs = table_state()      # raises if the bot is down/wedged -> caught below, LOUDLY
            if unreachable:
                print("[nn_driver] bot reachable again after %d failed poll(s) -- driving resumed"
                      % unreachable, flush=True)
                unreachable = 0
            now = ismyturn(gs)
            if now:
                # Act exactly once per turn, but if the hole isn't readable yet keep RETRYING
                # within the turn (decide_and_act returns False) instead of skipping it. The 1s
                # floor is just a glitch guard against a same-tick double-fire.
                if not acted_this_turn and (time.monotonic() - last_act) > 1.0:
                    if decide_and_act(gs):
                        acted_this_turn = True
                        last_act = time.monotonic()
                        wait_start = 0.0
                        warned = False
                    else:
                        t = time.monotonic()
                        if wait_start == 0.0:
                            wait_start = t
                        elif (t - wait_start) > 6.0 and not warned:
                            print("[nn_driver] my turn but hole unreadable >6s -- genuinely between hands / sat out?", flush=True)
                            warned = True
            else:
                acted_this_turn = False   # turn ended -> re-arm for the next hand
                wait_start = 0.0
                warned = False
        except Exception as e:
            # The bot is unreachable or its HTTP thread is wedged. This is NOT "not my turn" -- with
            # the autoplayer disengaged, nothing else is driving, so the bot is folding its arms at a
            # live table RIGHT NOW. Say so, every time, and escalate: silence here is what let a whole
            # session pass with zero actions.
            unreachable += 1
            print("[nn_driver] !! CANNOT READ BOT STATE (%s: %s) -- poll %d. The bot is NOT being "
                  "driven and the autoplayer is disengaged; NO ONE is acting."
                  % (type(e).__name__, e, unreachable), flush=True)
            if unreachable in (5, 25) or (unreachable and unreachable % 100 == 0):
                print("[nn_driver] !! %d consecutive failed polls (~%.0fs). Hiss's HTTP thread is "
                      "likely WEDGED -- restart Hiss; the NN driver cannot act until it responds."
                      % (unreachable, unreachable * POLL), flush=True)
        time.sleep(POLL)


if __name__ == "__main__":
    main()
