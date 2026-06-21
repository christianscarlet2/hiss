"""astrology.py -- ENERGY IN THE AIR for the pineal gland / omen system.

Reads the celestial weather into a single 0..1 ENERGY level: how much charge is in the air right
now. Sources, all deterministic so it always returns something:
  * the MOON  -- synodic phase + illumination (full moon = high charge; new moon = quiet)
  * the DAY RULER     -- classical planet of the weekday (Mars/Sun/Jupiter hot, Saturn heavy)
  * the PLANETARY HOUR -- Chaldean order, day-ruler starts the first hour after sunrise
A claude (or openai) call then REFINES the raw sky into a short reading + a calibrated energy
number; if the model is slow/unavailable we keep the astronomical value. Cached HOURLY (the sky
moves slowly) in the astro_energy table -- the brain reads it every heartbeat for free.

The pineal gland uses ENERGY to decide how hard to EXPLOIT: more energy in the air -> press harder
(exploit wider, prank more); a flat sky -> play it straight.

  energy_in_the_air(now_ms=None, use_llm=True) -> {energy, moon_phase, moon_illum, day_ruler,
        planetary_hour, planet_intensity, reading, source, ts_ms}
"""
import os, sys, json, time, math, subprocess

DSN = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")
CLAUDE_BIN = os.environ.get("CLAUDE_BIN", "claude")
ASTRO_MODEL = os.environ.get("ASTRO_MODEL", "haiku")     # cheap: runs once an hour
REFRESH_MS = int(os.environ.get("ASTRO_REFRESH_MS", str(3600 * 1000)))

# Chaldean order (slowest->fastest) drives the planetary hours; day rulers by weekday (Mon=0..Sun=6)
_CHALDEAN = ["Saturn", "Jupiter", "Mars", "Sun", "Venus", "Mercury", "Moon"]
_DAY_RULER = {0: "Moon", 1: "Mars", 2: "Mercury", 3: "Jupiter", 4: "Venus", 5: "Saturn", 6: "Sun"}
# how much raw CHARGE each planet carries (0..1) -- the malefics/luminaries run hot, Saturn is heavy
_INTENSITY = {"Mars": 1.0, "Sun": 0.9, "Jupiter": 0.8, "Mercury": 0.6,
              "Venus": 0.5, "Moon": 0.55, "Saturn": 0.3}
_PHASE_NAME = ["new", "waxing crescent", "first quarter", "waxing gibbous",
               "full", "waning gibbous", "last quarter", "waning crescent"]

_MEM = {"ts": 0, "payload": None}    # module cache between hourly refreshes


def _moon(now_ms):
    """Synodic phase 0..1 (0/1=new, .5=full) + illumination 0..1, from a known new-moon epoch."""
    # 2000-01-06 18:14 UTC new moon, in unix-ms
    epoch_ms = 947182440000.0
    syn = 29.530588853 * 86400000.0
    phase = ((now_ms - epoch_ms) % syn) / syn
    illum = (1.0 - math.cos(2 * math.pi * phase)) / 2.0
    name = _PHASE_NAME[int((phase * 8 + 0.5)) % 8]
    return phase, illum, name


def _planetary_hour(now_ms):
    """Approximate Chaldean planetary hour. Day=06:00->18:00 local split into 12; the first hour of
    the day is ruled by the day's planet, then Chaldean order cycles. Approximate sunrise is fine for
    an omen."""
    lt = time.localtime(now_ms / 1000.0)
    wd = (lt.tm_wday)                          # Mon=0..Sun=6
    ruler = _DAY_RULER[wd]
    start = _CHALDEAN.index(ruler)
    h = lt.tm_hour + lt.tm_min / 60.0
    # day hours 6..18 (12), night hours 18..30 (12); hour index 0..23 from sunrise
    if h >= 6:
        idx = int((h - 6) / 1.0) if h < 18 else 12 + int((h - 18) / 1.0)
    else:
        idx = 12 + int((h + 6) / 1.0)          # after midnight = late night hours
    planet = _CHALDEAN[(start + idx) % 7]
    return planet


def _astronomical(now_ms):
    phase, illum, name = _moon(now_ms)
    day_ruler = _DAY_RULER[time.localtime(now_ms / 1000.0).tm_wday]
    hour_planet = _planetary_hour(now_ms)
    pint = _INTENSITY.get(hour_planet, 0.6)
    dint = _INTENSITY.get(day_ruler, 0.6)
    # energy: the moon's fullness (charge), the active planetary hour's heat, a touch of the day ruler
    energy = 0.45 * illum + 0.40 * pint + 0.15 * dint
    energy = max(0.0, min(1.0, energy))
    return {"energy": round(energy, 3), "moon_phase": round(phase, 3), "moon_illum": round(illum, 3),
            "moon_name": name, "day_ruler": day_ruler, "planetary_hour": hour_planet,
            "planet_intensity": round(pint, 3),
            "reading": "%s moon (%.0f%% lit), hour of %s, %s's day -- %s charge"
                       % (name, illum * 100, hour_planet, day_ruler,
                          "high" if energy >= 0.66 else ("moderate" if energy >= 0.4 else "low")),
            "source": "astro", "ts_ms": int(now_ms)}


def _refine_with_llm(astro):
    """Ask claude (or openai) to calibrate the energy 0..1 + give a one-line reading. Best-effort; on
    any failure keep the astronomical value."""
    prompt = (
        "You are an astrologer reading the ENERGY IN THE AIR for a poker session. Sky right now: "
        "moon=%s (%.0f%% illuminated, phase %.2f), planetary hour of %s, day ruled by %s. "
        "Return STRICT JSON only: {\"energy\":0..1, \"reading\":\"one vivid sentence\"}. "
        "energy = how much charged, exploitable chaos is in the air (1=electric/full-moon-Mars, "
        "0=flat/heavy-Saturn). No prose outside the JSON."
        % (astro["moon_name"], astro["moon_illum"] * 100, astro["moon_phase"],
           astro["planetary_hour"], astro["day_ruler"]))
    # claude CLI first (uses the plan, no API key) ...
    try:
        r = subprocess.run([CLAUDE_BIN, "-p", prompt, "--model", ASTRO_MODEL, "--output-format", "json"],
                           capture_output=True, text=True, timeout=40,
                           creationflags=(0x08000000 if os.name == "nt" else 0))   # CREATE_NO_WINDOW: no console pop-up
        txt = (r.stdout or "").strip()
        try:
            env = json.loads(txt); txt = env.get("result", txt) if isinstance(env, dict) else txt
        except Exception:
            pass
        obj = _extract_json(txt)
        if obj and "energy" in obj:
            astro = dict(astro)
            astro["energy"] = max(0.0, min(1.0, float(obj["energy"])))
            astro["reading"] = str(obj.get("reading", astro["reading"]))[:200]
            astro["source"] = "claude:" + ASTRO_MODEL
            return astro
    except Exception as e:
        print("[astrology] claude error:", e, flush=True)
    # ... then openai if a key is present
    if os.environ.get("OPENAI_API_KEY"):
        try:
            from openai import OpenAI
            resp = OpenAI().chat.completions.create(
                model=os.environ.get("ASTRO_OPENAI_MODEL", "gpt-4o-mini"), max_tokens=150,
                messages=[{"role": "user", "content": prompt}])
            obj = _extract_json(resp.choices[0].message.content)
            if obj and "energy" in obj:
                astro = dict(astro)
                astro["energy"] = max(0.0, min(1.0, float(obj["energy"])))
                astro["reading"] = str(obj.get("reading", astro["reading"]))[:200]
                astro["source"] = "openai"
        except Exception as e:
            print("[astrology] openai error:", e, flush=True)
    return astro


def _extract_json(txt):
    if not txt:
        return None
    i, j = txt.find("{"), txt.rfind("}")
    if i < 0 or j <= i:
        return None
    try:
        return json.loads(txt[i:j + 1])
    except Exception:
        return None


def _cache_get(now_ms):
    if _MEM["payload"] and now_ms - _MEM["ts"] < REFRESH_MS:
        return _MEM["payload"]
    try:
        import psycopg2
        c = psycopg2.connect(DSN); cur = c.cursor()
        cur.execute("CREATE TABLE IF NOT EXISTS astro_energy (id int primary key, ts_ms bigint, payload jsonb)")
        cur.execute("SELECT ts_ms, payload FROM astro_energy WHERE id=1")
        r = cur.fetchone(); c.commit(); c.close()
        if r and now_ms - r[0] < REFRESH_MS:
            _MEM.update(ts=r[0], payload=r[1])
            return r[1]
    except Exception:
        pass
    return None


def _cache_put(payload):
    _MEM.update(ts=payload["ts_ms"], payload=payload)
    try:
        import psycopg2
        c = psycopg2.connect(DSN); cur = c.cursor()
        cur.execute("CREATE TABLE IF NOT EXISTS astro_energy (id int primary key, ts_ms bigint, payload jsonb)")
        cur.execute("INSERT INTO astro_energy (id,ts_ms,payload) VALUES (1,%s,%s) "
                    "ON CONFLICT (id) DO UPDATE SET ts_ms=EXCLUDED.ts_ms, payload=EXCLUDED.payload",
                    (payload["ts_ms"], json.dumps(payload)))
        c.commit(); c.close()
    except Exception:
        pass


def energy_in_the_air(now_ms=None, use_llm=True):
    now_ms = int(now_ms if now_ms is not None else time.time() * 1000)
    cached = _cache_get(now_ms)
    if cached:
        return cached
    astro = _astronomical(now_ms)
    if use_llm:
        astro = _refine_with_llm(astro)
    _cache_put(astro)
    return astro


if __name__ == "__main__":
    use_llm = "--no-llm" not in sys.argv
    e = energy_in_the_air(use_llm=use_llm)
    print(json.dumps(e, indent=2))
