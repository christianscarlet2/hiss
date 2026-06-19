---
name: hud-stats-active-vs-playing
description: HUD stats stuck at 0 (big sample) = CHandHistoryWriter used active() not playing() for fold/winner detection; phone "active" means has-the-turn
metadata:
  type: project
---

HUD stats read 0/"-" despite large samples because the hand-history pipeline logged only fold-walks.

**Bug 1 (C++ — the real one):** `CHandHistoryWriter::ObserveActions` + `ObserveResult` detected
folds and the uncontested winner from `Player(i)->active()`. On the ACR phone tablemap `active()`
means "**has the turn**" (live: `nplayersactive=1` vs `nplayersplaying=4`), NOT "in the hand". So
every opponent was instantly logged `folds`; once `_folded[i]=true` the seat is skipped, so no
call/raise/bet line ever emits → `hud_aggregator.py` numerators (vpip_n/pfr_n…) stay 0. The lone
"active" seat was also crowned winner. **Fix:** use `playersplayingbits` / `nplayersplaying`
(card presence = the standard "still in hand" signal) for both. Rebuild needed (Release|Win32).
NOTE: legacy `hh_session_*.txt` writer worked; the current ACR-format writer (feeds `hiss_log_hands`
→ HUD) was the broken one.

**Bug 2 (aggregator):** `hud_aggregator.py` `SEAT_RE` also matched SUMMARY lines
(`Seat 4: name (button) collected (2.50)` ends in `(<num>)`) → phantom players like
"christianbeast (button) collected". Fix: stop seat parsing at the first `***` marker.

**Data hygiene:** HUD stats come from postgres `hud_player_stats` (n/d counters), fed by
`hud_aggregator.py --watch` (default DSN user=postgres/dbpass, runs from `mcp/`), NOT PT4. The table
accumulates forever while `hiss_log_hands` rotates (~15 rows). After a writer fix, `TRUNCATE
hud_player_stats` + set `hud_aggregator_state.last_hand_id` to current `MAX(id)` so only clean
post-fix hands count, and restart the --watch process so it reloads the parser. See
[[explain-openai-acr-features]] (the complete/incomplete HH split), [[bb-display-depth-pushfold]].
