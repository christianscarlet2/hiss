#!/usr/bin/env python3
"""brain_watch.py -- a LIVE window into the brain.

Tails brain_state (the latest harmonized decision the brain wrote) and prints, every time it changes,
what the brain is READING and DECIDING -- the read, the perception of us, the considerations, the
pineal omen, any deep thought, the wisdom verdict, and the committed action. Watch it think while the
bot plays.

  python brain_watch.py
"""
import os, sys, time

DSN = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")
STREET = {1: "preflop", 2: "flop", 3: "turn", 4: "river"}

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def g(d, *path, default=None):
    for k in path:
        d = d.get(k) if isinstance(d, dict) else None
        if d is None:
            return default
    return d


def main():
    import psycopg2
    c = psycopg2.connect(DSN)
    last = None
    print("[brain_watch] watching the brain on %s ... (Ctrl+C to stop)" % DSN.split()[0], flush=True)
    while True:
        try:
            cur = c.cursor()
            cur.execute("SELECT ts_ms, handnumber, betround, villain, brain FROM brain_state WHERE id=1")
            r = cur.fetchone(); c.commit()
            if r and r[0] != last:
                last = r[0]
                b = r[4] or {}
                intu = b.get("intuition", {}) or {}
                cda = b.get("current_decided_action", {}) or {}
                per = b.get("perception", {}) or {}
                con = b.get("considerations", {}) or {}
                pin = b.get("pineal", {}) or {}
                intel = b.get("intelligence", {}) or {}
                tilt = intu.get("tilt", 0) or 0
                print("\n=== hand %s  %s  |  villain: %s ==="
                      % (r[1] or "-", STREET.get(r[2], "?"), r[3] or "(none)"), flush=True)
                print("  READ      : %s | villain-strength %s | conf %s%s"
                      % (intu.get("exploit"), intu.get("villain_strength"), intu.get("confidence"),
                         (" | TILT %.2f" % tilt) if tilt >= 0.2 else ""))
                if per.get("known"):
                    print("  PERCEIVE  : they see us as %s -> %s" % (per.get("image"), per.get("exploit")))
                print("  CONSIDER  : back[%s] present[%s] forward[plan %s%s]"
                      % (g(con, "backward", "villain_history"), g(con, "present", "exploit"),
                         g(con, "forward", "plan"), " | SHOW OF FORCE" if g(con, "forward", "show_of_force") else ""))
                if (pin.get("resonance") or 0) >= 0.4:
                    print("  PINEAL    : %s (resonance %.2f)" % (pin.get("read"), pin.get("resonance")))
                if b.get("deep_thought"):
                    print("  DEEP      : %s" % str(b["deep_thought"])[:140])
                if intel.get("wise"):
                    print("  WISDOM    : wise (%.2f)" % (intel.get("score") or 0))
                else:
                    print("  WISDOM    : VETO -> %s" % "; ".join(intel.get("concerns") or []))
                print("  >> DECIDE : %s%s  via %s / %s%s"
                      % ((cda.get("action") or "?").upper(),
                         (" %.2fbb" % cda.get("size_bb", 0)) if cda.get("size_bb") else "",
                         cda.get("exploit"), cda.get("plan"),
                         " [VETOED]" if cda.get("intelligence_veto") else
                         ("  <<exploit>>" if (cda.get("source") or "").startswith("exploit") else "")), flush=True)
            time.sleep(0.4)
        except KeyboardInterrupt:
            break
        except Exception:
            try:
                c.rollback()
            except Exception:
                pass
            time.sleep(1.0)


if __name__ == "__main__":
    main()
