# Hiss → Amazon SageMaker training plan
*Drafted 2026-06-15. Scopes a NN poker policy trained on poker.scarletbeast.com 6/8/9-max via the
headless Linux Hiss client, wired to every input we can, fit inside ~$200–300 free-tier credits, with
an MCP-driven Autonomous Improvement Loop (AIL) for the trained model.*

Prereq: the Linux headless client loads the harmonized OHF completely (see
`HISS_LOGGING_DAEMON_REQUIREMENTS.md` A-REMAINING — the `explain_*` symbol-engine stub). The OHF bot is
the **data generator + behavioral baseline (champion)**; the NN learns to beat it / the field.

---

## 1. Strategy & framing
Two complementary models, smallest-first to respect the budget:

- **Phase-1 — Imitation + value (supervised).** Learn a policy `π(action | infoset)` and a hand-EV head
  from logged hands (the OHF bot's decisions + outcomes from poker.scarletbeast.com). Cheap, stable,
  immediately useful as an **advisor** that A/Bs against the OHF. This is where the free-tier credits go.
- **Phase-2 — Policy improvement (offline RL / regret).** Once enough labeled data exists, improve with
  offline RL (CQL/IQL) or an OpenSpiel-style **Deep CFR** head toward Nash-ish play. Optional, gated on
  Phase-1 results + remaining credits. Self-play is expensive — keep it small/late.

Model: a compact **MLP/feature-tower** (not a transformer) — the infoset is a fixed feature vector, so
~3–5 dense layers (256–512 wide) with separate **policy** (fold/call/raise-buckets + bet-size head) and
**value** heads. <5M params → trains on a single small GPU in minutes, serves on CPU. Framework:
**PyTorch** on SageMaker (script mode, the official PyTorch DLC).

## 2. The information set (wire EVERYTHING we can)
Per decision point, emit one row from the headless Linux Hiss (it already evaluates all of these via the
engine + the harmonized OHF symbols). Gate every row on `validator_ok`/`validator_confidence` (drop
mis-scrapes). Features:
- **Cards/board**: hole (rank+suit, + isomorphic canonicalization), board, made-hand + draw flags
  (HaveTopPair/HaveOverPair/HaveSet/HaveFlush…, FlushPossible/StraightPossible), `prwin` (MC equity),
  board-texture (f$BoardWet/Dry/Scary/HighCardFoldy/Parched).
- **Position/stacks/pot**: position, nplayersdealt/nopponentsplaying, StackSize, EffStack, PotSize,
  AmountToCall, SPR (f$SPR), M/Mzone (f$M), bblind/sblind/tgi_ante, betround.
- **Tournament/ICM**: f$Stage, tgi_players_remaining, icm_fold/callwin/calllose, f$BubbleTighten.
- **Opponent (HUD)**: pt_vpip/pfr/flop_af/wtsd/hands/3bet (raischair), archetype flags
  (f$Opp_IsNit/Station/TAG/LAG/Fish/Maniac/Foldy/PotCommitted).
- **Timing tells**: lastraiseractiontime, f$TimingSaysWeak/Strong, myactiontime.
- **Action history**: Raises/Calls/Bets, BotRaisedBeforeFlop/OnFlop/OnTurn, betting line.
- **Tilt/meta** (from the existing engines): hero_drawdown/tilting, raiser_recent_bigloss/tilting.
- **Label/target**: the action actually taken + RaiseTo size; **reward** = chips won/lost on the hand
  (and Sklansky-$/all-in-adjusted EV from replay) for the value head + offline-RL phase.

Source of truth for outcomes: the **replay/Hiss telemetry** ([[replay-logging-system]]) +
the OHF/ACR hand-history writer — join decisions→results by hand id.

## 3. Data pipeline (headless Linux Hiss → S3)
1. Headless Linux Hiss daemons play/observe 6/8/9-max on poker.scarletbeast.com, emitting one JSON/row
   per decision (the infoset above) into the **Hiss postgres outbox** (reuse the existing outbox +
   `hiss_shipper.py`).
2. A shipper/export job batches rows → **Parquet on S3** (`s3://scarletbeast-hiss/training/…`),
   partitioned by date + table-size + daemon identity (see logging/daemon doc). Free tier: S3 5GB.
3. SageMaker **Processing job** (sklearn/pandas container) does feature engineering + the
   decision↔outcome join + train/val/test split → `s3://…/features/`. (Or do it in the export job to
   save credits.) Optional **Feature Store** later (skip on free tier — it bills).

## 4. Training on SageMaker (fit the credits)
- **Estimator**: `PyTorch` script-mode, `entry_point=train.py`. Start CPU `ml.m5.large`
  (~$0.115/hr) for the small MLP — likely enough and cheapest. Step up to one GPU `ml.g4dn.xlarge`
  (~$0.526/hr) only if training is slow or for Phase-2 RL. **Use Spot training** (`use_spot_instances=
  True`, `max_wait`) → up to ~70% off. Free tier includes 50 hrs of m4/m5/c4/c5 training/mo for 2 months.
- **HPO**: SageMaker Automatic Model Tuning, ≤10–20 jobs, Spot, early-stopping — keep it small.
- **Registry**: register every candidate in the **SageMaker Model Registry** with metrics; promotion is
  gated (below). Cheap.
- **Serving**: prefer **Serverless Inference** (scale-to-zero, pay-per-ms) or a single small
  real-time endpoint `ml.t2.medium`/`ml.c5.large` — but for the bot, **export the model and serve it
  IN-PROCESS** (TorchScript/ONNX inside the Linux Hiss daemon) to avoid a standing endpoint bill +
  network latency on the heartbeat. Endpoints only for offline batch eval.

## 5. Evaluation — champion/challenger (don't ship a worse bot)
- Hold out hands; compute **policy match vs OHF**, **value-head calibration**, and crucially **EV/100
  (bb/100, Sklansky-$/100)** on held-out replay hands.
- **Challenger must beat the champion** (current OHF or the deployed NN) by a margin on EV/100 over a
  min sample, with an **adversarial check** (does it spew vs stations? over-fold the bubble? — replay
  the regression suites) before promotion. Reuse `replay_hands`/`replay_stream`/`replay_frame` (MCP).

## 6. Inference wiring back to the bot (A/B against the OHF)
The OHF already has the **steering hijack hook**: `WHEN openai_action > -1000 RETURN openai_action`
in f$preflop/f$flop (and the `openai_*` symbols). Repoint that to the NN: the daemon runs the
TorchScript model on the infoset, writes its action into the `openai_action`/`*_action` override
symbol (heartbeat-thread only, per [[pt4-stats-heartbeat-thread-only]] / [[sb-engine-blocks-heartbeat]]
— never block the heartbeat with sync HTTP; in-process inference avoids that). Run **shadow mode first**
(log NN action, still act on OHF) → then **A/B by daemon identity** → then promote.

## 7. AIL via MCP — Autonomous Improvement Loop for the SageMaker model
Mirror the existing OHF AIL ([[improvement-cycle-cron]]) but for the model. Two layers:

**(a) MCP tool surface** — extend `mcp/hiss_mcp_server.py` (or a new `mcp/sagemaker_mcp_server.py`,
boto3-backed, AWS creds from the env/instance role):
- `sm_export_training_data` (trigger the outbox→S3 Parquet export), `sm_start_processing`,
  `sm_start_training_job(hyperparams, spot=true)`, `sm_describe_job`, `sm_job_metrics`,
  `sm_register_model`, `sm_evaluate(model)` (run the replay regression + EV/100 vs champion),
  `sm_promote(model)` / `sm_rollback`, `sm_deploy_inproc` (push TorchScript artifact to the daemons),
  `sm_list_models` / `sm_endpoints`.
These let Claude (or a cron) drive the whole loop conversationally and headlessly.

**(b) The loop** (champion/challenger, scheduled) — three driving options, pick per appetite:
1. **MCP-cron AIL (recommended, mirrors today's bot AIL):** a cron/`/loop` (like cron 03431755) every
   N hours calls the `sm_*` tools: export fresh hands → retrain (Spot) → `sm_evaluate` vs champion →
   if challenger wins by margin AND passes the adversarial replay suite → `sm_promote` + `sm_deploy_inproc`
   → journal to `Release/logs/improvement_journal.md` (+ the model registry). Claude reads metrics and
   decides; fully auditable. Stays in the existing AIL idiom.
2. **SageMaker Pipelines (native MLOps):** a `Pipeline` DAG (Process→Train→Eval→RegisterModel→Condition→
   Deploy) triggered by **EventBridge** schedule or on new-data threshold; Model Registry approval gate
   (manual or auto). The `sm_*` MCP tools wrap pipeline start/status so Claude can launch/observe it.
3. **SageMaker RL / self-play (Phase-2, late):** RLEstimator for policy improvement; gate hard on cost.
Always: **promotion is gated** (margin + adversarial replay) and **rollback is one tool call**.

## 8. Cost vs free-tier credits ($200–300)
- 2-month free tier: 50 hrs/mo m4/m5/c4/c5 training + 125 hrs/mo serverless/t2/t3 inference + 5GB S3.
- Phase-1 supervised MLP on Spot `ml.m5.large`: minutes/run → **single-digit dollars/month** even with
  daily retrains + HPO. S3 + processing: a few dollars. In-process serving: **$0** (no endpoint).
- Phase-2 GPU/RL only if justified; `ml.g4dn.xlarge` Spot ~$0.16/hr effective — budget a fixed cap.
- Realistic: **Phase-1 + the AIL fit comfortably in free-tier credits** with months of headroom; keep
  GPU/RL on a hard credit cap. The AIL should `log()` spend and stop at a budget like the OHF loops do.

## 9. Phased rollout
1. Finish the Linux headless load (explain_ stub) → daemons emit the infoset to the outbox.
2. Stand up the S3 export + a first supervised `train.py`; one manual SageMaker training run.
3. Wire shadow-mode inference (NN action logged, OHF acts); validate the infoset end-to-end.
4. Build the `sm_*` MCP tools; run the MCP-cron AIL on a schedule (export→train→eval→gate→deploy→journal).
5. A/B by daemon identity; promote on EV/100 win + adversarial pass. Phase-2 RL only if credits + results justify.

See also: `HISS_LOGGING_DAEMON_REQUIREMENTS.md` (data generators + per-daemon identity + DB log toggles),
[[replay-logging-system]], [[hiss-mcp-server]], [[improvement-cycle-cron]], [[sb-engine-blocks-heartbeat]].
