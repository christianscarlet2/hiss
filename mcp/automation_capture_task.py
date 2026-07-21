#!/usr/bin/env python3
r"""automation_capture_task.py -- the desktop-session half of automation_map.

Windows can only be captured -- or created where a human can see them -- from the session
that owns the desktop. Anything driven over SSH (or by a service) runs in session 0 and
sees no windows at all, so automation_map.py drops a request file and kicks the
"AutomationCapture" scheduled task, which is registered /it (interactive) and therefore
runs right here, on the logged-on desktop.

Request file  C:\tmp\automation_capture.req  is one of:
    <window title>            (legacy 2-line form)
    <output png path>
  or
    capture <title> <out png>
    openmirror <title>

The answer is written to C:\tmp\automation_task.done, which the caller polls.
"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import automation_map  # noqa: E402

REQ = r"C:\tmp\automation_capture.req"
DONE = r"C:\tmp\automation_task.done"
LOG = r"C:\tmp\automation_capture.log"


def log(msg):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(msg.rstrip() + "\n")


def answer(msg):
    log(msg)
    with open(DONE, "w", encoding="utf-8") as f:
        f.write(msg)


def main():
    if not os.path.exists(REQ):
        answer("no request file")
        return
    with open(REQ, encoding="utf-8") as f:
        lines = [ln.strip() for ln in f if ln.strip()]
    try:
        if not lines:
            answer("malformed request")
            return

        head = lines[0].split()
        verb = head[0].lower()

        if verb == "position" and len(head) > 1:
            answer(automation_map.position_mirror(head[1], direct=True))
        elif verb == "openmirror" and len(head) > 1:
            # allow_task would recurse -- we ARE the task.
            answer(automation_map.open_mirror(head[1], direct=True))
        elif verb == "capture" and len(head) > 2:
            answer("%s -> %s : %s" % (head[1], head[2],
                                      automation_map.capture(head[1], head[2], allow_task=False)))
        elif len(lines) >= 2:                      # legacy: title / out on two lines
            title, out = lines[0], lines[1]
            answer("%s -> %s : %s" % (title, out,
                                      automation_map.capture(title, out, allow_task=False)))
        else:
            answer("malformed request: %r" % lines)
    except SystemExit as e:
        answer("FAILED: %s" % e)
    except Exception as e:                          # never leave the caller polling forever
        answer("FAILED: %s: %s" % (type(e).__name__, e))
    finally:
        try:
            os.remove(REQ)
        except OSError:
            pass


if __name__ == "__main__":
    main()
