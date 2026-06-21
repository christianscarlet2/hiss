"""mischief.py -- the MISCHIEF layer of the brain.

Deviates from the regular (PAR) strategy to ANTAGONIZE opponents with odd / random bets -- the
min "1-bb" stab, donk leads into the preflop raiser, double-bets / overbets, min-raise needles,
weird off-size sizings -- WHENEVER it can do so cheaply. It never abandons the regular strategy:
it records the PAR action and only mutates it inside a SAFETY ENVELOPE (small pots, position, fold
equity, exploitable / tiltable villains), so the long-run EV stays AT PAR while the table-feel
turns chaotic and the opponents tilt. The brain's INTELLIGENCE / wisdom gate runs AFTER mischief,
so a reckless prank is still vetoed before it commits -- mischief can annoy, it can't punt.

  compute_mischief(g, intuition, current, prof, energy=0.0)
     -> {fired, kind, action, size_bb, par_action, par_size_bb, cost_bb, aggro_nudge, bluff_nudge, note}

"Secretly playing to par": par_action/par_size_bb are always preserved; if a prank would break the
envelope it silently reverts to par. The frequency scales with how exploitable / tilted the table is
and with the ENERGY in the air (astrology feeds the pineal which feeds mischief) -- more charge in
the air, more mischief.
"""
import random


def num(v, d=0.0):
    try:
        return float(v)
    except Exception:
        return d


# odd, deliberately "wrong-looking" pot fractions -- nothing clean, everything needling
_ODD_FRACS = [0.13, 0.27, 0.41, 0.55, 0.69, 0.77, 0.88, 1.13, 1.33, 1.66]


def _spot_rng(g, current):
    """Deterministic per-spot RNG: stable within a decision, varied across spots (no global Random
    state to leak between hands; reproducible for replay)."""
    seed = hash((str(g.get("handnumber")), int(num(g["syms"].get("betround"))),
                 current.get("action"), round(num(current.get("size_bb")), 1)))
    return random.Random(seed)


def _base_rate(intuition, prof, energy, smallball=True):
    """How often we cause mischief, 0..~0.7. More vs exploitable / tilting / fishy villains, more in
    position-flavoured spots, more when there's ENERGY in the air, and -- under SMALL BALL -- more
    in general so we RUN THE TABLE: get involved in many pots, act often, and brute-force +EV exits.
    Bounded so it stays 'at par'."""
    ex = intuition.get("exploit"); persona = intuition.get("persona")
    r = 0.10
    if smallball:
        r += 0.15                       # SMALL BALL: be in the action, run the table
    if ex in ("attack_fold", "attack_fast", "tilt_value", "image_bluff"):
        r += 0.22                       # they fold / they're steaming -> needle them relentlessly
    if persona in ("maniac", "fish_hunter"):
        r += 0.12
    if intuition.get("tilt", 0) >= 0.3:
        r += 0.12
    fp = num((prof or {}).get("fold_to_pressure"), -1)
    if fp >= 0.5:
        r += 0.10
    r += 0.20 * max(0.0, min(1.0, energy))   # the air's charge feeds the chaos
    return max(0.0, min(0.7, r))


def _scare_texture(g):
    """Board texture that CREDIBLY reps a big hand if we fire: a 3+ flush board, a paired board, or a
    high broadway top card. Drives the PRETEND / representation line. Cheap, from stock signals."""
    s = g["syms"]
    if num(s.get("nsuitedcommon")) >= 3:
        return "flush"
    board = (g.get("board") or "").split()
    ranks = [c[0] for c in board if c]
    if len(ranks) != len(set(ranks)):
        return "paired"
    if any(r in ("A", "K") for r in ranks[:3]):
        return "broadway"
    return None


def compute_mischief(g, intuition, current, prof, energy=0.0, affinity=1.0):
    s = g["syms"]
    bb = num(s.get("bblind"), 1.0) or 1.0
    pot = num(s.get("PotSize")); a2c = num(s.get("AmountToCall"))
    stack = num(s.get("StackSize"))
    br = int(num(s.get("betround")))
    in_pos = num(s.get("f$InPositionPost")) > 0
    strong = num(s.get("f$HaveStrongMade")) > 0
    draw = num(s.get("f$HaveBigDraw")) > 0
    nplay = num(s.get("nplayersplaying"), 2)
    waspfr = num(s.get("f$WasPreflopRaiser")) > 0

    par_action = current.get("action")
    par_size_bb = num(current.get("size_bb"))
    out = {"fired": False, "kind": None, "action": par_action, "size_bb": par_size_bb,
           "par_action": par_action, "par_size_bb": par_size_bb, "cost_bb": 0.0,
           "aggro_nudge": 0.0, "bluff_nudge": 0.0, "note": None}

    # never prank away a committed / show-of-force / genuinely strong-line spot, and never on a
    # decision the wisdom layer would obviously hate (huge pots): keep it CHEAP.
    if intuition.get("show_of_force") or num(s.get("f$Committed")) > 0:
        return out
    pot_bb = pot / bb
    # the safety envelope: the most a prank may COST beyond par (keeps EV at par)
    envelope_bb = max(1.5, min(0.06 * (stack / bb if stack else 100.0), 4.0))

    rng = _spot_rng(g, current)
    ex = intuition.get("exploit")
    foldable = bool(ex in ("attack_fold", "attack_fast", "image_bluff")
                    or num((prof or {}).get("fold_to_pressure"), -1) >= 0.5)
    kind = None; action = par_action; size_bb = par_size_bb; note = None

    # 0) PRETEND / REPRESENTATION (tied to mischief): when the board CREDIBLY reps a big hand (a scare
    #    texture) and we PREDICT the villain is weak / foldable, fire heavy to REP it and fold them out
    #    -- even with air, optimizing the pathway that wins the pot now. Deliberate, not random; the
    #    INTELLIGENCE/wisdom gate downstream still vetoes it if the fold equity isn't really there.
    scare = _scare_texture(g)
    if scare and foldable and par_action in ("check", "none", "fold", "call") and br >= 2 \
       and (in_pos or nplay <= 2) and not strong and rng.random() < 0.6:
        frac = rng.choice([0.85, 1.0, 1.25])       # pot-to-overbet: the size that reps the nuts
        kind = "represent"; action = "raise" if a2c > 0 else "bet"
        size_bb = round(max(a2c * 2.2, pot * frac) / bb, 2)
        note = "PRETEND: rep the %s, fold out a weak villain" % scare

    # No deliberate rep? Roll for random mischief; the OBSERVER branch scales how hard we antagonize
    # (ATTACK/TILT -> more, STEADY -> less) and SMALL BALL keeps us in the action.
    if kind is None:
        rate = max(0.0, min(0.78, _base_rate(intuition, prof, energy) * max(0.0, affinity)))
        if rng.random() > rate:
            return out

    # ---- catalogue of pranks, chosen by the spot ----
    # 1) THE 1-BB STAB ("1bet"): when we'd CHECK in a tiny pot (esp. in position), toss a single big
    #    blind in -- a needling min-stab that prices them in to nothing but tilts them into spew.
    if kind is None and par_action in ("check", "none") and a2c == 0 and pot_bb <= 14 and (in_pos or nplay <= 2):
        if rng.random() < 0.6:
            kind = "one_bb_stab"; action = "bet"; size_bb = 1.0
            note = "1-bb needle into a checked pot"

    # 2) DONK LEAD: out of position into the preflop raiser, lead a small ODD size instead of the
    #    expected check -- seizes initiative and scrambles their c-bet plan.
    if kind is None and par_action in ("check", "none") and a2c == 0 and br >= 2 and not waspfr \
       and not in_pos and pot_bb <= 22:
        frac = rng.choice([0.27, 0.33, 0.41])
        kind = "donk_lead"; action = "bet"; size_bb = round(pot * frac / bb, 2)
        note = "donk-lead %.0f%% into the raiser" % (frac * 100)

    # 3) MIN-RAISE NEEDLE: facing a small bet with fold equity, min-raise to annoy and steal -- cheap
    #    pressure that folds out their air and induces tilt-jams we can fold.
    if kind is None and par_action in ("call", "fold") and 0 < a2c <= 0.5 * pot \
       and num((prof or {}).get("fold_to_pressure"), -1) >= 0.45:
        kind = "min_raise_needle"; action = "raise"; size_bb = round((a2c * 2 + pot * 0.1) / bb, 2)
        note = "min-raise needle vs a foldy villain"

    # 4) DOUBLE-BET / OVERBET: when we're ALREADY value-betting/raising, blow the size up to an odd
    #    over-pot -- a polarizing double-up that taxes stations and bluff-catchers alike.
    if kind is None and par_action in ("raise", "bet") and par_size_bb > 0 and (strong or draw):
        frac = rng.choice([1.13, 1.33, 1.66])
        kind = "double_bet"; action = par_action
        size_bb = round(max(par_size_bb, pot * frac / bb), 2)
        note = "double-up overbet (%.0f%% pot)" % (frac * 100)

    # 5) ODD-SIZE SCRAMBLE: any other bet/raise -> jitter to a deliberately weird fraction so they
    #    can never size-read us.
    if kind is None and par_action in ("raise", "bet") and par_size_bb > 0:
        frac = rng.choice(_ODD_FRACS)
        kind = "odd_size"; action = par_action; size_bb = round(pot * frac / bb, 2)
        note = "odd-size scramble (%.0f%% pot)" % (frac * 100)

    if kind is None:
        return out

    # ---- enforce the SAFETY ENVELOPE: keep the EXTRA cost vs par cheap, else revert to par ----
    new_cost_bb = size_bb if action in ("bet", "raise") else 0.0
    par_cost_bb = par_size_bb if par_action in ("bet", "raise") else (a2c / bb if par_action == "call" else 0.0)
    extra = new_cost_bb - par_cost_bb
    # double-bet/overbet are value lines (we have it) -> their extra is fine; pure needles must stay
    # inside the envelope.
    if kind not in ("double_bet", "represent") and extra > envelope_bb:
        return out                      # too expensive a prank -> secretly play to par
    # 'represent' is a deliberate +EV bluff (value if they fold), exempt from the cheap envelope; the
    # downstream INTELLIGENCE/wisdom gate vetoes it when the predicted fold equity is too thin.
    if size_bb * bb > stack and stack > 0:
        size_bb = round(stack / bb, 2)  # never bet more than the stack

    out.update({"fired": True, "kind": kind, "action": action, "size_bb": size_bb,
                "cost_bb": round(max(0.0, extra), 2),
                "aggro_nudge": 0.12 if action in ("bet", "raise") else 0.0,
                "bluff_nudge": 0.20 if kind == "represent" else (0.15 if kind in ("one_bb_stab", "donk_lead", "min_raise_needle") else 0.0),
                "note": note})
    return out


if __name__ == "__main__":
    # smoke test
    g = {"handnumber": "123", "syms": {"betround": 2, "bblind": 1.0, "PotSize": 8.0, "AmountToCall": 0.0,
         "StackSize": 80.0, "f$InPositionPost": 1, "f$HaveStrongMade": 0, "f$HaveBigDraw": 0,
         "nplayersplaying": 2, "f$WasPreflopRaiser": 0, "f$Committed": 0}}
    intu = {"exploit": "attack_fold", "persona": "maniac", "tilt": 0.4, "show_of_force": False}
    cur = {"action": "check", "size_bb": 0.0}
    print(compute_mischief(g, intu, cur, {"fold_to_pressure": 0.7}, energy=0.8))
