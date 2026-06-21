#!/usr/bin/env python3
"""brain_service.py -- the swiftsnake BRAIN WORKER service.

Consumes the work_queue (bus.py) and runs the PARALLEL pathway-EV (brain_worker) across swiftsnake's
cores, returning the most profitable line. This is the IO-pipeline endpoint that lets the advisor
offload heavy compute over the message bus. Many copies can run (SKIP LOCKED makes claiming safe), so
the whole machine can chew on the brain. Run on swiftsnake (HISS_PG_DSN -> the primary):

  HISS_PG_DSN="host=192.168.1.137 dbname=hiss user=postgres password=dbpass" python brain_service.py
"""
import os
import bus
import brain_worker


def handle(job):
    wid, kind, payload = job
    p = payload or {}
    if kind == "pathway_eval":
        best, ranked = brain_worker.evaluate_pathways(
            p.get("brain", {}), p.get("prof", {}),
            p.get("pot_bb", 0), p.get("to_call_bb", 0), p.get("equity_hint", 0.45),
            p.get("candidates"))
        bus.complete(wid, {"best": best, "ranked": ranked[:8]})
    else:
        bus.complete(wid, {"error": "unknown kind %r" % kind})


def main():
    bus.ensure_schema()
    name = "brain@" + (os.uname().nodename if hasattr(os, "uname") else os.environ.get("COMPUTERNAME", "win"))
    print("[brain_service] online as %s -- waiting for pathway-eval work" % name, flush=True)

    def drain():
        n = 0
        while True:
            job = bus.claim(["pathway_eval"], name)
            if not job:
                return n
            handle(job); n += 1

    drain()                                   # clear any backlog on startup
    bus.subscribe(["work"], lambda ch, pl: drain(), idle=drain, idle_secs=2.0)


if __name__ == "__main__":
    main()
