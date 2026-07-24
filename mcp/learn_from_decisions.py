#!/usr/bin/env python3
r"""learn_from_decisions.py -- turn the human's manual-mode plays into bot improvements.

The loop the user asked for: use manual mode (learner.exe) to AUTOMATICALLY improve hiss.exe, but only
from decisions that were actually good.

  learner.exe logs each manual play to `learner_decisions` WITH the bot's own pick for the same spot
  (ohf_action / ohf_amount, captured live from the OHF's f$ decision functions). This daemon then:

  1. RECONCILE (per completed hand). For every un-reviewed decision, compare the human's action to the
     bot's. If they AGREE, mark it 'agreed' (nothing to learn). If they DIVERGE, go to step 2.

  2. EV+ GATE (claude -p). Ask claude, given the hand history + the spot, whether the HUMAN's line was
     +EV / at least as good as the bot's -- judged ONLY on the information available at decision time,
     NEVER on whether the hand happened to win (that would learn from variance). claude also returns a
     short PATTERN signature for the spot (e.g. "BTN open vs 3bet, 100bb, TT"). Verdict is stored on
     the row.

  3. ACCUMULATE (anti-overfit). One good hand never moves the strategy. Only when the SAME pattern
     signature collects >= MIN_PATTERN EV+ divergences do we act -- so a single unusual spot can't
     reshape the OHF.

  4. PROPOSE (claude -p). For a ripe pattern, claude reads the relevant .strategy_build/strategy/*.ohf
     and proposes a MINIMAL edit (old_snippet -> new_snippet + rationale) that would make the bot play
     these spots the way the human (correctly) did. We check the old_snippet actually exists in the
     file (applicable) and queue the proposal in `ohf_proposals` (status 'pending').

  5. YOU APPROVE. Nothing touches the live strategy automatically (the user chose propose-diffs). An
     approved proposal is applied + validate_ohf'd + reload_ohf'd by the approval step (apply_proposal).

Run:  python learn_from_decisions.py [--once] [--min-pattern 3] [--settle-secs 60] [--poll 45] [--dry]
Graceful: if claude is slow/unavailable it just leaves rows un-reviewed and retries next pass; it never
blocks the bot and never writes the live strategy.
"""
import os, sys, json, time, subprocess, re, shutil, tempfile, urllib.request

REPO       = os.environ.get("HISS_REPO", r"C:\www\openholdembot_old")
STRATEGY   = os.path.join(REPO, ".strategy_build", "strategy")
PSQL       = os.environ.get("HISS_PSQL", r"C:\Program Files\PostgreSQL\12\bin\psql.exe")
PGUSER     = os.environ.get("PGUSER", "postgres")
PGDB       = os.environ.get("PGDATABASE", "hiss")
PGPASS     = os.environ.get("PGPASSWORD", "dbpass")
CLAUDE_BIN = os.environ.get("CLAUDE_BIN", "claude")
CLAUDE_TIMEOUT = float(os.environ.get("LEARN_CLAUDE_TIMEOUT", "90"))

def argval(flag, d=None):
    return sys.argv[sys.argv.index(flag) + 1] if (flag in sys.argv and sys.argv.index(flag) + 1 < len(sys.argv)) else d

ONCE        = "--once" in sys.argv
DRY         = "--dry" in sys.argv                                  # log proposals but never write ohf_proposals-driven applies
MIN_PATTERN = int(argval("--min-pattern", "2"))                   # EV+ divergences of one signature before proposing
SETTLE_SECS = int(argval("--settle-secs", "60"))                 # a decision is "hand over" once this old (let the hand finish)
POLL_SECS   = float(argval("--poll", "45"))


def log(msg):
    print(time.strftime("%H:%M:%S  ") + msg, flush=True)


# ---------------------------------------------------------------- postgres (psql subprocess, like learner.exe)
def _psql(sql, read):
    env = dict(os.environ); env["PGPASSWORD"] = PGPASS
    flags = ["-t", "-A", "-F", "\x1f"] if read else []
    cmd = [PSQL, "-U", PGUSER, "-d", PGDB] + flags + ["-c", sql]
    p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=30,
                       creationflags=0x08000000)  # CREATE_NO_WINDOW
    if p.returncode != 0:
        raise RuntimeError(p.stderr.strip() or p.stdout.strip())
    return p.stdout

def q(sql):
    """SELECT -> list of rows, each a list of unit-separator-split fields."""
    out = _psql(sql, read=True)
    return [line.split("\x1f") for line in out.splitlines() if line != ""]

def qw(sql):
    _psql(sql, read=False)

def esc(s):
    return ("" if s is None else str(s)).replace("'", "''")


# ---------------------------------------------------------------- claude -p (headless), same shape as decision_advisor
def run_claude(prompt):
    cmd = [CLAUDE_BIN, "-p", prompt, "--output-format", "json"]
    env = dict(os.environ)
    env["HISS_NO_CHIME"] = "1"   # this is a headless spawn; don't fire the Stop-hook chime (see memory)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=CLAUDE_TIMEOUT, env=env,
                           creationflags=0x08000000)
    except Exception as e:
        log("claude call failed: %s" % e); return None
    txt = r.stdout or ""
    # claude --output-format json wraps the reply in an envelope {"result": "..."} or similar; the
    # model's own JSON is inside. Be liberal: try envelope.result, then the raw text, then any {...}.
    for candidate in (txt,):
        try:
            env_obj = json.loads(candidate)
            if isinstance(env_obj, dict) and "result" in env_obj:
                candidate = env_obj["result"]
        except Exception:
            pass
        obj = _extract_json(candidate)
        if obj is not None:
            return obj
    log("claude returned no parseable JSON (%d bytes)" % len(txt))
    return None

def _extract_json(t):
    if not t:
        return None
    try:
        return json.loads(t)
    except Exception:
        pass
    s, e = t.find("{"), t.rfind("}")
    if s >= 0 and e > s:
        try:
            return json.loads(t[s:e + 1])
        except Exception:
            return None
    return None


# ---------------------------------------------------------------- reconcile + EV+ gate
def _f(v):
    try: return float(v)
    except Exception: return None

def diverges(human_action, human_amt, bot_action, bot_amt):
    """True if the human's line differs from the bot's in a way worth judging. Different verbs always
    count; same bet/raise verb counts only if the size differs by > 25% (small sizing noise ignored)."""
    ha = (human_action or "").lower(); ba = (bot_action or "").lower()
    # normalise call/check (the learner logs 'call' for the combined Call/Check button)
    if ha in ("call", "check") and ba in ("call", "check"):
        return False
    if ha != ba:
        return True
    if ha in ("bet", "raise", "allin"):
        h, b = _f(human_amt), _f(bot_amt)
        if h and b and b > 0:
            return abs(h - b) / b > 0.25
    return False

EV_PROMPT = """You are a poker strategy reviewer for a No-Limit Hold'em bot (Hiss). A human operator
made a MANUAL play in a spot, and the bot's own strategy (OHF) would have played it differently. Judge
the HUMAN's decision.

CRITICAL: judge decision QUALITY given ONLY the information available at the moment of the decision --
pot odds, equity/ranges, position, stack depth, board texture, opponent tendencies. Do NOT consider
whether the hand happened to win or lose chips; a good fold that would have won is still good, a lucky
loose call is still bad. Result-based reasoning is forbidden.

THE SPOT (JSON):
%s

Return ONLY a JSON object, no prose:
{
  "ev_positive": true|false,          // is the human's line at least as good in EV as the bot's?
  "better_than_bot": true|false,      // is it STRICTLY better than the bot's line?
  "confidence": 0.0-1.0,
  "pattern": "short reusable signature of this spot type, position+action oriented, <=8 words",
  "reason": "one or two sentences, decision-time logic only"
}"""

def reconcile():
    # A decision is ready to review once its hand is over. Heuristic: older than SETTLE_SECS AND not the
    # currently-live hand for its instance. SETTLE_SECS alone is enough in practice (hands end quickly).
    rows = q("SELECT id, handnumber, betround, hero_cards, board, pot, amount_to_call, action, amount, "
             "reasoning, ohf_action, ohf_amount, COALESCE(game_state::text,'{}'), COALESCE(source,'ohf') "
             "FROM learner_decisions "
             "WHERE reviewed IS NOT TRUE AND ts < now() - interval '%d seconds' "
             "ORDER BY id ASC LIMIT 25" % SETTLE_SECS)
    for r in rows:
        (did, hand, betround, cards, board, pot, to_call, action, amount,
         reasoning, ohf_action, ohf_amount, gstate, source) = (r + [None] * 14)[:14]
        if not diverges(action, amount, ohf_action, ohf_amount):
            qw("UPDATE learner_decisions SET reviewed=TRUE, verdict='agreed' WHERE id=%s" % did)
            log("decision %s: agreed with bot (%s) -- nothing to learn" % (did, action))
            continue
        if not ohf_action:
            # the bot's pick wasn't captured (spot read failed) -> can't reconcile; mark reviewed=skip
            qw("UPDATE learner_decisions SET reviewed=TRUE, verdict='no-bot-read' WHERE id=%s" % did)
            log("decision %s: no bot read captured -- skipping" % did)
            continue
        spot = {
            "hand": hand, "street_betround": betround, "hero_cards": cards, "board": board,
            "pot": pot, "amount_to_call": to_call,
            "human_action": action, "human_amount": amount, "human_reasoning": reasoning,
            "bot_action": ohf_action, "bot_amount": ohf_amount,
        }
        try:
            spot["game_state"] = json.loads(gstate)
        except Exception:
            pass
        verdict = run_claude(EV_PROMPT % json.dumps(spot, indent=2))
        if verdict is None:
            log("decision %s: claude unavailable, will retry next pass" % did)
            continue   # leave un-reviewed
        vj = esc(json.dumps(verdict))
        qw("UPDATE learner_decisions SET reviewed=TRUE, verdict='%s' WHERE id=%s" % (vj, did))
        evp = bool(verdict.get("ev_positive"))
        log("decision %s: %s [src=%s] (human=%s bot=%s) EV+=%s pattern=%r"
            % (did, "EV+ DIVERGENCE" if evp else "not EV+", source, action, ohf_action,
               evp, verdict.get("pattern")))
        # ROUTE by source engine: an EV+ divergence vs the NN can't be fixed by an OHF edit (the NN
        # bypasses the OHF on NLH), so collect it as an NN retraining example instead. OHF divergences
        # fall through to ripe_patterns() -> OHF proposals.
        if evp and source == "nn":
            collect_nn_example(did, cards, board, pot, to_call, action, amount,
                               ohf_action, ohf_amount, verdict, gstate)


# ---------------------------------------------------------------- NN retraining dataset
# An EV+ human divergence vs the NN can't be fixed by editing the OHF (the NN bypasses it on NLH), so it
# becomes a supervised/preference training example -- the human's line is the target -- for the offline
# PPO/CFR retrain on swiftsnake. NOT a live one-click apply (the NN is weights, not rules).
def _numnull(v):
    try:
        return str(float(v))
    except Exception:
        return "NULL"

def collect_nn_example(did, cards, board, pot, to_call, human_action, human_amount, nn_action, nn_amount, verdict, gstate):
    try:
        gs = json.dumps(json.loads(gstate)) if gstate else "{}"
    except Exception:
        gs = "{}"
    ha = ("'%s'" % esc(human_action)) if human_action else "NULL"
    na = ("'%s'" % esc(nn_action)) if nn_action else "NULL"
    qw("INSERT INTO nn_training_examples "
       "(decision_id, hero_cards, board, pot, amount_to_call, preferred_action, preferred_amount, "
       "nn_action, nn_amount, pattern, verdict, game_state) VALUES "
       "(%d, '%s','%s',%s,%s,%s,%s,%s,%s,'%s','%s'::jsonb,'%s'::jsonb) "
       "ON CONFLICT (decision_id) DO NOTHING"
       % (int(did), esc(cards or ""), esc(board or ""), _numnull(pot), _numnull(to_call),
          ha, _numnull(human_amount), na, _numnull(nn_amount),
          esc(norm_sig(verdict.get("pattern"))), esc(json.dumps(verdict)), esc(gs)))

def list_nn_examples(limit=60):
    return q("SELECT id, to_char(created_at,'MM-DD HH24:MI'), COALESCE(hero_cards,''), COALESCE(board,''), "
             "COALESCE(preferred_action,''), COALESCE(nn_action,''), COALESCE(pattern,''), status "
             "FROM nn_training_examples ORDER BY id DESC LIMIT %d" % int(limit))


# ---------------------------------------------------------------- pattern accumulation + proposal
def norm_sig(pattern):
    return re.sub(r"\s+", " ", (pattern or "").strip().lower())[:120]

def ripe_patterns():
    """Signatures with >= MIN_PATTERN EV+ divergences that don't already have a live proposal.
    OHF-source only: NN divergences are routed to nn_training_examples, not OHF proposals."""
    rows = q("SELECT id, verdict FROM learner_decisions "
             "WHERE reviewed IS TRUE AND verdict LIKE '{%%' AND COALESCE(source,'ohf')='ohf' ")
    buckets = {}
    for did, verdict in rows:
        try:
            v = json.loads(verdict)
        except Exception:
            continue
        if not v.get("ev_positive"):
            continue
        sig = norm_sig(v.get("pattern"))
        if not sig:
            continue
        buckets.setdefault(sig, []).append(int(did))
    ripe = {}
    for sig, ids in buckets.items():
        if len(ids) < MIN_PATTERN:
            continue
        existing = q("SELECT 1 FROM ohf_proposals WHERE pattern_signature='%s' "
                     "AND status IN ('pending','approved','applied') LIMIT 1" % esc(sig))
        if existing:
            continue
        ripe[sig] = ids
    return ripe

def read_strategy_files():
    out = []
    if os.path.isdir(STRATEGY):
        for fn in sorted(os.listdir(STRATEGY)):
            if fn.endswith(".ohf"):
                p = os.path.join(STRATEGY, fn)
                try:
                    out.append((fn, open(p, encoding="utf-8", errors="replace").read()))
                except Exception:
                    pass
    return out

PROPOSE_PROMPT = """You improve a No-Limit Hold'em bot's OpenPPL strategy (OHF). Across several hands a
human operator repeatedly out-played the bot in the SAME kind of spot, and each of those human decisions
was independently judged +EV (decision-time logic, not results). Propose ONE minimal OHF edit that makes
the bot play this spot the way the human correctly did.

PATTERN: %s

SUPPORTING DECISIONS (spot + human line + bot line + reviewer verdict), JSON:
%s

THE STRATEGY FILES (.strategy_build/strategy/*.ohf), name then content:
%s

Rules:
- Change as little as possible. Prefer editing an existing WHEN/FORCE block over adding new ones.
- Valid OpenPPL only. There is NO '<>' or '!=' operator; use NOT (a = b).
- old_snippet MUST be copied verbatim from one of the files above (so the edit is applicable), and be
  unique within that file.

Return ONLY JSON:
{
  "target_file": "e.g. 40_preflop.ohf",
  "old_snippet": "exact text to replace (verbatim, unique in the file)",
  "new_snippet": "replacement text",
  "rationale": "why this makes the bot match the human's +EV line, 1-3 sentences"
}"""

def decision_brief(did):
    r = q("SELECT hero_cards, board, pot, amount_to_call, action, amount, ohf_action, ohf_amount, "
          "reasoning, verdict FROM learner_decisions WHERE id=%s" % did)
    if not r:
        return None
    c, b, pot, tc, a, amt, oa, oamt, reason, verdict = (r[0] + [None] * 10)[:10]
    try: v = json.loads(verdict)
    except Exception: v = {}
    return {"cards": c, "board": b, "pot": pot, "to_call": tc,
            "human": {"action": a, "amount": amt, "reasoning": reason},
            "bot": {"action": oa, "amount": oamt},
            "reviewer": {"reason": v.get("reason"), "confidence": v.get("confidence")}}

def propose(sig, ids):
    briefs = [b for b in (decision_brief(d) for d in ids[:8]) if b]
    files = read_strategy_files()
    if not files:
        log("no strategy files under %s -- cannot propose" % STRATEGY); return
    files_txt = "\n\n".join("=== %s ===\n%s" % (fn, txt) for fn, txt in files)
    prop = run_claude(PROPOSE_PROMPT % (sig, json.dumps(briefs, indent=2), files_txt))
    if not prop or not prop.get("old_snippet") or not prop.get("target_file"):
        log("pattern %r: claude produced no usable proposal" % sig); return
    tgt = prop["target_file"].strip().lstrip("/\\")
    old = prop.get("old_snippet", "")
    fmap = dict(files)
    validated, vout = False, ""
    if tgt not in fmap:
        vout = "target_file %r not among strategy files" % tgt
    elif old not in fmap[tgt]:
        vout = "old_snippet not found verbatim in %s (not applicable)" % tgt
    elif fmap[tgt].count(old) != 1:
        vout = "old_snippet not unique in %s (%d matches)" % (tgt, fmap[tgt].count(old))
    else:
        validated, vout = True, "applicable: old_snippet found uniquely in %s" % tgt
    arr = "{" + ",".join(str(int(i)) for i in ids) + "}"
    qw("INSERT INTO ohf_proposals "
       "(pattern_signature, supporting_ids, rationale, target_file, old_snippet, new_snippet, validated, validation_output) "
       "VALUES ('%s','%s','%s','%s','%s','%s',%s,'%s')" % (
           esc(sig), arr, esc(prop.get("rationale")), esc(tgt),
           esc(old), esc(prop.get("new_snippet")),
           "TRUE" if validated else "FALSE", esc(vout)))
    log("PROPOSAL queued for %r -> %s  (applicable=%s) [%d supporting]" % (sig, tgt, validated, len(ids)))


# ---------------------------------------------------------------- review + APPLY (approval step)
# The approval is the ONLY path that touches the live strategy. It is deliberately careful, because
# clobbering the live master with an OHF the running binary can't parse pops a Parse Error modal
# mid-session (see memory lint-clobbers-live-master). So: back up the segment, apply the edit, LINT to
# a STAGING master (HISS_MASTER_OUT -> temp, never the live file); only on PASS rebuild the live master
# and reload every running instance; on any failure REVERT and rebuild the live master from the
# reverted segments so the running bot is never left on a half-applied strategy.
BUILD_DIR   = os.path.join(REPO, ".strategy_build")
BACKUP_DIR  = os.path.join(BUILD_DIR, "ohf_backups")

def running_ports():
    ports = []
    for p in range(27654, 27665):
        try:
            urllib.request.urlopen("http://127.0.0.1:%d/api/table-state" % p, timeout=1).read()
            ports.append(p)
        except Exception:
            pass
    return ports

def _run_build(master_out=None):
    env = dict(os.environ)
    if master_out:
        env["HISS_MASTER_OUT"] = master_out
    try:
        r = subprocess.run([sys.executable, "build_and_lint.py"], cwd=BUILD_DIR,
                           capture_output=True, text=True, timeout=150, env=env,
                           creationflags=0x08000000)
        return r.returncode, (r.stdout or "") + (r.stderr or "")
    except Exception as e:
        return 99, "build_and_lint could not run: %s" % e

def list_proposals(status="pending"):
    return q("SELECT id, to_char(created_at,'MM-DD HH24:MI'), pattern_signature, target_file, "
             "validated, COALESCE(array_length(supporting_ids,1),0), status "
             "FROM ohf_proposals WHERE status='%s' ORDER BY id DESC" % esc(status))

def show_proposal(pid):
    r = q("SELECT id, pattern_signature, target_file, old_snippet, new_snippet, rationale, "
          "validated, COALESCE(validation_output,''), status FROM ohf_proposals WHERE id=%d" % int(pid))
    return r[0] if r else None

def reject_proposal(pid):
    qw("UPDATE ohf_proposals SET status='rejected' WHERE id=%d" % int(pid))
    log("proposal %s rejected" % pid); return True, "rejected"

def apply_proposal(pid):
    r = show_proposal(pid)
    if not r:
        return False, "no such proposal %s" % pid
    (_id, sig, target, old, new, rationale, validated, vout, status) = (list(r) + [None] * 9)[:9]
    if status == "applied":
        return False, "proposal %s is already applied" % pid
    if not target or old is None:
        return False, "proposal has no target_file/old_snippet"
    seg = os.path.join(STRATEGY, os.path.basename(target))
    if not os.path.isfile(seg):
        return False, "strategy segment not found: %s" % target
    text = open(seg, encoding="utf-8", errors="replace").read()
    n = text.count(old)
    if n == 0:
        return False, "old_snippet no longer present in %s (segment changed since the proposal)" % target
    if n != 1:
        return False, "old_snippet not unique in %s (%d matches) -- refusing an ambiguous edit" % (target, n)
    os.makedirs(BACKUP_DIR, exist_ok=True)
    bak = os.path.join(BACKUP_DIR, "%s.%s.bak" % (os.path.basename(target), time.strftime("%Y%m%d_%H%M%S")))
    shutil.copy2(seg, bak)
    open(seg, "w", encoding="utf-8").write(text.replace(old, new, 1))
    # 1) LINT to a staging master -- never the live file
    stage = os.path.join(tempfile.gettempdir(), "hiss_ohf_stage_%s.ohf" % pid)
    rc, out = _run_build(master_out=stage)
    if rc != 0:
        shutil.copy2(bak, seg)                                   # revert the segment
        qw("UPDATE ohf_proposals SET validated=FALSE, validation_output='%s' WHERE id=%d"
           % (esc("LINT FAILED (reverted, live strategy untouched):\n" + out[-3500:]), int(pid)))
        return False, "lint FAILED; reverted. Live strategy untouched.\n" + out[-1200:]
    # 2) PASSED -> rebuild the LIVE master (+ hiss-linux sync) and reload every running instance
    rc2, out2 = _run_build()
    if rc2 != 0:
        shutil.copy2(bak, seg); _run_build()                    # revert + rebuild live master clean
        qw("UPDATE ohf_proposals SET validated=FALSE, validation_output='%s' WHERE id=%d"
           % (esc("deploy build failed (reverted):\n" + out2[-3500:]), int(pid)))
        return False, "deploy build failed; reverted."
    reloaded = []
    for p in running_ports():
        try:
            urllib.request.urlopen("http://127.0.0.1:%d/api/reload-ohf" % p, timeout=5).read()
            reloaded.append(p)
        except Exception:
            pass
    qw("UPDATE ohf_proposals SET status='applied', validated=TRUE, validation_output='%s' WHERE id=%d"
       % (esc("APPLIED to %s; reloaded ports %s. Rollback backup: %s" % (target, reloaded, bak)), int(pid)))
    log("proposal %s APPLIED -> %s, reloaded %s (backup %s)" % (pid, target, reloaded, bak))
    return True, "Applied to %s and reloaded %s. Backup: %s" % (target, reloaded, bak)


def cli():
    """Review sub-commands (used by the learner.exe viewer and by hand). Returns True if it handled one."""
    if "--list" in sys.argv:
        for r in list_proposals("pending"):
            print(" | ".join(str(x) for x in r))
        return True
    if "--show" in sys.argv:
        r = show_proposal(argval("--show"))
        if not r: print("not found"); return True
        keys = ["id","pattern","target","old_snippet","new_snippet","rationale","validated","validation","status"]
        print(json.dumps(dict(zip(keys, r)), indent=2))
        return True
    if "--apply" in sys.argv:
        ok, msg = apply_proposal(argval("--apply")); print(("OK: " if ok else "FAIL: ") + msg); return True
    if "--reject" in sys.argv:
        ok, msg = reject_proposal(argval("--reject")); print(msg); return True
    return False


# ---------------------------------------------------------------- main
def pass_once():
    reconcile()
    for sig, ids in ripe_patterns().items():
        if DRY:
            log("[dry] would propose for %r (%d EV+ hits)" % (sig, len(ids)))
        else:
            propose(sig, ids)

def main():
    log("learn_from_decisions online  (min-pattern=%d settle=%ds poll=%.0fs%s)"
        % (MIN_PATTERN, SETTLE_SECS, POLL_SECS, "  DRY" if DRY else ""))
    while True:
        try:
            pass_once()
        except Exception as e:
            log("pass error: %s" % e)
        if ONCE:
            return
        time.sleep(POLL_SECS)

if __name__ == "__main__":
    # Review sub-commands (--list/--show/--apply/--reject) run once and exit; otherwise run the loop.
    if not cli():
        main()
