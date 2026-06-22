#!/usr/bin/env python3
"""manic_burst.py -- the Manic Burst daemon for Hiss. [Emrald request]

"If a table is very passive, turn on a manic burst from time to time where I act like a maniac to
wake people up and throw energy in their direction -- then go back to playing small ball by
adjusting the small-ball knob afterwards, maybe the length of a song."

What it does, in one slow loop:
  1. DISCOVER the live Hiss instance: scan ports 27654..27659, GET /api/table-state on each, and
     pick the first one that's SEATED (userchair >= 0). If none is seated, it idles (no burst).
  2. DETECT a VERY PASSIVE table from that instance's /api/table-state. Each players[] entry carries
     a nested hud[] array of {abbr,name,value,important} stats; the aggression factor is abbr "AF"
     (total_af) and the aggression frequency is "AFq". Among seated opponents (not the hero) that
     have a usable HUD sample, if the average AF is low (< MANIC_AF_MAX, default 1.2) the table is
     "very passive". A minimum number of sampled opponents (MANIC_MIN_OPP, default 2) is required so
     one nit doesn't trip it. NO usable HUD => "passive unknown" => DO NOT burst (never burst blind).
  3. FROM TIME TO TIME (not constantly) fire a MANIC BURST when the table is passive AND a cooldown
     (MANIC_COOLDOWN_SEC, default 1080s ~= 18 min, plus up to MANIC_COOLDOWN_JITTER_SEC of randomness
     so it isn't clockwork) has elapsed. A burst:
       * sets the runtime knobs to maniac:  aggro=1.0, bluff~=0.9, openrange=1.0  (POST /api/knob)
       * optionally also /strategy power for the burst (MANIC_USE_POWER, default on)
       * holds for "the length of a song": a randomized 180..240s (MANIC_BURST_MIN/MAX_SEC)
       * then REVERTS to small ball: /strategy smallball + knobs back to a calm baseline
         (aggro~=0.4, bluff~=0.3, openrange~=0.45) so it goes back to playing small ball
       * respects the cooldown before it can burst again.

The runtime knobs ride a pending Hiss rebuild, so /api/knob may 404 on the live binary. This daemon
FAILS SOFT: a non-200 / errored knob POST is logged and the loop keeps running; it starts biting the
moment the rebuilt Hiss is live.

Windowless by design (launch with pythonw.exe): the devnull-stdio guard below stops a windowed build's
None stdout from crashing us, HTTP is pure urllib (no child processes -> no console flash), and the
optional lilith speak uses CREATE_NO_WINDOW. [Emrald: daemons must never pop a console window.]

  python manic_burst.py [--bot-url http://127.0.0.1:27654] [--dry]
  (no --bot-url => auto-discovers the live instance across 27654..27659 every loop)
"""
import os, sys, time, json, random, subprocess, urllib.request, urllib.parse, urllib.error

# pythonw.exe (windowless) / a windowed build sets sys.stdout/stderr to None; back them with devnull
# so any print can't crash the daemon. Same guard as card_oracle.py / ail_server.py.
for _nm in ("stdout", "stderr"):
    if getattr(sys, _nm, None) is None:
        try:
            setattr(sys, _nm, open(os.devnull, "w"))
        except Exception:
            pass

LILITH = r"C:\www\openholdembot_old\Release\lilith.exe"
LOGDIR = r"C:\www\openholdembot_old\Release\logs"
LOGFILE = os.path.join(LOGDIR, "manic_burst.log")

# Ports to scan for a live Hiss instance.
PORT_LO, PORT_HI = 27654, 27659


def _argval(flag, default=None):
    if flag in sys.argv:
        i = sys.argv.index(flag)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return default


def _envf(name, default):
    try:
        return float(os.environ.get(name, default))
    except Exception:
        return float(default)


def _envi(name, default):
    try:
        return int(float(os.environ.get(name, default)))
    except Exception:
        return int(default)


def _envb(name, default):
    v = os.environ.get(name)
    if v is None:
        return bool(default)
    return v.strip().lower() in ("1", "true", "on", "yes", "y")


# --- config (all env-tunable) ----------------------------------------------
FIXED_URL = _argval("--bot-url")                          # if set, skip discovery and use this one
DRY       = "--dry" in sys.argv

POLL_SEC            = _envf("MANIC_POLL_SEC", 15)          # how often we look at the table
AF_MAX              = _envf("MANIC_AF_MAX", 1.2)           # avg opp AF below this => "very passive"
AFQ_MAX             = _envf("MANIC_AFQ_MAX", 35.0)         # OR avg opp AFq% below this => passive (secondary)
MIN_OPP             = _envi("MANIC_MIN_OPP", 2)            # need this many sampled opps to judge
MIN_SAMPLES         = _envi("MANIC_MIN_SAMPLES", 8)        # ignore a seat's HUD below this hands sample

COOLDOWN_SEC        = _envf("MANIC_COOLDOWN_SEC", 1080)    # ~18 min between bursts
COOLDOWN_JITTER_SEC = _envf("MANIC_COOLDOWN_JITTER_SEC", 300)  # +0..5 min randomness (not clockwork)
BURST_MIN_SEC       = _envf("MANIC_BURST_MIN_SEC", 360)    # 6 min lower bound [Emrald: 6-9 min burst]
BURST_MAX_SEC       = _envf("MANIC_BURST_MAX_SEC", 540)    # 9 min upper bound
AIL_BASE            = os.environ.get("AIL_SERVER_URL", "http://127.0.0.1:7900")
USE_POWER           = _envb("MANIC_USE_POWER", True)       # also flip f$Style -> power during the burst

# maniac knob targets
MANIC_AGGRO     = _envf("MANIC_AGGRO", 1.0)
MANIC_BLUFF     = _envf("MANIC_BLUFF", 0.9)
MANIC_OPENRANGE = _envf("MANIC_OPENRANGE", 1.0)
# small-ball / calm baseline to revert to ("go back to playing small ball")
CALM_AGGRO     = _envf("MANIC_CALM_AGGRO", 0.4)
CALM_BLUFF     = _envf("MANIC_CALM_BLUFF", 0.3)
CALM_OPENRANGE = _envf("MANIC_CALM_OPENRANGE", 0.45)


def log(msg):
    line = "%s %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg)
    try:
        os.makedirs(LOGDIR, exist_ok=True)
        with open(LOGFILE, "a", encoding="ascii", errors="replace") as f:
            f.write(line + "\n")
    except Exception:
        pass
    try:
        print(line, flush=True)
    except Exception:
        pass


def http_get_json(url, timeout=4):
    """GET -> parsed JSON dict, or None on any error (caller decides)."""
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception:
        return None


def http_get_ok(url, timeout=5):
    """GET; return (ok, status_or_err). Used for knob/strategy sets -- FAIL SOFT (never raise)."""
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            code = getattr(r, "status", None) or r.getcode()
            r.read()
            return (200 <= int(code) < 300), int(code)
    except urllib.error.HTTPError as e:
        return False, e.code
    except Exception as e:
        return False, str(e)


def speak(text):
    try:
        # CREATE_NO_WINDOW (0x08000000): lilith must never flash a console window. [Emrald]
        subprocess.Popen([LILITH, text], creationflags=0x08000000)
    except Exception as e:
        log("speak failed: %s" % e)


def ail_off():
    """Turn this 'manic' AIL toggle OFF (ail_server :7900) so its indicator shows OFF once the burst is over
    -- one burst per enable. ail_server marks it disabled and stops the daemon. [Emrald: manic auto-off]"""
    try:
        with urllib.request.urlopen(AIL_BASE + "/ail/toggle?name=manic&on=0", timeout=5) as r:
            r.read()
        return True
    except Exception as e:
        log("ail_off failed: %s" % e)
        return False


# --- instance discovery -----------------------------------------------------
def discover_live():
    """Return (base_url, table_state) for the first SEATED (userchair >= 0) live Hiss across the port
    range, else (None, None). A fixed --bot-url is honored (still must be reachable + seated)."""
    if FIXED_URL:
        base = FIXED_URL.rstrip("/")
        ts = http_get_json(base + "/api/table-state")
        if ts is not None and int(ts.get("userchair", -1)) >= 0:
            return base, ts
        return (base, ts) if ts is not None else (None, None)
    for port in range(PORT_LO, PORT_HI + 1):
        base = "http://127.0.0.1:%d" % port
        ts = http_get_json(base + "/api/table-state")
        if ts is None:
            continue
        if int(ts.get("userchair", -1)) >= 0:
            return base, ts
    return None, None


# --- passivity heuristic ----------------------------------------------------
def _stat_value(hud, *abbrs):
    """Pull the first matching HUD stat value as a float, or None. hud = list of {abbr,value,...}.
    Values are strings; non-numeric ('', '-', '—', 'N/A') => None (no usable sample)."""
    want = {a.lower() for a in abbrs}
    for st in (hud or []):
        if not isinstance(st, dict):
            continue
        if str(st.get("abbr", "")).lower() in want:
            raw = str(st.get("value", "")).strip().replace("%", "").replace(",", "")
            if raw in ("", "-", "--", "—", "n/a", "na", "?"):
                return None
            try:
                return float(raw)
            except Exception:
                return None
    return None


def assess_passive(ts):
    """Inspect the table-state. Return (verdict, detail) where verdict is one of:
      'passive'  -> very passive, burst-eligible
      'active'   -> aggressive/normal, no burst
      'unknown'  -> not enough usable HUD sample -> NEVER burst (don't burst blind).
    Heuristic: among seated opponents (not hero) with a usable sample (>= MIN_SAMPLES hands and a
    numeric AF), if we have >= MIN_OPP of them and their average AF < AF_MAX, the table is very
    passive. AFq is a secondary signal: a very low average aggression-frequency also flags passive."""
    hero = int(ts.get("userchair", -1))
    afs, afqs = [], []
    for p in (ts.get("players") or []):
        if not isinstance(p, dict):
            continue
        chair = int(p.get("chair", -1))
        if chair == hero:
            continue
        if not p.get("seated"):
            continue
        samples = p.get("samples", -1)
        try:
            samples = int(samples)
        except Exception:
            samples = -1
        if samples >= 0 and samples < MIN_SAMPLES:
            continue
        hud = p.get("hud")
        af = _stat_value(hud, "AF", "total_af")
        afq = _stat_value(hud, "AFq", "afq", "aggfreq")
        if af is not None:
            afs.append(af)
        if afq is not None:
            afqs.append(afq)
    n = len(afs)
    if n < MIN_OPP and len(afqs) < MIN_OPP:
        return "unknown", "only %d sampled opp(s) (need %d); not bursting blind" % (
            max(n, len(afqs)), MIN_OPP)
    avg_af = (sum(afs) / n) if n else None
    avg_afq = (sum(afqs) / len(afqs)) if afqs else None
    passive_af = (avg_af is not None and n >= MIN_OPP and avg_af < AF_MAX)
    passive_afq = (avg_afq is not None and len(afqs) >= MIN_OPP and avg_afq < AFQ_MAX)
    detail = "opps=%d avgAF=%s (<%.2f?) avgAFq=%s (<%.1f?)" % (
        n,
        ("%.2f" % avg_af) if avg_af is not None else "n/a", AF_MAX,
        ("%.1f" % avg_afq) if avg_afq is not None else "n/a", AFQ_MAX)
    if passive_af or passive_afq:
        return "passive", detail
    return "active", detail


# --- knob / style application (FAIL SOFT) -----------------------------------
def set_knob(base, name, value):
    url = "%s/api/knob?name=%s&value=%.3f" % (base, urllib.parse.quote(name), value)
    if DRY:
        log("(dry) would set knob %s=%.3f" % (name, value))
        return True
    ok, info = http_get_ok(url)
    if not ok:
        log("knob %s=%.3f POST FAILED (%s) -- fail-soft, continuing" % (name, value, info))
    return ok


def set_style(base, style):
    """Set f$Style via the terminal command channel: /api/terminal-input?text=/strategy <style>."""
    text = "/strategy %s" % style
    url = "%s/api/terminal-input?text=%s" % (base, urllib.parse.quote(text, safe=""))
    if DRY:
        log("(dry) would set style -> %s" % style)
        return True
    ok, info = http_get_ok(url)
    if not ok:
        log("style -> %s FAILED (%s) -- fail-soft, continuing" % (style, info))
    return ok


def apply_maniac(base):
    set_knob(base, "aggro", MANIC_AGGRO)
    set_knob(base, "bluff", MANIC_BLUFF)
    set_knob(base, "openrange", MANIC_OPENRANGE)
    if USE_POWER:
        set_style(base, "power")


def apply_smallball(base):
    set_style(base, "smallball")
    set_knob(base, "aggro", CALM_AGGRO)
    set_knob(base, "bluff", CALM_BLUFF)
    set_knob(base, "openrange", CALM_OPENRANGE)


def next_cooldown():
    return COOLDOWN_SEC + random.uniform(0.0, max(0.0, COOLDOWN_JITTER_SEC))


def main():
    log("Manic Burst online%s | scan %d..%d | passive if avgAF<%.2f (>=%d opps, >=%d hands) | "
        "burst aggro=%.2f bluff=%.2f open=%.2f power=%s for %.0f-%.0fs | revert smallball "
        "aggro=%.2f bluff=%.2f open=%.2f | cooldown ~%.0fs(+0..%.0f)"
        % ("  [DRY]" if DRY else "", PORT_LO, PORT_HI, AF_MAX, MIN_OPP, MIN_SAMPLES,
           MANIC_AGGRO, MANIC_BLUFF, MANIC_OPENRANGE, USE_POWER, BURST_MIN_SEC, BURST_MAX_SEC,
           CALM_AGGRO, CALM_BLUFF, CALM_OPENRANGE, COOLDOWN_SEC, COOLDOWN_JITTER_SEC))

    cooldown = next_cooldown()
    last_burst = -1e18          # monotonic time of the last burst start; allow an immediate first one
    idle_logged = False
    last_verdict = None

    while True:
        now = time.monotonic()
        base, ts = discover_live()
        if base is None or ts is None or int(ts.get("userchair", -1)) < 0:
            if not idle_logged:
                log("no live seated Hiss instance found -- idling")
                idle_logged = True
            time.sleep(POLL_SEC)
            continue
        idle_logged = False

        verdict, detail = assess_passive(ts)
        # Only log the verdict when it changes (or periodically) to keep the log concise.
        if verdict != last_verdict:
            log("table @ %s: %s | %s" % (base, verdict.upper(), detail))
            last_verdict = verdict

        ready = (now - last_burst) >= cooldown
        if verdict == "passive" and ready:
            dur = random.uniform(BURST_MIN_SEC, BURST_MAX_SEC)
            log("MANIC BURST start @ %s -- throwing energy at a passive table (%s); hold %.0fs | "
                "knobs aggro=%.2f bluff=%.2f openrange=%.2f%s"
                % (base, detail, dur, MANIC_AGGRO, MANIC_BLUFF, MANIC_OPENRANGE,
                   " +power" if USE_POWER else ""))
            speak("Manic burst. Throwing energy at a passive table.")
            apply_maniac(base)
            # Hold for the length of a song. Sleep in small slices so a Ctrl-break / shutdown is
            # responsive and so we don't wedge if the duration is long.
            burst_end = time.monotonic() + dur
            while time.monotonic() < burst_end:
                time.sleep(min(5.0, burst_end - time.monotonic()))
            # Revert to small ball -- the burst is over, calm back down.
            log("MANIC BURST end @ %s -- reverting to small ball | knobs aggro=%.2f bluff=%.2f "
                "openrange=%.2f, style=smallball" % (base, CALM_AGGRO, CALM_BLUFF, CALM_OPENRANGE))
            speak("Back to small ball.")
            apply_smallball(base)
            # The burst is OVER -> shut this AIL toggle off so the indicator reflects it (one burst per
            # enable; re-toggle it from the AIL tab to arm another). [Emrald: shut off after the burst]
            log("manic burst complete -> turning the 'manic' AIL toggle OFF (indicator off)")
            speak("Manic burst complete. Back to small ball.")
            ail_off()
            return

        time.sleep(POLL_SEC)


if __name__ == "__main__":
    main()
