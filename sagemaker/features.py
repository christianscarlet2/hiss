"""
features.py — the canonical Hiss→SageMaker information set.

ONE contract shared by the data emitter (Linux daemon), the training job, and live
inference. A "decision row" is the JSON the headless Hiss /decide endpoint produces at
each action point: {"action": <str>, "sym": {<engine-symbol>: <double>, ...}} plus the
table view we sent in. This module turns a decision row into a fixed-width float vector
(features) + the supervised targets (action class, bet-size fraction) + carries the
reward for the value head / offline-RL (joined from replay outcomes by hand id).

Keep this file dependency-light (numpy only) so it imports cleanly in the daemon, the
SageMaker container, and the inference path.
"""
from __future__ import annotations
import math
import numpy as np

# --- the engine symbols we read per decision (the daemon emits sym[...] for these) -----
# All are produced by the harmonized OHF + engine on Linux (verified loading 2026-06-15).
NUMERIC_SYMBOLS = [
    # equity / board texture
    "prwin", "handrank169",
    "f$BoardWet", "f$BoardDry", "f$ScaryBoard",
    "f$BoardHighCardFoldy", "f$BoardHighCardSticky", "f$BoardParched",
    # made-hand / draw (engine-native booleans)
    "HaveTopPair", "HaveOverPair", "HaveSet", "HaveTwoPair", "HaveFlush", "HaveStraight",
    "FlushPossible", "StraightPossible",
    "f$HaveStrongMade", "f$HaveBigMade", "f$HaveOnePair", "f$HaveComboDraw",
    "f$HaveBigDraw", "f$HaveWeakDraw",
    # position / players / street
    "betround", "nplayersdealt", "nopponentsplaying",
    "f$InPositionPre", "f$InPositionPost", "f$HeadsUpPot", "f$Is6Max",
    # stacks / pot / commitment
    "StackSize", "f$EffStack", "PotSize", "AmountToCall",
    "f$SPR", "f$Committed", "f$M", "f$Mzone",
    "f$DeepStack", "f$ShortStack", "f$PushFoldStack", "f$ReshoveSpot",
    "bblind", "sblind", "f$AnteTotal",
    # tournament / ICM
    "f$Stage", "tgi_players_remaining", "f$NearBubble", "f$InMoney",
    "f$BubbleTighten", "icm_fold", "icm_callwin", "icm_calllose",
    # opponent model (HUD) — 0 headless until PT4 wired, archetype flags from OHF
    "f$Opp_VPIP", "f$Opp_PFR", "f$Opp_AF", "f$Opp_WTSD", "f$Opp_Hands",
    "f$Opp_Known", "f$Opp_IsNit", "f$Opp_IsLoose", "f$Opp_IsStation", "f$Opp_IsPassive",
    "f$Opp_IsAggro", "f$Opp_IsTAG", "f$Opp_IsLAG", "f$Opp_IsFish", "f$Opp_IsManiac",
    "f$Opp_Foldy", "f$Opp_ThreeBetsLight", "f$Opp_PotCommitted",
    # timing tells
    "lastraiseractiontime", "f$TimingSaysWeak", "f$TimingSaysStrong",
    # action history
    "Raises", "Calls", "Bets",
    "BotRaisedBeforeFlop", "BotRaisedOnFlop", "BotRaisedOnTurn",
    # config / dials in effect (context)
    "f$Style", "f$OpenBase", "f$CbetFreq", "f$ThreeBetBluffFreq", "f$DoubleBarrel",
    # scrape trust
    "validator_ok", "validator_confidence",
]

# hole-card features computed from the dealt cards (not engine symbols)
HOLE_FEATURES = ["hole_hi", "hole_lo", "hole_suited", "hole_pair", "hole_gap", "hole_broadway"]

FEATURE_COLUMNS = NUMERIC_SYMBOLS + HOLE_FEATURES
N_FEATURES = len(FEATURE_COLUMNS)

# supervised target: the action the bot took, bucketed
ACTION_CLASSES = ["fold", "check", "call", "raise", "allin"]
ACTION_TO_IDX = {a: i for i, a in enumerate(ACTION_CLASSES)}

_RANK = {r: i for i, r in enumerate("23456789TJQKA", start=2)}  # 2..14


def _card_rank(c: str) -> int:
    return _RANK.get(c[0].upper(), 0) if c else 0


def hole_features(hole: str) -> dict:
    """hole like 'AsKh' -> normalized hi/lo rank, suited, pair, gap, broadway."""
    if not hole or len(hole) < 4:
        return {k: 0.0 for k in HOLE_FEATURES}
    r1, s1, r2, s2 = _card_rank(hole[0:1]), hole[1], _card_rank(hole[2:3]), hole[3]
    hi, lo = max(r1, r2), min(r1, r2)
    return {
        "hole_hi": hi / 14.0,
        "hole_lo": lo / 14.0,
        "hole_suited": 1.0 if s1 == s2 else 0.0,
        "hole_pair": 1.0 if r1 == r2 else 0.0,
        "hole_gap": (hi - lo) / 14.0,
        "hole_broadway": 1.0 if lo >= 10 else 0.0,
    }


def _f(x) -> float:
    try:
        v = float(x)
        return v if math.isfinite(v) else 0.0
    except (TypeError, ValueError):
        return 0.0


def vectorize(row: dict) -> np.ndarray:
    """decision row {sym:{...}, hole, board, ...} -> float32 feature vector (N_FEATURES)."""
    sym = row.get("sym", {}) or {}
    hf = hole_features(row.get("hole", ""))
    out = np.zeros(N_FEATURES, dtype=np.float32)
    for i, name in enumerate(NUMERIC_SYMBOLS):
        out[i] = _f(sym.get(name, 0.0))
    base = len(NUMERIC_SYMBOLS)
    for j, name in enumerate(HOLE_FEATURES):
        out[base + j] = hf[name]
    return out


def action_index(action: str) -> int:
    return ACTION_TO_IDX.get((action or "fold").lower(), 0)


def betsize_fraction(row: dict) -> float:
    """target bet sizing as a fraction of pot (0 when not raising); from the emitted raise-to."""
    sym = row.get("sym", {}) or {}
    pot = _f(sym.get("PotSize"))
    raiseto = _f(row.get("raiseto", sym.get("f$betsize")))
    if pot <= 0 or raiseto <= 0:
        return 0.0
    return float(np.clip(raiseto / pot, 0.0, 4.0)) / 4.0  # normalized to [0,1], cap 4x pot
