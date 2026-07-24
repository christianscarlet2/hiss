#!/usr/bin/env python3
"""decision_advisor.py -- the ISMYTURN decision advisor (the capstone of the introspection system).

On every rising edge of ismyturn (off the heartbeat, within the action-timer budget) it:
  1. assembles the BRAIN (synapse_map.brain: INTUITION + DECISION PLAN + DECISION + CURRENT DECIDED
     ACTION) + CONTEXT (board / stacks / pot / position / villain HUD + introspection profile) +
     a relevance-ranked recall of SIMILAR PAST SITUATIONS for this villain (decision_memory) +
     (optionally) the live table SCREENSHOT;
  2. asks `claude -p` (headless CLI) for the best questions for THIS spot, reasons over the villain's
     FORKS (check/bet/raise/fold) and how he might EXPLOIT us on each, and whether the current pathway
     is already in the decision plan + considered -- output is an exploit-oriented weighted advice;
  3. ACTS: pushes the advice into the OHF + NN via /api/knob (advice_* + aggro/bluff/persona/conf);
  4. REMEMBERS: writes the situation + decision + pathway + context + advice to decision_memory, so
     future decisions recall similar spots and the bot gets sharper.

Graceful degrade: if claude is slow/unavailable the knobs decay to neutral (~5s freshness in the OHF)
and the bot decides normally. Never blocks the bot. Run as an AIL daemon (ail_toggle).

  python decision_advisor.py [--bot-url http://127.0.0.1:27655] [--once] [--screenshot] [--poll 0.2]
"""
import os, sys, json, time, subprocess, urllib.request, urllib.parse, glob

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import synapse_map   # reuse gather() + brain() (the harmonizer)

DSN = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")
CLAUDE_BIN = os.environ.get("CLAUDE_BIN", "claude")
CLAUDE_TIMEOUT = float(os.environ.get("ADVISOR_CLAUDE_TIMEOUT", "9"))
FRAMES_DIR = os.environ.get("HISS_FRAMES_DIR", r"C:\www\openholdembot_old\Release\logs\frames")


def _argval(flag, d=None):
    return sys.argv[sys.argv.index(flag) + 1] if (flag in sys.argv and sys.argv.index(flag) + 1 < len(sys.argv)) else d


def bot_url():
    u = _argval("--bot-url")
    if u:
        return u.rstrip("/")
    try:
        p = open(r"C:\www\openholdembot_old\Release\logs\terminal_port.txt").read().strip()
        if p.isdigit():
            return "http://127.0.0.1:" + p
    except Exception:
        pass
    return "http://127.0.0.1:27655"


def get(base, path):
    try:
        with urllib.request.urlopen(base + path, timeout=4) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception:
        return {}


def push_knob(base, name, value):
    try:
        urllib.request.urlopen(base + "/api/knob?name=%s&value=%s" % (name, value), timeout=3).read()
    except Exception:
        pass


def newest_screenshot():
    """Best-effort: convert the newest heartbeat frame (.bmp) to a temp PNG claude can read. PIL optional."""
    try:
        bmps = sorted(glob.glob(os.path.join(FRAMES_DIR, "*.bmp")), key=os.path.getmtime)
        if not bmps:
            return None
        from PIL import Image
        out = os.path.join(FRAMES_DIR, "_advisor_frame.png")
        im = Image.open(bmps[-1])
        im.thumbnail((900, 1600))     # cap size so it never blows the latency budget
        im.save(out)
        return out
    except Exception:
        return None


def recall_similar(cur, villain, betround, limit=4):
    """Relevance-ranked recall: this villain's recent spots on this street (similar conditions)."""
    try:
        cur.execute("""SELECT pathway, decided_action->>'action', intuition->>'exploit',
                       decision_plan->>'label', pathway_in_plan
                       FROM decision_memory WHERE villain=%s AND betround=%s
                       ORDER BY id DESC LIMIT %s""", (villain, betround, limit))
        return [dict(pathway=r[0], action=r[1], exploit=r[2], plan=r[3], in_plan=r[4]) for r in cur.fetchall()]
    except Exception:
        return []


PROMPT = """You are an EXPLOIT-FOCUSED poker decision advisor for a heads-up/multiway hand. Your only
goal: the most profitable exploit of THIS villain in THIS spot. Be concise and decisive.

EXPLOITABLE PATTERNS TAKE PRECEDENCE: the engine (OHF/NN) only suggests a default action. When a
confident exploitable pattern backed by INTUITION + DECISION PLAN + DECISION + CONTEXT points elsewhere,
OVERRIDE the engine -- recommend the exploit line even if it differs from the engine's decision.

BRAIN (already harmonized -- intuition + plan + the engine's current decision):
%(brain)s

CONTEXT (live spot):
%(context)s

SIMILAR PAST SITUATIONS vs this villain (recall -- learn from them):
%(recall)s

Do this:
1) Name the 1-2 best QUESTIONS to ask about this spot.
2) Reason over the villain's FORKS (check / bet / raise / fold) and how he might EXPLOIT us on each;
   pick the line most punishing of / robust to those pathways.
3) Say whether the CURRENT pathway is already covered by the decision plan (pathway_in_plan).
4) Output WEIGHTED, exploit-oriented advice.

Output ONLY a JSON object, no prose, with these keys:
{"questions":[...], "fork_read":"...", "pathway_in_plan":true/false,
 "aggro":0..1, "bluff":0..1, "advice_raise":-1..1, "advice_value":-1..1, "advice_bluff":-1..1,
 "advice_fold":-1..1, "persona":-1..6, "confidence":0..1, "rationale":"one line"}"""


def build_prompt(brain, context, recall):
    return PROMPT % {"brain": json.dumps(brain, indent=0)[:2500],
                     "context": json.dumps(context, indent=0)[:1500],
                     "recall": json.dumps(recall, indent=0)[:900]}


FAST_MODEL = os.environ.get("ADVISOR_FAST_MODEL", "haiku")    # routine spots: fast, on the CLI plan (no API)
DEEP_MODEL = os.environ.get("ADVISOR_DEEP_MODEL", "sonnet")   # big/complex spots only


def pick_model(brain, context):
    """Fast model by default (low latency, on the current plan); the deeper model only where it pays:
    big pots / show-of-force / low-confidence reads / multiway."""
    intu = brain.get("intuition", {})
    big = context.get("pot_bb", 0) >= 25 or context.get("stack_bb", 999) <= 15
    if intu.get("show_of_force") or big or (intu.get("known") and intu.get("confidence", 1) < 0.4):
        return DEEP_MODEL
    return FAST_MODEL


def run_claude(prompt, image_path, model=None):
    cmd = [CLAUDE_BIN, "-p", prompt, "--output-format", "json"]
    if model:
        cmd += ["--model", model]
    if image_path:
        cmd += ["--allowedTools", "Read"]
        prompt += "\n\nThe live table screenshot is at: " + image_path + " -- Read it for board/stack/sizing ground-truth."
        cmd[2] = prompt
    try:
        # Silence the Claude Code Stop-hook chime for this headless call -- see the same note in
        # deep_thought.py. This fires on every decision while the bot plays, so without the gate the
        # speakers chime continuously for calls nobody is waiting on.
        _env = dict(os.environ); _env["HISS_NO_CHIME"] = "1"
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=CLAUDE_TIMEOUT, env=_env,
                           creationflags=(0x08000000 if os.name == "nt" else 0))
        env = json.loads(r.stdout)
        text = env.get("result", r.stdout) if isinstance(env, dict) else r.stdout
    except Exception as e:
        # stdout may already be the model JSON (non-envelope), or claude failed/timed out
        try:
            text = r.stdout  # type: ignore
        except Exception:
            return None
        if not text:
            print("[advisor] claude error:", e, flush=True)
            return None
    t = text.strip()
    if "```" in t:
        t = t.split("```")[1]
        if t.startswith("json"):
            t = t[4:]
    s, e = t.find("{"), t.rfind("}")
    if s < 0 or e < 0:
        return None
    try:
        return json.loads(t[s:e + 1])
    except Exception:
        return None


def clamp(v, lo, hi, d=0.0):
    try:
        return max(lo, min(hi, float(v)))
    except Exception:
        return d


def apply_advice(base, adv):
    push_knob(base, "advice_raise", clamp(adv.get("advice_raise"), -1, 1))
    push_knob(base, "advice_value", clamp(adv.get("advice_value"), -1, 1))
    push_knob(base, "advice_bluff", clamp(adv.get("advice_bluff"), -1, 1))
    push_knob(base, "advice_fold", clamp(adv.get("advice_fold"), -1, 1))
    push_knob(base, "advice_persona", clamp(adv.get("persona"), -1, 6, -1))
    push_knob(base, "advice_conf", clamp(adv.get("confidence"), 0, 1))
    if adv.get("aggro") is not None:
        push_knob(base, "aggro", clamp(adv.get("aggro"), 0, 1, 0.5))
    if adv.get("bluff") is not None:
        push_knob(base, "bluff", clamp(adv.get("bluff"), 0, 1, 0.5))


def pathway_label(g):
    """A compact label of the CURRENT pathway: street + facing + villain action."""
    s = g["syms"]
    br = int(synapse_map.num(s.get("betround")))
    a2c = synapse_map.num(s.get("AmountToCall"))
    raises = synapse_map.num(s.get("Raises"))
    street = {1: "pre", 2: "flop", 3: "turn", 4: "river"}.get(br, "?")
    facing = "raised" if raises >= 1 and a2c > 0 else ("bet" if a2c > 0 else "checked")
    return "%s/%s" % (street, facing)


def advise(base, conn, want_shot, log=True):
    synapse_map.BOT = base
    g = synapse_map.gather()
    b = synapse_map.brain(g)
    synapse_map.store_brain(b)       # refresh brain_state every pass = continuous refinement
    cur = conn.cursor() if conn else None
    recall = recall_similar(cur, b["villain"], b["betround"]) if (cur and b["villain"]) else []
    context = {"board": g.get("board"), "hero": g.get("hero"), "pathway": pathway_label(g),
               "pot_bb": round(synapse_map.num(g["syms"].get("PotSize")) / (synapse_map.num(g["syms"].get("bblind"), 1) or 1), 1),
               "to_call_bb": round(synapse_map.num(g["syms"].get("AmountToCall")) / (synapse_map.num(g["syms"].get("bblind"), 1) or 1), 1),
               "stack_bb": round(synapse_map.num(g["syms"].get("StackSize")) / (synapse_map.num(g["syms"].get("bblind"), 1) or 1), 1),
               "in_position": synapse_map.num(g["syms"].get("f$InPositionPost")) > 0,
               "hud": g.get("hud")}
    shot = newest_screenshot() if want_shot else None
    adv = run_claude(build_prompt(b, context, recall), shot, pick_model(b, context))
    pathway = context["pathway"]
    if adv:
        apply_advice(base, adv)
        print("[advisor] %s vs %s -> aggro=%.2f raise=%.2f persona=%s conf=%.2f | %s"
              % (pathway, b["villain"] or "?", clamp(adv.get("aggro"), 0, 1, 0.5),
                 clamp(adv.get("advice_raise"), -1, 1), adv.get("persona"),
                 clamp(adv.get("confidence"), 0, 1), str(adv.get("rationale"))[:80]), flush=True)
    # DONK-FEST -> get in CHEAP: widen our range via the openrange knob (a donk-heavy table won't punish
    # a cheap entry, and we bet heavy for value postflop when we connect). Safe: the knob only widens range.
    if b.get("intuition", {}).get("donkfest"):
        push_knob(base, "openrange", 0.75)
    # REMEMBER the situation -> decision_memory (recall fuel), once per decision (first pass only)
    if cur and log:
        try:
            cur.execute("""INSERT INTO decision_memory (ts_ms, handnumber, betround, villain, intuition,
                decision_plan, pathway, context, advice, decided_action, pathway_in_plan)
                VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)""",
                (b["ts_ms"], b["handnumber"], b["betround"], b["villain"],
                 json.dumps(b["intuition"]), json.dumps(b["decision_plan"]), pathway,
                 json.dumps(context), json.dumps(adv or {}), json.dumps(b["current_decided_action"]),
                 bool(adv.get("pathway_in_plan")) if adv else None))
            conn.commit()
        except Exception as e:
            conn.rollback(); print("[advisor] memory error:", e, flush=True)
    return adv


def ensure_schema(conn):
    cur = conn.cursor()
    cur.execute("""CREATE TABLE IF NOT EXISTS decision_memory (
        id bigserial primary key, ts_ms bigint, handnumber text, betround int, villain text,
        intuition jsonb, decision_plan jsonb, pathway text, context jsonb, advice jsonb,
        decided_action jsonb, pathway_in_plan bool)""")
    cur.execute("CREATE INDEX IF NOT EXISTS idx_decmem_villain ON decision_memory(villain, betround, id DESC)")
    conn.commit()


def main():
    base = bot_url()
    want_shot = "--screenshot" in sys.argv
    poll = float(_argval("--poll") or 0.2)
    conn = None
    try:
        import psycopg2
        conn = psycopg2.connect(DSN); ensure_schema(conn)
    except Exception as e:
        print("[advisor] no DB (memory/recall disabled):", e, flush=True)
    if "--once" in sys.argv:
        print(json.dumps(advise(base, conn, want_shot) or {}, indent=2)); return
    print("[advisor] watching ismyturn @ %s (fast=%s deep=%s, screenshot=%s)"
          % (base, FAST_MODEL, DEEP_MODEL, want_shot), flush=True)
    last_key = None      # the decision we last FIRED claude on
    last_claude = 0.0    # throttle for the extra claude passes during a turn
    last_brain = 0.0     # continuous between-turn brain refresh
    cached = None        # last advice -> re-pushed so the knobs never decay mid-decision
    last_hn = None       # last hand we ran the introspection look-ahead on
    while True:
        now = time.time()
        st = get(base, "/api/symbols?names=ismyturn,betround")
        my = synapse_map.num(st.get("ismyturn"))
        # handnumber is NOT an OHF symbol (querying it via /api/symbols logged "Unknown identifier:
        # handnumber" against the formula) -> read it from /api/table-state, where it actually lives. [Emrald]
        ts2 = get(base, "/api/table-state") or {}
        hn = ts2.get("handnumber")
        key = (hn, st.get("betround"))
        # INTROSPECTION LOOK-AHEAD (Emrald): the instant a NEW hand starts AND we can see our hole cards, run a
        # brain/introspection pass so the villain reads + plan are ready BEFORE it's our turn to act.
        if hn and hn != last_hn:
            uc = ts2.get("userchair", -1)
            have_hole = any(p.get("chair") == uc and any(c and c != "BACK" for c in (p.get("cards") or []))
                            for p in (ts2.get("players") or []))
            if have_hole:
                last_hn = hn
                synapse_map.BOT = base
                synapse_map.store_brain(synapse_map.brain(synapse_map.gather()))   # look-ahead for the new hand
                last_brain = now
        try:
            if my and my > 0:
                if key != last_key:
                    last_key = key                                  # NEW decision -> FIRE THE BRAIN ASAP
                    cached = advise(base, conn, want_shot, log=True)
                    last_claude = now; last_brain = now
                else:
                    if cached:
                        apply_advice(base, cached)                  # keep the advice fresh through a long decision
                    if now - last_claude >= 2.0:                    # KEEP COMPUTING: as many claude passes as we need
                        cached = advise(base, conn, want_shot, log=False) or cached
                        last_claude = now; last_brain = now
            else:
                last_key = None                                     # next ismyturn fires fresh
                if now - last_brain >= max(poll, 0.5):              # refine the brain as opponents' actions stream in
                    synapse_map.BOT = base
                    synapse_map.store_brain(synapse_map.brain(synapse_map.gather()))
                    last_brain = now
        except Exception as e:
            print("[advisor] loop error:", e, flush=True)
        time.sleep(poll)


if __name__ == "__main__":
    main()
