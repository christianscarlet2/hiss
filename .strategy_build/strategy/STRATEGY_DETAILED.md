# ScarletBeast Power Hold'em — Detailed Strategy Reference

A function-by-function walk-through of every segment, with the source citation
(`[N]` = Negreanu Small Ball, `[B]` = Brunson Power Poker) for each rule.

> **Reading the code.** OpenPPL evaluates a function top-to-bottom and fires the
> first `WHEN <condition> <action> FORCE` whose condition is true. `RETURN f$x`
> delegates to another function. `randomround` is a fresh `[0,1)` value that is
> **stable for the whole betting round**, so the bot commits to one mixed-strategy
> branch per street instead of flip-flopping — this is how Negreanu's and
> Brunson's "mix it up / stay unpredictable" advice is implemented without
> incoherence.

---

## 00_notes.ohf — equity engine

| Function | What it does |
|----------|--------------|
| `f$notes` | Documentation header (no logic). |
| `f$prwin_number_of_iterations` | 20 000 Monte-Carlo iterations for the `prwin` win-probability estimate. |
| `f$prwin_number_of_opponents` | Simulates against `nopponentsplaying` — the live opponents, not the whole table. |

---

## 05_config.ohf — the control panel

Everything tunable lives here. Edit one line, reload, done.

| Function | Returns | Notes / source |
|----------|---------|----------------|
| `f$Style` | 0 / 1 / 2 | **The style dial.** 0 small-ball, 1 power-poker, 2 hybrid. |
| `f$EffStack` | BB | Effective stack = min(our stack, biggest active opponent). |
| `f$DeepStack` | bool | `f$EffStack >= 60` → full speculative play. [N pp.33-34] |
| `f$ShortStack` | bool | `<= 25` → tighten, stop limping speculative hands. [B S8.3] |
| `f$PushFoldStack` | bool | `<= 10` → push/fold game. [N p.34 "below 10 bets"] |
| `f$InPositionPre` | bool | We are on the button or cut-off (last/near-last pre-flop). |
| `f$InPositionPost` | bool | Button/cut-off, or heads-up and not the small blind. |
| `f$HeadsUpPot` | bool | Exactly one opponent contesting the pot. |
| `f$OpenBase` | 2.5 / 3.5 | First-in open size: small-ball 2.5x, power 3.5x. [N pp.18-19] |
| `f$Open_RaiseTo` | BB | `f$OpenBase + Calls` — add 1bb per limper to isolate. [N p.24][B] |
| `f$ThreeBet_RaiseTo` | 9 / 11 / 13 | Bigger out of position. [N pp.26-27] |
| `f$FourBet_RaiseTo` | 22 / 26 | 4-bet-for-value target. |
| `f$BetPctFlop/Turn/River` | 55–90 | Post-flop bet as % of pot; bigger on wet boards & in power style. Same size for value and bluff (no tell). [N p.37-38] |
| `f$CbetFreq` | 0.60–0.90 | Air c-bet frequency as the pre-flop raiser. [B ~90%][N ~60%] |
| `f$CheapDraw` | bool | Price ≤ ~25% of pot. |
| `f$DecentPrice` | bool | Price ≤ ~33% of pot. |
| `f$WithinSpecRisk` | bool | Speculative call risks ≤ ~10% of stack. [N pp.29-30][B S8.6] |

---

## 10_opponents.ohf — "play the player"

Both books insist you play the **opponent's** hand, not just your own, and adjust
to player type. A bot can't read faces or table image, so this module substitutes
the **PokerTracker HUD stats** of the last raiser (the player we must react to).
Stats default to neutral until a `>= 30`-hand sample exists (`f$Opp_Known`).

| Function | Read | Strategic effect |
|----------|------|------------------|
| `f$Opp_VPIP/PFR/AF/WTSD` | raw stats (or neutral defaults) | inputs to the archetypes below |
| `f$Opp_IsNit` | VPIP < 17 | respect raises; 3-bet-bluff / steal more. [N p.30] |
| `f$Opp_IsLoose` | VPIP > 32 | isolate / value 3-bet wider. [N p.19] |
| `f$Opp_IsStation` | WTSD > 34 (or AF<1 & VPIP>40) | **value-bet thin, NEVER bluff.** [N p.158][B B9.3] |
| `f$Opp_IsPassive` | AF < 1.3 | their bets mean a real hand. |
| `f$Opp_IsAggro` | AF > 3.0 | induce, trap, bluff-catch wider. [N p.53] |
| `f$Opp_ThreeBetsLight` | PFR high & ≈ VPIP | don't 3-bet into him; just flat & outplay. [N p.22] |
| `f$Opp_Foldy` | nit, or low WTSD & VPIP | c-bets and steals print money. [B R1.2] |

---

## 20_lists.ohf — hand ranges

Shared ground between the authors: both prize **suited connectors** and **small
pairs** (cheap hands that flop monsters and win big pots) [N pp.15-16][B p.487],
Brunson adds "ace/king + any suited card = two shots" [B p.508], Negreanu adds wide
late-position steals.

| List | Role |
|------|------|
| `listPremium` | get-it-in-pre tier (AA KK QQ AKs AKo) |
| `listBigPair` | AA KK QQ JJ |
| `listSetMine` | 22–TT, flatted to flop a set (never reraised pre) [N p.21][B p.481] |
| `listOpenEP/MP/LP/BTN/SB` | open-raise ranges, widening by position (~16% → ~48%) |
| `list3betValue` | AA KK QQ JJ AKs AKo AQs |
| `list3betBluff` | ace-blockers + suited playables [N p.17,26] |
| `listCallOpen` | flat an open (set-mine + suited playables, mostly IP) |
| `listCall3bet` | flat a 3-bet in position to outplay [N p.22] |
| `list4betValue` / `list4betValuePower` | AA KK AKs (+ QQ AKo in power style) |
| `listTroubleOffsuit` | Brunson "trouble hands" — fold offsuit big cards to a raise [B p.505] |

---

## 30_classify.ohf — what do I have, on what board

Wraps the engine's `Have*` and board symbols into strategy concepts.

| Function | Meaning |
|----------|---------|
| `f$HaveBigMade` | straight or better — the commit hands [N "near-nuts"][B] |
| `f$HaveStrongMade` | two-pair/set/big-made, or a clean overpair/top-pair on a dry board |
| `f$HaveOnePair` | overpair or top pair — the **"don't go broke with one pair"** trap class [N p.125] |
| `f$HaveDecentMade` / `f$HaveMarginalMade` | showdown-value tiers |
| `f$HaveBigDraw` | nut/flush draw or open-ender — played fast [B D6.1-6.2] |
| `f$HaveComboDraw` | pair+draw or two draws — "two ways to win", get it in [B D6.6] |
| `f$HaveWeakDraw` / `f$HaveAnyDraw` | gutshots, backdoors |
| `f$BoardWet` / `f$BoardDry` | draw-heavy vs hit-or-miss texture [N p.63] |
| `f$ScaryBoard` | Brunson "frightening flop" — don't bluff into it [B R5.3] |
| `f$WasPreflopRaiser` | did we take the betting lead pre-flop |

---

## 40_preflop.ohf — the pre-flop tree

`f$preflop` dispatches by the number of raises this round:

```
f$PushFoldStack  -> f$preflop_pushfold   (<=10bb: jam/fold)
Raises >= 2      -> f$preflop_vs_3bet     (4-bet value / flat IP / fold)
Raises == 1      -> f$preflop_vs_raise    (3-bet value+bluff / flat / fold)
else             -> f$preflop_open        (open or, with limpers, f$preflop_limped)
```

Highlights:
- **Open** by position with the `listOpen*` ranges; short stacks tighten to the MP range.
- **Limped pots** → isolate strong/late hands with a bigger raise; over-limp
  speculative hands cheaply when deep. [N p.24][B]
- **Vs a raise** → flat premiums vs a light-3-better instead of 3-betting [N p.22];
  3-bet for value; 3-bet-bluff a foldy/loose opener *in position at a mixed
  frequency*; flat to set-mine; defend the big blind at a price.
- **Vs a 3-bet** → 4-bet AA/KK/AKs (power style adds QQ/AK); flat QQ/JJ/AK in
  position; set-mine only when deep **and** risking ≤10% of stack. [N pp.28-30]
- **Push/fold** → position-scaled jam range; call shoves only with premiums.

---

## 50_flop.ohf — "practically the whole game" [B R5.1]

`f$flop` splits on whether there's a bet to us.

**`f$flop_nobet`** (we can bet or check), in priority order:
1. Strong made hands → value/protection bet (bigger on wet boards).
2. Combo & big draws → semi-bluff. [B D6]
3. One pair → bet on wet boards / vs stations / as the raiser; else pot-control.
4. Air as the pre-flop raiser → c-bet at `f$CbetFreq`, **skipping scary boards and
   stations**, biased to heads-up / foldy opponents. [B R1.3][N ~60%]
5. In position with a weak draw → small stab to set up a turn take-away. [N float]
6. Otherwise check.

**`f$flop_vs_bet`** (facing a bet):
1. Big made / set / two-pair → value-raise (charge draws, build pot). [B R5.5]
2. Combo draw → **all-in** (two ways to win). [B D6.6]
3. Big draw → power style raises; otherwise call at a price.
4. One pair → peel at a reasonable price, **never go broke**. [N p.125]
5. Float a c-bet-happy opponent in position with equity; else fold.

---

## 60_turn.ohf — take it away, or slow down

**`f$turn_nobet`**:
- Strong made → value. Combo/big draws → keep semi-bluffing.
- **Float-the-turn take-away**: in position, called the flop, now checked to, vs a
  non-station → bet. [N pp.75-78]
- **Double-barrel**: flop aggressor, non-scary card, foldy opponent → bet. 
- One pair → thin value vs a station / power-style heads-up barrel, **else check**
  for pot control (the core "don't go broke with one pair" turn discipline). [N pp.125-130]

**`f$turn_vs_bet`**: value-raise big hands; shove combo draws; continue big draws at
a price; bluff-catch one pair only at a price vs non-passive players; never stack
off one pair.

---

## 70_river.ohf — value & bluff-catch (no draws left)

Negreanu keeps **river bluff frequency low** (a wild image gets you called) and
sizes every bet by what *this* opponent will call. [N pp.138-160]

**`f$river_nobet`**: value-bet made hands (thinner & smaller vs stations, fuller vs
non-passive); thin-value one pair vs a station / power-style heads-up; a
**low-frequency** triple-barrel bluff only as the flop+turn aggressor heads-up vs a
foldy opponent; else check.

**`f$river_vs_bet`**: value-raise the near-nuts vs non-passive; pay off (bluff-catch)
with strong made hands; bluff-catch one pair only vs bluffy players at a price;
fold vs passive value-bettors.

---

## 90_table.ohf — hopper functions

`f$sitin` (sit back in when sitting out), plus inert `f$sitout` / `f$leave` /
`f$rebuy` stubs you can extend.
