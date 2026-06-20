#!/usr/bin/env python3
"""ultra_mode.py -- ULTRA mode for Hiss.

Flips the bot between OHF (autoplayer) and NN (nn_driver) using the computer's system audio:
  * CHOICE  -- a sound-seeded coin flip, BIASED by loudness: the louder the system audio, the
               more likely NN (the aggressive student); quiet -> OHF.
  * CADENCE -- music / rhythm based: it detects beats (energy onsets) in the system audio and
               switches every --beats-per-switch beats, so busier / faster music switches more
               often. A --min-switch-secs floor stops process thrash; with no music it falls back
               to a slow --decide-secs timer.

Audio comes from the Windows master OUTPUT meter (pycaw IAudioMeterInformation peak), sampled fast
to build an energy envelope for onset detection. Switches hit the bot's mutually-exclusive endpoints
  OHF -> GET /api/autoplayer?on=1      NN -> GET /api/nn-driver?on=1
GAME-TYPE GATE: the NN is trained on Hold'em only, so ULTRA is NLH-ONLY. On a PLO / PLO8 table
(isomaha / isplo8) it never flips to NN and force-holds OHF (the OHF has the Omaha strategy trees).
Launched / killed by the in-Hiss ULTRA toolbar button. Run by hand:
  python ultra_mode.py --bot-url http://127.0.0.1:27654 [--beats-per-switch 16] [--min-switch-secs 12]
  add --dry to log decisions without actually switching (for tuning).
"""
import sys, time, urllib.request
from collections import deque

GOLDEN = 0.6180339887498949


def _argval(flag, default=None):
    if flag in sys.argv:
        i = sys.argv.index(flag)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return default


BOT_URL          = (_argval("--bot-url") or "http://127.0.0.1:27654").rstrip("/")
BEATS_PER_SWITCH = int(_argval("--beats-per-switch") or 16)    # rhythm: switch every N beats
MIN_SWITCH_SECS  = float(_argval("--min-switch-secs") or 12)   # floor between switches (anti-thrash)
DECIDE_SECS      = float(_argval("--decide-secs") or 45)       # no-music fallback cadence
LOUD_GAIN        = float(_argval("--loud-gain") or 2.5)        # loudness -> P(NN) gain
DRY              = "--dry" in sys.argv
GAME_POLL_SECS   = float(_argval("--game-poll-secs") or 3.0)  # re-check the live game-type this often

SAMPLE_HZ    = 60
REFRACTORY_S = 0.25          # min gap between beats (caps at ~240 BPM)
ONSET_FACTOR = 1.35          # energy must exceed the local average * this to count as a beat
ONSET_FLOOR  = 0.02          # ignore "beats" below this absolute energy (silence)
ENV_LEN      = SAMPLE_HZ     # ~1s energy history for the adaptive threshold


def get_meter():
    from pycaw.pycaw import AudioUtilities, IAudioMeterInformation
    from comtypes import CLSCTX_ALL, cast, POINTER
    dev = AudioUtilities.GetSpeakers()._dev   # raw IMMDevice (the wrapper has no Activate)
    iface = dev.Activate(IAudioMeterInformation._iid_, CLSCTX_ALL, None)
    return cast(iface, POINTER(IAudioMeterInformation))


def peak(meter):
    try:
        return float(meter.GetPeakValue())
    except Exception:
        return 0.0


def choose(seed, loudness, tick):
    """Sound-seeded coin flip, BIASED by loudness (louder system audio -> more likely NN)."""
    r = (seed * 131.0 + tick * GOLDEN) % 1.0
    p_nn = min(0.92, max(0.08, loudness * LOUD_GAIN))
    return ("nn" if r < p_nn else "ohf"), p_nn


def switch(mode):
    if DRY:
        print("[ultra] (dry) would switch -> %s" % mode, flush=True)
        return True
    path = "/api/autoplayer?on=1" if mode == "ohf" else "/api/nn-driver?on=1"
    try:
        urllib.request.urlopen(BOT_URL + path, timeout=5).read()
        return True
    except Exception as ex:
        print("[ultra] switch -> %s FAILED: %s" % (mode, ex), flush=True)
        return False


def game_is_omaha(prev):
    """True iff the live table is PLO / PLO8 (so the Hold'em-only NN must stay OFF and the autoplayer/OHF
    must drive -- Emrald: "i need autoplayer on for PLO and PLO8"). Reads only the LIGHT engine booleans
    isomaha / isplo8 / ispl via /api/symbols -- NEVER prwin / pt_ (those are heavy and would wedge the
    bot's HTTP thread, see memory api-symbols-crash-risk). ispl (pot-limit) is included because on these
    tables NLH is no-limit and PLO/PLO8 are pot-limit, so ispl catches Omaha even when isomaha lags at
    preflop -- a robust guarantee that ULTRA never flips to NN (autoplayer off) on a pot-limit table.
    On any error keep the previous value (fail-safe)."""
    import json
    try:
        raw = urllib.request.urlopen(BOT_URL + "/api/symbols?names=isomaha,isplo8,ispl", timeout=3).read()
        d = json.loads(raw.decode("utf-8", "replace"))
        return (float(d.get("isomaha", 0)) > 0.0) or (float(d.get("isplo8", 0)) > 0.0) \
            or (float(d.get("ispl", 0)) > 0.0)
    except Exception:
        return prev


def main():
    try:
        meter = get_meter()
    except Exception as ex:
        meter = None
        print("[ultra] audio meter unavailable (%s) -- fallback timer only" % ex, flush=True)
    print("[ultra] ULTRA online -> %s | rhythm cadence: every %d beats (floor %.0fs, fallback %.0fs) | "
          "louder audio biases toward NN (gain %.1f) | NLH-ONLY (NN gated off on PLO/PLO8)%s"
          % (BOT_URL, BEATS_PER_SWITCH, MIN_SWITCH_SECS, DECIDE_SECS, LOUD_GAIN,
             "  [DRY]" if DRY else ""), flush=True)

    env = deque(maxlen=ENV_LEN)
    beat_gaps = deque(maxlen=8)        # recent inter-beat intervals -> rough BPM
    cur = None
    tick = 0
    is_omaha = False                   # live PLO/PLO8 gate state (refreshed every GAME_POLL_SECS)
    last_game_poll = 0.0
    beats = 0
    last_beat = 0.0
    last_switch = time.monotonic()
    loud_sum = 0.0
    loud_n = 0
    dt = 1.0 / SAMPLE_HZ

    while True:
        now = time.monotonic()
        # GAME-TYPE GATE: refresh the live PLO/PLO8 flag on a slow cadence. If the table is Omaha and
        # the bot is on NN (or unknown), revert to OHF IMMEDIATELY -- don't wait for the next beat, so
        # the NN never drives more than ~GAME_POLL_SECS of an Omaha hand. ULTRA is NLH-only for now.
        if now - last_game_poll >= GAME_POLL_SECS:
            is_omaha = game_is_omaha(is_omaha)
            last_game_poll = now
            if is_omaha and cur != "ohf":
                if switch("ohf"):
                    print("[ultra] PLO/PLO8 detected -> force OHF (NN gated off for Omaha)", flush=True)
                    cur = "ohf"
                    last_switch = now
                    beats = 0
        e = peak(meter) if meter else 0.0
        loud_sum += e
        loud_n += 1
        avg = (sum(env) / len(env)) if env else 0.0
        # beat = a local energy onset above the adaptive threshold, past the refractory gap
        if e > ONSET_FLOOR and e > avg * ONSET_FACTOR and (now - last_beat) > REFRACTORY_S:
            beats += 1
            if last_beat:
                beat_gaps.append(now - last_beat)
            last_beat = now
        env.append(e)

        elapsed = now - last_switch
        rhythm_due = (beats >= BEATS_PER_SWITCH and elapsed >= MIN_SWITCH_SECS)
        fallback_due = (elapsed >= DECIDE_SECS)
        if rhythm_due or fallback_due:
            loudness = loud_sum / max(1, loud_n)
            bpm = (60.0 / (sum(beat_gaps) / len(beat_gaps))) if beat_gaps else 0.0
            mode, p_nn = choose(e, loudness, tick)
            tick += 1
            # GATE: ULTRA only flips to NN on NLH. On PLO/PLO8 the NN choice is suppressed -> stay OHF.
            gated = is_omaha and mode == "nn"
            if gated:
                mode = "ohf"
            tag = "NN" if mode == "nn" else "OHF"
            why = ("%d beats ~%.0f BPM" % (beats, bpm)) if rhythm_due else ("no-music %.0fs" % DECIDE_SECS)
            if gated:
                why += " [PLO/PLO8->OHF gated]"
            if mode != cur:
                if switch(mode):
                    print("[ultra] %s | loud=%.3f P(NN)=%.2f -> SWITCH to %s"
                          % (why, loudness, p_nn, tag), flush=True)
                    cur = mode
            else:
                print("[ultra] %s | loud=%.3f P(NN)=%.2f -> stay %s"
                      % (why, loudness, p_nn, tag), flush=True)
            beats = 0
            last_switch = now
            loud_sum = 0.0
            loud_n = 0
        time.sleep(dt)


if __name__ == "__main__":
    main()
