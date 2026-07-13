---
name: icm-chip-value
description: Track the hero's ICM chip-value (dollar equity), risk/bubble factor, M-ratio, big blinds, and the blind-level clock as a tournament progresses, and read the relevant info aloud via the Lilith speaker. Use during a tournament to monitor equity, near-bubble/pay-jump decisions, or when the user asks "what's my ICM / how much am I worth / how deep am I". Prompts for the tournament structure if it isn't set, and at random times asks how many players are left.
---

# ICM chip-value (tournament)

Computes the hero's **$ equity** (Independent Chip Model) plus risk premium, M-ratio,
big blinds, and time-to-next-level, then **speaks the relevant bits via Lilith**.
Config lives in postgres `icm_config`; readings log to `icm_snapshots`.
ICM math is `icm.py` in this skill folder (Malmuth-Harville Monte-Carlo).

## 0. THE LOBBY IS THE SOURCE OF TRUTH (`settings.lobby_info`)
**Do not ask the user, and do not trust `icm_config`.** The structure comes from ACR's own lobby, via
`lobby_fetch.sh` + Claude vision (§ "Auto-fetch" below). `icm_chip_daemon.py` reads
`settings.lobby_info` and will **refuse to speak any $ number** unless that row (a) names the
tournament we are actually sitting in and (b) is fresher than 30 min. It speaks depth (bb / M) either
way, because that comes straight off the felt and is always true.

Write `settings.lobby_info` with these keys (the daemon needs the starred ones):

| key | | note |
|---|---|---|
| `tournament` | ★ | e.g. `"$50 GTD Freeroll"` — matched against the scraped table name |
| `remaining` | ★ | players left **right now** — this is "players left"; nothing else supplies it |
| `entrants` | | field size; used to derive places-paid when the lobby doesn't give it |
| `prize_pool` | ★ | dollars |
| `avg_stack_bb` | ★ | the lobby prints it in BB ("Avg Stack 68.48 BB") — **the daemon needs the field in BB** |
| `first_place` | | 1st-place prize; calibrates the payout ladder exactly |
| `places_paid` | | if known; else derived as ~15% of `entrants` |
| `payouts` | | a real ladder if you can get one; else it is modelled from pool + places |
| `bb_chips` | | current big blind **in chips** — only needed to also quote $/chip |
| `starting_chips` | | |

**UNITS — the bug that made every number wrong:** ACR displays stacks in **big blinds**
(`bblind_fallback = 1.0`, so `/api/table-state` reports hero as e.g. `17.45`). The old code built the
field from `entrants * starting_stack`, which is in **chips** (1229 × 12000 = 14.7M), and compared a
17-bb hero against a 22,000-"chip" field — hero looked like a 0.0001% stack and equity collapsed to
~$0. Keep **everything in one unit (big blinds)**: that is why `avg_stack_bb` matters more than
`starting_chips`. If you only have chip figures, you must also supply `bb_chips`.

## 1. Gather current state (each invocation / loop pass)
- `hiss_status`; if unreachable, skip.
- `game_state` → hero stack, table stacks, blinds (`limits.sblind/bblind/ante`), # at table.
- **players left** = `lobby_info.remaining`. Never ask the user, and never carry it over from a
  previous tournament. If it is stale, re-run the lobby fetch (§ below).
- `hero_bb = hero_stack / bblind` (works whether the table reports chips or BB).

## 2. Compute
- **ICM equity + CHIP VALUE:** run `icm.py` via Bash. Pass stacks **in big blinds** and payouts in
  dollars, so `dollars_per_chip` comes back as **dollars per big blind** — the hero's chip value.
  - **Final table / ITM short field** (all stacks visible): pass exact `stacks` + flat `payouts` +
    `hero_index` → Monte-Carlo.
  - **MTT mid-field:** pass `{hero_stack, players_remaining, avg_stack, payouts}` → **exact closed
    form** (`homogeneous_equity`), ~100 ms even for a 593-runner field paying 184. (The Monte-Carlo
    is O(sims × places × field) and simply never returns at that size — it used to time out and the
    daemon reported $0.00.)
  Read back `hero_equity`, `equity_if_double`, `bubble_factor`, `dollars_per_chip`.
- **Sanity check any equity you compute:** a stack exactly at the field average must be worth about
  `prize_pool / players_remaining`. (593 left, $50 pool → an average stack ≈ $0.084.) If it isn't,
  the units are mixed — go back to §0.
- **Big blinds:** `hero_stack / bb`.
- **M-ratio:** `hero_stack / (sb + bb + ante * players_at_table)` (effective M).
- **Blind clock:** from `tournament_start_ts`, `level_minutes`, `blind_schedule`:
  elapsed → current level, minutes to next level, the upcoming blinds.

## 3. Speak the relevant info (Lilith, succinct)
Use `speak` with ONE short sentence tuned to the situation, e.g.:
- *"You're sitting on 18 big blinds, M of 9, worth about 42 dollars by I-C-M. Next level in 4 minutes."*
- Near a pay jump / high bubble factor: *"Bubble factor is 1.8 — tighten up, your stack is worth more than its chips."*
- Short stack: *"Under 10 big blinds — push/fold mode."*
Keep it to what's actionable now; don't read every metric every time.

## 4. Log
Insert an `icm_snapshots` row (handnumber, players_remaining, hero_stack, hero_equity, detail jsonb).

## Works with the poker-coach skill
This skill owns the MATH; the **poker-coach** skill owns the VOICE/advice. Each pass, after computing
equity/bubble-factor/depth, write a short `coach_notes` row (kind='icm') with the actionable takeaway
so it shows in learner.exe and the coach can build on it. Detect and announce the **bubble stage** from
`players_remaining` vs `places_paid` (P): far ( >1.5P ), approaching ( 1.15P–1.5P ), ON BUBBLE ( P–1.15P,
speak an alert ), in-the-money ( <=P ). The coach turns the numbers into "what your chips are worth and
how to play them."

## Auto-fetch the structure from the lobby (Claude vision, no asking the user)
Instead of prompting for players-remaining / structure, pull them from ACR's tournament-info
screen during a REAL tournament:

- Run `bash /c/www/openholdembot_old/mcp/lobby_fetch.sh 27654 3.5 6` -- it navigates to the
  info page + MORE INFO popup, captures `C:/tmp/lobby_main.png` + `C:/tmp/lobby_moreinfo.png`,
  and returns the bot to the table FAST. **Parse the PNGs AFTER it returns (async)** so the bot
  is back in the game while you read.
- Read off the info page: current **blinds/level** (=> chips_per_bb), **players Remaining**,
  **avg / largest / smallest stack**, **prize pool**, **bounty**, **entrants**, next level + timer.
  Off MORE INFO: **starting chips**, blind-level minutes, max seats, late-reg, PKO %.
- Update `icm_config` (players_remaining, avg_stack, starting_stack, current_level, total_entrants)
  and `set_table_game_info`. Then compute equity/bubble as usual; speak via Lilith.
- **places_paid / payout breakdown** are NOT on the info page -- they're behind the "PRIZE POOL >"
  and "STRUCTURE >" buttons. Until those are mapped as regions, derive places_paid from the prize
  pool / a typical ~15% paid, OR ask the user once. (Adding PRIZE_POOL / STRUCTURE regions +
  clicks is the future upgrade for exact payouts.)
- Good trigger: refresh on a blind-level change or every so often after the hero folds; cooldown so
  it isn't constant. Real tournaments only.

## Guardrails
- ICM equity is exact only when all remaining stacks are known (final table); the
  MTT-field number is an **approximation** (hero + average field) — say "about" when speaking.
- `speak` uses the shared lilith.exe, so this works during bot play without learner.exe.
- Don't spam: speak on meaningful change (level change, pay jump approaching, big stack
  swing), not every pass.
