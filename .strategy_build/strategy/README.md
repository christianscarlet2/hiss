# ScarletBeast — Power Hold'em

An opponent-aware **No-Limit Texas Hold'em** strategy for Hiss/OpenPPL, distilled
from two source texts and blended into one selectable engine:

| Tag | Source | Contribution |
|-----|--------|--------------|
| **[N]** | Daniel Negreanu — *"Small Ball"* (Power Hold'em Strategy, 2008) | small bets, wide-but-positional play, pot control, "don't go broke with one pair", low river-bluff frequency |
| **[B]** | Doyle Brunson — *"Power Poker"* (Super System, NLHE section) | relentless aggression, ~pot-sized bets, ~90% continuation bets, semi-bluffing draws all-in, leading big hands into the raiser |

Both books were OCR'd from scanned PDFs, distilled into rule specs, and translated
into OpenPPL. Every rule in the code carries a `[N]`/`[B]` citation back to its source.

---

## The STYLE dial

The two philosophies pull in opposite directions (small & controlled vs. big &
aggressive), so they are made **selectable** through a single switch in
[`05_config.ohf`](05_config.ohf):

```
##f$Style##
WHEN Others RETURN 2 FORCE      <-- change this number
```

| Value | Style | Behaviour |
|-------|-------|-----------|
| `0` | **Small Ball [N]** | 2.5x opens, ~55–60% c-bets, heavy pot control, never stack off without the near-nuts, minimal river bluffing |
| `1` | **Power Poker [B]** | 3.5x opens, ~85–90% c-bets, semi-bluff draws all-in, lead big hands, 4-bet QQ/AK, value-bet one pair |
| `2` | **Hybrid (default)** | small-ball sizing & discipline + Brunson's draw aggression and high c-bet frequency |

Everything else (ranges, opponent reads, board logic) is shared; the dial mainly
moves **bet sizing** and **aggression frequency**.

---

## How it loads

The strategy is **segmented across files that share one `f$` namespace**. There are
two equivalent ways to run it:

### A. Auto-loaded folder (default, requires the patched Hiss)
Every `*.ohf` in `bot_logic\Strategy\` is auto-loaded at startup
(`CFormulaParser::LoadStrategyFolder()`). Parsing order is irrelevant — cross-file
`f$` references resolve in the single `ParseAll()` pass after all files load. Just
run Hiss; the files in this folder *are* the bot.

### B. Single master file (portable / works on an un-patched Hiss)
All segments are also concatenated into **`Release\ScarletBeast_PowerHoldem.ohf`**.
Load that one file the normal way (File → Open), exactly like any other `.ohf`.

> **On precedence (no footgun).** Functions are added with *last-definition-wins*
> semantics (`CFunctionCollection::Add` deletes any existing same-named function
> first — there is no duplicate-name error). The load order is:
> OpenPPL library → demo-bots (Gecko/Termita/Winngy) → **`Strategy\` folder** →
> custom library → your loaded main file. So the `Strategy\` files cleanly
> **override the demo-bots** and become the live default bot, and if you also
> File→Open the master it simply re-overrides with identical logic — harmless.
>
> The one thing to know: if you load some *other* main `.ohf`, it loads **last**
> and will override this strategy. To run Power Hold'em, either keep the segments
> in `Strategy\` and load no conflicting main file, or load the master directly.

---

## File index

| File | Purpose |
|------|---------|
| [`00_notes.ohf`](00_notes.ohf) | Header notes + `prwin` (Monte-Carlo equity) configuration |
| [`05_config.ohf`](05_config.ohf) | **The STYLE dial** + every tunable threshold (sizes, stack regimes, pot-odds) |
| [`10_opponents.ohf`](10_opponents.ohf) | Opponent model from PokerTracker HUD stats (the machine-observable "play the player") |
| [`20_lists.ohf`](20_lists.ohf) | Hand ranges by position and action |
| [`30_classify.ohf`](30_classify.ohf) | Made-hand / draw / board-texture classification helpers |
| [`40_preflop.ohf`](40_preflop.ohf) | The pre-flop decision tree (open / 3-bet / 4-bet / push-fold) |
| [`50_flop.ohf`](50_flop.ohf) | Flop play (c-bet, value, semi-bluff, float) |
| [`60_turn.ohf`](60_turn.ohf) | Turn play (barrel, take-away, pot control) |
| [`70_river.ohf`](70_river.ohf) | River play (value, thin value, bluff-catch) |
| [`90_table.ohf`](90_table.ohf) | Table-management hopper functions (sit-in, etc.) |

See **[STRATEGY_DETAILED.md](STRATEGY_DETAILED.md)** for a function-by-function
walk-through, and **[LIMITATIONS.md](LIMITATIONS.md)** for book concepts that a bot
cannot fully implement and how each was handled.

---

## Quick tuning guide

All knobs live in [`05_config.ohf`](05_config.ohf):

| Knob | Default | Effect |
|------|---------|--------|
| `f$Style` | `2` | overall style (see above) |
| `f$OpenBase` | 2.5 / 3.5 | first-in open size in BB |
| `f$ThreeBet_RaiseTo` / `f$FourBet_RaiseTo` | 9–13 / 22–26 | 3-bet / 4-bet target size in BB |
| `f$BetPctFlop/Turn/River` | 55–90 | post-flop bet size as % of pot |
| `f$CbetFreq` | 0.60–0.90 | how often to c-bet air as the pre-flop raiser |
| `f$DeepStack` / `f$ShortStack` / `f$PushFoldStack` | 60 / 25 / 10 BB | stack-depth regime cutoffs |
| `f$WithinSpecRisk` | 10% of stack | max risk on a speculative set-mine / connector call |
| `f$UseTimingTells` | 1 | bet-timing reads on/off (snap = weak, tank = strong) |
| `f$SnapSeconds` / `f$TankSeconds` | 1.5 / 6 | timing-tell thresholds in seconds |

Ranges live in [`20_lists.ohf`](20_lists.ohf) — widen or tighten the `listOpen*`
lists to taste.
