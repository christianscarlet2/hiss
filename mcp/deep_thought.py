#!/usr/bin/env python3
"""deep_thought.py -- ASYNC "deep thought" for the synapse system.

ANY point in the synapse graph can request a DEEP THOUGHT -- an LLM reasoning pass about that node /
spot -- WITHOUT blocking. It's dispatched over the message QUEUE (bus.py) and a worker answers with a
FAST model (claude haiku) or the MAIN model (claude sonnet), or OpenAI as needed. The result lands in
the deep_thoughts table + a 'thought.ready' NOTIFY, and the synapse reads it when ready (non-blocking).

  think(node, context, question, depth='fast'|'deep', provider='claude'|'openai') -> job id (async)
  recent(node, max_age_ms)        -> the latest thought for a node (or None)
  serve()                         -> the worker loop (run on swiftsnake; many copies OK)

  python deep_thought.py --serve          # the async worker
  python deep_thought.py --ask            # smoke test: enqueue one + await it
"""
import os, sys, json, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bus

CLAUDE_BIN = os.environ.get("CLAUDE_BIN", "claude")
FAST_MODEL = os.environ.get("DT_FAST_MODEL", "haiku")
DEEP_MODEL = os.environ.get("DT_DEEP_MODEL", "sonnet")
TIMEOUT = float(os.environ.get("DT_TIMEOUT", "20"))


def ensure_schema(c=None):
    own = c is None
    if own:
        c = bus.connect()
    cur = c.cursor()
    cur.execute("""CREATE TABLE IF NOT EXISTS deep_thoughts (id bigserial primary key, ts_ms bigint,
        node text, provider text, model text, question text, thought text, context jsonb);
        CREATE INDEX IF NOT EXISTS idx_dt_node ON deep_thoughts(node, id DESC);""")
    if own:
        c.close()


def think(node, context, question, depth="fast", provider="claude"):
    """Enqueue a deep thought (ASYNC). Returns the job id; DON'T block on it -- read recent(node) later."""
    return bus.enqueue("deep_thought", {"node": node, "context": context, "question": question,
                                        "depth": depth, "provider": provider})


def recent(node, max_age_ms=None):
    c = bus.connect(); cur = c.cursor()
    cur.execute("SELECT ts_ms, model, thought FROM deep_thoughts WHERE node=%s ORDER BY id DESC LIMIT 1", (node,))
    r = cur.fetchone(); c.close()
    if not r:
        return None
    if max_age_ms and (int(time.time() * 1000) - r[0]) > max_age_ms:
        return None
    return {"ts_ms": r[0], "model": r[1], "thought": r[2]}


def _run_claude(prompt, model):
    try:
        r = subprocess.run([CLAUDE_BIN, "-p", prompt, "--model", model, "--output-format", "json"],
                           capture_output=True, text=True, timeout=TIMEOUT)
        try:
            env = json.loads(r.stdout)
            return env.get("result", r.stdout) if isinstance(env, dict) else r.stdout
        except Exception:
            return r.stdout
    except Exception as e:
        print("[deep_thought] claude error:", e, flush=True)
        return None


def _run_openai(prompt, model):
    try:
        from openai import OpenAI
        resp = OpenAI().chat.completions.create(model=model, max_tokens=400,
                                                messages=[{"role": "user", "content": prompt}])
        return resp.choices[0].message.content
    except Exception:
        return None


def answer(payload):
    p = payload or {}
    depth = p.get("depth", "fast"); provider = p.get("provider", "claude")
    prompt = ("You are the DEEP THOUGHT of a poker brain's synapse system. Reason briefly and sharply "
              "about this point, exploit-first, and give ONE actionable insight in <= 3 sentences.\n"
              "NODE: %s\nQUESTION: %s\nCONTEXT: %s" %
              (p.get("node"), p.get("question"), json.dumps(p.get("context"))[:1800]))
    thought = None
    if provider == "openai":
        model = os.environ.get("DT_OPENAI_DEEP", "gpt-4o") if depth == "deep" else os.environ.get("DT_OPENAI_FAST", "gpt-4o-mini")
        thought = _run_openai(prompt, model)
        if thought is None:
            provider = "claude"        # graceful fallback to claude
    if provider == "claude":
        model = DEEP_MODEL if depth == "deep" else FAST_MODEL
        thought = _run_claude(prompt, model)
    return provider, model, (thought or "").strip()


def serve():
    bus.ensure_schema(); ensure_schema()
    name = "deepthought@" + (os.uname().nodename if hasattr(os, "uname") else os.environ.get("COMPUTERNAME", "win"))
    print("[deep_thought] online as %s -- async LLM reasoning for any synapse point" % name, flush=True)

    def drain():
        while True:
            job = bus.claim(["deep_thought"], name)
            if not job:
                return
            wid, kind, payload = job
            provider, model, thought = answer(payload)
            node = (payload or {}).get("node")
            c = bus.connect(); cur = c.cursor()
            cur.execute("INSERT INTO deep_thoughts (ts_ms,node,provider,model,question,thought,context) "
                        "VALUES (%s,%s,%s,%s,%s,%s,%s)",
                        (int(time.time() * 1000), node, provider, model,
                         (payload or {}).get("question"), thought, json.dumps((payload or {}).get("context"))))
            cur.execute("DELETE FROM deep_thoughts WHERE id < (SELECT max(id)-5000 FROM deep_thoughts)")
            c.commit(); c.close()
            bus.complete(wid, {"provider": provider, "model": model, "thought": thought})
            bus.publish("thought.ready", {"node": node, "thought": thought})
            print("[deep_thought] %s/%s @ %s: %s" % (provider, model, node, (thought or "")[:90]), flush=True)

    drain()
    bus.subscribe(["work"], lambda ch, pl: drain(), idle=drain, idle_secs=2.0)


if __name__ == "__main__":
    if "--serve" in sys.argv:
        serve()
    elif "--ask" in sys.argv:
        ensure_schema()
        wid = think("test.river", {"spot": "river bluff-catch", "villain": "tilting"}, "call or fold one pair?")
        print("enqueued", wid, "-> awaiting (run --serve in another shell)...")
        print("thought:", bus.await_result(wid, 25))
    else:
        ensure_schema(); print("deep_thought: schema ok (use --serve or --ask)")
