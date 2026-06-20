#!/usr/bin/env python3
"""leak_scan.py - scan the local hiss_log_* telemetry for known decision leaks.

Read-only. Run each improvement cycle (or after a strategy/engine change) to catch
regressions fast. Exits 0 always; prints a concise report. The headline check is the
one that caught the all-in bug: a hand whose decision trace WANTED to jam/raise but the
recorded action was fold (see memory: allin-keypad-zero-betsize).

Usage:  python leak_scan.py
"""
import os, subprocess, sys

PSQL   = os.environ.get("HISS_PSQL", r"C:\Program Files\PostgreSQL\12\bin\psql.exe")
PGUSER = os.environ.get("PGUSER", "postgres")
PGDB   = os.environ.get("PGDATABASE", "hiss")
PGPASS = os.environ.get("PGPASSWORD", "dbpass")


def q(sql):
    env = dict(os.environ); env["PGPASSWORD"] = PGPASS
    cmd = [PSQL, "-U", PGUSER, "-d", PGDB, "-A", "-F", "|", "-t", "-c", sql]
    # CREATE_NO_WINDOW (0x08000000): never pop a psql console/terminal window. [Emrald]
    p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=60,
                       creationflags=(0x08000000 if os.name == "nt" else 0))
    if p.returncode != 0:
        raise RuntimeError(p.stderr.strip() or p.stdout.strip())
    return [ln for ln in p.stdout.splitlines() if ln.strip()]


CHECKS = [
    # name, SQL (returns rows = problems), how-to-read
    ("JAM/RAISE BUT FOLDED",
     r"""SELECT id, hero_cards, action FROM hiss_log_decisions
         WHERE action='f$fold'
           AND (trace LIKE '%f$allin = 1.000%' OR trace LIKE '%f$raise = 1.000%')
         ORDER BY id DESC LIMIT 25;""",
     "Strategy wanted to jam/raise but the bot folded (the all-in-keypad bug class)."),

    ("HIGH-EQUITY FOLD (prwin>=0.55)",
     r"""SELECT d.id, d.hero_cards, round(s.value::numeric,3)
         FROM hiss_log_decisions d
         JOIN hiss_log_symbols s ON s.handnumber=d.handnumber AND s.name='prwin'
         WHERE d.action='f$fold' AND s.value::numeric >= 0.55
         ORDER BY d.id DESC LIMIT 25;""",
     "Folded with >=55% win prob (review: facing a bet/shove this may be an equity leak)."),

    ("SUB-1bb BETSIZE",
     r"""SELECT id, hero_cards, action, amount FROM hiss_log_decisions
         WHERE amount > 0 AND amount < 1 ORDER BY id DESC LIMIT 25;""",
     "A betsize below 1bb was recorded (should be floored to 1)."),
]


def main():
    print("=== Hiss leak scan ===")
    total_problems = 0
    for name, sql, how in CHECKS:
        try:
            rows = q(sql)
        except Exception as e:
            print(f"[{name}] ERROR: {e}")
            continue
        if rows:
            total_problems += len(rows)
            print(f"\n[{name}]  {len(rows)} hit(s) -- {how}")
            for r in rows:
                print("   ", r)
        else:
            print(f"[{name}]  clean")
    # context
    try:
        mix = q("SELECT action||' '||count(*) FROM hiss_log_decisions GROUP BY action ORDER BY 1;")
        print("\naction mix (local window):", ", ".join(mix) or "(none)")
    except Exception:
        pass
    print(f"\n{total_problems} flagged decision(s) total.")
    print("Note: pre-fix hands may still appear until they rotate out of the 10-hand local window.")


if __name__ == "__main__":
    main()
