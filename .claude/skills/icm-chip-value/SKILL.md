---
name: icm-chip-value
description: Track the hero's ICM chip-value (dollar equity), risk/bubble factor, M-ratio, big blinds, and the blind-level clock as a tournament progresses, and read the relevant info aloud via the Lilith speaker. Use during a tournament to monitor equity, near-bubble/pay-jump decisions, or when the user asks "what's my ICM / how much am I worth / how deep am I". Prompts for the tournament structure if it isn't set, and at random times asks how many players are left.
---

# ICM chip-value (tournament)

Computes the hero's **$ equity** (Independent Chip Model) plus risk premium, M-ratio,
big blinds, and time-to-next-level, then **speaks the relevant bits via Lilith**.
Config lives in postgres `icm_config`; readings log to `icm_snapshots`.
ICM math is `icm.py` in this skill folder (Malmuth-Harville Monte-Carlo).

## 0. Ensure the tournament structure (prompt if missing)
`pg_query` (db `hiss`): `SELECT * FROM icm_config WHERE id=1`.
If empty or missing key fields, **ask the user** for and store:
- `tournament_name`, `buyin`, `total_entrants`, `places_paid`, `starting_stack`
- `payouts` — JSON array place→amount, e.g. `[{"place":1,"amount":1000}, ...]` (or a flat
  `[1000,600,...]` list for `icm.py`)
- `blind_schedule` — JSON `[{"level":1,"sb":10,"bb":20,"ante":0}, ...]`
- `level_minutes`, `tournament_start_ts`
Write with `pg_query allow_write=true`.

## 1. Gather current state (each invocation / loop pass)
- `hiss_status`; if unreachable, skip.
- `game_state` → hero stack, table stacks, blinds (`limits.sblind/bblind/ante`), # at table.
- `players_remaining` from `icm_config`. **If it looks stale (or ~1 in 4 passes at
  random), refresh it:** `speak` "How many players are left?" and ask the user; update
  `icm_config.players_remaining` (+ `avg_stack` if they give it).
- `total_chips = total_entrants * starting_stack`; `avg_stack = total_chips / players_remaining`.

## 2. Compute
- **ICM equity:** run `icm.py` via Bash.
  - **Final table / ITM short field** (players_remaining ≤ table size and all stacks
    visible): pass exact `stacks` + flat `payouts` + `hero_index`.
  - **MTT mid-field:** pass `{hero_stack, players_remaining, avg_stack, payouts}`.
  Read back `hero_equity`, `equity_if_double`, `bubble_factor`, `dollars_per_chip`.
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
