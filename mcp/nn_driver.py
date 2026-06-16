#!/usr/bin/env python3
"""nn_driver.py - drive the LIVE hiss.exe with the trained neural net, no rebuild.

The "special setting" is simply running this script: start it = the NN plays; stop it = the
OHF plays. It uses only the bot's existing HTTP API, so it can't touch the live binary.

Flow (mirrors the proven poker-server decideHissNN path):
  1. GET  http://127.0.0.1:27654/api/table-state         -> live table (hero hole, board, stacks)
  2. POST http://192.168.1.39:8087/decide  (seat-view)   -> {sym}  (headless ScarletBeast = full infoset)
  3. POST http://192.168.1.39:8088/nn-decide ({sym,hole,board,bblind}) -> {action, f$betsize, f$allin}
  4. GET  http://127.0.0.1:27654/api/action?do=<a>[&amount=<bb>]&force=1   -> the bot clicks it

Run the bot with the AUTOPLAYER OFF so the NN (not the OHF) drives. One action per (hand,street).
  python nn_driver.py            # live: NN plays
  python nn_driver.py --dry-run  # read + decide + print, but DON'T click (safe test)
"""
import os, sys, json, time, subprocess, urllib.parse

BOT    = os.environ.get("NN_BOT_URL",    "http://127.0.0.1:27654")
DECIDE = os.environ.get("NN_DECIDE_URL", "http://192.168.1.39:8087/decide")
NN     = os.environ.get("NN_URL",        "http://192.168.1.39:8088/nn-decide")
POLL   = float(os.environ.get("NN_POLL_S", "0.8"))
DRY    = "--dry-run" in sys.argv

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
    """live /api/table-state -> the /decide seat-view, or None if it's not the hero's turn."""
    hero = gs.get("userchair", -1)
    toact = gs.get("toact", -1)
    players = gs.get("players", []) or []
    nchairs = gs.get("nchairs", len(players)) or len(players)
    if hero is None or hero < 0:
        return None                       # observer / not seated -> nothing to drive
    if toact != hero:
        return None                       # not our turn
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


def main():
    print("[nn_driver] %s  bot=%s decide=%s nn=%s" % ("DRY-RUN" if DRY else "LIVE", BOT, DECIDE, NN), flush=True)
    last_key = None
    while True:
        try:
            gs = _get(BOT + "/api/table-state")
            sv = seat_view(gs)
            if sv:
                key = (sv["_handnumber"], sv["_board"], sv["hole"])     # one action per (hand,street)
                if key != last_key:
                    dr = _post(DECIDE, {k: v for k, v in sv.items() if not k.startswith("_")})
                    sym = dr.get("sym", {}) if isinstance(dr, dict) else {}
                    nn = _post(NN, {"sym": sym, "hole": sv["hole"], "board": sv["board"], "bblind": sv["bblind"]})
                    a = nn.get("action", "fold")
                    bb = float(nn.get("f$betsize", 0) or 0)
                    allin = ((float(nn.get("f$allin", 0) or 0)) > 0) or a == "allin"
                    do = "allin" if allin else ("raise" if a == "raise" else a)
                    amount = bb if (do == "raise") else 0
                    note = ""
                    if do == "raise" and amount <= 0:
                        # NN said raise but gave no numpad size -> an unsized raise can misfire on
                        # the real table; downgrade to the always-legal call rather than guess.
                        do, amount, note = "call", 0, "  (raise->call: no size)"
                    print("[nn_driver] %s hole=%s board=%s -> NN: %s%s%s  (val=%s)" %
                          (sv["_handnumber"], sv["hole"], sv["board"] or "-", do,
                           (" to %.1fbb" % amount) if amount else "", note, nn.get("value")), flush=True)
                    if not DRY:
                        click(do, amount)
                    last_key = key
        except Exception as e:
            print("[nn_driver] err:", e, flush=True)
        time.sleep(POLL)


if __name__ == "__main__":
    main()
