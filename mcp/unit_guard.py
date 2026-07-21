#!/usr/bin/env python3
r"""unit_guard.py  (runs on windows-beast)

Halt autoplay on any PLAYING hiss.exe instance whose money symbols are in inconsistent units.

THE FAULT THIS CATCHES
----------------------
The phones display money in big blinds (p7bet scrapes "1", c0pot0 scrapes "1.5BB"), so the
tablemap sets bblind_fallback=1.0. If anything pushes DOLLAR/CHIP blinds into
/api/table-game-info (e.g. a vision read or a Claude session posting the tournament stakes line
as sb=150&bb=300 instead of sb=0.5&bb=1.0&chips_per_bb=300), then g_tgi_bblind overrides the
fallback (CBlindGuesser.cpp:61-64, CSymbolEngineTableLimits.cpp:247-251) and every money symbol
is normalised twice:

    nbetsround = maxbet / bblind        (CSymbolEngineHistory.cpp:255-258)
               = 1.0 / 300 = 0.0033

Every OpenPPL "nbetsround >= 1" test then fails, the book concludes "checked to me, I am first
in" on every postflop street, and the bot DONK BETS continuously -- worsening at each blind
level. Diagnosed on A17 2026-07-19. CSymbolEngineValidator.cpp:187 already notices ("preflop
round bets are below the posted blinds") but only warns, so autoplay keeps running.

THE INVARIANT
-------------
The minimum legal bet in no-limit is ONE big blind, so nbetsround -- defined as maxbet/bblind --
is either exactly 0 (nobody has bet) or >= 1. It can be fractional above 1 (a 2.5 BB bet gives
2.5); it can NEVER land strictly between 0 and 1. A value in (0, 1) is arithmetically impossible
and proves the bets and the blind are denominated differently.

This guard only DISENGAGES and reports. It deliberately does not "fix" the blinds, because
guessing the right unit is what caused the fault in the first place.
"""
import json, sys, time, urllib.request

PORTS = range(27654, 27665)
LOG = r"C:\tmp\unit_guard.log"
EPS = 0.99          # nbetsround must be 0 or >= 1; (0, EPS) is impossible
SYMS = "nbetsround,AmountToCall,PotSize,bblind,sblind,tgi_bblind,tgi_chips_per_bb,betround"


def log(msg):
    line = time.strftime("%Y-%m-%d %H:%M:%S  ") + msg
    print(line, flush=True)
    try:
        with open(LOG, "a") as f:
            f.write(line + "\n")
    except OSError:
        pass


def get(port, path, timeout=5):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def check(port):
    """Return True if the instance is healthy (or not applicable), False if autoplay was halted."""
    try:
        state = json.loads(get(port, "/api/table-state", timeout=1.5))
    except Exception:
        return True                                   # not listening -- nothing to guard

    if state.get("observer"):
        return True                                   # observer: engine never acts, dollar blinds are correct

    try:
        s = json.loads(get(port, "/api/symbols?names=" + SYMS))
    except Exception as e:
        log("port %d: could not read symbols (%s)" % (port, e))
        return True

    # Two independent detectors. (A) is always available; (B) only speaks while a bet is live, but
    # is the direct arithmetic proof, so keep both.
    why = None

    # (A) Frame check -- action-independent. Every table in this fleet is the ACR android client,
    # which displays money in big blinds (android_9max / android_s10 / android_EMU all set
    # bblind_fallback=1.0). So a PLAYING instance must operate at bblind ~ 1.0; anything materially
    # larger means chip/dollar blinds were pushed into the BB frame. NOTE: this bound is specific to
    # a big-blind-display fleet -- revisit it if a chip-denominated table is ever added.
    bb = s.get("bblind")
    if bb is not None and bb > 1.5:
        why = ("bblind=%.4f but this is a big-blind-display table (expected ~1.0) -- chip/dollar "
               "blinds were pushed into the BB frame" % bb)

    # (B) Arithmetic check -- minimum legal bet is one big blind, so nbetsround is 0 or >= 1 and can
    # never land strictly between. Only observable while someone has money in.
    n = s.get("nbetsround")
    if n is not None and 0 < n < EPS:
        why = "nbetsround=%.4f is impossible (min legal bet is 1 BB)" % n

    if why is None:
        return True

    log("port %d: UNIT MISMATCH -- %s. "
        "bblind=%s sblind=%s tgi_bblind=%s tgi_chips_per_bb=%s nbetsround=%s AmountToCall=%s PotSize=%s betround=%s"
        % (port, why, s.get("bblind"), s.get("sblind"), s.get("tgi_bblind"),
           s.get("tgi_chips_per_bb"), n, s.get("AmountToCall"), s.get("PotSize"), s.get("betround")))
    log("port %d: table=%r -- the bets and the blind are in different units, so the book will "
        "read every street as 'first in' and donk bet." % (port, state.get("table")))

    try:
        get(port, "/api/autoplayer?on=0")
        log("port %d: AUTOPLAY DISENGAGED. Fix with: "
            "/api/table-game-info?sb=0.5&bb=1.0&chips_per_bb=<real big blind>  "
            "(for a big-blind display), then re-engage." % port)
    except Exception as e:
        log("port %d: FAILED to disengage autoplay (%s) -- INTERVENE MANUALLY" % (port, e))
    return False


def main():
    once = "--once" in sys.argv
    while True:
        bad = [p for p in PORTS if not check(p)]
        if once:
            return 1 if bad else 0
        time.sleep(30)


if __name__ == "__main__":
    sys.exit(main())
