#!/usr/bin/env python3
"""ultra_mode.py -- ULTRA mode for Hiss.

Randomly flips the bot between OHF (the autoplayer) and NN (nn_driver.py), using the AVERAGE
LEVEL of the computer's system audio as the entropy source for the coin flip -- the machine's
own sound decides which brain drives.

Every --decide-secs it samples the Windows master OUTPUT meter (pycaw IAudioMeterInformation,
peak 0..1) many times across ~1s and averages them. That noisy average is folded into a
low-discrepancy (golden-ratio) draw in [0,1); >= 0.5 -> NN, else OHF. The fractional fold is
very sensitive to the average (a ~0.008 change flips it), so the sound genuinely drives the
switch; the golden-ratio term keeps it moving even in silence. If the chosen mode differs from
the current one it switches via the bot's HTTP endpoints (which are mutually exclusive):
    OHF -> GET /api/autoplayer?on=1   (also disengages the NN driver)
    NN  -> GET /api/nn-driver?on=1    (also disengages the autoplayer)

Launched/killed by the in-Hiss ULTRA toolbar button (CHeartbeatThread.ApplyUltraEngage), or run
by hand:
    python ultra_mode.py --bot-url http://127.0.0.1:27654 [--decide-secs 45]
"""
import sys, time, urllib.request

GOLDEN = 0.6180339887498949
WINDOW_SECS = 1.0     # how long each averaging window listens to the system audio
SAMPLES = 50          # peak reads per window


def _argval(flag, default=None):
    if flag in sys.argv:
        i = sys.argv.index(flag)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return default


BOT_URL = (_argval("--bot-url") or "http://127.0.0.1:27654").rstrip("/")
DECIDE_SECS = float(_argval("--decide-secs") or 45)


def get_meter():
    """Master render-endpoint audio meter (system output level). None if unavailable."""
    from pycaw.pycaw import AudioUtilities, IAudioMeterInformation
    from comtypes import CLSCTX_ALL, cast, POINTER
    dev = AudioUtilities.GetSpeakers()._dev   # raw IMMDevice (the wrapper has no Activate)
    iface = dev.Activate(IAudioMeterInformation._iid_, CLSCTX_ALL, None)
    return cast(iface, POINTER(IAudioMeterInformation))


def avg_level(meter):
    """Average of SAMPLES master-output peak reads over ~WINDOW_SECS -> [0,1]."""
    if meter is None:
        time.sleep(WINDOW_SECS)
        return 0.0
    vals = []
    dt = WINDOW_SECS / SAMPLES
    for _ in range(SAMPLES):
        try:
            vals.append(float(meter.GetPeakValue()))
        except Exception:
            vals.append(0.0)
        time.sleep(dt)
    return sum(vals) / len(vals) if vals else 0.0


def decide(avg, tick):
    """The noisy system-audio average drives a low-discrepancy coin flip."""
    r = (avg * 131.0 + tick * GOLDEN) % 1.0
    return "nn" if r >= 0.5 else "ohf"


def switch(mode):
    path = "/api/autoplayer?on=1" if mode == "ohf" else "/api/nn-driver?on=1"
    try:
        urllib.request.urlopen(BOT_URL + path, timeout=5).read()
        return True
    except Exception as e:
        print("[ultra] switch -> %s FAILED: %s" % (mode, e), flush=True)
        return False


def main():
    try:
        meter = get_meter()
    except Exception as e:
        meter = None
        print("[ultra] audio meter unavailable (%s) -- running on silence" % e, flush=True)
    print("[ultra] ULTRA mode online -> %s, decide every %.0fs (sound-driven OHF<->NN)"
          % (BOT_URL, DECIDE_SECS), flush=True)
    cur = None
    tick = 0
    while True:
        avg = avg_level(meter)
        mode = decide(avg, tick)
        tick += 1
        tag = "NN" if mode == "nn" else "OHF"
        if mode != cur:
            if switch(mode):
                print("[ultra] avg=%.4f -> SWITCH to %s" % (avg, tag), flush=True)
                cur = mode
        else:
            print("[ultra] avg=%.4f -> stay %s" % (avg, tag), flush=True)
        time.sleep(max(0.0, DECIDE_SECS - WINDOW_SECS))


if __name__ == "__main__":
    main()
