#!/usr/bin/env python3
"""ail_feedback.py -- the AIL's reader for the spoken feedback loop.

voice_feedback.py records what Emrald says, pins it to the live hand (ts_ms + handnumber), and
stores it in postgres `voice_feedback` as a replay-aligned indicator. THIS tool is the other half:
the Autonomous Improvement Loop (AIL) calls it each cycle to (1) pull the feedback it hasn't acted
on yet, with the replay context needed to investigate, and (2) mark each row applied once handled,
so feedback is acted on exactly once.

  python ail_feedback.py                      # pending (unapplied) feedback, newest first, as a digest
  python ail_feedback.py --json               # same, machine-readable
  python ail_feedback.py --all                # include already-applied rows
  python ail_feedback.py --limit 40
  python ail_feedback.py --mark 42 --note "raised f$river_bluff_freq; see journal 2026-06-19"
  python ail_feedback.py --mark 42,43,44 --note "..."   # mark several at once
  python ail_feedback.py --dismiss 9          # applied=true, note="dismissed (noise/trivial)"

Each pending row carries everything the AIL needs to act:
  id · NYC time · category · sentiment · bot mode · hand · betround · board · hero · transcript
  + a replay pointer: MCP  replay_stream(hand=<h>, ts=<ts_ms>)  /  replay_frame(hand=<h>, ts=<ts_ms>)
    and the UI at http://192.168.1.39/replay.html (pick the hand; the voice tick sits at this ts).

Noise guard: whisper picks up ambient audio, so trivial blips ("you", "okay", "thank you") are
flagged [noise?]. The AIL should --dismiss those and act only on substantive notes.
"""
import sys, os, json, time

# Transcripts may contain non-cp1252 characters; force UTF-8 so printing the digest never crashes.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

DSN = os.environ.get("HISS_PG_DSN",
                     "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")
REPLAY_UI = os.environ.get("HISS_REPLAY_UI", "http://192.168.1.39/replay.html")

COLS = ["id", "ts_ms", "created_at", "handnumber", "betround", "board", "hero_cards",
        "pot", "mode", "transcript", "category", "sentiment", "applied", "applied_note"]


def _argval(flag, default=None):
    if flag in sys.argv:
        i = sys.argv.index(flag)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return default


def nyc(ms):
    try:
        # epoch ms -> NYC wall clock (no tz lib dependency: use localtime offset note in the label)
        import datetime
        return datetime.datetime.utcfromtimestamp(int(ms) / 1000.0).strftime("%Y-%m-%d %H:%M:%SZ")
    except Exception:
        return str(ms)


def is_substantive(transcript, category):
    """Heuristic: an instruction/criticism/praise/question, or a note with real content,
    is worth the AIL's attention. Short ambient blips are not."""
    t = (transcript or "").strip()
    words = [w for w in t.replace(",", " ").split() if w.isalpha()]
    if category in ("instruction", "criticism", "question"):
        return True
    if category == "praise" and len(words) >= 2:
        return True
    return len(words) >= 4


def connect():
    import psycopg2
    return psycopg2.connect(DSN)


def fetch(conn, pending_only=True, limit=60):
    cur = conn.cursor()
    where = "WHERE NOT applied" if pending_only else ""
    cur.execute("SELECT %s FROM voice_feedback %s ORDER BY id DESC LIMIT %%s"
                % (",".join(COLS), where), (limit,))
    out = []
    for row in cur.fetchall():
        out.append(dict(zip(COLS, row)))
    return out


def mark(conn, ids, note):
    cur = conn.cursor()
    cur.execute("UPDATE voice_feedback SET applied=true, applied_note=%s WHERE id = ANY(%s)",
                (note, ids))
    conn.commit()
    return cur.rowcount


def render(rows):
    if not rows:
        return "No pending voice feedback. (The loop is caught up.)"
    lines = ["%d row(s) of voice feedback:\n" % len(rows)]
    for r in rows:
        tag = "" if is_substantive(r["transcript"], r["category"]) else "  [noise?]"
        applied = "" if not r["applied"] else "  (APPLIED: %s)" % (r["applied_note"] or "")
        sent = r["sentiment"]
        sent_s = ("+%.0f" % sent) if (sent or 0) > 0 else ("%.0f" % (sent or 0))
        lines.append("#%-4d %s  [%s %s]  mode=%s%s%s"
                     % (r["id"], nyc(r["ts_ms"]), (r["category"] or "note").upper(), sent_s,
                        r["mode"] or "-", tag, applied))
        ctx = []
        if r["handnumber"]:
            ctx.append("hand %s" % r["handnumber"])
        if r["betround"]:
            ctx.append("round %s" % r["betround"])
        if r["board"]:
            ctx.append("board %s" % r["board"])
        if r["hero_cards"]:
            ctx.append("hero %s" % r["hero_cards"])
        if r["pot"]:
            ctx.append("pot %s" % r["pot"])
        if ctx:
            lines.append("       " + " | ".join(ctx))
        lines.append("       \"%s\"" % (r["transcript"] or "").strip())
        if r["handnumber"]:
            lines.append("       replay: replay_stream(hand=%s, ts=%s) | %s"
                         % (r["handnumber"], r["ts_ms"], REPLAY_UI))
        lines.append("")
    return "\n".join(lines)


def main():
    note = _argval("--note")
    mark_arg = _argval("--mark")
    dismiss_arg = _argval("--dismiss")
    as_json = "--json" in sys.argv
    pending_only = "--all" not in sys.argv
    limit = int(_argval("--limit") or 60)

    conn = connect()

    if dismiss_arg is not None:
        ids = [int(x) for x in str(dismiss_arg).split(",") if x.strip()]
        n = mark(conn, ids, note or "dismissed (noise/trivial)")
        print("dismissed %d row(s): %s" % (n, ids))
        return

    if mark_arg is not None:
        if not note:
            print("--mark requires --note \"what you did about it\"", file=sys.stderr)
            sys.exit(2)
        ids = [int(x) for x in str(mark_arg).split(",") if x.strip()]
        n = mark(conn, ids, note)
        print("marked %d row(s) applied: %s -> %s" % (n, ids, note))
        return

    rows = fetch(conn, pending_only=pending_only, limit=limit)
    if as_json:
        for r in rows:
            r["created_at"] = str(r["created_at"])
            r["pot"] = float(r["pot"]) if r["pot"] is not None else None
            r["sentiment"] = float(r["sentiment"]) if r["sentiment"] is not None else None
            r["substantive"] = is_substantive(r["transcript"], r["category"])
        print(json.dumps(rows, indent=2))
    else:
        print(render(rows))


if __name__ == "__main__":
    main()
