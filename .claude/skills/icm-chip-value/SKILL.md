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

## Guardrails
- ICM equity is exact only when all remaining stacks are known (final table); the
  MTT-field number is an **approximation** (hero + average field) — say "about" when speaking.
- `speak` uses the shared lilith.exe, so this works during bot play without learner.exe.
- Don't spam: speak on meaningful change (level change, pay jump approaching, big stack
  swing), not every pass.
