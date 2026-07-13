#!/usr/bin/env python3
"""measure_live.py - does the NN actually beat the OHF on the REAL tables?

That question had never been asked. measure_nn.py only ever looked at the swiftsnake self-play
pool, whose win rates are dominated by table composition (the pool contains two deliberate donator
bots, so "hiss-nn +185 bb/100" mostly measures how often it sat with the station). The LIVE result
-- what the bot actually won on ACR -- sits in postgres `hand_results`, and nothing read it.

This reads it, splits it by which engine was driving, and -- the part that matters -- tells you
honestly whether the sample can support ANY conclusion at all. Poker variance is enormous: with a
per-hand sd around 80bb, a few hundred hands cannot distinguish a world-beater from a losing bot.
A number without its error bar is worse than no number, because you will act on it.

  hand_results     what each hand won   (written by the synapse harmonizer -- keep it RUNNING)
  bot_nn_decision  the NN acted here    (written by nn_driver)
  bot_engine_beat  the NN was in charge (written by nn_driver every ~10s)

A hand is attributed to the NN if the NN decided in it, or if the driver was alive within
BEAT_WINDOW of it (which also captures hands the NN was dealt but never had to act in -- walks,
folded-around blinds -- and those count: they are part of its win rate).

  python measure_live.py                # everything attributable
  python measure_live.py --days 7       # last 7 days only
  python measure_live.py --chips-per-bb 1.0
"""
import os, sys, math, time

DSN = os.environ.get("HISS_PG_DSN",
                     "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")

BEAT_WINDOW_MS = 30_000      # a hand within this of a driver heartbeat was NN-driven
CHIPS_PER_BB   = 1.0         # ACR shows stacks in BB and bblind_fallback pins bb=1.0, so net IS bb


def _arg(flag, default):
    if flag in sys.argv:
        i = sys.argv.index(flag)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return default


def norm_cdf(z):
    return 0.5 * (1.0 + math.erf(z / math.sqrt(2.0)))


def describe(nets, per_bb):
    """n, mean, sd, and the 95% interval -- in bb/100, the only units a poker result means anything in."""
    n = len(nets)
    if n == 0:
        return None
    bb = [x / per_bb for x in nets]
    mean = sum(bb) / n
    if n > 1:
        var = sum((x - mean) ** 2 for x in bb) / (n - 1)
        sd = math.sqrt(var)
        se = sd / math.sqrt(n)
    else:
        sd = se = float("nan")
    return {
        "n": n, "mean": mean, "sd": sd, "se": se,
        "per100": mean * 100.0,
        "ci100": 1.96 * se * 100.0 if n > 1 else float("nan"),
        "total": sum(bb),
    }


def hands_needed(sd, edge_per100):
    """Hands required to resolve an edge of `edge_per100` bb/100 at 95% confidence."""
    if not sd or edge_per100 <= 0:
        return float("inf")
    return (1.96 * sd / (edge_per100 / 100.0)) ** 2


def bar(label, s):
    if s is None:
        print("  %-16s  no hands" % label)
        return
    print("  %-16s  n=%-7d  %+9.1f bb/100   95%% CI %+9.1f .. %+9.1f   (sd %.1f)"
          % (label, s["n"], s["per100"], s["per100"] - s["ci100"], s["per100"] + s["ci100"], s["sd"]))


def main():
    import psycopg2
    per_bb = float(_arg("--chips-per-bb", CHIPS_PER_BB))
    days = _arg("--days", None)
    since_ms = 0
    if days:
        since_ms = int((time.time() - float(days) * 86400) * 1000)

    c = psycopg2.connect(DSN)
    cur = c.cursor()

    cur.execute("SELECT handnumber, ts_ms, net FROM hand_results "
                "WHERE net IS NOT NULL AND ts_ms >= %s ORDER BY ts_ms", (since_ms,))
    hands = cur.fetchall()

    cur.execute("SELECT DISTINCT handnumber FROM bot_nn_decision")
    nn_hands = {r[0] for r in cur.fetchall()}

    cur.execute("SELECT ts_ms FROM bot_engine_beat ORDER BY ts_ms")
    beats = [r[0] for r in cur.fetchall()]

    # attribution only exists from the moment the driver first recorded anything; hands before that
    # are honestly unknowable, and calling them OHF would be a lie that flatters the NN.
    cur.execute("SELECT min(ts_ms) FROM (SELECT ts_ms FROM bot_engine_beat "
                "UNION ALL SELECT ts_ms FROM bot_nn_decision) q")
    epoch = cur.fetchone()[0]

    cur.execute("SELECT max(ts_ms) FROM hand_results")
    newest = cur.fetchone()[0]
    c.close()

    def nn_driven(hn, ts):
        if str(hn) in nn_hands:
            return True
        lo, hi = ts - BEAT_WINDOW_MS, ts + BEAT_WINDOW_MS
        import bisect
        i = bisect.bisect_left(beats, lo)
        return i < len(beats) and beats[i] <= hi

    buckets = {"nn": [], "ohf": [], "unattributed": []}
    for hn, ts, net in hands:
        if epoch is None or ts < epoch:
            buckets["unattributed"].append(net)
        elif nn_driven(hn, ts):
            buckets["nn"].append(net)
        else:
            buckets["ohf"].append(net)

    nn, ohf, un = (describe(buckets[k], per_bb) for k in ("nn", "ohf", "unattributed"))

    print("=" * 92)
    print("LIVE RESULT - NN vs OHF on real tables" + ("  (last %s days)" % days if days else ""))
    print("=" * 92)

    # --- measurement health: a silent dead pipeline is how you end up flying blind for a month ---
    age_h = (time.time() * 1000 - newest) / 3_600_000 if newest else None
    print("\nMEASUREMENT HEALTH")
    if newest is None:
        print("  !! hand_results is EMPTY. The synapse harmonizer is not writing. Nothing is being")
        print("     measured, and no result below can be trusted. Turn it on before anything else.")
    elif age_h > 6:
        print("  !! newest hand_result is %.1f hours old -- the synapse harmonizer is probably DOWN." % age_h)
        print("     Hands are being played and NOT recorded. Fix this before reading anything below.")
    else:
        print("  ok  newest hand_result is %.1f hours old -- results are flowing." % age_h)
    print("  attributed hands: %d NN / %d OHF / %d before attribution existed"
          % (len(buckets["nn"]), len(buckets["ohf"]), len(buckets["unattributed"])))

    print("\nWIN RATE")
    bar("NN driver", nn)
    bar("OHF", ohf)
    if un:
        bar("(unattributed)", un)

    # --- the verdict, and the honesty about whether there IS one -----------------------------
    print("\nVERDICT")
    if not nn or not ohf or nn["n"] < 2 or ohf["n"] < 2:
        print("  NO COMPARISON POSSIBLE YET - one arm has no hands.")
        print("  Run the bot on both engines. Until then there is nothing to compare.")
    else:
        diff = nn["per100"] - ohf["per100"]
        se = math.sqrt(nn["se"] ** 2 + ohf["se"] ** 2) * 100.0
        z = diff / se if se > 0 else 0.0
        p = 2 * (1 - norm_cdf(abs(z)))
        print("  NN - OHF = %+.1f bb/100   (SE %.1f, z = %+.2f, p = %.3f)" % (diff, se, z, p))
        if p < 0.05:
            better = "the NN" if diff > 0 else "the OHF"
            print("  SIGNIFICANT at 95%%: %s is genuinely ahead on this sample." % better)
        else:
            print("  NOT SIGNIFICANT. This gap is indistinguishable from noise -- do NOT promote,")
            print("  demote or 'confirm' anything on it. It is the sound of variance, not skill.")

    # --- how much evidence would it actually take ---------------------------------------------
    print("\nHOW MUCH EVIDENCE WOULD IT TAKE")
    sd = (nn or ohf or un or {}).get("sd")
    if sd and not math.isnan(sd):
        print("  at the measured per-hand sd of %.1f bb, to resolve an edge of:" % sd)
        for edge in (100.0, 50.0, 25.0, 10.0, 5.0):
            need = hands_needed(sd, edge)
            print("    %6.0f bb/100  ->  %12s hands per arm" % (edge, "{:,.0f}".format(need)))
        played = len(hands)
        if played:
            resolvable = 100 * 1.96 * sd / math.sqrt(played)
            print("  with the %s hands you have, the smallest edge you could detect is +/- %.0f bb/100."
                  % ("{:,}".format(played), resolvable))
    else:
        print("  (not enough hands to even estimate the variance)")
    print()


if __name__ == "__main__":
    main()
