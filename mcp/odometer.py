#!/usr/bin/env python3
"""odometer.py - record what every hand WON. Nothing else. Never switches itself off.

This exists because the measurement kept quietly disconnecting.

`hand_results` -- the per-hand P&L that every other tool reads (measure_live.py, the A/B gate, the
AIL's loss-weighted feedback) -- was written by synapse_map.py --watch. But synapse_map is ALSO the
brain, and "brain disengage" explicitly flips the synapse AIL switch off (ail_server: `_state
["enabled"]["synapse"] = False`). Stopping Hiss disengages the brain. So every Hiss restart silently
killed the odometer, and the bot went on playing hands that were never scored. Measured: 102 hands
seen in six hours, 2 of them recorded.

A measurement that turns itself off without saying so is worse than no measurement, because you keep
trusting it. So the odometer is now its own daemon: it does ONE thing, it depends on nothing, and no
feature toggle can take it down with it. synapse_map keeps its own copy for when the brain is up --
the writes are idempotent (ON CONFLICT on handnumber), so the two cannot corrupt each other.

  python odometer.py                 # follow the live bot, record every hand
  python odometer.py --once          # one poll, print what it sees, exit
"""
import os, sys, time, json, urllib.request

BOT = os.environ.get("NN_BOT_URL", "http://127.0.0.1:27654")
DSN = os.environ.get("HISS_PG_DSN",
                     "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")
POLL_S = float(os.environ.get("ODOMETER_POLL_S", "1.0"))


def get(path, timeout=4):
    try:
        with urllib.request.urlopen(BOT + path, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception:
        return None


def table_state():
    ts = get("/api/table-state")
    if not ts:
        return None
    hero = ts.get("userchair", -1)
    players = ts.get("players") or []
    me = next((p for p in players if p.get("chair") == hero), None) if hero is not None and hero >= 0 else None
    cards = [c for c in ((me or {}).get("cards") or []) if isinstance(c, str) and len(c) == 2
             and c[0] in "23456789TJQKA" and c[1] in "cdhs"]
    return {
        "hand": str(ts.get("handnumber") or ""),
        "balance": float((me or {}).get("balance") or 0) or None,
        "has_cards": len(cards) >= 2,
        "observer": bool(ts.get("observer")),
        "nchairs": ts.get("nchairs"),
    }


def bblind():
    s = get("/api/symbols?names=bblind")
    try:
        return float((s or {}).get("bblind") or 1.0) or 1.0
    except Exception:
        return 1.0


# The same sanity guards the synapse tracker uses. A hand's net is the hero's stack at the START of
# the NEXT hand minus the stack at the start of THIS one -- the only source that covers every hand
# (the ACR hand-history writer only logs hands the hero stayed in, so it is win-biased).
#
# NOTE the table-identity check is deliberately NOT copied here. It compared the OCR'd table TITLE,
# which is unstable garbage text -- "em351811.|LNWERERaaILWalackeySargS300..." vs
# "em351811.|LNWERERaaIEMalachySargS300..." is the SAME table, and it was throwing the hand away as a
# "table switch". The stake guards below already reject the thing that check was protecting against
# (a stack that jumps because we moved tables), without discarding good hands on OCR noise.
def plausible(prev_hand, prev_bal, new_bal, bb):
    if not prev_hand or prev_bal is None or new_bal is None:
        return None
    if not (str(prev_hand).isdigit() and len(str(prev_hand)) >= 6):
        return None
    bb = bb if (bb and bb > 0) else 1.0
    cap = bb * 2000.0
    if not (0 < prev_bal < cap and 0 < new_bal < cap):
        return None
    net = round(new_bal - prev_bal, 2)
    if abs(net) > bb * 1000.0:                 # no real hand swings this much -> OCR garbage
        return None
    if net < 0 and -net > prev_bal + bb:       # can't lose more than you sat down with
        return None
    return net


def record(handnumber, ts_ms, net, start_bal, end_bal):
    import psycopg2
    c = psycopg2.connect(DSN)
    c.autocommit = True
    with c.cursor() as cur:
        cur.execute(
            "INSERT INTO hand_results (handnumber, ts_ms, net, start_balance, end_balance) "
            "VALUES (%s,%s,%s,%s,%s) ON CONFLICT (handnumber) DO UPDATE SET "
            "net=EXCLUDED.net, end_balance=EXCLUDED.end_balance, ts_ms=EXCLUDED.ts_ms",
            (handnumber, ts_ms, net, start_bal, end_bal))
    c.close()


def main():
    once = "--once" in sys.argv
    print("[odometer] recording every hand's result from %s -> hand_results" % BOT, flush=True)
    cur_hand, cur_bal, cur_had_cards, cur_observer = "", None, False, False
    recorded = skipped = 0
    while True:
        st = table_state()
        if st is None:
            if once:
                print("[odometer] bot unreachable"); return
            time.sleep(POLL_S); continue
        if once:
            print("[odometer] %s" % st); return

        hn, bal = st["hand"], st["balance"]
        # remember, within the hand, that the hero really was dealt in (and wasn't just observing)
        if hn and hn == cur_hand:
            if st["has_cards"]:
                cur_had_cards = True
            if st["observer"]:
                cur_observer = True

        if hn and hn != cur_hand:                       # hand boundary -> score the one that ended
            if cur_hand and cur_had_cards and not cur_observer:
                net = plausible(cur_hand, cur_bal, bal, bblind())
                if net is None:
                    skipped += 1
                    print("[odometer] hand %s: implausible stacks (%s -> %s) -- not recorded"
                          % (cur_hand, cur_bal, bal), flush=True)
                else:
                    try:
                        record(cur_hand, int(time.time() * 1000), net, cur_bal, bal)
                        recorded += 1
                        print("[odometer] hand %s: %+.2f  (%d recorded / %d skipped)"
                              % (cur_hand, net, recorded, skipped), flush=True)
                    except Exception as e:
                        print("[odometer] DB write failed for hand %s: %s" % (cur_hand, e), flush=True)
            cur_hand, cur_bal = hn, bal
            cur_had_cards = st["has_cards"]
            cur_observer = st["observer"]
        elif not cur_hand and hn:
            cur_hand, cur_bal = hn, bal
            cur_had_cards, cur_observer = st["has_cards"], st["observer"]

        time.sleep(POLL_S)


if __name__ == "__main__":
    main()
