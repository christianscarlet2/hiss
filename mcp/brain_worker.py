#!/usr/bin/env python3
"""brain_worker.py -- PARALLEL pathway evaluator for the Hiss brain, built to use swiftsnake's 32
cores. Given the harmonized brain context + the villain profile, it evaluates many candidate
action x size pathways IN PARALLEL (predict the villain's response + a rough EV per pathway) and
returns the ranked best, so the advisor/brain confirms the MOST PROFITABLE pathway before acting.

Pure compute + a profile dict in (no live-bot dependency), so it runs wherever the cores are. To move
the brain to swiftsnake: run synapse_map.py --watch THERE (HISS_PG_DSN -> the primary, --bot-url -> the
Hiss LAN address); it imports this to fan candidate pathways across the cores.

  from brain_worker import evaluate_pathways
  best, ranked = evaluate_pathways(brain, prof, pot_bb=20, to_call_bb=6, equity_hint=0.5)
"""
import os
try:
    from multiprocessing import Pool
except Exception:
    Pool = None

# candidate (action, bet-fraction-of-pot) lines to evaluate in parallel
CANDIDATES = [("fold", 0.0), ("call", 0.0), ("raise", 0.5), ("raise", 0.75),
              ("raise", 1.0), ("raise", 1.5), ("raise", 2.5)]


def _num(v, d=0.0):
    try:
        return float(v)
    except Exception:
        return d


def predict_response(prof, action):
    """Standalone copy of the villain-response model (so the worker has no synapse_map dependency)."""
    if action != "raise":
        return {"fold": 0.0, "call": 1.0, "raise": 0.0}
    ex = (prof.get("exploits") or {}) if prof else {}
    fp = _num(prof.get("fold_to_pressure"), -1) if prof else -1
    pf = fp if fp >= 0 else 0.45
    if ex.get("overfold") or ex.get("folds_to_3bet"): pf = max(pf, 0.70)
    if ex.get("never_folds") or ex.get("tilting"): pf = min(pf, 0.15)
    cf = _num(prof.get("cont_freq"), -1) if prof else -1
    pr = 0.30 if (ex.get("keeps_firing") or (cf >= 0 and cf > 0.6)) else 0.12
    if ex.get("never_folds"): pr = min(pr + 0.10, 0.40)
    pf = min(pf, 1.0 - pr)
    return {"fold": round(pf, 2), "call": round(max(0.0, 1.0 - pf - pr), 2), "raise": round(pr, 2)}


def _ev_of(args):
    action, frac, prof, pot, to_call, equity = args
    if action == "fold":
        return (action, frac, 0.0, {})                                   # fold = 0 EV baseline (sunk pot)
    if action == "call":
        return (action, frac, round(equity * (pot + to_call) - to_call, 2), {"call": 1.0})
    bet = frac * pot
    r = predict_response(prof, "raise")
    won = equity * (pot + 2 * bet) - bet                                 # EV when called
    ev = r["fold"] * pot + r["call"] * won + r["raise"] * (won - bet * 0.5)
    return (action, frac, round(ev, 2), r)


def evaluate_pathways(brain, prof, pot_bb, to_call_bb, equity_hint=0.45, candidates=None):
    """Fan the candidate pathways across the cores; return the most profitable + the ranked list."""
    cands = candidates or CANDIDATES
    args = [(a, f, prof or {}, _num(pot_bb), _num(to_call_bb), _num(equity_hint, 0.45)) for a, f in cands]
    results = None
    if Pool is not None and len(args) > 2:
        try:
            with Pool(min(len(args), os.cpu_count() or 4)) as p:
                results = p.map(_ev_of, args)
        except Exception:
            results = None
    if results is None:
        results = [_ev_of(a) for a in args]
    ranked = sorted(results, key=lambda r: r[2], reverse=True)
    best = ranked[0]
    return ({"action": best[0], "size_frac": best[1], "ev": best[2], "response": best[3]},
            [{"action": a, "size_frac": f, "ev": ev, "response": r} for a, f, ev, r in ranked])


if __name__ == "__main__":
    # self-test against a foldy villain: a pot-size bet should beat calling/folding.
    demo_prof = {"fold_to_pressure": 0.7, "cont_freq": 0.2, "exploits": {"overfold": 1}}
    best, ranked = evaluate_pathways({}, demo_prof, pot_bb=20, to_call_bb=6, equity_hint=0.4)
    print("best:", best)
    for r in ranked:
        print("  ", r)
