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

BOT    = os.environ.get("NN_BOT_URL",    "http://127.0.0.1:27654")
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


def decide_and_act(mono):
    gs = _get(BOT + "/api/table-state")
    sv = seat_view(gs)
    if not sv:
        print("[nn_driver] my turn but no readable hole (sat out / between hands) -- skip", flush=True)
        return
    # Sym straight from the live bot (local, DB-free) -- not swiftsnake's /decide.
    sym = _get(BOT + "/api/symbols?names=" + NUMERIC_SYMBOLS)
    nn = _post(NN, {"sym": sym, "hole": sv["hole"], "board": sv["board"], "bblind": sv["bblind"]})
    a = nn.get("action", "fold")
    bb = float(nn.get("f$betsize", 0) or 0)
    allin = ((float(nn.get("f$allin", 0) or 0)) > 0) or a == "allin"
    do = "allin" if allin else ("raise" if a == "raise" else a)
    amount = bb if (do == "raise") else 0
    note = ""
    if do == "raise" and amount <= 0:
        # NN said raise but gave no numpad size -> an unsized raise can misfire on the real
        # table; downgrade to the always-legal call rather than guess.
        do, amount, note = "call", 0, "  (raise->call: no size)"
    print("[nn_driver] %s hole=%s board=%s -> NN: %s%s%s  (val=%s)" %
          (sv["_handnumber"], sv["hole"], sv["board"] or "-", do,
           (" to %.1fbb" % amount) if amount else "", note, nn.get("value")), flush=True)
    if not DRY:
        click(do, amount)


def main():
    print("[nn_driver] %s  bot=%s nn=%s" % ("DRY-RUN" if DRY else "LIVE", BOT, NN), flush=True)
    print("[nn_driver] gating on ismyturn (rising edge). Waiting for your turn...", flush=True)
    prev = False
    last_act = 0.0
    while True:
        try:
            now = ismyturn()
            if now and not prev and (time.monotonic() - last_act) > 3.0:   # rising edge + cooldown
                decide_and_act(time.monotonic())
                last_act = time.monotonic()
            prev = now
        except Exception as e:
            print("[nn_driver] err:", e, flush=True)
        time.sleep(POLL)


if __name__ == "__main__":
    main()
