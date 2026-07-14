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
import os, sys, re, json, math, time, subprocess, urllib.parse, urllib.request

def _argval(flag, default):
    if flag in sys.argv:
        i = sys.argv.index(flag)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return default
def _discover_bot_url():
    """Hiss binds a DIFFERENT terminal port per session (27654, then 27655, ...) and publishes the one
    it actually got to Release/logs/terminal_port.txt. When parse_guard restarts a wedged Hiss, the new
    instance can land on a different port -- and anything still aimed at 27654 polls a dead socket
    forever, silently driving nothing. Hiss passes --bot-url when it launches us, so this only matters
    for a hand-started driver; but a hand-started driver aimed at the wrong port is exactly the kind of
    quiet nothing that is hard to notice. Read the file Hiss already writes."""
    try:
        pf = os.path.join(os.environ.get("HISS_REPO", r"C:\www\openholdembot_old"),
                          "Release", "logs", "terminal_port.txt")
        with open(pf) as f:
            return "http://127.0.0.1:%d" % int(f.read().strip())
    except Exception:
        return "http://127.0.0.1:27654"


# Bot URL precedence: --bot-url <url>  >  $NN_BOT_URL  >  the port Hiss published  >  27654.
# Hiss launches the driver with --bot-url http://127.0.0.1:<its own terminal port> from the
# NN-driver button, so that always wins when it starts us.
BOT    = _argval("--bot-url", os.environ.get("NN_BOT_URL") or _discover_bot_url())
NN     = os.environ.get("NN_URL",        "http://192.168.1.39:8088/nn-decide")
POLL   = float(os.environ.get("NN_POLL_S", "0.6"))
DRY    = "--dry-run" in sys.argv

# Must match kDesperationStackBB / kDesperationRaiseBB in Shared/MagicNumbers/MagicNumbers.h -- the
# OHF autoplayer and this driver have to behave the same way at this depth, or "which engine was
# driving" silently becomes a strategy difference.
DESPERATION_STACK_BB = 2.0    # below this many BB: never fold
DESPERATION_RAISE_BB = 3.0    # what to shove; the table caps it at our stack


def _bot_port():
    """The Hiss instance we drive. brain_state is keyed by it, so we read OUR bot's brain, not the
    other instance's (see brain_override)."""
    m = re.search(r":(\d+)", BOT)
    return int(m.group(1)) if m else 27654

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

# curl vs urllib, and why it is SPLIT.
#
# The LAN leg genuinely needs curl: on this box something (Defender / firewall app-filtering) resets
# python's outbound LAN POSTs mid-body. That is a real, diagnosed problem and it stays on curl.
#
# But that decision was applied to EVERY leg, including the three localhost calls (table-state,
# symbols, action) -- which are polled continuously. Measured: ~25.7 ms for a curl subprocess vs
# ~12.5 ms in-process, so ~13 ms of pure process-spawn overhead per call, and roughly 6,000 curl
# processes an hour. Loopback never had the firewall problem the LAN leg has; it was collateral.
#
# So: localhost goes in-process, the LAN keeps curl. And loopback still falls back to curl if urllib
# fails, so the worst case is the behaviour we already had.
def _curl_get(url):
    out = subprocess.run(["curl", "-s", "--max-time", "6", url],
                         capture_output=True, text=True).stdout
    return json.loads(out)


def _get(url):
    if url.startswith("http://127.0.0.1") or url.startswith("http://localhost"):
        try:
            with urllib.request.urlopen(url, timeout=6) as r:
                return json.loads(r.read().decode("utf-8", "replace"))
        except Exception:
            pass          # loopback hiccup -> fall back to the curl path we always used
    return _curl_get(url)


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
        # Reuse the pooled connection (see _pg_conn) instead of opening a NEW postgres connection on
        # every decision. A TCP connect + auth inside the act-now path is latency the bot spends while
        # a clock is running, and it silently swallowed every failure -- if psycopg2 was missing or PG
        # was down, the brain simply never fired and nothing anywhere said so.
        # brain_state is keyed by the Hiss PORT (one brain row per instance). Reading the old shared id=1
        # made two instances read each OTHER's brain: the row carried the other table's handnumber, the
        # same-hand gate below rejected it, and the brain silently stopped steering this bot.
        r = _pg_query_one("SELECT ts_ms, handnumber, brain FROM brain_state WHERE id=%s", (_bot_port(),))
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
_PG = {"conn": None, "warned": False, "read_warned": False}


def _pg_conn():
    """One pooled connection, opened lazily. Everything that touches postgres in this driver goes
    through here -- the attribution writes AND the brain-state read that runs inside the decision."""
    import psycopg2
    if _PG["conn"] is None or _PG["conn"].closed:
        dsn = os.environ.get("HISS_PG_DSN",
                             "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")
        _PG["conn"] = psycopg2.connect(dsn)
        _PG["conn"].autocommit = True
    return _PG["conn"]


def _pg_drop():
    try:
        if _PG["conn"] is not None:
            _PG["conn"].close()
    except Exception:
        pass
    _PG["conn"] = None


def _pg_exec(sql, params):
    if DRY:
        return
    try:
        with _pg_conn().cursor() as cur:
            cur.execute(sql, params)
    except Exception as e:
        if not _PG["warned"]:
            print("[nn_driver] WARNING: cannot record engine attribution (%s) -- the bot plays on, "
                  "but these hands will not be measurable" % e, flush=True)
            _PG["warned"] = True
        _pg_drop()


def _pg_query_one(sql, params=None):
    """Read one row on the pooled connection. Used by brain_override, which runs INSIDE the decision
    path -- so it must never open a fresh connection there, and must never fail silently forever."""
    try:
        with _pg_conn().cursor() as cur:
            cur.execute(sql, params)
            return cur.fetchone()
    except Exception as e:
        if not _PG["read_warned"]:
            print("[nn_driver] WARNING: cannot read brain_state (%s) -- the brain will NOT steer the "
                  "NN until this is fixed; the net's own action stands." % e, flush=True)
            _PG["read_warned"] = True
        _pg_drop()
        return None


def record_beat():
    _pg_exec("INSERT INTO bot_engine_beat (ts_ms, engine, pid) VALUES (%s, 'nn', %s)",
             (int(time.time() * 1000), os.getpid()))


def record_decision(handnumber, betround, action, amount, note):
    _pg_exec("INSERT INTO bot_nn_decision (handnumber, ts_ms, betround, action, amount, note) "
             "VALUES (%s, %s, %s, %s, %s, %s)",
             (str(handnumber), int(time.time() * 1000), betround, action,
              float(amount or 0), (note or "").strip() or None))


# --- OPPONENT READ INJECTION (roadmap item 4) --------------------------------------------------
# The OHF sends f$Opp_* as PLACEHOLDERS (Known=0, VPIP/PFR/AF defaults ~25/18/2), so the net gets no
# real read on live tables. Here we look up the AGGRESSOR's VERIFIED HUD stats (hud_player_stats,
# populated by hud_aggregator.py) and overwrite the placeholders with a REAL read -- a Python port of
# the server BotBrain::oppFeatures (KEEP IN SYNC) so the read->adjust mapping the net learned
# server-side applies here too. FAIL-SAFE: any problem leaves the placeholder and the decision goes
# on. Only KNOWN villains (>=20 hands) are overridden; unknown villains keep the OHF placeholder.
_OPP_COLS = ("vpip_n", "vpip_d", "pfr_n", "pfr_d", "threeb_n", "threeb_d", "f3b_n", "f3b_d",
             "ftc_n", "ftc_d", "aggr_actions", "call_actions", "wtsd_n", "wtsd_d")
_OPP_WARNED = [False]
_OPP_LOGGED = set()   # villains already audit-logged this session


def _pg_query(sql, params):
    """Read-only query on the pooled connection (runs in DRY too -- reads never mutate)."""
    try:
        with _pg_conn().cursor() as cur:
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
        if name not in _OPP_LOGGED:
            _OPP_LOGGED.add(name)
            arche = [k.replace("f$Opp_Is", "") for k in feats if k.startswith("f$Opp_Is") and feats[k]]
            _line = "opp-read %-14s VPIP=%.0f AF=%.2f Known=%d -> %s%s" % (
                name[:14], vpip, af, feats["f$Opp_Known"], arche or ["(no archetype)"],
                " +Foldy" if feats["f$Opp_Foldy"] else "")
            try:
                with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "opp_reads.log"), "a") as _fh:
                    _fh.write("%d %s\n" % (int(time.time()), _line))
            except Exception:
                pass
            if DRY:
                print("[nn_driver] " + _line, flush=True)
    except Exception as e:
        print("[nn_driver] opp-read skipped (%s) -- OHF placeholder kept" % e, flush=True)
    return sym


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

    # DESPERATION: UNDER 2 BB WE NEVER FOLD.  [Emrald: "if my bb is under 2, bet 3 or call anything"]
    #
    # Mirrors CAutoplayer::ExecuteDesperationShoveOrCall so the NN driver and the OHF autoplayer
    # behave identically at this depth. Below 2 BB there is nothing left to protect -- the blinds take
    # the stack next orbit whether we fold or not -- so folding is the one move that cannot win.
    #
    # We do NOT ask the NN. This is not a decision to be weighed; it is the decision. Skipping the
    # model also means it cannot talk us out of it with a fold, and it saves a round trip.
    #
    # The shove is sent as raise-to-3 even though we hold less than 2: the table accepts an over-bet
    # and caps it at our real stack, so with 1.4 BB behind "raise to 3" simply IS an all-in -- and it
    # stays right even if the stack scraped a little wrong.
    _bbl = float(sv.get("bblind", 1) or 1)
    if _bbl > 0:
        _stack_bb = (float(sv.get("stack", 0) or 0) + float(sv.get("bet", 0) or 0)) / _bbl
        if 0 < _stack_bb < DESPERATION_STACK_BB:
            _f = (gs.get("fckra") or "").upper()
            if "R" in _f or "A" in _f:
                _do, _amt = "raise", DESPERATION_RAISE_BB    # Hiss caps it at our stack -> a jam
            elif "C" in _f:
                _do, _amt = "call", 0                        # call anything
            elif "K" in _f:
                _do, _amt = "check", 0                       # nothing to call; never worse than folding
            else:
                _do, _amt = None, 0
            if _do:
                print("[nn_driver] %s DESPERATION: %.2f BB left (< %.1f) -- never fold. -> %s%s  [btns=%s]"
                      % (sv["_handnumber"], _stack_bb, DESPERATION_STACK_BB, _do.upper(),
                         (" to %.0fbb (table caps it -> all-in)" % _amt) if _amt else "", _f or "-"),
                      flush=True)
                record_decision(sv["_handnumber"], gs.get("betround"), _do, _amt, "desperation <2bb")
                if not DRY:
                    click(_do, _amt, hand=sv.get("_handnumber"), betround=gs.get("betround"))
                return True

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
    sym = _inject_opp_read(gs, sym)   # roadmap 4: real villain HUD read -> f$Opp_* (fail-safe)
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
    fckra = (gs.get("fckra") or "").upper()
    amt_to_call = float(sym.get("AmountToCall", 0) or 0)

    # A CHECK BUTTON THAT ISN'T THERE.
    #
    # AmountToCall is derived as (largest bet - my bet) from the opponents' bet pills, so ONE
    # mis-scraped pill collapses it to 0 -- and then every rule below "reconciles" a call into a
    # check. But the bar in that spot is Fold | Call | Raise Options: there IS no Check button. Hiss
    # itself says so (fckra reports C and no K), and its own source documents where the click lands:
    # "a check with no Check button present was clicked blind and landed on RAISE OPTIONS: the raise
    # panel popped open and the hand stalled."
    #
    # A live Call button with no Check button PROVES there is something to call. So when the two
    # disagree, the button bar wins -- it is the table telling us directly what our options are,
    # while AmountToCall is an inference built on the flakiest thing we scrape. We do NOT guess the
    # amount and we do NOT act on a state we know is inconsistent: we return False, which makes the
    # caller re-read and re-decide within the same turn (the bet pill usually recovers next frame).
    call_button = "C" in fckra
    check_button = "K" in fckra
    if call_button and not check_button and amt_to_call <= 0.001:
        print("[nn_driver] !! INCONSISTENT: a live CALL button (fckra=%s) but AmountToCall=0 -- the "
              "bet scrape is lying. NOT acting on it; re-reading. (Refusing to 'check' a button that "
              "does not exist.)" % (fckra or "-"), flush=True)
        return False                       # do not consume the turn; the caller retries

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

    # Last line of defence: never send a CHECK when the table says there is no Check button. Hiss
    # would refuse the click anyway (CAutoplayerButton::Click returns false when not clickable) and
    # the bot would silently stall, which is exactly the "it just sat there" symptom.
    if do == "check" and fckra and not check_button:
        if call_button:
            print("[nn_driver] !! decided CHECK but there is no Check button (fckra=%s) -- not "
                  "acting; re-reading rather than clicking blind." % fckra, flush=True)
            return False
        print("[nn_driver] !! decided CHECK with no Check button and no Call button (fckra=%s) -- "
              "not acting." % fckra, flush=True)
        return False
    print("[nn_driver] %s hole=%s board=%s -> NN: %s%s%s  [btns=%s]  (val=%s)" %
          (sv["_handnumber"], sv["hole"], sv["board"] or "-", do,
           (" to %.1fbb" % amount) if amount else "", note, fckra or "-", nn.get("value")), flush=True)
    record_decision(sv["_handnumber"], gs.get("betround"), do, amount, note)
    if not DRY:
        # Stamped with the spot it was decided for: Hiss drops the request if the hand or the street
        # has moved on before the button becomes clickable. Without this a fold decided for hand N
        # could still be pending when hand N+1 deals, and fire there.
        click(do, amount, hand=sv.get("_handnumber"), betround=sym.get("betround"))
    return True


def main():
    print("[nn_driver] %s  bot=%s nn=%s" % ("DRY-RUN" if DRY else "LIVE", BOT, NN), flush=True)
    print("[nn_driver] gating on ismyturn (per-turn latch; retries until the hole scrapes). Waiting for your turn...", flush=True)
    acted_this_turn = False     # acted on THIS turn already? re-armed on the falling edge of ismyturn
    last_act = 0.0
    ACT_WATCHDOG_S = 5.0        # still our turn this long after acting -> the click never landed
    nn_fail = 0                 # consecutive DECISION failures (swiftsnake/LAN, not Hiss)
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
                # WATCHDOG: we "acted", but it is STILL our turn.
                #
                # The latch is set when the HTTP request is accepted, not when the button is actually
                # clicked -- and the click can silently never happen: Hiss keeps the request pending
                # while the button isn't clickable, and (since the spot-stamp fix) DROPS it outright
                # once the hand or street moves on. Both leave the driver believing it acted while the
                # clock runs down to a timeout-fold. Nothing re-armed it, because ismyturn never fell.
                #
                # So if the turn is still ours several seconds after acting, the action did not land.
                # Re-arm and decide again. Re-sending simply overwrites Hiss's pending request, so the
                # worst case is one duplicate click of the SAME action in the SAME spot -- which the
                # spot-stamp makes safe, and which is strictly better than sitting there timing out.
                if acted_this_turn and (time.monotonic() - last_act) > ACT_WATCHDOG_S:
                    print("[nn_driver] !! acted %.0fs ago but it is STILL my turn -- the click never "
                          "landed (button not clickable, or a stale action was dropped). Re-deciding."
                          % (time.monotonic() - last_act), flush=True)
                    acted_this_turn = False
                    last_act = 0.0          # let the re-decide fire on this pass, not in another 1s

                if not acted_this_turn and (time.monotonic() - last_act) > 1.0:
                    # Diagnose the RIGHT machine. The decision leg talks to swiftsnake over the LAN;
                    # when it fails it used to fall through to the handler below, which prints
                    # "CANNOT READ BOT STATE ... Hiss's HTTP thread is likely WEDGED -- restart Hiss".
                    # That blames the one component that was working, and sends you to restart it.
                    try:
                        acted = decide_and_act(gs)
                        nn_fail = 0
                    except Exception as e:
                        nn_fail += 1
                        acted = False
                        print("[nn_driver] !! DECISION FAILED (%s: %s) -- this is the NN service / LAN "
                              "(%s). Hiss is fine; the BRAIN is unreachable. Do not restart Hiss."
                              % (type(e).__name__, e, NN), flush=True)
                        if nn_fail >= 3:
                            print("[nn_driver] !! %d decision failures in a row -- handing this table "
                                  "back to the OHF autoplayer so SOMEONE is acting." % nn_fail, flush=True)
                            try:
                                _get(BOT + "/api/autoplayer?on=1")
                            except Exception:
                                pass
                    if acted:
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
