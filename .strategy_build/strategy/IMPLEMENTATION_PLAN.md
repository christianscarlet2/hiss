# Implementation Plan — the remaining "human read" features

This is a concrete, Hiss-specific plan for the features that were reported as not
implementable in [LIMITATIONS.md](LIMITATIONS.md): **physical tells, feel/soul
reads, tilt detection, true leveling**, plus the two enabling additions called out
there — a **self-image / gear-changing memory** and the **opponent action-timing
feed**.

It is grounded in how Hiss actually works, so each item says *where the signal comes
from*, *what new C++ is needed*, *how the OHF consumes it*, and an honest
**feasibility / effort / risk** verdict.

---

## How Hiss exposes a "read" to the strategy (the pattern)

Every read the OHF can use is produced by a **symbol engine**: a class deriving from
`CVirtualSymbolEngine` with `UpdateOnHeartbeat / UpdateOnHandreset / UpdateOnNewRound
/ UpdateOnMyTurn` hooks, an `EvaluateSymbol(name, *result)` that answers a symbol
query, and `SymbolsProvided()` that registers its symbol names. You instantiate it
once in `CEngineContainer` (`AddSymbolEngine(...)`). The OHF then reads the symbol by
name. `CSymbolEngineTimingTells` is the reference example, and the **bet-timing feed
below is already built this way**.

### The persistence constraint (important, and it shaped this plan)

OpenPPL **user-variables** (`me_st_`, `me_re_`, `me_inc_`, `me_add_`, `me_sub_`) and
the per-hand memory symbols are **cleared on every hand-reset**
(`CSymbolEngineOpenPPLUserVariables::UpdateOnHandreset()` calls `_user_variables.clear()`).
So they are usable for *within-hand* memory only. **Anything that must remember
across hands (self-image, an opponent's recent behaviour, tilt) cannot live in
user-variables** — it must live in a dedicated C++ engine whose member arrays are
*not* wiped on hand-reset (exactly how `CSymbolEngineTimingTells._timing_seconds[]`
already survives hand-resets). Every cross-hand feature below therefore specifies a
small persistent engine.

---

## Status overview

| # | Feature | Verdict | Effort | Priority |
|---|---------|---------|--------|----------|
| 0 | **Opponent action-timing feed** | ✅ **DONE** (`lastraiseractiontime`, wired into vs-bet logic) | — | shipped |
| 1 | **Self-image memory + gear-changing** | ✅ Feasible — flagship | M | **build first** |
| 2 | **True leveling (1.5-level)** | ⚠️ Approximable, OHF-only once #1 exists | S | second |
| 3 | **Tilt detection** | ⚠️ Heuristic proxy feasible | M–L | third |
| 4 | **Bet-sizing tells** (bonus) | ✅ Feasible (fully observable) | M | optional |
| 5 | **Physical tells** | ❌ Not applicable online; only narrow scrapeable analogs | — | drop / niche |
| 6 | **Feel / soul reads** | ❌ Non-computable by definition → = aggregate of #0–#4 | — | n/a |

---

## 0. Opponent action-timing feed — DONE

Already implemented (see LIMITATIONS.md). `CSymbolEngineTimingTells` measures each
chair's dwell on its `pNactive` highlight and now also exposes
`lastraiseractiontime`. The strategy derives `f$TimingSaysWeak` / `f$TimingSaysStrong`
and folds them into the flop/turn/river vs-bet decisions. Listed here for
completeness because it was one of the two "future additions" named in the report.

---

## 1. Self-image memory + gear-changing  ⭐ (build first)

**Why first:** highest ROI, lowest risk, needs **no scraping** — the bot already
knows its own actions — and it is the prerequisite for leveling (#2). Both books hinge
on it: Negreanu's "your wild image gets you called" (bluff less / value bigger) and
Brunson's "maintain the aggressive image".

**Observable signal (all already inside Hiss):** our own per-hand actions —
`BotRaisedBeforeFlop`, `BotRaisedOnFlop/Turn`, whether we c-bet, whether we reached
showdown, won/lost. The executed action is known at the autoplayer
(`CAutoplayerTrace` / the primary-formula result).

**New C++ — `CSymbolEngineSelfImage`:**
- Members survive hand-resets (ring buffer of the last ~25 of *our* hands):
  ```cpp
  int  _played[25], _raised_pf[25], _cbet[25], _showed_down[25];  // ring of recent hands
  int  _idx; // current slot
  ```
- `UpdateOnHandreset()`: commit the just-finished hand's flags into the ring
  (advance `_idx`). Read the flags from the betting-action symbols / autoplayer trace.
- `EvaluateSymbol()` exposes rolling rates over the window:
  | symbol | meaning |
  |--------|---------|
  | `my_recent_pfr` | fraction of recent hands we raised pre-flop |
  | `my_recent_aggression` | c-bets + raises per recent hand |
  | `hands_since_showdown` | how long since we showed a hand |
  | `my_image_wild` | bool: recent PFR/aggression above a "loose" cutoff |
  | `my_image_tight` | bool: recent PFR very low and few showdowns |
- Register in `CEngineContainer`. ~1 new file pair, ~120 lines, mirrors the timing engine.

**OHF wiring (new `15_selfimage.ohf` segment):**
- `f$ImageWild` / `f$ImageTight` wrap the symbols (with an enable knob).
- Gear-change, applied as soft modifiers:
  - **Wild image** → opponents call lighter: **bluff less** (lower the river-bluff
    `randomround` gates and `f$CbetFreq`), **value-bet bigger and thinner** (bump
    `f$BetPct*`), and *widen* thin value-bets vs stations.
  - **Tight image** → we get more respect: **steal/3-bet-bluff more** (raise the
    `list3betBluff` frequency and button-open width), **c-bet more**.

**Feasibility:** ✅ fully observable. **Risk:** low — pure tendency-shifting, capped.
**Effort:** Medium (one engine + one OHF segment).

---

## 2. True leveling — "what does he put me on"  (after #1)

True recursive leveling is not achievable, but a defensible **1.5-level** version is,
**with no new C++** once #1 and the existing PT stats are in place.

**Synthesis (OHF-only):** `f$OppIsThinking` = the raiser has a solid sample and a
reg-like profile (e.g. `f$Opp_Known AND f$Opp_PFR > 12 AND (f$Opp_VPIP - f$Opp_PFR) < 12`).
A thinking opponent *adjusts to our image*; a fish does not. Combine with #1:

| Our image | vs thinking opp | vs non-thinking opp |
|-----------|-----------------|----------------------|
| Wild | he calls us down → **value bigger, bluff rarely** | ignores image → play straightforward value |
| Tight | he folds to us → **bluff/steal more, barrel more** | ignores image → just value |

**Feasibility:** ⚠️ approximation (one level of adjustment, not a recursion). Good
enough to capture the exploit the books describe. **Effort:** Small (OHF only).
**Risk:** low.

---

## 3. Tilt detection  (heuristic proxy)

**Goal:** detect an opponent who just took a bad beat / big loss and is now playing
too many hands too aggressively, then **value-bet him thinner and never bluff him**.

**Observable signals in Hiss:**
- **Stack deltas** — per-chair balances are scraped every heartbeat
  (`CTableState`); a sharp drop at showdown = "lost a big pot".
- **Showdown context** — `CHandHistoryShowdown` already tracks shown hands.
- **Aggression baseline** — PokerTracker stats (`pt_*_chair`) give each villain's
  normal VPIP/AF.

**New C++ — `CSymbolEngineTilt`** (persistent, per chair):
- Ring of recent stack deltas per chair (sampled at hand-reset).
- A short rolling window of each chair's recent VPIP/raise count (from the
  poker-action engine) to compare against the PT baseline.
- `EvaluateSymbol()`:
  | symbol | meaning |
  |--------|---------|
  | `raiser_recent_bigloss` | the player we face dropped > ~25% of stack in the last few hands |
  | `raiser_overactive` | his recent VPIP/aggression is well above his PT baseline |
  | `raiser_maybe_tilting` | both of the above |

**OHF wiring:** vs a `raiser_maybe_tilting` opponent → widen bluff-catch (call one
pair without needing the aggressive read), value-bet thinner and bigger, and disable
bluffing into him.

**Feasibility:** ⚠️ a noisy *proxy*, not true tilt — needs reliable showdown/stack-delta
detection and a meaningful PT baseline. **Effort:** Medium–High. **Risk:** medium
(false positives); keep it a soft modifier and require a PT sample before trusting it.

---

## 4. Bet-sizing tells  (bonus — fully observable)

Not in the original ❌ list, but it's the most reliable *online* "tell" after timing
and worth noting: many opponents size differently for value vs bluff (e.g. small =
weak/blocker, overbet = polarised). Everything needed is observable: `BetSize`,
`PotSize`, and a per-opponent history of (street, size-bucket, showdown result).

**New C++ — `CSymbolEngineSizingTells`:** per chair, track the bet-size buckets
(fraction of pot) they've used and, where a showdown revealed the hand, whether that
size was value or bluff. Expose `raiser_bet_fraction` (this bet ÷ pot) and, with
enough samples, `raiser_overbet_is_polarised` / `raiser_small_is_weak`.

**OHF wiring:** treat a tiny bet from a "small = weak" villain like a snap (bluff-catch
wider); respect an overbet from a "polarised" villain (fold marginal made hands).

**Feasibility:** ✅ observable. **Effort:** Medium. **Risk:** low–medium (needs
samples; until then `raiser_bet_fraction` alone — bet size relative to pot — is
already useful and needs no history).

---

## 5. Physical tells  ❌ (not applicable online)

Body language, chip handling, eye movement, table-talk require a **live video of a
human**. Hiss scrapes an Android poker *app* — there is no such signal, so the
classic physical tells are **out of scope for online play** and stay reported, not
faked.

**The only scrapeable analogs**, if the target app renders them, are *UI* tells, and
they would be added as ordinary tablemap regions + a tiny symbol engine:
- **Auto-action / pre-action tells** — some apps show that a player pre-selected
  "check/fold" or "call any" (their action fires instantly and uniformly). This is
  partly **already captured by the timing feed** (instant action). A dedicated
  `pNautoaction` region could make it explicit.
- **Emote / animation states** — if the app shows emotes or a "thinking" animation,
  scrape that region and expose `pNemote`.

**Verdict:** drop the generic physical-tell goal online; the timing feed (#0) and
sizing tells (#4) are the real-world online substitutes. The UI-tell regions are a
low-value niche, build only if the specific app surfaces them.

---

## 6. Feel / soul reads  ❌ (non-computable → defined as the aggregate)

"Feel" is, by definition, not a discrete signal. The honest engineering mapping is:
**a bot's "read" = the weighted aggregate of every observable signal** — PT
archetypes (`10_opponents`), board texture (`30_classify`), `prwin`, the timing feed
(#0), self-image (#1), leveling (#2), tilt (#3), and sizing tells (#4). There is no
separate thing to build; implementing #1–#4 *is* the machine's version of "feel".

---

## Recommended roadmap

1. **Self-image engine (#1)** — biggest strategic gain, no scraping, low risk. Ship,
   then turn on gear-changing in the OHF.
2. **Leveling (#2)** — OHF-only, immediately after #1.
3. **Sizing tells (#4)** — start with `raiser_bet_fraction` (no history needed), add
   the learned buckets later.
4. **Tilt (#3)** — last; most infrastructure (stack-delta + showdown + baseline) and
   the noisiest.
5. **Physical/UI tells (#5)** — only if the specific app exposes emote/auto-action
   regions.

### Cross-cutting guardrails
- Every read is a **soft modifier** gated behind an enable knob (like
  `f$UseTimingTells`) and a **minimum sample** (like `f$Opp_Known >= 30 hands`); a
  read never overrides a strong made hand or a real draw.
- Keep the heavy work off the **heartbeat thread** — the self-image/tilt engines do
  almost nothing per heartbeat and only a little at hand-reset, so they won't stall
  the bot (cf. the Scarlet-Beast HTTP-on-heartbeat stall lesson).
- Prefer **decay/rolling windows** over lifetime averages so the bot reacts to recent
  dynamics, which is the point of all four reads.
