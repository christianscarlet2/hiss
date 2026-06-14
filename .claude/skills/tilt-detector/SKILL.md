---
name: tilt-detector
description: Detect tilt for the Hiss poker bot — both the hero (protect: stack drawdown, VPIP/aggression spikes, bad-beat streaks, disliked-decision clusters) and opponents (exploit: recent big loss + over-aggression). Reads live state + history via the hiss-bot MCP and speaks alerts via Lilith. Use when monitoring a session, when the user asks "am I tilting / is anyone tilting", or on the analysis loop.
---

# Tilt detector (hero + opponent)

Two layers already exist; this skill is the **advisory** layer that ties them together
and speaks alerts. The **real-time** layer is the C++ `CSymbolEngineTilt` (symbols the
OHF reacts to in-hand). Use the MCP tools below.

## Signals

### Live (via MCP `symbols`) — the C++ engine, fast/in-hand
- `hero_drawdown` (0..1) — fraction of the hero's recent peak stack lost
- `hero_tilting` — drawdown ≥ ~0.33
- `raiser_drawdown`, `raiser_recent_bigloss`, `raiser_maybe_tilting` — the player we face

### History (via MCP `pg_query`) — deeper, loop-cadence
- `game_state_log` — hero stack trajectory over recent hands (corroborate drawdown)
- `learner_decisions` — **manual-play behavioral tilt**: VPIP/aggression spike vs the
  human's baseline, a cluster of recent `self_liked=false` follow-ups, looser/faster
  plays right after a loss
- `bot_hand_review` — a run of recent `classification='bad_luck'` losses (bad-beat streak → tilt risk)
- `symbols` `pt_vpip_raischair` / `pt_af_raischair` — opponent baseline for the exploit read

## Procedure (each invocation / loop pass)
1. `hiss_status`. If unreachable, skip.
2. `game_state` → hero chair/stack, current handnumber, opponents.
3. **Hero tilt score** — combine: `hero_tilting`/`hero_drawdown` (live) + recent
   `game_state_log` drawdown + bad-beat streak in `bot_hand_review` + (if manual play)
   behavioral spike in `learner_decisions`. Score 0..1.
4. **Opponent tilt** — for seated opponents with a PT sample: `raiser_maybe_tilting`
   (live) and/or recent big stack drop + above-baseline aggression (`pt_af`).
5. **Act:**
   - Log every detection to `tilt_events` (subject, kind, score, reason, hero_stack, detail, action_taken).
   - If hero tilt is **moderate** (≥0.4): `speak` a short Lilith alert, e.g.
     *"Heads up — you're down about forty percent in the last dozen hands and playing looser. Consider a break."*
   - If hero tilt is **severe** (≥0.7): `speak` the alert AND `autoplayer_toggle` off
     (the bot stops until the user resumes); note `action_taken='autoplayer_off'`.
   - If an **opponent** is likely tilting: `speak` a brief exploit note, e.g.
     *"Seat five just dropped a big pot and is over-firing — value-bet thinner, don't bluff them."*
6. **Debounce:** don't repeat the same alert within ~10 minutes (check the latest
   matching `tilt_events` row before speaking again).
7. One- or two-line summary of what was detected/spoken.

## Guardrails
- Keep alerts short and succinct (they're read aloud) — a single sentence.
- Soft, advisory: never change the OHF here. (Real-time strategy reaction is the C++
  engine's job; if the user wants the OHF to react to `raiser_maybe_tilting` etc.,
  that's an OHF edit, not this skill.)
- Require a PT sample (≥ ~30 hands) before trusting an opponent read.
- `speak` works during bot play (it uses the shared lilith.exe), so this does NOT need
  learner.exe to be open.
