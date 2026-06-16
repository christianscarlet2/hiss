"""
emit.py — the data-emission layer: one training row per decision.

The headless Linux Hiss play-loop makes a decision per to-act spot (via the same engine
path /decide uses). For each, build a canonical row and append it to the outbox. Outcome
(reward_bb) is back-filled by hand id from the replay/Hiss telemetry after the hand ends.

Two sinks, pick per deployment:
  * JSONL append (cheapest; the shipper batches → Parquet on S3)
  * postgres `hiss_training` table (joins cleanly with the existing replay outbox)

Carries the per-daemon identity (see HISS_LOGGING_DAEMON_REQUIREMENTS.md) so streams from
N daemons stay separable. DB-logging toggles gate whether rows actually flow.
"""
from __future__ import annotations
import json, os, time

# canonical row = the table view we fed in + the engine's decision surface + a join key.
def build_row(table_view: dict, decide_response: dict, *,
              daemon_id: str, hand_id: str, ts_ms: int | None = None) -> dict:
    return {
        "daemon_id": daemon_id,
        "hand_id": hand_id,
        "ts_ms": ts_ms if ts_ms is not None else int(time.time() * 1000),
        "hole": table_view.get("hole", ""),
        "board": table_view.get("board", ""),
        "action": decide_response.get("action", "none"),
        "raiseto": decide_response.get("raiseto", decide_response.get("sym", {}).get("f$betsize", 0)),
        "sym": decide_response.get("sym", {}),   # the full evaluated symbol surface = the infoset
        "reward_bb": None,                          # back-filled from replay by hand_id
    }


def emit_jsonl(row: dict, path: str) -> None:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "a") as fh:
        fh.write(json.dumps(row, separators=(",", ":")) + "\n")


def emit_decision(table_view: dict, decide_response: dict, *, hand_id: str,
                  jsonl_path: str | None = None, conn=None, identity: str | None = None) -> bool:
    """Gated emit: stamps the daemon identity and only writes when advanced_logging is ON
    for this daemon (resolved from hiss_log_settings, identity-row || global '*')."""
    from logging_control import resolve_identity, is_enabled
    identity = identity or resolve_identity()
    if not is_enabled("advanced_logging", identity):
        return False  # logging toggled off for this daemon -> nothing flows
    row = build_row(table_view, decide_response, daemon_id=identity, hand_id=hand_id)
    if conn is not None:
        emit_postgres(row, conn)
    if jsonl_path:
        emit_jsonl(row, jsonl_path)
    return True


# postgres sink — schema mirrors the row; reward_bb back-filled by a join job.
DDL = """
CREATE TABLE IF NOT EXISTS hiss_training (
  id          BIGSERIAL PRIMARY KEY,
  daemon_id   TEXT NOT NULL,
  hand_id     TEXT NOT NULL,
  ts_ms       BIGINT NOT NULL,
  hole        TEXT,
  board       TEXT,
  action      TEXT,
  raiseto     DOUBLE PRECISION,
  sym         JSONB NOT NULL,
  reward_bb   DOUBLE PRECISION,
  shipped     BOOLEAN NOT NULL DEFAULT FALSE
);
CREATE INDEX IF NOT EXISTS hiss_training_unshipped ON hiss_training (shipped) WHERE NOT shipped;
CREATE INDEX IF NOT EXISTS hiss_training_hand ON hiss_training (hand_id);
"""


def emit_postgres(row: dict, conn) -> None:
    """conn = a psycopg2/psycopg connection (the existing hiss DB)."""
    with conn.cursor() as cur:
        cur.execute(
            "INSERT INTO hiss_training (daemon_id,hand_id,ts_ms,hole,board,action,raiseto,sym) "
            "VALUES (%s,%s,%s,%s,%s,%s,%s,%s)",
            (row["daemon_id"], row["hand_id"], row["ts_ms"], row["hole"], row["board"],
             row["action"], float(row["raiseto"] or 0), json.dumps(row["sym"])))
    conn.commit()


# --- daemon integration note --------------------------------------------------------
# The C++ play-loop already computes `action` + the `sym` map for /decide (engined.cpp).
# Cheapest wiring: after each decision, the loop POSTs {table_view, decide_response} to a
# tiny local sink (this module behind a 1-route HTTP shim) OR writes the JSONL line itself.
# Reward back-fill: a periodic job joins hand_id -> chips won/lost from the replay outbox
# and UPDATEs reward_bb. The shipper then batches shipped=false rows -> S3 Parquet.
