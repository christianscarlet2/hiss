#!/usr/bin/env python3
"""progression_bard.py -- every 10 hands (~5 per bot across the two phones), summarize the PROGRESSION
and have lilith.exe READ something clever about it aloud.

Watches hand_results (the per-hand net stream the synapse brain writes). When HANDS_PER_SUMMARY new
hands have landed, it gathers the run (net, win rate, biggest pot, stack trend) + the brain's current
read (exploit / observer branch / energy), asks claude for a short, CLEVER, characterful line about how
the session is progressing, and pipes it through Release\\lilith.exe (TTS). [Emrald]

  python progression_bard.py --watch          # daemon
  python progression_bard.py --once --say "..."  # speak a literal line (test lilith)
"""
import os, sys, json, time, subprocess

DSN = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")
LILITH = os.environ.get("HISS_LILITH", r"C:\www\openholdembot_old\Release\lilith.exe")
CLAUDE_BIN = os.environ.get("CLAUDE_BIN", "claude")
CLAUDE_MODEL = os.environ.get("BARD_MODEL", "haiku")     # cheap; runs ~ every 10 hands
HANDS_PER_SUMMARY = int(os.environ.get("BARD_HANDS", "10"))   # ~5 per bot across the two phones
POLL_SEC = float(os.environ.get("BARD_POLL", "15"))

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def speak(text):
    """Pipe the line through lilith.exe (TTS). Best-effort, windowless, non-blocking."""
    if not text:
        return
    try:
        subprocess.Popen([LILITH, text], creationflags=0x08000000)
        print("[bard] lilith: " + text, flush=True)
    except Exception as e:
        print("[bard] lilith error:", e, flush=True)


def _run_claude(prompt):
    try:
        r = subprocess.run([CLAUDE_BIN, "-p", prompt, "--model", CLAUDE_MODEL, "--output-format", "json"],
                           capture_output=True, text=True, timeout=45,
                           creationflags=(0x08000000 if os.name == "nt" else 0))   # CREATE_NO_WINDOW
        txt = (r.stdout or "").strip()
        try:
            env = json.loads(txt)
            if isinstance(env, dict) and "result" in env:
                return str(env["result"]).strip()
        except Exception:
            pass
        return txt
    except Exception as e:
        print("[bard] claude error:", e, flush=True)
        return None


def gather(cur, since_ms):
    """The new run of hands since `since_ms`, plus the brain's current read. Returns (rows, summary, brain)."""
    cur.execute("SELECT handnumber, net, start_balance, end_balance, ts_ms FROM hand_results "
                "WHERE ts_ms > %s ORDER BY ts_ms", (since_ms,))
    rows = cur.fetchall()
    nets = [float(r[1]) for r in rows if r[1] is not None]
    summary = {
        "hands": len(rows),
        "net": round(sum(nets), 2),
        "wins": sum(1 for n in nets if n > 0),
        "losses": sum(1 for n in nets if n < 0),
        "biggest_win": round(max(nets), 2) if nets else 0.0,
        "biggest_loss": round(min(nets), 2) if nets else 0.0,
        "end_stack": round(float(rows[-1][3]), 2) if rows and rows[-1][3] is not None else None,
    }
    # MOMENTUM: are the last few hands trending up or down vs the earlier ones in this run?
    if len(nets) >= 4:
        half = len(nets) // 2
        early, late = sum(nets[:half]), sum(nets[half:])
        summary["momentum"] = ("heating up" if late > early + 0.5 else
                               "cooling off" if late < early - 0.5 else "steady")
    else:
        summary["momentum"] = "steady"
    # PER-PLAYER reads: the recently-active opponents by VPIP / PFR / aggression + their own win/loss.
    players = []
    try:
        cur.execute(
            "SELECT player, vpip_n, vpip_d, pfr_n, aggr_actions, call_actions, hands "
            "FROM hud_player_stats WHERE hands >= 4 AND updated_at > now() - interval '20 min' "
            "AND player NOT ILIKE 'Seat %' ORDER BY updated_at DESC LIMIT 5")
        for pl, vn, vd, pn, aa, ca, h in cur.fetchall():
            vpip = round(100.0 * vn / vd) if vd else None
            pfr = round(100.0 * pn / vd) if vd else None
            af = round(aa / ca, 1) if ca else (aa if aa else 0)
            style = ("nit" if (vpip is not None and vpip < 15) else
                     "TAG" if (vpip is not None and vpip < 28 and (pfr or 0) >= 12) else
                     "station/fish" if (vpip is not None and vpip >= 40 and (af or 0) < 1.5) else
                     "LAG/maniac" if (vpip is not None and vpip >= 35 and (af or 0) >= 2.0) else "reg")
            players.append({"name": pl, "vpip": vpip, "pfr": pfr, "af": af, "style": style, "hands": h})
    except Exception:
        pass
    summary["players"] = players
    brain = {}
    try:
        # brain_state is keyed by Hiss port (one row per instance); the bard narrates the machine as a
        # whole, so it takes the freshest brain rather than the retired shared id=1.
        cur.execute("SELECT brain FROM brain_state ORDER BY ts_ms DESC LIMIT 1")
        b = (cur.fetchone() or [None])[0] or {}
        intu = b.get("intuition", {}) or {}
        obs = b.get("observer_strategy", {}) or {}
        brain = {"exploit": intu.get("exploit"), "branch": obs.get("branch"), "villain": b.get("villain"),
                 "energy": (b.get("pineal", {}) or {}).get("energy")}
    except Exception:
        pass
    return rows, summary, brain


def narrate(summary, brain):
    trend = "up" if summary["net"] > 0 else ("down" if summary["net"] < 0 else "flat")
    pls = summary.get("players", []) or []
    pl_txt = "; ".join("%s VPIP %s%% PFR %s%% AF %s (%s, %dh)"
                       % (p["name"], p["vpip"], p["pfr"], p["af"], p["style"], p["hands"]) for p in pls) or "(no reads yet)"
    prompt = (
        "You are the sly, clever house spirit of a poker bot called Hiss (a red ghost named Jasper). In 2-4 "
        "short SPOKEN sentences, give a wry, sharp read of the session's progression AND a quick characterful "
        "line about EACH opponent by their VPIP / style / momentum -- like a shark sizing up the table out loud. "
        "Not a stat dump; make it clever. Our last %d hands: net %+0.2f (%s), %d wins / %d losses, biggest pot "
        "%+0.2f, worst %+0.2f, stack now %s, momentum %s. The opponents: %s. Brain read: exploit=%s, branch=%s, "
        "energy=%s. Name the players and call out who to hunt and who to dodge. No emojis, no markdown."
        % (summary["hands"], summary["net"], trend, summary["wins"], summary["losses"],
           summary["biggest_win"], summary["biggest_loss"], summary["end_stack"], summary.get("momentum"),
           pl_txt, brain.get("exploit"), brain.get("branch"), brain.get("energy")))
    line = _run_claude(prompt)
    if not line:
        # graceful fallback -- still say something with the player reads
        who = (", ".join("%s is a %s at %s%% VPIP" % (p["name"], p["style"], p["vpip"]) for p in pls[:3])) or "the table is fresh"
        line = ("We're %s %+0.2f over %d hands, %d and %d, momentum %s. %s. I'm reading every one of them."
                % (trend, summary["net"], summary["hands"], summary["wins"], summary["losses"], summary.get("momentum"), who))
    return line[:500]


def main():
    import psycopg2
    if "--once" in sys.argv:
        i = sys.argv.index("--say") if "--say" in sys.argv else -1
        speak(sys.argv[i + 1] if i >= 0 and i + 1 < len(sys.argv) else "Hiss bard online. The cards are listening.")
        return
    c = psycopg2.connect(DSN); cur = c.cursor()
    # start from NOW (don't narrate history); track the ts_ms watermark of the last hand we've counted.
    cur.execute("SELECT coalesce(max(ts_ms),0) FROM hand_results"); wm = cur.fetchone()[0]; c.commit()
    print("[bard] online -- a clever word every %d hands (~5 per bot). watermark=%s" % (HANDS_PER_SUMMARY, wm), flush=True)
    while True:
        try:
            cur.execute("SELECT count(*) FROM hand_results WHERE ts_ms > %s", (wm,))
            n = cur.fetchone()[0]; c.commit()
            if n >= HANDS_PER_SUMMARY:
                rows, summary, brain = gather(cur, wm); c.commit()
                if rows:
                    wm = rows[-1][4]                      # advance the watermark to the last hand summarized
                    line = narrate(summary, brain)
                    speak(line)
            time.sleep(POLL_SEC)
        except KeyboardInterrupt:
            break
        except Exception as e:
            try: c.rollback()
            except Exception: pass
            print("[bard] loop error:", e, flush=True)
            time.sleep(POLL_SEC)


if __name__ == "__main__":
    main()
