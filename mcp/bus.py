#!/usr/bin/env python3
"""bus.py -- the Hiss message bus + IO pipelines.

ONE place for every part to talk to every other part -- Hiss, the brain (synapse_map), the advisor
(decision_advisor), the swiftsnake parallel worker (brain_service/brain_worker), the aggregators, the
NN driver -- over the EXISTING postgres (no new infra; works cross-machine via the primary). Two
primitives:

  PUB/SUB events (low-latency, postgres LISTEN/NOTIFY):
    publish(channel, payload)            -- fire an event ('ismyturn','action','brain_ready','advice_ready')
    subscribe([channels], on_msg, ...)   -- block + dispatch events to on_msg(channel, payload)

  WORK QUEUE (request/response pipeline for the parallel swiftsnake brain):
    enqueue(kind, payload) -> id         -- push a job, NOTIFY 'work'
    claim(kinds, worker) -> (id,kind,payload)|None   -- a worker pulls the next job (SKIP LOCKED)
    complete(id, result)                 -- write the result, NOTIFY 'work_done'
    await_result(id, timeout) -> result|None         -- the requester waits for it

Channels (the IO pipelines between parts):
  hiss.ismyturn / hiss.action / hiss.handreset   Hiss -> advisor/brain   (it's our turn / a villain acted)
  brain.ready                                    brain -> advisor        (a fresh harmonized read is up)
  advice.ready                                   advisor -> Hiss/NN      (new exploit advice pushed)
  work / work_done                               advisor <-> swiftsnake  (parallel pathway-EV dispatch)
"""
import os, json, time, select
import psycopg2
import psycopg2.extensions

DSN = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")


def connect():
    c = psycopg2.connect(DSN)
    c.set_isolation_level(psycopg2.extensions.ISOLATION_LEVEL_AUTOCOMMIT)   # NOTIFY needs autocommit
    return c


def ensure_schema(c=None):
    own = c is None
    if own:
        c = connect()
    cur = c.cursor()
    cur.execute("""
      CREATE TABLE IF NOT EXISTS bus_messages (id bigserial primary key, ts_ms bigint, channel text, payload jsonb);
      CREATE INDEX IF NOT EXISTS idx_busmsg_ch ON bus_messages(channel, id DESC);
      CREATE TABLE IF NOT EXISTS work_queue (id bigserial primary key, ts_ms bigint, kind text,
        payload jsonb, status text DEFAULT 'pending', result jsonb, claimed_by text, done_ms bigint);
      CREATE INDEX IF NOT EXISTS idx_workq_pending ON work_queue(status, id) WHERE status='pending';
    """)
    if own:
        c.close()


def _ident(ch):
    # postgres channel idents: keep them simple (alnum + . _) so LISTEN works without quoting headaches
    return "".join(ch_ for ch_ in ch if ch_.isalnum() or ch_ in "._")


def publish(channel, payload, c=None, log=True):
    own = c is None
    if own:
        c = connect()
    cur = c.cursor()
    body = json.dumps(payload)
    if log:
        cur.execute("INSERT INTO bus_messages (ts_ms, channel, payload) VALUES (%s,%s,%s)",
                    (int(time.time() * 1000), channel, body))
        cur.execute("DELETE FROM bus_messages WHERE id < (SELECT max(id)-20000 FROM bus_messages)")
    cur.execute("SELECT pg_notify(%s, %s)", (_ident(channel), body[:7000]))   # NOTIFY payload cap 8000
    if own:
        c.close()


def subscribe(channels, on_msg, idle=None, stop=None, idle_secs=1.0):
    """Block, LISTEN on channels, dispatch each notification to on_msg(channel, payload)."""
    c = connect()
    cur = c.cursor()
    for ch in channels:
        cur.execute("LISTEN " + _ident(ch))
    while not (stop and stop()):
        if select.select([c], [], [], idle_secs) == ([], [], []):
            if idle:
                idle()
            continue
        c.poll()
        while c.notifies:
            n = c.notifies.pop(0)
            try:
                payload = json.loads(n.payload) if n.payload else {}
            except Exception:
                payload = {"raw": n.payload}
            try:
                on_msg(n.channel, payload)
            except Exception as e:
                print("[bus] on_msg error:", e, flush=True)


def enqueue(kind, payload):
    c = connect()
    cur = c.cursor()
    cur.execute("INSERT INTO work_queue (ts_ms, kind, payload) VALUES (%s,%s,%s) RETURNING id",
                (int(time.time() * 1000), kind, json.dumps(payload)))
    wid = cur.fetchone()[0]
    cur.execute("SELECT pg_notify('work', %s)", (str(wid),))
    c.close()
    return wid


def claim(kinds, worker="w"):
    c = connect()
    cur = c.cursor()
    cur.execute("""UPDATE work_queue SET status='claimed', claimed_by=%s
                   WHERE id = (SELECT id FROM work_queue WHERE status='pending' AND kind = ANY(%s)
                               ORDER BY id FOR UPDATE SKIP LOCKED LIMIT 1)
                   RETURNING id, kind, payload""", (worker, list(kinds)))
    r = cur.fetchone()
    c.close()
    return (r[0], r[1], r[2]) if r else None


def complete(wid, result):
    c = connect()
    cur = c.cursor()
    cur.execute("UPDATE work_queue SET status='done', result=%s, done_ms=%s WHERE id=%s",
                (json.dumps(result), int(time.time() * 1000), wid))
    cur.execute("SELECT pg_notify('work_done', %s)", (str(wid),))
    c.close()


def await_result(wid, timeout=8.0):
    c = connect()
    cur = c.cursor()
    cur.execute("LISTEN work_done")
    deadline = time.time() + timeout
    while time.time() < deadline:
        cur.execute("SELECT status, result FROM work_queue WHERE id=%s", (wid,))
        r = cur.fetchone()
        if r and r[0] == "done":
            c.close()
            return r[1]
        select.select([c], [], [], max(0.05, min(0.5, deadline - time.time())))
        c.poll()
        while c.notifies:
            c.notifies.pop(0)
    c.close()
    return None


if __name__ == "__main__":
    # self-test: enqueue a job, claim+complete it, await the result (round-trips the whole pipeline).
    ensure_schema()
    wid = enqueue("ping", {"hello": "world"})
    job = claim(["ping"], "selftest")
    assert job and job[0] == wid, "claim failed"
    complete(wid, {"pong": job[2]})
    res = await_result(wid, 3.0)
    print("bus round-trip OK ->", res)
