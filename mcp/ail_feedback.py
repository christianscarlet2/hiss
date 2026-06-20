#!/usr/bin/env python3
"""ail_feedback.py -- the AIL's reader + SYNTHESIZER for the spoken feedback loop.

voice_feedback.py records what Emrald says, pins it to the live hand (ts_ms + handnumber), and stores
it in postgres `voice_feedback`. THIS tool is the other half: the Autonomous Improvement Loop (AIL)
calls it each cycle to (1) pull the feedback it hasn't acted on, with replay context, (2) CORRELATE
each note to the money result of the hand it is reacting to -- ESPECIALLY losing hands, which are the
highest-signal "fix this" indicators -- and (3) mark each row applied once handled.

  python ail_feedback.py                 # pending feedback, LOSING-hand notes first, with $ result
  python ail_feedback.py --synthesize    # loss-weighted synthesis: recent losing hands + what you said
  python ail_feedback.py --json
  python ail_feedback.py --all           # include already-applied rows
  python ail_feedback.py --limit 40
  python ail_feedback.py --mark 42[,43] --note "what you did"
  python ail_feedback.py --dismiss 9     # applied=true, note="dismissed (noise/trivial)"

Money result: each note is matched to the hand that COMPLETED at/just-before it (you usually react
AFTER a hand ends, so the reaction pins to the next hand) -- net = collected - invested, parsed from
the ACR hand-history in hiss_log_hands. A note on a hand you LOST is never treated as noise.

  id . NYC time . [CATEGORY sent] . mode . $RESULT . hand/board/hero . transcript . replay pointer
"""
import sys, os, json, re

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

DSN = os.environ.get("HISS_PG_DSN",
                     "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")
REPLAY_UI = os.environ.get("HISS_REPLAY_UI", "http://192.168.1.39/replay.html")
LOSS_WINDOW_MS = 180000   # a note reacts to a hand that finished within this window before it

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
        import datetime
        return datetime.datetime.utcfromtimestamp(int(ms) / 1000.0).strftime("%Y-%m-%d %H:%M:%SZ")
    except Exception:
        return str(ms)


def is_substantive(transcript, category):
    t = (transcript or "").strip()
    words = [w for w in t.replace(",", " ").split() if w.isalpha()]
    if category in ("instruction", "criticism", "question"):
        return True
    if category == "praise" and len(words) >= 2:
        return True
    return len(words) >= 4


# ---- hero net result from an ACR hand-history ----------------------------------------------
def parse_hero_net(hh_text):
    """(hero_name, net) for a complete hand; net = collected - invested (negative = lost).
    Per-street accounting: 'raises X to Y' SETS the street commit to Y; calls/bets/blinds ADD;
    an uncalled bet returned to the hero is refunded."""
    if not hh_text:
        return None, None
    m = re.search(r'Dealt to (.+?) \[', hh_text)
    if not m:
        return None, None
    hero = m.group(1).strip()
    invested = 0.0
    street = 0.0
    collected_body = 0.0
    collected_summary = 0.0
    for raw in hh_text.splitlines():
        line = raw.strip()
        if line.startswith("***") and any(k in line for k in
                                          ("HOLE CARDS", "FLOP", "TURN", "RIVER", "SHOW DOWN", "SUMMARY")):
            invested += street
            street = 0.0
            continue
        sm = re.search(r'Seat \d+: ' + re.escape(hero) + r'\b.*collected \(([\d.]+)\)', line)
        if sm:
            collected_summary = max(collected_summary, float(sm.group(1)))
            continue
        if not line.startswith(hero):
            continue
        mu = re.search(r'Uncalled bet \(([\d.]+)\) returned to ' + re.escape(hero), line)
        if mu:
            street -= float(mu.group(1)); continue
        mb = re.search(r'posts (?:the )?(?:small blind|big blind|ante) ([\d.]+)', line)
        if mb:
            street += float(mb.group(1)); continue
        mr = re.search(r'raises [\d.]+ to ([\d.]+)', line)
        if mr:
            street = float(mr.group(1)); continue
        mc = re.search(r'\bcalls ([\d.]+)', line)
        if mc:
            street += float(mc.group(1)); continue
        mbet = re.search(r'\bbets ([\d.]+)', line)
        if mbet:
            street += float(mbet.group(1)); continue
        mcoll = re.search(r'collected ([\d.]+) from', line)
        if mcoll:
            collected_body += float(mcoll.group(1)); continue
    invested += street
    collected = collected_summary if collected_summary > 0 else collected_body
    return hero, round(collected - invested, 2)


_hand_cache = {}


def _has_hand_results(conn):
    cur = conn.cursor()
    cur.execute("SELECT to_regclass('hand_results')")
    return cur.fetchone()[0] is not None


def hand_net(conn, handnumber):
    if not handnumber:
        return None, None
    if handnumber in _hand_cache:
        return _hand_cache[handnumber]
    cur = conn.cursor()
    # authoritative: the per-hand stack-delta tracker (covers wins AND losses, unlike hiss_log_hands)
    if _has_hand_results(conn):
        cur.execute("SELECT net FROM hand_results WHERE handnumber=%s", (handnumber,))
        row = cur.fetchone()
        if row and row[0] is not None:
            res = (None, round(float(row[0]), 2)); _hand_cache[handnumber] = res; return res
    cur.execute("SELECT hh_text FROM hiss_log_hands WHERE handnumber=%s AND complete "
                "ORDER BY id DESC LIMIT 1", (handnumber,))
    row = cur.fetchone()
    res = parse_hero_net(row[0]) if row else (None, None)
    _hand_cache[handnumber] = res
    return res


def reaction_hand(conn, ts_ms):
    """The hand whose RESULT landed at/just-before this note (what the note is reacting to)."""
    cur = conn.cursor()
    if _has_hand_results(conn):
        cur.execute("SELECT handnumber, net FROM hand_results WHERE ts_ms<=%s AND ts_ms>=%s "
                    "ORDER BY ts_ms DESC LIMIT 1", (ts_ms, ts_ms - LOSS_WINDOW_MS))
        row = cur.fetchone()
        if row:
            return row[0], None, (round(float(row[1]), 2) if row[1] is not None else None)
    cur.execute("SELECT handnumber, hh_text FROM hiss_log_hands WHERE complete AND ts_ms<=%s "
                "AND ts_ms>=%s ORDER BY ts_ms DESC LIMIT 1", (ts_ms, ts_ms - LOSS_WINDOW_MS))
    row = cur.fetchone()
    if not row:
        return None, None, None
    hero, net = parse_hero_net(row[1])
    return row[0], hero, net


def enrich(conn, rows):
    """Attach the money result the note is about: prefer the hand that finished just before it,
    fall back to the note's pinned handnumber. Sets r['result_hand'], r['result_net']."""
    for r in rows:
        rh, _, rnet = reaction_hand(conn, r["ts_ms"])
        if rnet is None:
            rh, rnet = (r["handnumber"], hand_net(conn, r["handnumber"])[1])
        r["result_hand"] = rh
        r["result_net"] = rnet
    return rows


def connect():
    import psycopg2
    return psycopg2.connect(DSN)


def fetch(conn, pending_only=True, limit=60):
    cur = conn.cursor()
    where = "WHERE NOT applied" if pending_only else ""
    cur.execute("SELECT %s FROM voice_feedback %s ORDER BY id DESC LIMIT %%s"
                % (",".join(COLS), where), (limit,))
    return [dict(zip(COLS, row)) for row in cur.fetchall()]


def mark(conn, ids, note):
    cur = conn.cursor()
    cur.execute("UPDATE voice_feedback SET applied=true, applied_note=%s WHERE id = ANY(%s)", (note, ids))
    conn.commit()
    return cur.rowcount


def loss_rank(r):
    """Sort key: losing-hand notes first (biggest loss first), then the rest by recency."""
    net = r.get("result_net")
    lost = (net is not None and net < 0)
    return (0 if lost else 1, net if lost else 0, -r["id"])


def money_tag(net):
    if net is None:
        return "$?"
    if net < -0.001:
        return "*** LOST %.2f ***" % (-net)
    if net > 0.001:
        return "won %.2f" % net
    return "flat"


def render(rows):
    if not rows:
        return "No pending voice feedback. (The loop is caught up.)"
    rows = sorted(rows, key=loss_rank)
    nloss = sum(1 for r in rows if (r.get("result_net") or 0) < 0)
    lines = ["%d row(s) of voice feedback (%d on LOSING hands -> shown first):\n" % (len(rows), nloss)]
    for r in rows:
        net = r.get("result_net")
        lost = (net is not None and net < 0)
        # a note on a hand you lost is signal, never noise
        tag = "" if (lost or is_substantive(r["transcript"], r["category"])) else "  [noise?]"
        applied = "" if not r["applied"] else "  (APPLIED: %s)" % (r["applied_note"] or "")
        sent = r["sentiment"] or 0
        lines.append("#%-4d %s  [%s %+.0f]  mode=%s  %s%s%s"
                     % (r["id"], nyc(r["ts_ms"]), (r["category"] or "note").upper(), sent,
                        r["mode"] or "-", money_tag(net), tag, applied))
        ctx = []
        if r.get("result_hand"):
            ctx.append("hand %s" % r["result_hand"])
        if r["board"]:
            ctx.append("board %s" % r["board"])
        if r["hero_cards"]:
            ctx.append("hero %s" % r["hero_cards"])
        if ctx:
            lines.append("       " + " | ".join(ctx))
        lines.append("       \"%s\"" % (r["transcript"] or "").strip())
        if r.get("result_hand"):
            lines.append("       replay: replay_stream(hand=%s, ts=%s)  [if MCP stale: python mcp/replay_cli.py stream %s %s] | %s"
                         % (r["result_hand"], r["ts_ms"], r["result_hand"], r["ts_ms"], REPLAY_UI))
        lines.append("")
    return "\n".join(lines)


def synthesize(conn, limit_hands=50):
    """Loss-weighted synthesis: recent LOSING hands + the voice notes spoken around each, so the AIL
    focuses its improvement on the spots that actually cost money."""
    cur = conn.cursor()
    losses = []
    nscanned = 0
    if _has_hand_results(conn):
        # authoritative balance-delta tracker -> captures EVERY losing hand (fold, lost showdown, ...)
        cur.execute("SELECT count(*) FROM hand_results"); nscanned = cur.fetchone()[0]
        cur.execute("SELECT handnumber, ts_ms, net FROM hand_results WHERE net < 0 "
                    "ORDER BY ts_ms DESC LIMIT %s", (limit_hands,))
        losses = [(hn, ts, net, None) for hn, ts, net in cur.fetchall()]
    if not losses:
        # fallback to parsing hiss_log_hands (win-biased, but better than nothing pre-tracker)
        cur.execute("SELECT handnumber, ts_ms, hh_text FROM hiss_log_hands WHERE complete "
                    "ORDER BY id DESC LIMIT %s", (limit_hands,))
        hands = cur.fetchall(); nscanned = max(nscanned, len(hands))
        for hn, ts, hh in hands:
            hero, net = parse_hero_net(hh)
            if net is not None and net < 0:
                losses.append((hn, ts, net, hero))
    if not losses:
        return ("No losing hands found in %d tracked hand(s) -- nothing to synthesize. (If the hero is "
                "winning or the result tracker just started, this fills in as hands complete.)" % nscanned)
    out = ["LOSS-WEIGHTED SYNTHESIS -- %d losing hand(s) of %d tracked:\n" % (len(losses), nscanned)]
    total = 0.0
    for hn, ts, net, hero in sorted(losses, key=lambda x: x[2]):
        total += net
        cur.execute("SELECT id, transcript, category FROM voice_feedback WHERE ts_ms BETWEEN %s AND %s "
                    "ORDER BY ts_ms", (ts - 20000, ts + LOSS_WINDOW_MS))
        notes = cur.fetchall()
        out.append("hand %s  LOST %.2f  (hero %s)" % (hn, -net, hero or "?"))
        if notes:
            for nid, tr, cat in notes:
                out.append("    #%d [%s] \"%s\"" % (nid, (cat or "note"), (tr or "").strip()[:90]))
        else:
            out.append("    (no voice note -- still worth a silent replay if the loss was big)")
        out.append("    -> investigate: replay_stream(hand=%s, ts=%s)  [if MCP stale: python mcp/replay_cli.py stream %s %s]" % (hn, ts, hn, ts))
        out.append("")
    out.append("total over these hands: %.2f bb-equiv" % total)
    out.append("\nAIL: for each losing hand WITH a note, treat the note as the leak hypothesis, confirm it")
    out.append("in the replay, and fix it (OHF/knob/calibration). Mark the note applied with what you did.")
    return "\n".join(out)


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
        print("dismissed %d row(s): %s" % (mark(conn, ids, note or "dismissed (noise/trivial)"), ids))
        return

    if mark_arg is not None:
        if not note:
            print("--mark requires --note \"what you did about it\"", file=sys.stderr)
            sys.exit(2)
        ids = [int(x) for x in str(mark_arg).split(",") if x.strip()]
        print("marked %d row(s) applied: %s -> %s" % (mark(conn, ids, note), ids, note))
        return

    if "--synthesize" in sys.argv:
        print(synthesize(conn))
        return

    rows = enrich(conn, fetch(conn, pending_only=pending_only, limit=limit))
    if as_json:
        for r in rows:
            r["created_at"] = str(r["created_at"])
            r["pot"] = float(r["pot"]) if r["pot"] is not None else None
            r["sentiment"] = float(r["sentiment"]) if r["sentiment"] is not None else None
            r["substantive"] = is_substantive(r["transcript"], r["category"])
            r["lost_money"] = (r.get("result_net") is not None and r["result_net"] < 0)
        print(json.dumps(sorted(rows, key=loss_rank), indent=2))
    else:
        print(render(rows))


if __name__ == "__main__":
    main()
