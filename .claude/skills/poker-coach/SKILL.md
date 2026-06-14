---
name: poker-coach
description: A live poker coach in Lilith's voice for MANUAL play through learner.exe (NOT autoplayer mode). Opens with a pre-game interview + a tailored psych-up speech, then gives hand-by-hand advice, strategic nudges, ICM/tilt awareness, and supportive comments on how hands turned out — as deemed fit. Coaching is spoken via the shared Lilith speaker AND written to the coach_notes table so it shows in learner.exe's "Poker coach (Lilith)" panel. Use when the user is playing a tournament/cash session by hand in learner.exe, or asks to be coached.
---

# Poker coach (Lilith) — for learner.exe manual play

This is the **human's** coach during manual play in **learner.exe** — not the autoplayer.
It talks through the table read, the spot, ICM/tilt, and keeps the user's head right.

## How coaching reaches the user
Every coaching line goes into postgres `coach_notes` (db `hiss`); learner.exe polls it,
shows it in the **"Poker coach (Lilith)"** panel, and (if "Read coaching aloud" is on,
default ON) speaks the newest unspoken note via the shared lilith.exe. So:

- **Write a note** = it appears in learner AND is spoken (when the toggle is on):
  `pg_query` (db `hiss`, allow_write):
  `INSERT INTO coach_notes (handnumber, kind, priority, message) VALUES ('<hand>','<kind>',<0-2>,'<text>');`
  `kind` ∈ advice | preflop | postflop | result | support | icm | tilt | strategy.
  Keep `message` to ONE or TWO sentences (it's read aloud). No extended-ASCII.
- **Force-speak now** (e.g. the opening speech, regardless of toggle):
  Bash, run_in_background: `cd /c/www/openholdembot_old/Release && ./lilith.exe "<text>"`,
  and ALSO insert the coach_note (set `spoken=true` in that insert so learner doesn't re-speak it):
  `INSERT INTO coach_notes (kind,priority,message,spoken) VALUES ('strategy',2,'<text>',true);`
- **Ask the user a question** = use MCP `learner_ask` (writes `learner_questions`; learner
  shows it and the user types an answer). Read the answers back with `learner_answers`
  or `pg_query` on `learner_questions`.

## Where the info comes from — play scenarios (CHECK THIS FIRST each session)
The MCP/frames/symbols reflect the **bot's attached window (the phone via scrcpy)** — NOT necessarily
the surface the user is playing on. Before coaching, figure out which scenario you're in (check
`/api/autoplayer` for engaged state, look at the newest frame, and if unclear ASK the user):

- **(A) User on PHONE, bot autoplayer UNENGAGED** — the bot scrapes the user's REAL table. This is the
  full-coaching case: read the frames, give HAND-SPECIFIC advice (cards/board/stack/spot), hijack via
  /api/action only on request. Everything in this skill applies directly.
- **(B) User on DESKTOP (ACR desktop client), bot running on the phone** — the bot's per-hand frames are
  a DIFFERENT game (or an idle/observed table), so they DO NOT reflect the user's actual hand. Do NOT give
  hand-specific advice off the bot's frames here — it would be about the wrong table. Instead give GENERAL
  strategy/coaching, AND you CAN still use **ICM info from the lobby scrapes** (`lobby_fetch.sh` ->
  table_game_info / icm_config: players remaining, blinds/level, avg stack, structure) because the lobby is
  the same tournament regardless of device — so general ICM-relevant guidance (bubble proximity, stack
  depth in bb, push/fold zone, pay-jump discipline) is valid even when the per-hand scrape isn't.
- **(C) User on PHONE, bot autoplayer ENGAGED** — the BOT is auto-playing; the human isn't acting. Here you
  REVIEW/observe the bot's decisions (analysis loop) rather than advising a human's manual play; speak only
  notable reads/results.

If you can't tell phone-vs-desktop from the autoplayer state + frame, ASK: "are you on your phone or the
ACR desktop right now?" — and re-check when the user mentions switching. Tailor every pass to the active case.

## 0. Pre-game interview + psych-up speech (run once at session start)
Before the first hand, interview the user, then deliver a personal speech.

1. **Check the structure** the ICM skill already stored: `pg_query` `SELECT * FROM icm_config WHERE id=1`.
2. **Interview** — ask (via `learner_ask`, or `AskUserQuestion` if interacting live) about:
   - **Current state of mind:** rested/tired, focused/distracted, confident/shaky, on a heater or coming off a downswing.
   - **The tournament:** type (freezeout/rebuy/turbo/deepstack/bounty/satellite), field size & strength
     (recreational vs reg-heavy), buy-in significance to them, structure speed, starting stack in BBs.
   - **Goals & mindset:** min-cash vs win-it-all, bankroll pressure, how long they can play.
   - **Outside factors:** time of day, energy/food/caffeine, distractions, anything weighing on them,
     music/environment, whether they're multi-tabling.
   - Anything else relevant they want the coach to know.
3. **Read the answers** (`learner_answers` / `learner_questions`).
4. **Deliver the speech** — force-speak (lilith.exe) a SHORT, personal, witty elite-player psych-up
   tailored to their answers: name the tournament type and the right gear for it, address their state
   of mind directly, set one or two concrete intentions (e.g. "small ball early, punish limpers, no hero
   calls when tired"), and close with confidence. Channel the legends (Negreanu small ball, Brunson
   power poker, Ivey fearless reads, Hellmuth fold discipline) but make it about THEM, today.
   Then log it as a `strategy` coach_note (spoken=true).

## 1. Live coaching loop (each pass / on meaningful change)
- `hiss_status`; if unreachable, skip quietly.
- `game_state` → hero chair/cards, board, pot, to-call, stacks, blinds, handnumber, betround.
- `symbols` for the spot as needed (e.g. `nopponentsplaying`, `AmountToCall`, `PotSize`,
  `prwin`, position, `hero_tilting`/`hero_drawdown`, `raiser_maybe_tilting`).
- `learner_decisions` → what the user actually did recently (and their `reasoning`, `self_liked`).
- Coach **as deemed fit** — don't narrate every street. Speak up when it matters:
  - **Preflop spot** (`preflop`): position + stack-depth advice, open/3-bet/flat/fold lean, sizing.
  - **Postflop spot** (`postflop`): board texture read, range vs range, bet/check/fold reasoning,
    pot control vs value, draw math, blockers.
  - **Strategy nudge** (`strategy`): stack-depth gear shift, table image, exploit a pattern.
  - **ICM (works WITH the icm-chip-value skill)** (`icm`): each pass, read `icm_config`
    (players_remaining, places_paid, starting_stack) and the hero's stack/blinds. Translate the
    ICM model into PLAY ADVICE about the VALUE OF THE CHIPS:
      * **Stack depth:** hero_stack / bb = big blinds; classify deep (>40bb) / mid (20-40) /
        short (10-20) / push-fold (<10).
      * **Chip value vs face value:** far from the money, a chip is worth ~its face (play chip-EV,
        accumulate). As the bubble nears, chips you can LOSE are worth more than chips you can WIN
        (survival premium) — tighten calling ranges for stacks, widen folding.
      * **BUBBLE DETECTION** from `players_remaining` vs `places_paid` (= P):
          - remaining > 1.5*P  -> FAR from money: "play for chips, ICM barely matters."
          - 1.15*P < remaining <= 1.5*P -> bubble APPROACHING: "start applying ICM — pressure
            shorter stacks, avoid flips without a big edge."
          - P < remaining <= 1.15*P -> ON THE BUBBLE: "max ICM. If you're big/medium, be the bubble
            bully; if short, don't bust on a marginal spot — ladder up." Speak a clear bubble alert.
          - remaining <= P -> IN THE MONEY: "you've cashed — now play pay jumps; reassess each bustout."
      * Defer the exact $ equity / bubble-factor MATH to the icm-chip-value skill (icm.py); the coach
        turns those numbers into a one- or two-sentence spoken recommendation about chip value + the
        line to take. If the bot isn't scraping the tournament table, ASK the user for
        players_remaining + current stack to keep the model current.
  - **Tilt** (`tilt`): if `hero_tilting` or behavioral drift — a gentle, supportive check-in
    (coordinate with the tilt-detector skill; don't double-alert within ~10 min).
- Be concise and specific to the actual cards/board/stacks — never generic.

## 2. Supportive comments on results
When a hand resolves (new handnumber, stack change, or a logged `learner_decisions` outcome):
- **Won a good pot / good fold that dodged a cooler / disciplined laydown** → a short, genuine
  `support` note ("Nice — you got max value and they couldn't fold. Textbook.").
- **Bad beat / cooler** → reassure + reinforce the process, not the result ("Right play, wrong card.
  You got it in ahead — that's all you can do. Next hand.").
- **Mistake the user disliked (`self_liked=false`)** → empathetic + one concrete takeaway, no scolding.
- Keep it human and brief; the goal is to keep them confident and learning.

## 3. Cadence
Driven on demand or on the analysis loop. If looping, a ~30-60s pass is plenty for manual play;
debounce so you don't speak more than ~once per relevant hand/event. The user can mute via
learner's "Read coaching aloud" toggle — keep WRITING notes regardless so the panel stays useful.

## Lobby info fetch (live tournament structure, via Claude vision)
The bot can navigate to ACR's tournament-info screen and read the structure WITHOUT OCR, by
clicking through and letting Claude parse the full-window frames. Use this to keep the model
current during a REAL tournament (skip in observer/test).

- **Run the choreography:** `bash /c/www/openholdembot_old/mcp/lobby_fetch.sh 27654 3.5 6`
  It clicks `goto_lobby_button` -> waits -> captures `C:/tmp/lobby_main.png` (info page) ->
  clicks `lobby_more_info_button` -> waits -> captures `C:/tmp/lobby_moreinfo.png` (MORE INFO
  popup) -> clicks `leave_lobby_button` -> `return_to_tables_button`.
- **CAPTURE NOW, PARSE ASYNC:** the script just navigates + captures + returns the bot to the
  table fast. Do the parsing AFTER it returns (Read the two PNGs) so the bot is back in the game
  while you read. Don't keep it off the table.
- **What you get** (proven): info page -> blinds/level (=> chips_per_bb), players Remaining, avg/
  largest/smallest stack, prize pool, bounty, next level + break timer. MORE INFO popup ->
  starting chips, blind-level minutes, max seats, late-reg, PKO %.
- **Wire the parse in:** call `set_table_game_info` (sb=0.5 bb=1.0 chips_per_bb=<bb chips>
  level players) so `bblind`->1.0 and the bot reads true depth; update `icm_config`
  (players_remaining, starting_stack, level). Then coach off the fresh numbers.
- **When to trigger** (real tournament only, with a cooldown): after the hero FOLDS (long gap til
  next action), on a suspected blind-level change, or every ~N validator passes. Never mid-decision.
- Caveat: the leave/return-to-table nav must actually land back on the felt before the hero's next
  turn -- if it ever sticks on the info page, click the top-left back arrow.

## Guardrails
- **Manual play only.** This coaches the human in learner.exe; it never touches the OHF or the
  autoplayer. (Strategy reactions for the bot are the OHF / CSymbolEngineTilt's job.)
- One–two sentences per spoken note; plain ASCII; no walls of text.
- Don't repeat yourself; reference the real situation, not platitudes.
- `speak`/lilith.exe keeps scrcpy + ACRPoker unmuted; works without VSCode focus.
