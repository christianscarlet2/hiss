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
| **Bet-timing / "weak-looking bet" reads** [B p.102] | Hiss does not expose opponent action latency to OpenPPL | ❌ Not implementable. Reported. |

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
- What's lost is the **human read layer**: physical tells, table-image adaptation,
  leveling wars, and true multi-street planning. Where a machine-observable proxy
  existed (PokerTracker stats, betting-action history, board texture, `prwin`,
  `randomround`), it was **built in**. Where none existed, the feature is **reported
  here** rather than faked.

If you want any of the ⚠️/❌ items pursued further, the most tractable additions
would be: (a) a self-image / recent-action memory using OpenPPL user-variables to
enable gear-changing, and (b) an opponent-timing feed from Hiss into a new symbol
to approximate bet-timing tells. Both require Hiss-side code, not just OpenPPL.
