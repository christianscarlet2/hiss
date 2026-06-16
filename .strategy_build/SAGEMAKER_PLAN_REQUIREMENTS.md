# SageMaker training plan — requirements captured (to draft after harmonization + Linux sync)

Emrald's stated requirements for the plan (2026-06-15):

1. **Goal**: train a neural network on `poker.scarletbeast.com` tables (6-max, 8-max, 9-max)
   using the **headless Linux Hiss client** (`/var/www/hiss-linux` on swiftsnake) as the data
   generator, trained on **Amazon SageMaker** using free-tier credits (~$200–300, exact TBD).
2. **Information set (wire EVERY input we can)**: timing tells (`lastraiseractiontime` etc.), bet
   sizes/amounts, hole + board cards, PokerTracker HUD stats (VPIP/PFR/AF/WTSD/...), **validated**
   symbol scrapes (gate on `validator_ok`/confidence), plus the new feature dials (M/SPR/stage/ICM/
   board-texture/opponent archetypes) — the whole OHF symbol surface as features.
3. **Scope to free-tier credits**: recommend the technologies + instance types + job sizing that fit
   the credit budget; call out costs explicitly.
4. **AIL via MCP (this requirement, added by Emrald)**: advise the **processes available for creating
   an Autonomous Improvement Loop (AIL) for the newly-created SageMaker model, driven via MCP** —
   i.e. how Claude/the agent autonomously runs collect→retrain→evaluate→gate→deploy→monitor on a
   schedule, mirroring the existing OHF AIL (cron 03431755, journal at Release/logs/improvement_journal.md).

## AIL-via-MCP — candidate processes to detail in the plan
- **MCP tool surface** (extend `mcp/hiss_mcp_server.py` or a new `mcp/sagemaker_mcp_server.py`, boto3-backed):
  `sm_start_training_job`, `sm_describe_job`, `sm_job_metrics`, `sm_register_model`,
  `sm_deploy_endpoint` / `sm_update_endpoint`, `sm_invoke_endpoint` (eval), `sm_promote`/`sm_rollback`,
  `sm_list_endpoints`. These let the agent drive the loop conversationally and from cron.
- **SageMaker-native MLOps**: SageMaker **Pipelines** (process→train→eval→register→deploy DAG),
  **Model Registry** + approval gates, **Automatic Model Tuning** (HPO), optional **SageMaker RL**
  for self-play / policy improvement. EventBridge schedule to kick the pipeline.
- **The loop itself** (champion/challenger): headless Linux Hiss streams the validated information set
  → S3 (optionally a Feature Store) → scheduled retrain/fine-tune → **evaluate vs the current champion
  on held-out replay hands** (reuse the replay/Hiss telemetry: replay_hands/stream/frame) → promote only
  if it beats champion by a margin → update endpoint → journal to improvement_journal.md → repeat.
  Drive it via the existing **/loop or cron** mechanism calling the `sm_*` MCP tools, exactly like the
  current AIL drives OHF improvements. Adversarial/eval gate before any promotion.
- **Inference wiring back to the bot**: the trained model serves as an advisor (mirror the existing
  `openai_action`/steering hijack path in 40_preflop/50_flop — a model endpoint can feed the same
  `*_action` override symbol), so the NN's decisions can be A/B'd against the OHF live.

See also: [[hiss-mcp-server]], [[improvement-cycle-cron]], [[replay-logging-system]],
[[sb-engine-blocks-heartbeat]] (don't block the heartbeat thread with sync inference HTTP).
Linux sync (`/var/www/hiss-linux`) is the prerequisite — it's the data generator and must run headless.
