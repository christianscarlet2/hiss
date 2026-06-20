---
name: tilt-detector
description: Detect tilt for the Hiss poker bot — both the hero (protect: stack drawdown, VPIP/aggression spikes, bad-beat/misplay streaks, disliked-decision clusters, the human's own negative voice feedback) and opponents (exploit: recent big loss + over-aggression). Pulls every signal the bot now exposes (live tilt symbols, voice_feedback, observations, learner_decisions, bot_hand_review, icm_config, synapse_state, HUD), guards against scrape glitches, then surfaces findings BOTH by speaking via Lilith AND by writing coach_notes into learner.exe's Poker-coach panel. Use when monitoring a session, when the user asks "am I tilting / is anyone tilting", or on the analysis loop.
---

# Tilt detector (hero + opponent)

Three layers now. The **real-time** layer is the C++ `CSymbolEngineTilt` (live symbols the
OHF reacts to in-hand). This **advisory** layer ties everything together, scores tilt from the
*many* signals the bot now produces, and **surfaces what it finds two ways**:

1. **Speaks** the alert aloud via Lilith (`speak` / `lilith.exe`), and
2. **Writes a `coach_notes` row** (`kind='tilt'`) so it shows in **learner.exe's "Poker coach
   (Lilith)" panel** and the **poker-coach** skill can build on it.

So a tilt discovery is both heard and written, landing in the same place the coach lives.
Use the MCP tools below.

---

## Signals (wire in everything the bot now exposes)

### Live (MCP `symbols`) — the C++ engine, fast / in-hand
- `hero_drawdown` (0..1) — fraction of the hero's recent peak stack lost
- `hero_tilting` — drawdown >= ~0.33
- `raiser_drawdown`, `raiser_recent_bigloss`, `raiser_maybe_tilting` — the player we face
- Scoring context: `balance`, `nopponentsplaying`, `handsplayed`, `prevaction`

### History / new bot intelligence (MCP `pg_query`, db `hiss`)
- **`voice_feedback`** (NEW — the human's own voice; weight it HEAVILY when fresh):
  Emrald's mic feedback pinned to the live hand. Columns: `created_at`, `ts_ms`, `handnumber`,
  `mode`, `transcript`, `category`, `sentiment` (real; **< 0 = negative**). A recent row with
  `sentiment < 0`, a frustrated/annoyed `transcript`, or a "disliked"-style `category` is a
  **direct hero-tilt signal** — the human told you they're heated. Look back ~15 min.
- **`observations`** (NEW — `observe_learn.py`): `scope='session', subject='hero'` carries the
  session trend (e.g. "down trend -> tighten"); `scope='villain'` carries per-opponent reads
  (short stack, recent big loss, over-aggression) for the **exploit** side. Uses `ts_ms`.
  *Unit caveat:* the session `net` can be unit-confused under BB-display scraping — sanity-check
  it (glitch guard) before treating a big negative as real.
- **`learner_decisions`** — **manual-play behavioral tilt**: VPIP/aggression spike vs the human's
  baseline, a cluster of recent `self_liked=false`, looser/faster plays right after a loss.
- **`bot_hand_review`** — a run of recent `classification='bad_luck'` (bad-beat streak -> tilt risk)
  or a cluster of `classification='misplay'`. Ignore stale `classification='bug'`/scrape rows.
- **`game_state_log`** — hero stack trajectory to corroborate `hero_drawdown`. **May be empty** —
  if so, do NOT treat the absence as confirmation; fall back to other corroboration.
- **`icm_config`** (NEW — tournament pressure, shared with the icm-chip-value skill): bubble stage
  + stack depth set how *costly* a tilt-spew is right now. Scales the alert threshold (below).
- **`synapse_state`** (NEW — `synapse_map.py`): latest unified node/synapse snapshot (`state` jsonb);
  optional aggregate cross-check when individual signals disagree.
- **HUD** (`symbols` `pt_vpip_raischair` / `pt_af_raischair`, or `game_state` hud[]) — opponent
  baseline for the exploit read (need a sample first).

---

## Play-mode awareness (check FIRST each pass — mirrors poker-coach)
What "hero tilt" *means* depends on who is driving the hero seat. Detect the autoplayer state via
`/api/autoplayer` (or the latest `autoplayer_engaged():` line in `Release/logs/oh_0.log`):

- **Autoplayer ENGAGED** -> the **bot** is auto-playing the phone. "Hero tilt" = **stack/downswing
  protection** for the *bot* (the human is not emotionally on this table). `voice_feedback` /
  `learner_decisions` are NOT this-table emotional tilt — weight the stack/drawdown + bad-beat
  signals. Severe + corroborated may pull the autoplayer (guarded, below).
- **Autoplayer DISENGAGED** -> the **human** is playing the phone manually. `voice_feedback` and
  `learner_decisions` behavioral drift are the **PRIMARY** hero-tilt signals; never toggle the
  autoplayer (it's already off) — alert + coach the human instead.

---

## Scrape-glitch guard (REQUIRED before any alert or action)
The live `hero_drawdown` and the observation `net` are computed off scraped balances, which under
**BB-display** scraping can spike (a transient chip-count misread inflates the "recent peak" -> a
phantom ~0.98 drawdown; a pot/balance read like `pot=131` vs sub-20bb stacks is a tell). A bot that
acts on these cries wolf and can shut itself off mid-tournament on noise. So:

1. **Sanity-check the magnitude.** A hero stack **at or above the field average** (`icm_config.avg_stack`
   or the table average from `game_state`) is *not* "down 98%". A `net` larger than the hero's whole
   stack is impossible. Treat such reads as glitches.
2. **Require corroboration.** Do **NOT** score hero tilt >= 0.4 on a single live flag alone. A real
   alert needs at least ONE independent corroborating signal: real `game_state_log` drawdown, a
   `bot_hand_review` bad-beat/misplay streak, negative recent `voice_feedback`, or a behavioral spike
   in `learner_decisions`.
3. If the flags fire but fail corroboration -> log a `tilt_events` row with **score < 0.4,
   `action_taken='monitor'`, NO `speak`, NO autoplayer change.** (This is the correct, common outcome;
   it proves the detector ran without false-alarming.)

---

## Procedure (each invocation / loop pass)
1. `hiss_status`. If unreachable, skip.
2. Determine **play mode** (autoplayer engaged/disengaged) — see above.
3. `game_state` -> hero chair/stack, current handnumber, opponents, blinds.
4. Pull the live signals (`symbols`) + the history / new-intelligence signals (`pg_query`).
5. **Apply the glitch guard.** Discard implausible drawdown/net; note which signals survived.
6. **Hero tilt score (0..1)** — combine the *corroborated* signals:
   - live `hero_tilting`/`hero_drawdown` (post-guard) + real `game_state_log` drawdown
   - bad-beat / misplay streak in `bot_hand_review`
   - (autoplayer disengaged) behavioral spike in `learner_decisions`
   - **negative recent `voice_feedback`** (strong; the human said so)
   - session downtrend in `observations` (post-guard)
7. **ICM-aware threshold** (from `icm_config`: places_paid = P, remaining = R, hero bb):
   - On/near the bubble (R <= ~1.15*P) **or** short stack (< ~12bb) -> a spew is catastrophic ->
     **lower** the speak threshold (~0.30) and the autoplayer-off threshold (~0.55).
   - Far from money (R > ~1.5*P) **and** deep (> ~30bb) -> variance is normal -> **raise** thresholds
     (speak ~0.50, off ~0.75).
   - Otherwise defaults (speak 0.40, off 0.70).
8. **Opponent tilt** — for seated opponents with a PT sample (>= ~30 hands): `raiser_maybe_tilting`
   (live) and/or `observations` villain "recent big loss + over-aggression" + above-baseline `pt_af`.

## Act — speak AND write to the coach panel
For every meaningful detection do BOTH outputs, then log it:
- **Speak** (Lilith, `speak`) — ONE short sentence (it's read aloud).
- **Write a `coach_notes` row** so it shows in learner.exe's Poker-coach panel and the coach can
  build on it (`pg_query` allow_write):
  `INSERT INTO coach_notes (handnumber, kind, priority, message, spoken) VALUES ('<hand>','tilt',<0-2>,'<text>', true);`
  Set `spoken=true` when you already spoke it via `speak` (so learner doesn't re-speak it); priority
  2 for severe, 1 for moderate, 0 for an opponent-exploit note.
- **Log a `tilt_events` row** (always, even on monitor):
  `(subject, kind, score, reason, handnumber, hero_stack, detail jsonb, action_taken)`.

Severity actions (after the glitch guard + ICM threshold):
- **Moderate (>= speak threshold):** speak + coach_note, e.g.
  *"Heads up — rough stretch the last dozen hands and you're playing looser. Take a breath, tighten up."*
- **Severe (>= off threshold) AND corroborated:** speak + coach_note (priority 2) AND
  `autoplayer_toggle` off; set `action_taken='autoplayer_off'`. **Never** pull the bot on an
  uncorroborated/glitchy signal.
- **Opponent likely tilting:** brief exploit `speak` + a priority-0 coach_note, e.g.
  *"Seat five just shipped a big pot and is over-firing — value-bet thinner, don't bluff them."*

## Debounce (coordinate with poker-coach + icm skills)
Don't repeat the same alert within ~10 min: check the latest matching `tilt_events` AND recent
`coach_notes` (kind in tilt/icm) before speaking again, so this skill, the coach, and the ICM skill
don't talk over each other.

## Output
A one- or two-line summary of what was detected, what survived the glitch guard, and what was
spoken / written / acted on.

---

## Guardrails
- Keep spoken/coach lines to ONE sentence, plain ASCII — they're read aloud.
- Soft + advisory: this skill **never edits the OHF**. Real-time strategy reaction is the C++
  engine's job; an OHF reaction to `raiser_maybe_tilting` is an OHF edit, not this skill.
- Require a PT sample (>= ~30 hands) before trusting an opponent read.
- The glitch guard is not optional — a false autoplayer-off mid-tournament is worse than a missed
  soft tilt nudge.
- `speak` / `lilith.exe` keeps scrcpy + ACRPoker unmuted and works during bot play without
  learner.exe open, so both outputs work whether the human is on the phone or desktop.
