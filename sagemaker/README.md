# Hiss → SageMaker package

The NN training pipeline for the Hiss poker bot. Full design: `../​.strategy_build/SAGEMAKER_TRAINING_PLAN.md`.
Built AWS-independent so it's plug-and-play the moment AWS creds land on the box.

## Files
| file | role |
|------|------|
| `features.py` | **the contract** — the information set: maps a `/decide` row → fixed feature vector + targets. Shared by emitter, trainer, inference. |
| `emit.py` | **data-emission** — one training row per decision → JSONL or postgres `hiss_training`; reward back-filled from replay by `hand_id`. |
| `train.py` | **trainer** — SageMaker PyTorch script-mode MLP (policy + bet-size + value heads). Saves `model.pt` + TorchScript `model_scripted.pt` for in-process inference. |
| `sm_tools.py` | **AIL primitives** — boto3 SageMaker ops (status / ensure_bucket / ensure_execution_role / start_training / describe / evaluate / promote). Dormant until creds. |
| `requirements.txt` | deps |

## Data flow
```
headless Linux Hiss daemons (6/8/9-max on poker.scarletbeast.com)
   │  per decision: build_row(table_view, /decide response)   [emit.py]
   ▼
hiss_training (postgres)  ──reward back-fill by hand_id (replay outbox)──┐
   │  shipper batches shipped=false                                       │
   ▼                                                                      │
s3://scarletbeast-hiss/features/*.parquet  ◄──────────────────────────────┘
   │  SageMaker PyTorch training (Spot)   [train.py / sm_tools.start_training]
   ▼
model.pt + model_scripted.pt  →  champion/challenger eval (replay EV/100 + adversarial)
   │  if challenger wins → promote
   ▼
in-process TorchScript inference in the daemon → openai_action override (A/B vs OHF)
```

## Plug-and-play once AWS creds exist (`aws configure --profile hiss`)
```bash
pip install -r requirements.txt
python -c "import sm_tools,json; print(json.dumps(sm_tools.status(),indent=2))"   # verify identity
python -c "import sm_tools; print(sm_tools.ensure_bucket()); print(sm_tools.ensure_execution_role())"
export HISS_SM_ROLE_ARN=<arn printed above>
# once features/*.parquet exist in S3:
python -c "import sm_tools; print(sm_tools.start_training())"
```
Local smoke test (no AWS): drop a few `*.jsonl` rows in `./data` then
`python train.py --train ./data --epochs 3 --model-dir ./model`.

## AIL via MCP (next)
`sm_tools` is the reusable core; a thin MCP wrapper exposes `sm_status / sm_start_training /
sm_describe / sm_evaluate / sm_promote` so the cron/loop AIL drives export→train→eval→gate→
deploy→journal, mirroring the OHF AIL (cron 03431755). Promotion gated on EV/100 + adversarial replay.

## Status (2026-06-15)
- Headless Linux Hiss **loads the harmonized OHF + decides correctly** (the data generator is ready).
- Scaffold (this package) built. **Pending:** AWS creds on the box → then bucket/role/first training run,
  the daemon emit-wiring, and the `sm_*` MCP wrapper + AIL.
