# PokerBooks incorporation — progress tracker

Standing task: parse the relevant NLHE subset of `C:\Users\scarl\Downloads\PokerBooks`
(265 docs), reconcile into this OHF with citations, and report unimplementable concepts
as a Hiss-improvement backlog (see LIMITATIONS.md). Off-target genres skipped: PLO/Omaha,
limit hold'em, memory-training, live tells/body-language, video poker, razz, bridge.

Source-tag convention is documented in `00_notes.ohf`. Deploy = copy
`.strategy_build/strategy/*.ohf` → `Release/bot_logic/Strategy/` then **restart Hiss**
(hot-reload is unreliable, see [[ohf-reload-and-trees]]); verify `f$Style=2`,
`f$OpenBase=2.5` after restart.

---

## TIER 1 — DONE (2026-06-15)

Highest OHF impact: push-fold/ICM, microstakes exploit, SPR/commitment, hand-reading, math.
17 books, parsed via parallel subagents, reconciled + lint-clean. Live dials added:
`f$M`/`f$Mzone`, `f$SPR`/`f$Committed`, `f$RequiredEquity`, `f$SetMineOK` (15×),
`f$AnteTotal`, `f$Stage`/`f$NearBubble`/`f$BubbleTighten` (bubble-scaled ICM gate),
`f$Is6Max`, `f$BetPctVsStation`; new archetypes `f$Opp_IsTAG/IsLAG/IsFish/IsManiac` +
broadened station; flop get-it-in-when-committed + value-big-vs-station.

- [HoH] Harrington on Hold'em Vol 1 + 2
- [KE] Kill Everyone · [KP] Kill Phil
- [SSNL] Small Stakes No-Limit Hold'em (Miller/Mehta/Flynn)
- [NLTAP] No Limit Hold'em Theory & Practice — ⚠️ supplied PDF is a TOC-only promo; re-obtain
- [ToP] Theory of Poker · [SM] Hold'em for Advanced Players
- [CtM] Crushing the Microstakes · [uNL] Moving Through uNL
- [EM] Playing the Player + How to Read Hands · [TN] The Poker Blueprint
- [SNG] SNG Blueprint 1-4 · Moshman Sit'n'go Strategy · Shaw Secrets of Sit'n'gos
- [PMM] Poker Math That Matters · [MoH] The Math of Hold'em · [V6] How to Beat 6-Max Cash (Vosti)

NOT YET DEPLOYED — source tree only. Mirror + restart Hiss at an idle moment.

---

## TIER 2 WAVE 1 — DONE (2026-06-15, deployed live + lint/parse clean)

18 books parsed (HC, PNL, ASH, EG, ER, LTBR, JL, RE, SNF1/2, W, SYS, NSS, HUM, HUSNG, DFR, RF).
Reconciled & LIVE: hand-class-aware `f$Committed` (PNL precise SPR bands — corrected the Tier-1 flat
version), tiered `f$SetMineOK`, `f$PushFoldStack` 13→15, HU `f$CbetFreq` bump + power cap, new
`f$ReshoveSpot`+`listReshove`, `f$DoubleBarrel` turn gate, `f$ThreeBetBluffFreq`, `f$Open_RaiseTo_HU`,
`listOpenHU`/`listDefendHU` (+wired), `listOpenEP6`/`listOpenMP6` 6-max branch (+wired).

### Tier 2 Wave 1 — SPECIFIED but DEFERRED (ready to wire in the harmonization pass)
- **f$StructureSpeed / f$SpeedShade** [SNF] — tournament structure-speed axis (patience/utility factor);
  needs blind-clock/structure sheet for precision, coarse proxy via bblind/tgi_level. Backlog.
- **f$SurvivalMode / f$FlatPayout / f$BigStack / f$WinnerTakeAll / f$NearBust** [NSS][SYS] — DON/satellite
  pure-survival + winner-take-all modes; needs a new `tgi_flat_payout` lobby field (format detection).
- **f$TripleBarrel** [ER][LTBR] — river third-barrel gate (scare/draw-completing rivers, foldy non-stations).
- **f$FourBetBluff / list4betBluff** [ER][RE] — 4-bet-bluff the unbalanced reg (ace-blockers); band-gated 35-55bb.
- **f$BubblePressureSpot** [RE] — aggressor-side bubble jam (don't let bubble-tighten suppress fold-equity jams).
- **f$Light4betBand** [RE] — gate light 4-bets to 35-55bb (dies <35bb).
- **listReshove18_25 widen, listJamEP +K8s/K9o/QTo** [RE][NSS], **HU depth-tiered BB jam lists** [HUSNG],
  **listSmallBallDeep (100bb full-utility)** [SNF], **f$ThinValue river sizing** [EG], **donk/min-bet read** [HC][CtM].
- **Full-ring EP/MP tightening** [DFR] — current full-ring EP (~16%) is over-wide vs DFR (~9-12%); only the
  6-max branch was added this round.

## TIER 2 WAVE 2 — DONE (2026-06-15, source tree, lint/parse clean)

~30 books mined; near-zero net-new (as anticipated). LIVE deltas added this wave:
`f$OpenBase` SB-opens-3x [WPT1]; `f$SetMineOK` MTT-stage guard (~25x once f$Stage>=1
or <=30bb, cash unchanged) [WPT2]; `f$BoardHighCardFoldy`/`BoardHighCardSticky`/
`BoardParched` (30_classify) + 50_flop air-c-bet gate on J/T-high dry boards [GG];
`f$TurnBetGeo` symbol (available, wiring deferred) [GG]; `f$Opp_PotCommitted`
(10_opponents) gating `f$DoubleBarrel` + river triple-barrel + `f$ThreeBetBluffFreq`
[SunTzu VII.36]; ~15 [Tao]/[SunTzu]/[48L]/[Prince] citation reinforcements (no
behavior change). Backlog #13-21 in LIMITATIONS.md.

- [WPT1/WPT2] Winning Poker Tournaments One Hand at a Time v1 + v2 — **net-new** (SB size, MTT set-mine guard)
- [GG] Phil Gordon's Little Gold Book — **net-new** (high-card c-bet texture, turn geometry)
- [Tao] Tao Te Ching · [SunTzu] Art of War — philosophy layer (1 net-new gate: don't-press-a-desperate-foe)
- [48L] Greene 48 Laws of Power · [Prince] Machiavelli — citation reinforcement only, no new dial
- [LGY] Largay NL: A Complete Course — intro NL, fully subsumed (implied odds/value-sizing/typing/game-selection)
- [CNL] Cloutier/McEvoy Championship NL&PL · [AOW] Apostolico Tournament Poker & the Art of War — overlap / multi-hand-memory (backlog)
- [SUZ] Suzuki Poker Tournament Strategy · [STC] Stomp the Competition · [DFT] Destination Final Table · [TT] Tournament Tactics — low strategy density, subsumed by spine
- [NW] Negreanu Hold'em Wisdom — small-ball derivative of [N], subsumed
- [HCC] Gaines Hole Card Confessions — "starter" archetype (backlog #19); rest not machine-observable
- [PPM] Practical Poker Math (Dittmar) · [PMVT] poker_math VT — fundamentals, fully subsumed
- [ANL] Analytical No-Limit (image-only) · [EoP] Angelo Elements of Poker · [IPP] Newall Intelligent Poker Player · [8M] Sklansky Eight Mistakes — GTO/limit/mental, no net-new dial
- [MC] Mike Caro University Tuesday Session lectures — conceptual/live, subsumed by typing/gear/position
- [PKR] ~69 PKR magazine articles (cash/six-max/heads-up) — ~95% in spine; 4bet/3bet sizing & per-street-AF → backlog #15-18

### [SKIP] FIXED-LIMIT / off-target (mined, confirmed off-target — limit holdem does not map to NL sizing)
- [HLG] Hilger Internet Texas Hold'em · [JON] Jones Winning Low-Limit Hold'em · [SWY] Swayne Advanced Degree in Hold'em
- [KYO] King Yao Hold'em Brain · [STX] Stoxtrader Winning Tough Hold'em Games (steal-freq 3bet table → backlog #17)
- [B-SS2] Brunson SS2 NLHE chapter (already [B] core, fully mined) · [H] Hellmuth Play Poker Like the Pros (text layer stripped, unmineable) · "Little Green Book"/#16 PDF = spam download page, not the book

---

## TIER 2 — QUEUE (on-target NLHE, not yet parsed)

Cash / 6-max / full-ring:
- Professional No-Limit Hold'em (Flynn/Mehta/Miller) — .mobi, needs ebook-convert
- Harrington on Cash Games Vol 1 + 2 · Harrington on Online Cash 6-Max
- Ryan Fees 6maxNL Guide · Dynamic Full Ring Poker (Splitsuit)
- Easy Game (Seidman) · Let There Be Range (Nguyen/CTS) · NL Workbook: Exploiting Regulars (Nguyen)
- Danny Ashman Secrets of Short-handed NL · Stoxtrader Winning Tough Hold'em Games
- No-Limit Hold'em: A Complete Course (Largay) · Internet Texas Hold'em (Hilger)
- Winning Low Limit Hold'em (Jones) · Swayne's Advanced Degree in Hold'em
- Small Stakes NL companion: Professional Poker (Blade), Holdem Brain (King Yao)

Tournament / SNG / HU:
- Secrets of Professional Tournament Poker Vol 1 (J. Little)
- Snyder Tournament Formula 1 + 2 · The Raiser's Edge (Elky/Nelson)
- Championship No-Limit & Pot-Limit (Cloutier/McEvoy) · Kill Phil already done
- Winning Poker Tournaments One Hand at a Time Vol 1 + 2
- Earn $30k/mo STT (Wiseman) · Systematic SNG · Tournament Tactics (Rounder)
- Suzuki Poker Tournament Strategy · Tournament Poker & Art of War (Apostolico)
- Stomp the Comp · destination-final-table · Secrets of Non-Standard SNGs (Shaw)
- Heads-Up (Moshman) · HUSNG Mersenneary · PKR Heads-Up articles

General strategy / math / psychology-to-coach:
- Super System 2 (already [B] core; mine remainder) · Hellmuth Play Poker Like the Pros (finish [H])
- Phil Gordon Green/Gold Book · Negreanu Holdem Wisdom · Decide to Play Great Poker (Duke, .mobi)
- Poker Winners Are Different / Eight Mistakes (Sklansky) · Don't Listen to Phil Hellmuth (Schmidt)
- Analytical No-Limit Hold'em · Elements of Poker (Angelo) · Intelligent Poker Player (Newall)
- PKR cash + six-max articles (~60 short pieces) · Caro lectures (strategy ones, not tells)
- Math: Practical Poker Math (Dittmar), poker_math VT, Hole Card Confessions (Gaines)
- Mental game → route to the Lilith coach, not the OHF: Mental Game of Poker (Tendler),
  Zen and the Art of Poker, Peak Performance Poker, Ace on the River (Greenstein)

---

## SKIP (off-target — confirmed)
PLO/Omaha (Hwang ×4, Slowhabit/Nguyen PLO, Chambers, Hutchinson, dandeppen, PLO From Scratch),
limit hold'em (Jacobs/Brier, shorthanded-limit articles, Fundamentals of Poker),
memory training (O'Brien, Fry, "Sieve to CIA", Memory Program, Devanand),
tells/body-language (Caro Book of Tells, Read 'Em & Reap, Navarro, Pease, Lieberman,
Dimitrius, Caro tell-lectures), video poker (Wong), razz (Cogert), bridge (Visual Bridge),
neocheating/novelty (Wallace), pure-narrative (For Richer For Poorer, Education of a Poker Player).
