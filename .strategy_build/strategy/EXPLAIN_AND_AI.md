# Explanations, Steering, OpenAI Hijack & ACR Hand Histories

Four features built on top of the Power Hold'em strategy. The EXPLAIN feature and
the ACR hand-history writer are **live**. The OpenAI advisor / steering / hijack /
runtime-improve subsystem is **built but disabled by default** (no network call is
made and play is never changed until you explicitly enable it).

---

## 1. EXPLAIN — variable-filled decision narration (LIVE)

Annotate any decision branch so that, **when it fires**, a templated explanation
with live values is printed to the **Decisions** pane of the Terminal.

**Define a template** on a `//~` comment line (comments are parser-safe, so
`{tokens}`, `%`, `$` never trip the OpenPPL tokenizer; `{tokens}` are any
symbol/`f$function`). The `0` body is a required, unused valid expression:
```
##f$expl_fold_tank##
//~ FOLD one pair -- raiser TANKED {lastraiseractiontime}s; pot {PotSize}bb, call {AmountToCall}bb.
0
```
**Trigger it** by putting `explain_<tag>` at the TAIL of the branch's `WHEN`
(OpenPPL short-circuits `AND`, so it fires only when that branch is actually
selected, and it evaluates to `true` so it's transparent):
```
WHEN f$HaveOnePair AND f$TimingSaysStrong AND explain_fold_tank Fold FORCE
```
Output: `[why] FOLD one pair -- raiser TANKED 7.20s; pot 14bb, call 9bb.`

- Engine: `CSymbolEngineExplain` (`explain_*` symbols). Templates live in
  [`15_explain.ohf`](15_explain.ohf); examples are already wired into the open,
  3-bet, c-bet, value, semi-bluff, timing-fold/float, push-fold and bluff-catch
  branches.
- De-duplicated per street/turn (so live values don't spam), suppressed during
  parsing. Add `explain_<tag>` to any branch and a matching `f$expl_<tag>` template.

---

## 2. ACR-format hand histories for PokerTracker 4 (LIVE)

Because the phone client gives you no hand histories, `CHandHistoryWriter` now
reconstructs each hand from the screen scrape and writes it in **Americas Cardroom
format** so PokerTracker 4's ACR importer accepts it. Output is split:

```
Release\handhistory\complete\     <- import these into PT4
Release\handhistory\incomplete\   <- review these (missing data)
```

A hand is **complete** only if it has the structure PT4 needs: real names + stacks
+ bet amounts + a known button + hero, blinds seen from the start (not joined
mid-hand), and a terminal result (won uncontested or a showdown). Everything else
goes to `incomplete\`. Point PT4's auto-import at the `complete\` folder.

Format mirrors ACR: `Game Hand #… - Tournament #<session> - Holdem (No Limit) -
Level 1 (sb/bb) - <UTC>`, `Table '…' N-max Seat #b is the button`, `Seat n: name
(stack)`, `posts the small/big blind`, `*** HOLE CARDS ***`, `bets/calls/raises X
to Y/folds (and is all-in)`, `*** FLOP/TURN/RIVER ***`, `*** SHOW DOWN ***`,
`*** SUMMARY ***`. Reconstruction from scraping is approximate — that's exactly why
the complete/incomplete split exists.

---

## 3. Steering console + OpenAI hijack (BUILT, DISABLED BY DEFAULT)

Type commands into the Terminal prompt (native or browser), or use the browser
buttons (**⚡ Hijack → OpenAI**, **■ Stop**, **▶ Play**):

| Command | Effect |
|---------|--------|
| `/stop` / `/play` | pause / resume (sets `openai_paused`) |
| `/strategy smallball\|power\|hybrid` | **retunes `f$Style` live** (this one works with no OpenAI) |
| `/goto <anchor>` | sets `openai_steer_anchor` (anchors: preflop/flop/turn/river) |
| `/hijack [why]` | send game state + context + chat + decisions to OpenAI for a recommendation |
| `/improve <text>` | ask OpenAI to propose an `.ohf` edit from the live context + decision tree + your instruction |

**Anchors** are tagged in the `.ohf` with `// @anchor: <name> (id N)` (on
`f$preflop/f$flop/f$turn/f$river`) and mapped in `COpenAiAdvisor::AnchorIdFromName`.

**OpenAI hijack of a decision** is pre-wired: each street entry begins with
`WHEN openai_action > -1000 RETURN openai_action FORCE`. While disabled,
`openai_action` is the `kOpenAiNoOpinion` sentinel, so it never fires and the bot
uses its own logic. When enabled and the advisor returns a raise size, it's taken.

**Auto-hijack** for spots with no good heuristic fit: enable the preference, then
wire `f$NoHeuristicFit AND f$ConsultOpenAI` at a fallback (see
[`16_steering.ohf`](16_steering.ohf)). `f$ConsultOpenAI` fires `openai_consult`.

### Enabling (registry `HKCU\Software\ScarletBeast`)
| Value | Type | Meaning |
|-------|------|---------|
| `OpenAiEnabled` | DWORD | `1` = allow live calls (default `0` = stub) |
| `OpenAiAutoHijack` | DWORD | `1` = auto-consult on no-fit spots |
| `OpenAiApiKey` | SZ | your API key (never hard-coded) |
| `OpenAiModel` | SZ | model id (default `gpt-4o-mini`) |

### What "disabled" means
- `COpenAiAdvisor::Ask()` assembles the full context JSON (game state + the four
  Terminal panes) but **does not send it** — the HTTPS POST is a clearly-marked
  `TODO` seam. It logs `[openai:disabled] would send …` to the Decisions pane.
- `/improve` never overwrites your live strategy; a proposal would be written to
  `Strategy\proposed\` for manual review.
- Nothing changes play. To go live later, implement the one stubbed `Ask()` HTTPS
  call **on a background thread** (never the heartbeat — see the SB heartbeat-stall
  lesson) and have it set `openai_action` / return a recommendation string.

### Files
`COpenAiAdvisor.{h,cpp}` (brain + commands + context), `CSymbolEngineOpenAI.{h,cpp}`
(`openai_*` symbols), routing in `ChatTerminalWindow.cpp` (`TerminalBrowserInject`
+ `SendChatText`), browser UI in `public/terminal.html` + `assets/terminal.{js,css}`.
