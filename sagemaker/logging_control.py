"""
logging_control.py — daemon identity + DB-backed advanced-logging toggles.

Single source of truth = the `hiss_log_settings` table in the hiss postgres DB:
  identity PK ('*' = global default; a daemon_id row overrides it)
  advanced_logging | reporting | replays  (booleans)

Effective(identity) = the identity row's flags, each falling back to '*'.
Used by: the Linux daemon emit path (gate what flows), hiss.exe (native), the
web control at hiss.scarletbeast.com, and the MCP `log_settings` tool.

Backends (auto): psycopg2 if HISS_PG_DSN/psycopg available (Linux daemons over the
LAN), else shell out to psql.exe (Windows, where the DB is local — mirrors the MCP).
"""
from __future__ import annotations
import os, socket, subprocess, json

KINDS = {"advanced_logging", "reporting", "replays"}
_ALIASES = {"advanced": "advanced_logging", "logging": "advanced_logging", "adv": "advanced_logging",
            "report": "reporting", "reports": "reporting", "replay": "replays"}

PSQL = os.environ.get("HISS_PSQL", r"C:\Program Files\PostgreSQL\12\bin\psql.exe")
PGUSER = os.environ.get("PGUSER", "postgres")
PGDB = os.environ.get("PGDATABASE", "hiss")
PGPASS = os.environ.get("PGPASSWORD", "dbpass")
DSN = os.environ.get("HISS_PG_DSN")  # e.g. "host=192.168.1.50 dbname=hiss user=hiss password=..."


def resolve_identity() -> str:
    """Per-daemon identity for advanced logging. HISS_IDENTITY wins; else host[-table]."""
    v = os.environ.get("HISS_IDENTITY")
    if v:
        return v
    host = socket.gethostname()
    tbl = os.environ.get("HISS_TABLE")
    return f"{host}-t{tbl}" if tbl else host


def normalize_kind(k: str) -> str:
    k = (k or "").strip().lower()
    k = _ALIASES.get(k, k)
    if k not in KINDS:
        raise ValueError(f"unknown log kind '{k}' (use: advanced_logging | reporting | replays)")
    return k


# --- DB backends ------------------------------------------------------------------
def _query(sql: str, write: bool = False):
    """Run SQL, return list[dict] for selects. Tries psycopg2, falls back to psql.exe."""
    try:
        import psycopg2, psycopg2.extras
        dsn = DSN or f"dbname={PGDB} user={PGUSER}"
        with psycopg2.connect(dsn) as conn:
            with conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
                cur.execute(sql)
                rows = cur.fetchall() if cur.description else []
            conn.commit()
        return [dict(r) for r in rows]
    except ImportError:
        # psql.exe fallback (Windows-local DB), JSON-aggregated for easy parsing
        wrapped = f"SELECT coalesce(json_agg(t),'[]') FROM ({sql}) t" if sql.lstrip().lower().startswith("select") else sql
        cmd = [PSQL, "-U", PGUSER, "-d", PGDB, "-t", "-A", "-c", wrapped]
        env = {**os.environ, "PGCLIENTENCODING": "UTF8", "PGPASSWORD": PGPASS}
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=20, env=env)
        if proc.returncode != 0:
            raise RuntimeError("psql failed: " + (proc.stderr.strip() or proc.stdout.strip()))
        out = proc.stdout.strip()
        return json.loads(out) if (out and sql.lstrip().lower().startswith("select")) else []


def get_effective(identity: str | None = None) -> dict:
    """Resolve the effective flags for an identity (its row, each flag falling back to '*')."""
    identity = identity or resolve_identity()
    rows = _query("SELECT identity, advanced_logging, reporting, replays FROM hiss_log_settings "
                  f"WHERE identity = '*' OR identity = '{identity.replace(chr(39), '')}'")
    g = next((r for r in rows if r["identity"] == "*"), {"advanced_logging": False, "reporting": False, "replays": True})
    s = next((r for r in rows if r["identity"] == identity), None)
    eff = {k: (s[k] if s and s.get(k) is not None else g.get(k)) for k in KINDS}
    eff["identity"] = identity
    return eff


def set_flag(kind: str, value: bool, identity: str = "*", by: str = "api") -> dict:
    kind = normalize_kind(kind)
    identity = identity.replace("'", "")
    val = "true" if value else "false"
    _query(
        f"INSERT INTO hiss_log_settings(identity,{kind},updated_by) VALUES ('{identity}',{val},'{by}') "
        f"ON CONFLICT (identity) DO UPDATE SET {kind}=EXCLUDED.{kind}, updated_at=now(), updated_by='{by}'",
        write=True)
    return get_effective(identity if identity != "*" else None)


def list_all() -> list:
    return _query("SELECT identity, advanced_logging, reporting, replays, "
                  "to_char(updated_at,'YYYY-MM-DD HH24:MI') AS updated_at, updated_by "
                  "FROM hiss_log_settings ORDER BY (identity='*') DESC, identity")


def is_enabled(kind: str, identity: str | None = None) -> bool:
    return bool(get_effective(identity).get(normalize_kind(kind)))


# --- CLI: python logging_control.py status | <kind> on|off [identity] -------------
if __name__ == "__main__":
    import sys
    a = sys.argv[1:]
    if not a or a[0] in ("status", "list"):
        for r in list_all():
            print(r)
    else:
        kind = a[0]
        val = (len(a) > 1 and a[1].lower() in ("on", "1", "true", "yes"))
        ident = a[2] if len(a) > 2 else "*"
        print(set_flag(kind, val, identity=ident, by="cli"))
