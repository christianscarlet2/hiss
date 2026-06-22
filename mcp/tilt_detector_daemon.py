#!/usr/bin/env python3
"""tilt_detector_daemon.py - the always-on tilt watch for the tilt-detector skill (Lilith's voice).

Every ~45s it scores HERO tilt (protect) and OPPONENT tilt (exploit) from the signals the bot exposes:
  HERO:    recent stack drawdown (hand_results), a losing streak, a fresh bad beat (bot_bad_luck), and
           your own NEGATIVE voice feedback (voice_feedback sentiment).
  VILLAIN: a recent big loss + over-aggression (HUD AF) surfaced in `observations`.
When a threshold trips it speaks ONCE (de-bounced) via Release\\lilith.exe, logs a coach_note (so it shows
in learner.exe's panel), and records a tilt_events row. AIL-toggleable. [Emrald]

Run:  python mcp/tilt_detector_daemon.py     (the AIL 'tilt' switch launches/kills it)
"""
import os, sys, json, time, subprocess

BOT     = os.environ.get("HISS_BOT_URL", "http://127.0.0.1:27654")
PSQL    = os.environ.get("HISS_PSQL", r"C:\Program Files\PostgreSQL\12\bin\psql.exe")
PGUSER  = os.environ.get("PGUSER", "postgres")
PGDB    = os.environ.get("PGDATABASE", "hiss")
PGPASS  = os.environ.get("PGPASSWORD", "dbpass")
RELEASE = os.environ.get("HISS_RELEASE", r"C:\www\openholdembot_old\Release")
LILITH  = os.path.join(RELEASE, "lilith.exe")
TICK_S  = 45
REPEAT_COOLDOWN_S = 240          # don't repeat the SAME warning within 4 min
NOWIN = (0x08000000 if os.name == "nt" else 0)


def log(*a): print("[tilt]", *a, file=sys.stderr, flush=True)


def psql(sql):
    env = dict(os.environ); env["PGPASSWORD"] = PGPASS
    try:
        p = subprocess.run([PSQL, "-U", PGUSER, "-d", PGDB, "-t", "-A", "-F", "|", "-c", sql],
                           capture_output=True, text=True, env=env, timeout=30, creationflags=NOWIN)
        return p.stdout.strip() if p.returncode == 0 else ""
    except Exception:
        return ""


def esc(s): return str(s).replace("'", "''")
def num(v, d=0.0):
    try: return float(v)
    except Exception: return d


def speak(text, kind="criticism"):
    log("speak:", text[:80])
    psql("INSERT INTO coach_notes (kind,priority,message,spoken) VALUES ('%s',2,'%s',true);" % (kind, esc(text)))
    try:
        subprocess.run([LILITH, text], cwd=RELEASE, timeout=120, creationflags=NOWIN)
    except Exception as e:
        log("lilith failed:", e)


def record(subject, kind, score, reason):
    psql("INSERT INTO tilt_events (subject,kind,score,reason) VALUES ('%s','%s',%.2f,'%s');"
         % (esc(subject), esc(kind), score, esc(reason)))


def hero_signals():
    """Return (drawdown_bb, lose_streak, bad_beat_recent, neg_voice_recent)."""
    # last 12 hand nets (bb units assumed net is in bb or chips; we use sign + magnitude trend)
    rows = psql("SELECT net FROM hand_results ORDER BY ts_ms DESC LIMIT 12;")
    nets = [num(x) for x in rows.split("\n") if x.strip() != ""]
    lose_streak = 0
    for n in nets:                      # most-recent first
        if n < 0: lose_streak += 1
        else: break
    drawdown = -sum(nets) if nets and sum(nets) < 0 else 0.0
    bad = psql("SELECT count(*) FROM bot_bad_luck WHERE ts > now() - interval '10 minutes';")
    bad_beat_recent = int(num(bad))
    voice = psql("SELECT count(*) FROM voice_feedback WHERE sentiment IN ('negative','criticism') "
                 "AND ts_ms > (extract(epoch from now())*1000 - 600000);")
    neg_voice = int(num(voice))
    return drawdown, lose_streak, bad_beat_recent, neg_voice


def villain_tilt():
    """A recently-loud villain to exploit: from observations flagged tilt/aggro in the last 15 min."""
    return psql("SELECT coalesce(string_agg(distinct villain,', '),'') FROM observations "
                "WHERE ts > now() - interval '15 minutes' AND (kind ILIKE '%tilt%' OR detail ILIKE '%over-aggress%' "
                "OR detail ILIKE '%steaming%' OR detail ILIKE '%spew%');")


def main():
    log("tilt detector daemon up.")
    last_said = {}    # signature -> ts (de-bounce)

    def maybe(sig, text, subject, kind, score, reason):
        now = time.time()
        if now - last_said.get(sig, 0) < REPEAT_COOLDOWN_S:
            return
        last_said[sig] = now
        speak(text, kind=("instruction" if subject == "villain" else "criticism"))
        record(subject, kind, score, reason)

    while True:
        try:
            drawdown, streak, badbeat, negvoice = hero_signals()
            # HERO tilt — protect
            if negvoice > 0:
                maybe("hero_voice",
                      "Emrald, I heard the frustration. That's the tilt talking. Breathe. The next decision "
                      "is the only one that matters -- make it clean, not angry.",
                      "hero", "voice", 0.8, "negative voice feedback x%d" % negvoice)
            elif streak >= 4:
                maybe("hero_streak",
                      "Four-plus losses in a row. The math doesn't care about the streak, but your patience "
                      "might. Tighten up, fold the marginal, wait for the spot you love.",
                      "hero", "streak", 0.7, "lose streak %d" % streak)
            elif badbeat >= 2:
                maybe("hero_badbeat",
                      "Two bad beats in ten minutes. Clinical detachment now -- you got it in ahead, the river "
                      "betrayed you. Reset your face, reset your range, next hand.",
                      "hero", "badbeat", 0.6, "bad beats %d/10min" % badbeat)
            # VILLAIN tilt — exploit
            v = villain_tilt()
            if v:
                maybe("villain_" + v[:24],
                      "Watch %s -- they're steaming. Value-bet them relentlessly and stop bluffing into the "
                      "calling station. Let the tilt pay you." % v,
                      "villain", "exploit", 0.7, "observations flag: %s" % v)
        except Exception as e:
            log("loop error:", e)
        time.sleep(TICK_S)


if __name__ == "__main__":
    main()
