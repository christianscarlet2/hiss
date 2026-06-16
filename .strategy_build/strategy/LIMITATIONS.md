# Limitations — what the books teach that a bot can't fully do

Your instruction was: *if a feature can't be implemented because of Hiss/OpenPPL
limits, first try to build the limitation into the bot, otherwise report it.* This
is that report. Each row is a concept from the source texts, what the limitation
is, and **how it was handled** — ✅ built a substitute, ⚠️ partial/approximated, or
❌ not implementable (reported).

---

## 1. Reads that need a human / live table

| Book concept | Limitation | Handling |
|--------------|-----------|----------|
| **Physical tells** — speed of action, chip handling, body language, "reverse tells", the Gus/French-player reads [N pp.49-50,131-134][B p.466] | No camera, no timing data on a phone-scraped table | ❌ **Not implementable.** Reported. None of the tell-based decisions are coded. |
| **"Feel" / subconscious reads** ("something told me he was weak") [N p.131][B p.430] | Not a computable signal | ❌ Not implementable. Reported. |
| **Tilt detection** (opponent steaming after a beat) [B p.431] | Needs session-long behavioural modelling Hiss doesn't track | ❌ Not implementable. Reported. (PokerTracker AF/VPIP give a *static* aggression read, not a tilt delta.) |
| **Bet-timing / "weak-looking bet" reads** [B p.102][Caro] | ~~Hiss does not expose opponent action latency~~ | ✅ **NOW IMPLEMENTED.** `CSymbolEngineTimingTells` measures each chair's dwell on its `pNactive` highlight and exposes `lastraiseractiontime` (seconds the player we react to took). [`05_config.ohf`](05_config.ohf) derives `f$TimingSaysWeak` (snap ≤ 1.5s → lean weak) / `f$TimingSaysStrong` (tank ≥ 6s → lean strong); the flop/turn/river vs-bet logic folds a bare one pair to a tanked bet and bluff-catches/floats wider vs a snap. Heads-up only, soft modifier, `f$UseTimingTells = 0` to disable. |

## 2. Opponent typing & history

| Book concept | Limitation | Handling |
|--------------|-----------|----------|
| **"Play the opponent, not your hand"** [N P1][B P7.1] — the spine of both books | OpenPPL has no innate opponent memory | ✅ **Built in** via [`10_opponents.ohf`](10_opponents.ohf): the PokerTracker HUD stats of the last raiser drive every archetype branch. |
| **Player archetypes** — nit / station / loose / tricky / rock [N pp.19-22,158][B pp.432-435] | Qualitative live judgments | ⚠️ **Approximated** by VPIP/PFR/AF/WTSD thresholds. "Tricky / capable of a check-raise bluff" is *not* modelled — there's no stat for it. |
| **Weak-player exploit** — "if a weak player double-checks, bet automatically; if he bets, he has it" [B B9.5] | Requires a reliable weak/strong classification per villain | ⚠️ Partially: `f$Opp_Foldy` / `f$Opp_IsStation` stand in. The full "auto-bet when a known-weak player shows weakness twice" is **disabled by default** (assumes a thinking population) to avoid spewing into unknowns. |

## 3. Table image, metagame & leveling

| Book concept | Limitation | Handling |
|--------------|-----------|----------|
| **Your own table image** — "you've raised 3 of the last 4", "your wild image gets you called" [N pp.20,150-154][B p.483] | Hiss/OpenPPL does not track *our own* recent-hand history across hands for the bot to perceive how it's seen | ⚠️ **Reproduced behaviourally, not perceptually.** We can't know how others see us, but we *act* the way the image-aware player acts: high, consistent c-bet frequency (`f$CbetFreq`) and uniform bet sizing (value = bluff size) so we're hard to read — exactly the result the image advice is chasing. The *adaptive* "now that they've seen me wild, switch gears" loop is **not** implemented. |
| **Leveling / "what does he put me on"** — most river bluff/sizing decisions [N pp.183-190] | Requires modelling the opponent's model of us | ❌ Not implementable as written. ⚠️ Substituted with stat-based bluff gating (`f$Opp_Foldy`) + a deliberately **low** river-bluff frequency. |

## 4. Multi-street planning

| Book concept | Limitation | Handling |
|--------------|-----------|----------|
| **"Decide on the turn what you'll bet on every river runout"** [N pp.113-123] | OpenPPL evaluates each street independently; there is no carried-forward plan object | ⚠️ **Approximated** with the betting-history symbols (`BotRaisedOnFlop`, `BotCalledOnFlop`, `BotRaisedOnTurn`): the float-take-away, double-barrel, and triple-barrel lines reconstruct "what street was I the aggressor on" rather than executing a pre-committed plan. Good enough for the common lines; not a true game-tree plan. |
| **Bluff-out counting / combined real+bluff outs** [N pp.111-123] | No symbol enumerates "cards that miss me but let me represent" | ⚠️ Approximated by `prwin` + the pot-odds helpers (`f$CheapDraw`/`f$DecentPrice`) and the semi-bluff lines, rather than an exact bluff-out tally. |

## 5. Bet-sizing nuance

| Book concept | Limitation | Handling |
|--------------|-----------|----------|
| **Size every bet by what THIS specific opponent will call** [N p.160] | Needs a per-villain calling-frequency model | ⚠️ Approximated: bet smaller vs stations for thin value, fuller vs non-passive (`f$BetPct*` + archetype branches). Not a continuous per-opponent function. |
| **"Defensive bet" OOP to set the price** [N pp.155-160] | Implementable in principle but read-dependent and easy to turn into spew | ⚠️ Folded into pot-control checking instead of a distinct under-bet line, to stay safe. |
| **Fractional big-blind bet amounts** (e.g. a 2.5x open, ⅗-pot bets) | The phone table is bet via the two-successive-clicks numpad; non-integer chip amounts depend on the casino accepting decimals (`nDecimalPoint` region) | ⚠️ Works where the table accepts decimals; if your table only takes whole chips, set `f$OpenBase = 3` and prefer the named pot fractions. Flagged for you to verify on poker.scarletbeast.com. |

## 6. Things that aren't in-game decisions

| Book concept | Handling |
|--------------|----------|
| **Insurance / side-bets** [B pp.511-512] | ❌ Out of scope — not a betting-round decision the autoplayer makes. |
| **Game/seat selection, buy-in sizing, sitting with the biggest stack** [B p.450] | ❌ Out of scope for the decision file (a table-selection concern). |
| **"Rush" self-loosening — play every pot after you win one** [B R1.9] | ⚠️ *Observable in principle* (we know if we won the last pot) but **not implemented** — it's −EV without the live-game context Brunson assumes, and risks the bot spewing. Left out by design; could be added with a memory symbol if you want it. |

---

## Net result

- The **engine** of both books — position, speculative hands, aggression, pot
  control, "don't go broke with one pair", opponent-aware adjustments, balanced
  (randomised) frequencies — **is fully implemented**.
- What's lost is the rest of the **human read layer**: physical tells, table-image
  adaptation, leveling wars, tilt detection, and true multi-street planning. Where a
  machine-observable proxy existed (PokerTracker stats, betting-action history, board
  texture, `prwin`, `randomround`, **and now bet-timing**), it was **built in**. Where
  none existed, the feature is **reported here** rather than faked.

**Bet-timing reads are now implemented** (see the row above). A concrete, phased
implementation plan for the *remaining* ❌ items — physical tells, feel/soul reads,
tilt detection, true leveling, plus the self-image / gear-changing memory — lives in
**[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md)**.

---

# Hiss-improvement backlog — Tier-1 library expansion (2026-06-15)

New engine work surfaced while folding the wider NLHE library (Harrington, Kill
Everyone/Phil, SSNL, Theory of Poker, Crushing the Microstakes, Poker Math That
Matters, the SNG canon, Ed Miller, Tri Nguyen) into the OHF. These are concepts the
books teach that OpenPPL/Hiss **cannot express today** — ranked by value. The dials
that WERE implementable this round (M/M-zones, SPR/commitment, exact required-equity,
15× set-mining, bubble-scaled ICM gate, TAG/LAG/fish/maniac typing, value-big-vs-station)
are already live in 05_config/10_opponents/50_flop.

| # | Concept (source) | Why the OHF can't do it | Engine work needed |
|---|------------------|-------------------------|--------------------|
| 1 | **Real lobby payout curve** [SNG][KE] | `f$icm_prize1..8` is a hardcoded top-heavy placeholder where all 8 places "pay", so `f$PaidPlaces`/`f$NearBubble`/`f$BubbleTighten` stay inert. The stage machinery is built and waiting. | Feed the actual payout table + places-paid from the existing `lobby_fetch.sh → settings.lobby_info` pipe into the prize-curve symbols + `tgi_players_remaining`. **Highest leverage — unlocks all the bubble dials already written.** |
| 2 | **Per-opponent bubble factor / true N-player ICM solver** [KE p120-126][SNG] | `f$ICM_CallEV` is a single static one-shot calc; real bubble play needs every live stack + each villain's range run through ICM to get a per-villain risk premium and the exact push/call frontier. | Extend `CSymbolEngineICM` to enumerate opponent calling ranges and expose `bubble_factor[chair]` + a threshold-range symbol, not just one EV. |
| 3 | **Node-by-node range narrowing / combinatorics** [EM HRH][TN] | OHF sees only aggregate HUD stats + the current board — no per-street action-history range object, no combo counter, no card-removal math. Hand-reading is the core of three of these books. | A street-indexed range/combinatorics module (Flopzilla-style) fed by `f$Opp_*` + betting history, exposing combos-that-beat-us / fold-equity-by-range. |
| 4 | **Size-bucketed fold stats** [PMM][MoH][CtM] | `f$AutoProfitBluff`/`f$MDF`/double-barrel exploits need *fold-to-bet-of-size-B by street*, not one aggregate fold-to-cbet. HUD currently exposes only flop AF / coarse buckets. | Surface `pt_fold_flop_cbet`, `pt_fold_turn_cbet`, `pt_fold_to_3bet`, `pt_fold_to_4bet`, `pt_3bet` (heartbeat-thread only, per the PT4-race rule) as symbols. |
| 5 | **Multi-street line planning** [SSNL][ToP][N] | OpenPPL decides one street per heartbeat with no carried-forward plan (check-raise-all-in vs lead-lead-shove to be the last bet with a draw OOP). Approximated today only by betting-history symbols. | A persisted "intended line" state across heartbeats with board-texture-conditioned barrel triggers. |
| 6 | **Mixed / card-anchored randomization** [ToP ch.19][KE] | `randomround` is decision-time noise decorrelated from holdings, so a thinking villain can't be kept indifferent; no per-hand "push X% of the time" table. | A deterministic hash of hole cards → [0,1) exposed as a symbol for card-anchored frequency mixing; optional per-handrank push-mix table. |
| 7 | **Per-session dynamic / Bayesian opponent updating** [EM HRH p146][CtM p209] | Only the static long-run PT4 sample is visible; can't raise P(light-3bettor) after observing reraises this session, or value-bet lighter after stacking a fish who now thinks we bluff. | A per-seat Bayesian profiler + short-window pot/showdown ledger keyed by persistent player ID (feeds, and overlaps, the tilt-detector skill). |
| 8 | **Multiway field-range model** [EM HRH p133][HoH][CtM] | `f$Opp_*` describes only the last raiser; can't model "which of 3 opponents is the fish vs the TAG" or P(someone holds a premium) across the field. | Per-seat archetype tagging within a single hand + a multiway-aware flop dispatcher; `n×x` premium-behind estimator. |
| 9 | **Q-ratio (stack ÷ field-average stack)** [HoH2 p126] | Needs total-field chip count + players remaining tournament-wide; the table scrape sees only the local table. | Ingest lobby/tournament info (avg stack, players left) into an `f$Q` symbol via the lobby pipe. |
| 10 | **Reverse-implied-odds / implied-odds quality as EV** [PMM][MoH][SM] | `prwin` is raw equity, not equity-when-money-goes-in; no down-weighting of dominated draws or weak-villain payoff. | Range-aware equity that derates dominated draws + a per-opponent implied-odds multiplier learned from replay telemetry. |
| 11 | **Board-texture / perceived-range classifier** [CtM p137-147][EM] | Bot has `f$BoardWet/Dry/Scary` but no paired/dry-rag/broadway/monotone/"bingo" classes and no model of whose range the board hits. | A lightweight board-texture classifier exposed as symbols, feeding three-tier c-bet sizing and range-advantage barreling. |
| 12 | **Bubble timing / seat-geometry tactics** [SoS p85][SNG] | Stalling for the blinds on the bubble and "big stack on my left = fewer steals" need turn-timing control + seat-relative stack geometry the autoplayer doesn't model. | Turn-timing control + a seat-relative stack-position symbol set. |

Lower-priority / analysis-only (not live dials): Sklansky-dollar per-decision EV
accounting from replay hands (a leak-finder for the improvement loop, [MoH p740]);
re-OCR of Harrington Vol 1's two-column body and the "Effective M for short tables"
multiplier (Vol 2 p277) at higher DPI; obtain a full (non-promo) copy of *NL Theory
and Practice* and the *Professional No-Limit* .mobi for direct mining.

---

# Hiss-improvement backlog — Tier-2 Wave-2 library expansion (2026-06-15)

MTT decision-logs ([WPT1/2]), Gordon's combinatoric NLHE ([GG]), and the
philosophy layer ([Tao]/[SunTzu]/[48L]/[Prince]) plus a batch of off-target
FIXED-LIMIT / intro books ([LGY][CNL][AOW][HCC][ANL][EoP][IPP][8M][PPM][PMVT]
[MC][NW][PKR] etc.). **Implemented this wave:** SB opens 3x ([WPT1], `f$OpenBase`);
MTT-stage set-mine guard ([WPT2], `f$SetMineOK`); high-card c-bet texture ([GG],
`f$BoardHighCardFoldy`/`Sticky`/`Parched` + 50_flop air-c-bet gate); turn geometry
symbol ([GG], `f$TurnBetGeo`, available); "don't press a desperate foe" bluff gate
([SunTzu VII.36], `f$Opp_PotCommitted` → `f$DoubleBarrel`/river-bluff/3bet-bluff-freq);
and ~15 [Tao]/[SunTzu]/[48L]/[Prince] **citation reinforcements** on existing dials
(no behavior change). The items below are NOT encodable today.

| # | Concept (source) | Why the OHF can't do it | Engine work needed |
|---|------------------|-------------------------|--------------------|
| 13 | **Reshove-aware opening** [WPT1] — fold A7o/JTo-type opens when a 12-20bb light-reshove stack sits BEHIND ("never open a hand you aren't willing to call a shove with"). Hero's own short side is covered (`f$ReshoveSpot`/push-fold). | No symbol for the **minimum stack of players yet to act**; `EffectiveMaxStacksizeOfActiveOpponents` is the max, not the shortest-behind. | A new C++ `min_stack_behind` / `shortest_yet_to_act` symbol to gate `listOpenBTN`/`listOpenLP`. |
| 14 | **`f$TurnBetGeo` wiring** [GG] — symbol is defined and available but not yet used on the turn value line. | `RaiseBy <amount-in-bb>` vs `RaiseBy <pct>%` mixing on one street is awkward and untested in the autoplayer's two-click numpad path. | A bb-amount RaiseBy turn-value branch (gated to ~SPR<=2) verified against the betsize/numpad pipeline. |
| 15 | **4-bet sizing as %-of-stack + 4-bet-bluff range** [PKR][ANL] — `f$FourBet_RaiseTo` is a fixed 22/26bb (right only ~100bb); add A5s/Ax-blocker 4-bet-bluffs at ~1:3 vs a fold-to-4bet TAG, never vs maniac/fish. | No `pt_fold_to_4bet` / `pt_3bet` HUD stats exposed; band-gated light-4bet (35-55bb) and `list4betBluff` were specified in Tier-2 Wave-1 but deferred. | Surface fold-to-4bet/3bet% (heartbeat-thread only) + add `list4betBluff`/`f$FourBetBluffFreq`/`f$Light4betBand`. |
| 16 | **Open-size-relative 3-bet sizing** [PKR] — 3-bet ≈ 3× the OPEN IP / 4× OOP / 5× vs a station, +per-limper; `f$ThreeBet_RaiseTo` is a fixed 9/11/13bb regardless of the open size. | OpenPPL can read `AmountToCall` but the per-villain "size up vs station" and "+per caller" tuning is a continuous function the flat dial can't express cleanly. | A 3-bet sizing function keyed on the facing raise size + caller count + opponent type. |
| 17 | **Opponent steal-frequency-indexed 3-bet/defend** [STX] — gate light reshoves/defends by exact attempt-to-steal% & fold-BB-to-steal% rather than the coarse `f$Opp_IsLoose`. | Those positional HUD stats are not in the `pt_*_raischair` set. | Surface per-position steal% / fold-to-steal% stats. |
| 18 | **Per-street AF & fold-to-cbet exploits** [GG][PKR][HCC] — float a high-flop-AF/low-turn-AF "one-and-done" c-bettor; >65% fold-to-flop-cbet → bluff any board, <50% → value-bet thin & stop bluffing; the vs-TAG "bet flop, CHECK turn, bet river" thin-value line. | HUD exposes only flop AF + VPIP/PFR/WTSD; no `pt_fold_flop_cbet`, no per-street AF, no own-bet street history beyond `BotRaisedOn*`. | Surface `pt_fold_flop_cbet`/`pt_turn_af`; add an `f$Opp_FoldsToCbet` archetype + a planned multi-street line state. |
| 19 | **"Starter" archetype** [HCC] — wide VPIP-PFR gap (>=8) at moderate VPIP (~20-30): isolate/value thin, no 3-bet-bluff, no post-flop bluff. | Borderline-overlaps `f$Opp_IsLoose` + station value logic already present; low marginal value. | Optional `f$Opp_IsStarter = VPIP 20-30 AND (VPIP-PFR)>=8` if replay shows a distinct leak. |
| 20 | **Preflop open-frequency mixing** [48L L48] — open ranges are deterministic list lookups, range-readable over a huge sample; postflop is already randomized. | No card-anchored mixing symbol; `randomround` is decision-time noise (see #6). | Per-hand hash-of-holecards → [0,1) symbol to mix the bottom-of-range opens. EV ~0 vs non-reading micro fields; defer. |
| 21 | **Multi-hand image / leveling memory** [AOW][CNL][NW][IPP][MC] — "show a bluff to set up a value bet" (Cloutier A7-then-77), gear-shift FOR deception, per-player-behind reraise-tightness, leveling one step above villain. Plus GTO balanced bluff:value river sizing [ANL][IPP]. | Requires table-image / multi-hand opponent-model memory + a combinatoric range engine the bot deliberately does not keep (it is exploit-first, not GTO). | Per-seat session memory + a range/combo model (overlaps #2/#3/#7); GTO river balance is intentionally NOT adopted. |
