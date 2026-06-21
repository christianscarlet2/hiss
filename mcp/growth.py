#!/usr/bin/env python3
"""growth.py -- the brain's SELF-GROWTH loop.

The brain recognizes which of its AREAS can GROW -- by joining the decision TELEMETRY (brain_log: what
it decided, by exploit / plan / persona / source) with the OUTCOMES (hand_results: net bb) -- and, when
an area is reliably underperforming, it GROWS: automatically triggering a rewrite by Claude (headless
`claude -p`) of the source that governs that area, handed the evidence + synapses + telemetry. Safe by
default: it flags the growth area + writes a GROWTH BRIEF (review) unless AUTO_REWRITE=1, in which case
it spawns the growth agent which edits the .strategy_build SOURCE (never the live master), validates with
build_and_lint to a temp, and does NOT deploy (the human does the elevated restart).

  python growth.py            # assess + flag once
  python growth.py --watch    # loop
  AUTO_REWRITE=1 python growth.py --watch   # also auto-spawn the growth agent (rewrite)
"""
import os, sys, json, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bus

DSN = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")
ROOT = r"C:\www\openholdembot_old"
CLAUDE_BIN = os.environ.get("CLAUDE_BIN", "claude")
MIN_SAMPLE = int(os.environ.get("GROW_MIN_SAMPLE", "20"))
LOSS_BB = float(os.environ.get("GROW_LOSS_BB", "-3.0"))     # avg net per hand below this = an area to grow
AUTO_REWRITE = os.environ.get("AUTO_REWRITE", "0") == "1"

# which SOURCE governs each brain area -> handed to the growth agent.
AREA_SOURCES = {
    "exploit": ["mcp/synapse_map.py (compute_intuition + _exploits classification)",
                "mcp/introspect_aggregator.py (_exploits, _classify)",
                ".strategy_build/strategy/11_introspection.ohf (f$Exploit_*/f$Intuition_*)"],
    "plan":    ["mcp/synapse_map.py (compute_plan)", ".strategy_build/strategy/13_streetplan.ohf"],
    "persona": ["mcp/synapse_map.py (compute_intuition persona)",
                ".strategy_build/strategy/11_introspection.ohf (f$BotPersona)"],
    "source":  [".strategy_build/strategy/05_config.ohf (f$AggroFreqMult/f$BluffFreqMult)",
                "mcp/synapse_map.py (resolve_action exploit precedence)"],
}


def ensure_schema(cur):
    cur.execute("""CREATE TABLE IF NOT EXISTS growth_log (id bigserial primary key, ts_ms bigint,
        area_type text, area_key text, sample int, avg_net real, total_net real, evidence jsonb,
        brief text, status text DEFAULT 'flagged')""")


def score(cur, dim):
    """avg/total net per value of a brain-log dimension (exploit/plan/source/persona), joined to outcomes."""
    cur.execute("""SELECT (brain->'current_decided_action'->>%s) AS k, count(*), round(avg(hr.net)::numeric,2),
                          round(sum(hr.net)::numeric,2)
                   FROM brain_log bl JOIN hand_results hr ON hr.handnumber = bl.handnumber
                   WHERE (brain->'current_decided_action'->>%s) IS NOT NULL
                   GROUP BY 1 HAVING count(*) >= %s ORDER BY 3 ASC""", (dim, dim, MIN_SAMPLE))
    return [dict(area_type=dim, area_key=r[0], sample=r[1], avg_net=float(r[2] or 0), total_net=float(r[3] or 0))
            for r in cur.fetchall()]


def build_brief(a):
    src = AREA_SOURCES.get(a["area_type"], ["mcp/synapse_map.py"])
    return (
        "SELF-GROWTH TASK for the Hiss poker brain.\n\n"
        "GROWTH AREA: %s = %r is underperforming: avg %.2f bb/hand over %d hands (total %.1f bb).\n\n"
        "GOVERNING SOURCE (edit the .strategy_build SOURCE, never the live Release master):\n  - %s\n\n"
        "EVIDENCE: query brain_log JOIN hand_results / decision_memory for this area; inspect the losing\n"
        "spots, the villain reads (opponent_profile), the synapse_state, and decision_memory recall.\n\n"
        "GOAL: grow the logic so this area is +EV (or neutral). Keep edits ADDITIVE/safe (fire only on\n"
        "confident reads). Then VALIDATE: HISS_MASTER_OUT=<temp> python .strategy_build/build_and_lint.py\n"
        "(must be lint-clean). Do NOT deploy to the live master and do NOT restart Hiss (Emrald does the\n"
        "elevated restart). Summarize the change + why the telemetry says it grows the area."
        % (a["area_type"], a["area_key"], a["avg_net"], a["sample"], a["total_net"], "\n  - ".join(src))
    )


AREA_SOURCES["overwork"] = ["mcp/synapse_map.py (optimize the named function: cache repeated profile/DB "
                            "reads, avoid redundant work, batch queries -- WITHOUT changing its outputs)"]


def score_load(cur):
    """Find OVERWORKED brain parts (slow per call / hot) from brain_load -> growth areas to OPTIMIZE."""
    cur.execute("""SELECT func, sum(calls), round(avg(avg_ms)::numeric,3), round(max(max_ms)::numeric,2),
                          round(sum(total_ms)::numeric,1)
                   FROM brain_load WHERE ts_ms > %s GROUP BY func ORDER BY 5 DESC""",
                (int(time.time() * 1000) - 3600 * 1000,))          # last hour
    out = []
    for func, calls, avg_ms, max_ms, total_ms in cur.fetchall():
        if float(avg_ms or 0) >= 25.0 or float(max_ms or 0) >= 120.0:   # slow per call or spiky
            out.append(dict(area_type="overwork", area_key=func, sample=int(calls or 0),
                            avg_net=-float(avg_ms or 0), total_net=-float(total_ms or 0),
                            evidence=dict(avg_ms=avg_ms, max_ms=max_ms, total_ms=total_ms, calls=calls)))
    return out


def build_load_brief(a):
    e = a["evidence"]
    return ("SELF-GROWTH (PERFORMANCE) TASK for the Hiss poker brain.\n\n"
            "OVERWORKED PART: %s -- avg %.2f ms/call, max %.2f ms, %s calls, %.1f ms total (last hour).\n\n"
            "GOVERNING SOURCE:\n  - %s\n\n"
            "GOAL: make this function FASTER (cache repeated DB/profile reads, avoid redundant work, batch\n"
            "queries) WITHOUT changing its outputs. Validate the brain still runs: python mcp/synapse_map.py\n"
            "--brain. The brain runs hot every tick, so shaving this frees latency for deeper thought."
            % (a["area_key"], e["avg_ms"], e["max_ms"], e["calls"], e["total_ms"], AREA_SOURCES["overwork"][0]))


def grow(a, brief):
    """Trigger the growth. Enqueues a 'rewrite' job on the bus (audit trail) and, when AUTO_REWRITE,
    spawns the headless growth agent (staged + validated, never auto-deployed)."""
    bus.enqueue("rewrite", {"area_type": a["area_type"], "area_key": a["area_key"],
                            "avg_net": a["avg_net"], "sample": a["sample"], "brief": brief})
    if not AUTO_REWRITE:
        return "flagged (review; set AUTO_REWRITE=1 to auto-grow)"
    try:
        subprocess.Popen([CLAUDE_BIN, "-p", brief, "--model", os.environ.get("GROW_MODEL", "sonnet"),
                          "--allowedTools", "Read", "Edit", "Bash", "Grep", "Glob"],
                         cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         creationflags=(0x08000000 if os.name == "nt" else 0))   # CREATE_NO_WINDOW
        return "growth agent spawned"
    except Exception as e:
        return "spawn failed: %s" % e


def run_once(conn):
    cur = conn.cursor(); ensure_schema(cur); conn.commit()
    areas_grown = 0
    for dim in ("exploit", "plan", "source", "persona"):
        try:
            areas = score(cur, dim)
        except Exception:
            conn.rollback(); continue
        for a in areas:
            if a["sample"] >= MIN_SAMPLE and a["avg_net"] <= LOSS_BB:
                brief = build_brief(a)
                action = grow(a, brief)
                cur.execute("""INSERT INTO growth_log (ts_ms,area_type,area_key,sample,avg_net,
                    total_net,evidence,brief,status) VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s)""",
                    (int(time.time() * 1000), a["area_type"], a["area_key"], a["sample"], a["avg_net"],
                     a["total_net"], json.dumps(a), brief, action))
                conn.commit(); areas_grown += 1
                print("[growth] GROWTH AREA: %s=%r avg %.2f bb x%d -> %s"
                      % (a["area_type"], a["area_key"], a["avg_net"], a["sample"], action), flush=True)
    # OVERWORKED parts (slow / hot brain components) -> growth-to-OPTIMIZE
    try:
        for a in score_load(cur):
            brief = build_load_brief(a)
            action = grow(a, brief)
            cur.execute("""INSERT INTO growth_log (ts_ms,area_type,area_key,sample,avg_net,total_net,
                evidence,brief,status) VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s)""",
                (int(time.time() * 1000), a["area_type"], a["area_key"], a["sample"], a["avg_net"],
                 a["total_net"], json.dumps(a["evidence"]), brief, action))
            conn.commit(); areas_grown += 1
            print("[growth] OVERWORKED: %s avg %.2f ms x%d -> %s"
                  % (a["area_key"], -a["avg_net"], a["sample"], action), flush=True)
    except Exception:
        conn.rollback()
    if not areas_grown:
        print("[growth] no areas to grow yet (need %d+ hands < %.1f bb, or a slow part)" % (MIN_SAMPLE, LOSS_BB), flush=True)
    return areas_grown


def main():
    import psycopg2
    conn = psycopg2.connect(DSN)
    bus.ensure_schema()
    if "--watch" in sys.argv:
        print("[growth] watching the brain's telemetry to grow it (auto-grow=%s)" % AUTO_REWRITE, flush=True)
        while True:
            try:
                run_once(conn)
            except Exception as e:
                conn.rollback(); print("[growth] error:", e, flush=True)
            time.sleep(float(os.environ.get("GROW_EVERY", "900")))     # every 15 min
    else:
        run_once(conn)


if __name__ == "__main__":
    main()
