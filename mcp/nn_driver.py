#!/usr/bin/env python3
"""nn_driver.py - drive the LIVE hiss.exe with the trained neural net, no rebuild.

The "special setting" is simply running this script: start it = the NN plays; stop it = the
OHF plays. It uses only the bot's existing HTTP API, so it can't touch the live binary.

Flow (turn-gated on ismyturn; the sym comes from the LIVE bot, not swiftsnake's /decide):
  1. poll http://127.0.0.1:27654/api/symbols?names=ismyturn   -> rising edge = the hero's turn
  2. GET  http://127.0.0.1:27654/api/table-state               -> hero hole, board, bblind
  3. GET  http://127.0.0.1:27654/api/symbols?names=<86 NUMERIC_SYMBOLS> -> the full sym (local, DB-free)
  4. POST http://192.168.1.39:8088/nn-decide ({sym,hole,board,bblind}) -> {action, f$betsize, f$allin}
  5. GET  http://127.0.0.1:27654/api/action?do=<a>[&amount=<bb>]&force=1   -> the bot clicks it

Only /nn-decide is remote (pure inference, ~10ms); everything else is local, so a swamped
swiftsnake DB can't stall the NN. Run the bot with the AUTOPLAYER OFF so the NN drives.
  python nn_driver.py            # live: NN plays
  python nn_driver.py --dry-run  # read + decide + print, but DON'T click (safe test)
"""
import os, sys, json, time, subprocess, urllib.parse

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
# these locally via /api/symbols (verified 86/86), so we read the sym straight from the bot --
# fast and reliable -- instead of swiftsnake's /decide (which hangs under postgres load).
NUMERIC_SYMBOLS = ("prwin,handrank169,f$BoardWet,f$BoardDry,f$ScaryBoard,f$BoardHighCardFoldy,"
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
    "f$Style,f$OpenBase,f$CbetFreq,f$ThreeBetBluffFreq,f$DoubleBarrel,validator_ok,"
    "validator_confidence")

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


def click(action, amount):
    q = {"do": action, "force": "1"}
    if action in ("raise", "bet") and amount and amount > 0:
        q["amount"] = "%.2f" % amount
    return _get(BOT + "/api/action?" + urllib.parse.urlencode(q))


def ismyturn():
    """The bot's real turn signal (toact is unreliable). 1 == it's the hero's turn to act."""
    try:
        v = _get(BOT + "/api/symbols?names=ismyturn").get("ismyturn", 0)
        return (v or 0) >= 1
    except Exception:
        return False


def decide_and_act():
    """Returns True if we read the seat and clicked (or decided, in DRY); False if the hole
    isn't readable yet so the CALLER should retry within the same turn (do NOT skip the turn --
    that silently drops the action and looks like a phantom 'sit out / between hands')."""
    gs = _get(BOT + "/api/table-state")
    sv = seat_view(gs)
    if not sv:
        return False                       # hole not scraped yet -> retry, don't consume the turn
    # Sym straight from the live bot (local, DB-free) -- not swiftsnake's /decide.
    sym = _get(BOT + "/api/symbols?names=" + NUMERIC_SYMBOLS)
    nn = _post(NN, {"sym": sym, "hole": sv["hole"], "board": sv["board"], "bblind": sv["bblind"]})
    a = nn.get("action", "fold")
    bb = float(nn.get("f$betsize", 0) or 0)
    allin = ((float(nn.get("f$allin", 0) or 0)) > 0) or a == "allin"
    do = "allin" if allin else ("raise" if a == "raise" else a)
    amount = bb if (do == "raise") else 0
    note = ""
    if do == "raise":
        # Legal raise sizing (NLHE): an opening raise must be >= 2bb (when bb=1bb), and a re-raise
        # must be >= the last raise increment. If the NN's raise-TO does NOT meet the legal minimum,
        # do NOT raise -- fall back to call if facing a bet, else check (Emrald's rule: "otherwise
        # hit call or check in that succession"). A full-stack shove is always legal, so a raise-TO
        # at/over the effective stack becomes all-in.
        bbl     = float(sv.get("bblind", 1) or 1) or 1.0
        my_bet  = float(sv.get("bet", 0) or 0)
        to_call = float(sym.get("AmountToCall", 0) or 0)
        highest = my_bet + to_call                    # current highest bet, relative to hero
        min_raise_to = highest + max(bbl, to_call)    # last increment ~ to_call; never < one bb
        if highest <= bbl + 1e-6:                     # only the blinds are in -> opening raise
            min_raise_to = max(min_raise_to, 2.0 * bbl)
        # Postflop OPENING bet (nothing to call -> a bet, not a raise): size it at least a QUARTER
        # of the pot (Emrald's rule). Only bumps the size up; raises facing a bet keep their sizing.
        betround = float(sym.get("betround", 0) or 0)
        if betround >= 2 and to_call <= 0.001 and amount > 0:
            quarter_pot = 0.25 * (float(sym.get("PotSize", 0) or 0)) / bbl   # pot money units -> bb
            if quarter_pot > amount:
                amount = quarter_pot
                note = "  (postflop bet -> >=1/4 pot %.2fbb)" % amount
        eff_max = my_bet + float(sv.get("stack", 0) or 0)
        if amount > 0 and amount >= eff_max - 1e-6:
            do, amount, note = "allin", 0, "  (raise>=stack -> allin)"
        elif amount < min_raise_to - 1e-6:            # below the legal minimum -> call, else check
            if to_call > 0.001:
                do, amount, note = "call", 0, "  (raise %.2f<min %.2f -> call)" % (amount, min_raise_to)
            else:
                do, amount, note = "check", 0, "  (raise %.2f<min %.2f -> check)" % (amount, min_raise_to)
    # Reconcile call/check with the actual spot: with no bet to call the table shows a CHECK
    # button (not Call), so do=call would find no button and never click. AmountToCall>0 means
    # we're facing a bet (can't check). Uses AmountToCall from the sym we already fetched.
    # The phone keypad only accepts 0.5 increments (6.5 or 7, not 6.6) -> snap to nearest half-bb.
    if amount and amount > 0:
        amount = int(amount * 2 + 0.5) / 2.0
    amt_to_call = float(sym.get("AmountToCall", 0) or 0)
    if do == "call" and amt_to_call <= 0.001:
        do, note = "check", note + "  (call->check: nothing to call)"
    elif do == "check" and amt_to_call > 0.001:
        do, note = "call", note + "  (check->call: facing a bet)"
    print("[nn_driver] %s hole=%s board=%s -> NN: %s%s%s  (val=%s)" %
          (sv["_handnumber"], sv["hole"], sv["board"] or "-", do,
           (" to %.1fbb" % amount) if amount else "", note, nn.get("value")), flush=True)
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
    while True:
        try:
            now = ismyturn()
            if now:
                # Act exactly once per turn, but if the hole isn't readable yet keep RETRYING
                # within the turn (decide_and_act returns False) instead of skipping it. The 1s
                # floor is just a glitch guard against a same-tick double-fire.
                if not acted_this_turn and (time.monotonic() - last_act) > 1.0:
                    if decide_and_act():
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
            print("[nn_driver] err:", e, flush=True)
        time.sleep(POLL)


if __name__ == "__main__":
    main()
