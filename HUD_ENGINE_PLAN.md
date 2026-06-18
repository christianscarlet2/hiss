# Own-Data HUD Engine — Architecture

**Goal.** Replace the PokerTracker-4 dependency in the Hiss HUD with the bot's **own** observed
hands. Compute per-player stats (VPIP, PFR, 3Bet, AF, …) from **every** hand the bot logs —
**including incomplete ones** — counting **verified players only**. Result: each Hiss instance's
HUD is self-sufficient (no PT4), and because both instances log to the same `hiss` DB, their
observations **pool** into one shared player-stats store (this also fixes the "HUD only shows on
one instance" problem — today the blank instance simply has no PT4 connection/data).

## Current state (what we're replacing)
- `CHudManager::RefreshIfNeeded` (Hiss/HudManager.cpp) pulls **everything** from PT4:
  `PT_DLL_GetStat("pt_hands", chair)` for sample size, `PT_DLL_GetStat("pt_<symbol>", chair)` per
  stat, gated on `p_pokertracker_thread->IsConnected()`. No PT4 ⇒ samples = -1, stats cleared ⇒
  blank HUD.
- Hands are already logged to postgres `hiss_log_hands(handnumber, complete, hh_text, ...)` in a
  PokerStars-style format (Seat N: Name (stack) … posts blinds … *** HOLE CARDS *** … folds/
  calls/raises/bets … *** SUMMARY ***). Both complete AND incomplete hands land here.
- The HUD consumer interface stays the same: `SamplesForChair(chair)` (int) and
  `StatsForChair(chair)` (vector<SHudStatValue{abbreviation, full_name, important, value}>), plus
  the stat **definitions** loaded from the .pt4hud profile (abbreviation/important/full_name). We
  keep this contract — only the data *source* changes.

## Architecture (3 pieces)

### 1. Capture — log every hand, tag verified seats
- Ensure the ACR hand-history writer logs **all** hands incl. incomplete (it already writes the
  `complete` flag; confirm incomplete hands are persisted, not dropped — only 17 hands exist now,
  so capture is sparse/under-firing and must be made reliable).
- Add per-hand **verified-seat** info so the aggregator can honour "verified only". Cleanest:
  a JSONB column `hiss_log_hands.verified_players` = array of the seat names the bot's
  name-verification trusted that hand (mirrors `game_state` per-seat `verified=true`). Unverified
  / misread names are excluded from stats.

### 2. Aggregate — Python service on swiftsnake (parser + counters)
- New `~/hiss-sagemaker/hud_aggregator.py` (or `mcp/`): polls `hiss_log_hands` by `id > last_seen`,
  parses each `hh_text`, attributes actions per player per street, and upserts counters.
- **Parser**: PokerStars-format reader → per player: position, preflop action sequence, postflop
  bet/call/raise/fold per street, blinds posted, went-to-showdown, etc.
- **Verified filter**: only increment a player's counters if that player ∈ this hand's
  `verified_players`.
- **Store**: postgres `hud_player_stats(player TEXT PRIMARY KEY, hands INT, vpip_n/vpip_d,
  pfr_n/pfr_d, threeb_n/threeb_d, f3b_n/f3b_d, cbet_n/cbet_d, ftc_n/ftc_d, steal_n/steal_d,
  fts_n/fts_d, aggr_actions, call_actions, …, updated_at)`. Numerator/denominator pairs so we can
  render % and AF accurately and keep accumulating. SHARED across both instances ⇒ pooled volume.
- Idempotent: track processed hand ids (a watermark table or a `processed` flag) so re-runs don't
  double-count. Backfills the full history on first run.
- Runs as a small loop/cron (like the other swiftsnake services).

### 3. Serve — HUD reads the shared store (replaces PT4)
- In `CHudManager::RefreshIfNeeded`, drop the `PT_DLL_*` path. For each chair: take the bot's
  **verified** player name (`p_table_state->Player(chair)->name()` when its verified flag is set),
  query `hud_player_stats` for that name, set `_chair_samples[chair] = hands`, and build
  `_chair_stats[chair]` by mapping each loaded **definition** (`symbol`/abbreviation/important) to
  the computed stat (e.g. `vpip = 100*vpip_n/vpip_d`, `af = aggr_actions/call_actions`).
- Query path from C++: reuse `p_tablemap_db` (already connected to `hiss`) with a small
  `GetHudStatsForPlayer(name)` method, throttled by the existing 2.5s refresh guard. Unverified
  seats ⇒ samples -1 (no box), exactly as desired ("verified symbols only").
- No bot/driver/overlay changes beyond the data source; the |-separated 3-row green-balance render
  we just built stays.

## Stat set (match existing abbreviations from the .pt4hud profile / game_state)
VPIP (VP), PFR (PF), 3Bet (3B), Fold-to-3Bet (F3B), 4Bet (4B), Fold-to-4Bet (F4B), CBet flop (CB),
Fold-to-flop-CBet (FTC), CBet turn/river (CBT/CBR), Steal/ATS, Fold-to-Steal (FTS), Total
Aggression (AF), RFI, n=hands. Each is numerator/denominator (or action-count ratio for AF).

## Build milestones
- **H1 — capture:** verify/fix that all hands incl. incomplete log to `hiss_log_hands`; add
  `verified_players` JSONB (bot writes it at log time).
- **H2 — schema:** create `hud_player_stats` + a processed-watermark.
- **H3 — aggregator:** `hud_aggregator.py` parser + counters + upsert; backfill; run as a service.
- **H4 — serve:** rewrite `CHudManager::RefreshIfNeeded` to read `hud_player_stats` via
  `p_tablemap_db`; rebuild Hiss; remove the PT4 gate.
- **H5 — validate:** stats populate from real + incomplete hands; HUD shows on BOTH instances;
  verified-only confirmed; numbers sane vs known players.

## Open decisions (confirm before building)
1. **Aggregator location:** Python on swiftsnake (recommended — easy parser, shared, backfills) vs
   in-bot C++ (real-time but duplicated per instance + harder parser). → leaning Python.
2. **Verified signal:** add `verified_players` JSONB to `hiss_log_hands` at log time (recommended),
   vs inferring "real name" heuristically. → leaning explicit column.
3. **Sample threshold:** min hands before a stat shows (e.g. hide % until n≥ ~5) so early numbers
   aren't noise. → small floor, show n= always.

## Risks
- Parser robustness on ACR format quirks (hero-unknown, uncalled bets, all-ins, side pots, sit-outs).
- Capture reliability — only 17 hands logged so far; stats need volume, so fixing capture (H1) is as
  important as the engine.
- Stat-definition correctness (standard poker defs; validate against a couple known players).
