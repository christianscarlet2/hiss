#!/usr/bin/env python3
"""
icm.py -- Independent Chip Model calculator (Malmuth-Harville, Monte-Carlo).

Used by the icm-chip-value skill. Works for a known set of stacks (final table /
ITM short field) OR an MTT approximation (hero + a field of average stacks).

Input: JSON on argv[1] or stdin, one of:
  {"stacks":[12000,8000,...], "payouts":[100,60,40], "hero_index":0, "sims":40000}
  {"hero_stack":12000, "players_remaining":120, "avg_stack":9000,
   "payouts":[...full payout list, place 1..places_paid...], "sims":40000}

Output: JSON
  {"hero_equity":.., "hero_chip_pct":.., "equity_if_double":.., "equity_if_bust":0.0,
   "bubble_factor":.., "dollars_per_chip":.., "n_modeled":.., "method":"exact|mc"}

Notes
- Finishing order is sampled Malmuth-Harville style: the next finisher (highest place
  among those remaining) is drawn proportional to current chips. Averaged over `sims`.
- For large fields we model the field as hero + (players_remaining-1) equal average
  stacks (or split the remaining chips) -- an approximation, good enough for advice.
- random/seed: a fixed seed keeps successive readings comparable.
"""
import sys, json, random

def homogeneous_equity(hero, avg, n_field, payouts):
    """EXACT hero equity when the field is modelled as n_field IDENTICAL average stacks.

    That is precisely the MTT mid-field mode, and it has a closed form -- so we never simulate it.
    Malmuth-Harville draws the next finisher proportional to chips, so while the hero is still in with
    k field players left, P(hero is drawn now) = hero / (hero + k*avg). Walking places from the top:

        P(reach place p) = prod_{j<p} [ (k0-j)*avg / (hero + (k0-j)*avg) ]
        P(hero takes p)  = P(reach p) * hero / (hero + (k0-p)*avg)

    Summing P(p)*payout[p] is O(len(payouts)) and instant. The Monte-Carlo it replaces was
    O(sims * places * field) -- ~2 billion operations for a 593-runner freeroll paying 184, which
    simply never returned (120s timeout) and left the daemon speaking $0.00 equity."""
    k0 = max(0, int(n_field))
    if hero <= 0:
        return 0.0
    eq = 0.0
    reach = 1.0                      # P(hero still unfinished when place p is awarded)
    for p in range(len(payouts)):
        k = k0 - p                   # field players still in (hero has survived p eliminations)
        if k < 0:
            # Everyone else is out; hero necessarily takes this place.
            eq += reach * payouts[p]
            reach = 0.0
            break
        denom = hero + k * avg
        if denom <= 0:
            break
        p_here = hero / denom
        eq += reach * p_here * payouts[p]
        reach *= (1.0 - p_here)
        if reach <= 1e-12:
            break
    return eq


def mc_equities(stacks, payouts, sims, hero_index):
    n = len(stacks)
    npay = len(payouts)
    eq = [0.0] * n
    for _ in range(sims):
        idx = list(range(n))
        chips = list(stacks)
        total = sum(chips)
        place = 0
        # draw finishers from 1st downward, proportional to remaining chips
        while idx and place < npay:
            r = random.random() * total
            acc = 0.0
            pick = 0
            for k in range(len(idx)):
                acc += chips[k]
                if r <= acc:
                    pick = k
                    break
            eq[idx[pick]] += payouts[place]
            total -= chips[pick]
            del idx[pick]; del chips[pick]
            place += 1
    for i in range(n):
        eq[i] /= sims
    return eq

def main():
    raw = sys.argv[1] if len(sys.argv) > 1 else sys.stdin.read()
    d = json.loads(raw)
    payouts = [float(x) for x in d.get("payouts", [])]
    sims = int(d.get("sims", 40000))
    random.seed(1234567)   # deterministic across readings

    if "stacks" in d and d["stacks"]:
        stacks = [float(x) for x in d["stacks"]]
        hero = int(d.get("hero_index", 0))
        method = "mc"
    else:
        hero_stack = float(d["hero_stack"])
        rem = int(d["players_remaining"])
        avg = float(d.get("avg_stack", 0) or 0)
        if avg <= 0:
            # if no avg given, assume an even split of an implied field
            avg = hero_stack
        stacks = [hero_stack] + [avg] * max(0, rem - 1)
        hero = 0
        method = "exact-homogeneous"

    n = len(stacks)
    if n == 0 or not payouts:
        print(json.dumps({"error": "need stacks and payouts"})); return

    if method == "exact-homogeneous":
        # Closed form -- no simulation. See homogeneous_equity(): the MC path is O(sims*places*field)
        # and does not finish on a big field (593 players, 184 paid).
        n_field = n - 1
        e0   = homogeneous_equity(stacks[hero], avg, n_field, payouts)
        e_up = homogeneous_equity(stacks[hero] * 2.0, avg, n_field, payouts)
    else:
        eq = mc_equities(stacks, payouts, sims, hero)
        e0 = eq[hero]
        dbl = list(stacks); dbl[hero] = stacks[hero] * 2.0
        e_up = mc_equities(dbl, payouts, max(8000, sims // 2), hero)[hero]
    e_bust = 0.0

    total_chips = sum(stacks)
    chip_pct = stacks[hero] / total_chips if total_chips else 0.0
    dpc = e0 / stacks[hero] if stacks[hero] else 0.0
    # bubble/risk factor: $ risked per chip on a bust vs $ gained per chip on a double.
    gain = (e_up - e0)
    risk = (e0 - e_bust)
    bubble = (risk / gain) if gain > 1e-9 else float("inf")

    print(json.dumps({
        "hero_equity": round(e0, 4),
        "hero_chip_pct": round(chip_pct, 4),
        "equity_if_double": round(e_up, 4),
        "equity_if_bust": e_bust,
        "bubble_factor": (round(bubble, 3) if bubble != float("inf") else None),
        "dollars_per_chip": round(dpc, 8),
        "n_modeled": n,
        "method": method,
    }))

if __name__ == "__main__":
    main()
