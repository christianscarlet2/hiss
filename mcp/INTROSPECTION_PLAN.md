# Opponent Introspection System — build spec (single source of truth)

One per-opponent signal set, separated by **gametype (nlhe/plo/plo8)**, feeding FOUR consumers:
the **OHF** (deep-wired), the **NN driver**, the **synapse harmonizer**, and a real-time
**Claude decision advisor**. Goal of every answer: **find the most profitable EXPLOIT of THIS
player in THIS spot.** Build is one bundle → **single Hiss rebuild + OHF build_and_lint + restart.**

## 0. The spine — INTUITION (everything harmonizes into one read)
All signals — HUD, introspection (rhythm/exploits/timing/showdown), the card/range guess, the synapse
state, and the live advisor — are **harmonized by the synapse harmonizer into a single INTUITION**, and
the strategy makes its decisions FROM that intuition, not from a dozen raw symbols read separately.
Concretely: the synapse graph fuses every node into intuition outputs, surfaced as `f$Intuition_*`
(e.g. `f$Intuition_Aggression`, `f$Intuition_VillainStrength`, `f$Intuition_Exploit`,
`f$Intuition_Confidence`). The OHF decision points (§3) read `f$Intuition_*`; the raw `intro_*`/HUD/
advisor symbols are the *inputs* the intuition is harmonized from, weighted by confidence + sample.
The NN consumes the same intuition vector. This is the unifying layer — build §3/§7 so decisions flow
through it.

## Status legend  ✅ done/validated · 🟡 coded (needs the rebuild) · ⬜ todo

## 1. Data foundation ✅ (live, validated on real hands)
- `hud_player_stats` re-keyed **(player, gametype)**; `hud_aggregator.py` tags each hand's gametype
  from the hh_text first line (`gametype_from_hh`). ✅
- `introspect_aggregator.py` — incremental (watermark) over `hiss_log_hands` (a pruned outbox, so we
  capture each hand as it flows), builds the durable **rolling last-100-hands model per
  (player,gametype)**:
  - `opponent_hand` (window source), `opponent_profile` (surfaced model), `opponent_timing` (latency),
    `introspect_state` (watermark). Schema live. ✅
  - Per-profile signals: **cont_freq** (rhythm = likelihood to KEEP firing), **aggr_index**,
    **fold_to_pressure**, **sd_strong_rate** (showdown: did he have it), **fastbet_tell**
    (P(strong | bet fast) — timing tell), **profile** label, and an **exploits** JSON of 0/1 flags:
    `overfold, folds_to_3bet, gives_up, keeps_firing, never_folds, honest, fast_is_weak, fast_is_strong`. ✅

## 2. C++ engine layer
- `CTablemapDB`: `GetHudPlayerStats(player, gametype, out)` (sum-collapse, gametype filter) +
  `GetOpponentProfile(player, gametype, SOppProfile*)` + `EmitOpponentTiming(batch)`. 🟡 (header+impl done)
- `CSymbolEngineOpponentIntrospection` (NEW) ⬜ — register after pokertracker. Per chair:
  - loads `SOppProfile` by resolved player name + current gametype (throttled, like HUD; no per-beat DB);
  - live in-RAM **ring buffer (≤100)** of this session's observed villain actions + context bucket
    (street, facing-bet, pot-odds, position, board-texture, was-aggressor, action, size);
  - **live action latency**: stamp when each chair becomes to-act vs when it acts → fast/normal/slow;
    batch rows → `EmitOpponentTiming` on hand end (off the hot path).
  - Provides chair-suffixed symbols (multiplexer already maps `_raischair`/`_aggressorchair`):
    `intro_known, intro_contfreq, intro_aggrindex, intro_foldpress, intro_sdstrong, intro_fasttell,
     intro_lastspeed, intro_simN, intro_profile, intro_rangestrength` and
    `exploit_overfold, exploit_folds3bet, exploit_givesup, exploit_keepsfiring, exploit_neverfolds,
     exploit_honest, exploit_fastweak, exploit_faststrong`.
  - **intro_rangestrength** = card/holdings guess (0..1) from line + profile + synapse read (§5).
- Gametype string helper: `g_table_is_omaha` + `isplo8` → "nlhe"/"plo"/"plo8". ⬜
- `HudManager`: pass current gametype to `GetHudPlayerStats`; trigger introspection profile load on
  name-resolve. ⬜
- Knob engine `CSymbolEngineOpenAI` + `/api/knob`: add advice channels (generic key→g_knob map) so the
  advisor can steer `openai_knob_advice_*` (aggro/bluff/raise-lean/fold-lean/confidence) with no further
  rebuild after this one. ⬜

## 3. OHF deep rewire ⬜
- `11_introspection.ohf` (NEW): `f$Intro_*` (blend `intro_*` with HUD `f$Opp_*`), `f$Exploit_*`
  (the exploit flags), `f$Range_*` (range-strength read), `f$Advice_*` (reads advisor knobs). The
  3 questions → `f$Intro_SimilarAction`, `f$Intro_WillKeepFiring` (cont_freq), `f$Intro_FastTell`.
- Weave through: `12_aggressor.ohf` f$OppRepresents (story credibility ⊕ range guess),
  `05_config.ohf` f$CbetFreq/f$ThreeBetBluffFreq/f$DoubleBarrelFreq/f$BetPct*/f$Committed,
  `40_preflop.ohf` (3-bet/iso vs folds_to_3bet/overfold), `50/60/70_*.ohf` `*_vs_bet` raise/call/fold
  + bluff/value lines. Rule: **never bluff `keeps_firing`/`never_folds`; barrel `overfold`/`gives_up`;
  attack `fast_is_weak`; respect `fast_is_strong`/`honest`; value-target stations big.**

## 3b. Table read — DONK-FEST 🟡 (engine done, OHF wire ⬜)
Engine counts seated donks (fish/station/loose-passive) → `intro_ndonks` / `intro_donkfest` (>=3).
OHF `f$Table_DonkFest`: against a donk-heavy freeroll, **get in CHEAP** (limp/min-open/flat wide,
small preflop investment — they don't punish) and **bet HEAVY for value** postflop when we connect
(they pay off) to STACK UP. Drives preflop sizing/openrange down + postflop value sizing up; pairs
with `f$BotPersona` fish-hunter.

## 3c. STREET-STRATEGY FORMER ⬜ (multi-street planning — deep wire)
Stop deciding street-by-street in isolation. On an early street FORM a multi-street PLAN and execute
toward it on later streets. Plans (buckets): pot-control / flat-then-bluff-scare (e.g. "flat the flop,
represent the flush & barrel if a flush card lands") / check-raise-then-barrel / value-3-streets /
delayed-cbet / give-up. The former **considers BUCKETS of the villain(s)' exploit PATHWAYS** — how
they could attack us on each line (float & stab, raise our cbet, donk-lead, check-raise bluff,
call-down light) — and picks the plan most robust to / most punishing of those pathways, weighted by
intuition + advisor. Persist the chosen plan across streets via OHF memory symbols (me_st_/me_re_,
reset on handreset like the aggressor system) + an advisor `advice_plan` channel. New
`13_streetplan.ohf`: `f$FormPlan` (sets the plan on flop from hand+board+f$Intuition_*+villain
exploit-pathway buckets), `f$Plan` (reads it), `f$Plan_IsBluffScare/IsCheckRaise/IsValueTriple/
IsPotControl/...` executed in 50/60/70. The advisor's fork reasoning (§4) is the richest plan former;
the OHF f$FormPlan is the no-advisor fallback. Feeds and is fed by INTUITION (§0).

## 3d. SHOW OF FORCE / investment protection ⬜ (intuition sub-layer)
Once we've put a considerable amount in (past a BB threshold / SPR), we're ATTACHED to those chips and
must protect them with exploitable force. New intuition reads: `f$ChipsInvested` (our contribution this
hand, in BB), `f$Intuition_Committed` (extends the existing f$Committed with the investment-attachment:
past the threshold we don't fold off the investment), and `f$Intuition_ShowOfForce` (when a big
bet/shove is the best way to WIN the pot now — fold equity vs the villain (intro_foldpress / exploit_
overfold / range read) + denial of equity + our investment make an overbet/jam +EV). Drives:
overbet/shove sizing when force wins it, never-fold-off-a-big-investment, and protective barrels.
Weighs into f$Intuition_Aggression + the bet-sizing + the *_vs_bet fold/call gates.

## 3e. THE BRAIN — unified easy API 🟡 (built + validated in synapse_map.py)
`synapse_map.py` harmonizes the introspection inputs (opponent_profile, gametype-matched) into
**INTUITION + DECISION PLAN + DECISION -> CURRENT DECIDED ACTION**, the single result. Grown synapses:
signal.introspection + signal.opponents + knob.advice -> intuition.read -> plan.line + output.decided.
Easy API: `python synapse_map.py --brain` (JSON) + the `brain_state` postgres table (single row id=1,
upserted every --watch tick) that ANY consumer reads (OHF future, NN, advisor, MCP). VALIDATED live:
returns {intuition{exploit,villain_strength,aggression,tilt,show_of_force,donkfest,persona,confidence},
decision_plan{code,label,street}, decision{fold,call,raise,betsize}, current_decided_action{action,
size_bb,exploit,plan,persona,confidence}}. Works now (reads profiles directly); stays consistent once
the OHF deep-rewire makes the engine decision itself intuition-driven.

## 3f. ISMYTURN decision loop + DECISION MEMORY ⬜
At **ismyturn** the decision references **INTUITION + DECISION PLAN + DECISION + CONTEXT** (the brain +
the live spot): check whether the CURRENT pathway (villain's line / our situation) **is in the decision
plan and was considered**; if yes, execute the planned line; if not, adapt (advisor re-forms). Then act.
And **REMEMBER** the situation + decision + pathway + context in a new `decision_memory` table so future
decisions recall similar past situations to derive more insightful action (closes the loop back into
introspection's "what do we/he do in similar conditions"). The advisor (§4) drives this; decision_memory
is queried for similar-situation recall + fed to the NN.

## 4. Decision Advisor (capstone) ⬜  — `mcp/decision_advisor.py`
- Trigger: **ismyturn rising edge** (poll cached `/api/symbols`), off-heartbeat, action-timer budget.
- Assemble a **relevance-ranked context bundle**: introspection answers + their relevance, the
  per-opponent **HUD stats**, the relevant symbols, dial/knob settings, synapse state, game state.
- **Attach the live table SCREENSHOT** (claude -p is multimodal): the latest heartbeat frame
  (`logs/frames/<ms>.bmp` / `/api/frames` / table_screenshot, PNG) so Claude sees board texture, bet
  sizing, stack depths, who's left to act — visual ground-truth the symbols can't fully encode, and a
  cross-check on the scrape. Down-scale + cap so it never blows the action-timer budget.
- `claude -p` (headless CLI, chosen) → best **questions** for the spot + reasoning over the villain's
  **forks** (check/bet/raise/fold), each answered from introspection+synapse+HUD, output a weighted,
  **exploit-oriented** recommendation (aggro/bluff/raise/fold leans + confidence + per-fork plan).
- Emit: push `openai_knob_advice_*` via `/api/knob` (OHF reads) + write a `decision_advice` row
  (NN + synapse read). **Graceful degrade**: short timeout; if late, prior knob values stand, bot
  decides normally. Never blocks the heartbeat.

## 5. Card/range guess ⬜  (intro_rangestrength + f$Range_*)
Estimate villain holdings strength from: betting line (story), profile (a nit's raise ≠ a maniac's),
fold_to_pressure, timing tell, and synapse state. 0=capped/bluffy → attack; 1=nutted → fold. Feeds
f$OppRepresents and the advisor's fork reasoning.

## 5b. Bot meta-persona ⬜  (adopt the opponent's worst nightmare)
Being a maniac / fish / calling-station is sometimes the highest-EV COUNTER, not just an opponent
label. The advisor + introspection can prescribe the BOT's own archetype as a deliberate exploit
(maniac vs a passive fold-heavy table, station vs over-bluffers, nit vs maniacs) by driving the
existing style knobs `openai_knob_aggro/bluff/openrange` (the manic_burst.py / ULTRA infra already
does table-reactive persona swings). New `f$BotPersona` layer in `05_config.ohf` reads an advisor
persona recommendation and swings the style dials; bounded so it never tilts into -EV spew.
Persona is chosen **per the specific villain in the pot** (especially heads-up) — whatever best
exploits THAT particular player (maniac to blow a nit off pots, station to snap a chronic bluffer,
tight-value vs a maniac) — not a table-wide default. Keyed off intro_profile/exploits for that chair.

## 6. NN driver ⬜  — `mcp/nn_driver.py` + sagemaker `features.py`/`net_def.py`
Add `intro_*`/`exploit_*`/`intro_rangestrength`/advice features to the `/decide` feature vector for the
villain chair so the NN conditions on opponent reads; advisor advice biases the sampled action now,
features added to the spec for future training.

## 7. Synapse harmonizer ⬜  — `mcp/synapse_map.py`
Introspection / exploit / timing / range / advisor nodes + synapses → `synapse_state`, ghost inference,
NN feature spec. Unifies the signal so it's visible in the Synapse tab and shared with the NN.

## 8. Build (LAST) ⬜
Edit all source freely (doesn't touch the running bot). Then ONE pass: compile Hiss (Release config,
`Hiss.sln /t:Hiss`), `build_and_lint.py` for the OHF, terminate+relaunch Hiss, live-verify the full
loop on real opponents. Risk: a single broken symbol pops Parse-Error modals on the seated bot
([[lint-clobbers-live-master]]) — everything must compile + lint before the restart.
