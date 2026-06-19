#!/usr/bin/env python3
"""card_oracle.py -- The 666 Card Oracle for Hiss.

A draw predictor that bends your "catch the next card" odds by the resonance between the system
audio (the SAME music ULTRA listens to) and the Beast's number, 666 -- and by which brain the
music has put in charge (ULTRA's NN vs OHF phase).

Each gaze it:
  1. reads the live draw from the bot: nouts -> the TRUE odds of improving on the next card
     (nouts / unseen cards), only meaningful on the flop (3 board) or turn (4 board).
  2. samples the master output meter and turns the average into a "beast number" in 0..999.
  3. measures how close the music sits to 666:  res666 = 1 at 666, fading to 0 by 333 / 999.
     If the music currently has the NN at the wheel (the machine channels the signal) res is sharpened.
  4. bends the odds:  omened = true * (0.666 + res666)  ->  x0.666 (the omen turns away) up to
     x1.666 (THE BEAST FAVORS YOU).

It DISPLAYS the omened odds beside the true odds. It does NOT change the real cards (it can't) and
does NOT touch the bot's decisions -- a pure divination overlay. --speak voices strong omens via
lilith.exe.

  python card_oracle.py --bot-url http://127.0.0.1:27654 [--every 4] [--speak]
"""
import sys, time, json, subprocess, urllib.request

LILITH = r"C:\www\openholdembot_old\Release\lilith.exe"


def _argval(flag, default=None):
    if flag in sys.argv:
        i = sys.argv.index(flag)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return default


BOT   = (_argval("--bot-url") or "http://127.0.0.1:27654").rstrip("/")
EVERY = float(_argval("--every") or 4)
SPEAK = "--speak" in sys.argv


def get_meter():
    from pycaw.pycaw import AudioUtilities, IAudioMeterInformation
    from comtypes import CLSCTX_ALL, cast, POINTER
    dev = AudioUtilities.GetSpeakers()._dev
    iface = dev.Activate(IAudioMeterInformation._iid_, CLSCTX_ALL, None)
    return cast(iface, POINTER(IAudioMeterInformation))


def avg_level(meter, n=30, win=0.6):
    if meter is None:
        time.sleep(win)
        return 0.0
    vals = []
    dt = win / n
    for _ in range(n):
        try:
            vals.append(float(meter.GetPeakValue()))
        except Exception:
            vals.append(0.0)
        time.sleep(dt)
    return sum(vals) / len(vals) if vals else 0.0


def _get(url):
    try:
        with urllib.request.urlopen(url, timeout=5) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception:
        return {}


def fnum(d, k):
    try:
        return float(d.get(k) or 0)
    except Exception:
        return 0.0


def mode_now():
    if _get(BOT + "/api/nn-driver").get("engaged"):
        return "NN"        # the machine is at the wheel -> channels the signal
    if _get(BOT + "/api/ultra").get("engaged"):
        return "ULTRA"
    return "OHF"


def speak(text):
    try:
        subprocess.Popen([LILITH, text])
    except Exception as e:
        print("[oracle] speak failed:", e, flush=True)


def main():
    try:
        meter = get_meter()
    except Exception as e:
        meter = None
        print("[oracle] no audio meter (%s) -- gazing into silence" % e, flush=True)
    print("[oracle] The 666 Card Oracle awakens -> %s, gazing every %.0fs%s"
          % (BOT, EVERY, "  [voiced]" if SPEAK else ""), flush=True)
    last_spoke = 0.0
    while True:
        sym = _get(BOT + "/api/symbols?names=nouts,prwin,betround,f$HaveFlushDraw,f$HaveStraightDraw,f$HaveComboDraw")
        ts  = _get(BOT + "/api/table-state")
        nouts = fnum(sym, "nouts")
        board = [c for c in (ts.get("commonCards") or []) if c]
        nboard = len(board)
        a = avg_level(meter)
        beast = a * 1000.0
        res = max(0.0, 1.0 - abs(beast - 666.0) / 333.0)   # 1 at 666, 0 by 333 / 999
        mode = mode_now()
        if mode == "NN":
            res = min(1.0, res * 1.11)                      # the machine sharpens the signal

        # A "next card" only exists on the flop (3) or turn (4), and only with a live draw.
        if nouts <= 0 or nboard < 3 or nboard >= 5:
            print("[oracle] %-5s beast=%3.0f res666=%.2f | no live draw to divine (outs=%.0f board=%d)"
                  % (mode, beast, res, nouts, nboard), flush=True)
            time.sleep(max(0.0, EVERY - 0.6))
            continue

        unseen = 52 - 2 - nboard
        true_catch = nouts / unseen
        modulation = 0.666 + res                           # 0.666 .. 1.666
        omened = min(0.99, true_catch * modulation)
        verdict = ("THE BEAST FAVORS YOU" if res >= 0.66 else
                   "the signal is faint"   if res >= 0.33 else
                   "the omen turns away")
        print("[oracle] %-5s outs=%.0f true=%.1f%% | beast=%3.0f res666=%.2f x%.3f -> OMENED %.1f%%  (%s)"
              % (mode, nouts, true_catch * 100, beast, res, modulation, omened * 100, verdict), flush=True)
        if SPEAK and res >= 0.66 and (time.monotonic() - last_spoke) > 30:
            speak("The Beast stirs at six six six. Your draw of %d outs completes at %d percent."
                  % (int(nouts), int(omened * 100)))
            last_spoke = time.monotonic()
        time.sleep(max(0.0, EVERY - 0.6))


if __name__ == "__main__":
    main()
