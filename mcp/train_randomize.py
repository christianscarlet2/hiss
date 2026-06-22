#!/usr/bin/env python3
"""train_randomize.py - DOMAIN RANDOMIZATION for training-data generation. [Emrald 2026-06-21]

When we regenerate self-play data to retrain PPO (and feed M3/M4 distill), we do NOT want every Hiss
bot playing the SAME profile -- that produces a narrow dataset. This daemon turns the Synapse-Harmonizer
knobs RANDOMLY on each bot and flips the game STYLE (smallball / power holdem / hybrid) randomly, RE-ROLLED
PER BOT PER HAND. The result is data that spans the whole strategy space, so the trained net is robust
across styles and can learn style-CONDITIONED play (the chosen style+knobs are logged so they can also
become NN input features -- see hiss-sagemaker/train_ppo.py state vector).

Levers (all no-rebuild, already in the live engine):
  * STYLE   ->  POST /api/terminal-input?text=/strategy smallball|power|hybrid   (sets f$Style 0/1/2)
  * KNOBS   ->  GET  /api/knob?name=openrange|aggro|bluff|cbet&value=0..1        (openai_knob_*)

It targets every live Hiss instance (terminal ports 27654..27664) and re-rolls a bot's profile the
instant its handnumber advances (true per-hand randomization), with a time fallback so a stuck table
still rotates. Logs each roll to logs/train_randomize.log + the `train_randomize` postgres table.

  python train_randomize.py [--ports 27654,27655] [--every 4] [--max-hold 45]
                            [--uniform-frac 0.5] [--jitter 0.22] [--once] [--dry]

Run this ONLY while generating training data -- it deliberately makes the bots play sub-optimally-diverse.
Stop it (or toggle it off in the AIL tab) before real-money play.
"""
import argparse, json, os, random, sys, time, urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
LOGDIR = os.path.join(os.path.dirname(HERE), "Release", "logs")
LOGFILE = os.path.join(LOGDIR, "train_randomize.log")
DSN = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")

# Style centers for the four knobs (0..1). Per-bot jitter is added on top so each bot explores AROUND
# the style, not exactly at it. smallball = wide+cheap+disciplined; power = aggressive+big+bluffy;
# hybrid = balanced. cbet is the continuation-bet frequency dial.
STYLES = {
    "smallball": {"anchor": 11, "openrange": 0.58, "aggro": 0.42, "bluff": 0.44, "cbet": 0.58},
    "power":     {"anchor": 12, "openrange": 0.62, "aggro": 0.82, "bluff": 0.72, "cbet": 0.78},
    "hybrid":    {"anchor": 13, "openrange": 0.52, "aggro": 0.60, "bluff": 0.56, "cbet": 0.66},
}
KNOBS = ("openrange", "aggro", "bluff", "cbet")


def clamp01(x):
    return 0.0 if x < 0.0 else (1.0 if x > 1.0 else x)


def log(msg):
    line = time.strftime("%Y-%m-%d %H:%M:%S ") + msg
    print(line, flush=True)
    try:
        os.makedirs(LOGDIR, exist_ok=True)
        with open(LOGFILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


def _get(port, path, timeout=3):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception:
        return None


def _post_cmd(port, text, timeout=3):
    try:
        urllib.request.urlopen("http://127.0.0.1:%d/api/terminal-input?text=%s"
                               % (port, urllib.parse.quote(text)), timeout=timeout).read()
        return True
    except Exception:
        return False


def _push_knob(port, name, value, timeout=3):
    try:
        urllib.request.urlopen("http://127.0.0.1:%d/api/knob?name=%s&value=%s"
                               % (port, name, value), timeout=timeout).read()
        return True
    except Exception:
        return False


def live_ports(candidates):
    out = []
    for p in candidates:
        # /api/autoplayer responds on any running instance (bare GET just reports state)
        if _get(p, "/api/autoplayer", timeout=1) is not None:
            out.append(p)
    return out


def handnumber(port):
    ts = _get(port, "/api/table-state", timeout=2) or {}
    for key in ("handnumber", "hand_number", "hand"):
        if key in ts:
            try:
                return int(ts[key])
            except Exception:
                pass
    return None


def roll_profile(uniform_frac, jitter):
    """Pick a style + knob set. Half the time use style-centered knobs + jitter (coherent profiles); the
    other half use fully-uniform knobs (broad coverage of the corners of the space)."""
    style = random.choice(list(STYLES.keys()))
    base = STYLES[style]
    if random.random() < uniform_frac:
        knobs = {k: round(random.random(), 3) for k in KNOBS}
        mode = "uniform"
    else:
        knobs = {k: round(clamp01(base[k] + random.uniform(-jitter, jitter)), 3) for k in KNOBS}
        mode = "centered"
    return style, base["anchor"], knobs, mode


def apply_profile(port, style, knobs, dry):
    if dry:
        return
    _post_cmd(port, "/strategy " + style)
    for k, v in knobs.items():
        _push_knob(port, k, v)


def store(port, hand, style, anchor, knobs, mode):
    try:
        import psycopg2
        c = psycopg2.connect(DSN, connect_timeout=2)
        cur = c.cursor()
        cur.execute("CREATE TABLE IF NOT EXISTS train_randomize (id bigserial primary key, ts_ms bigint, "
                    "port int, handnumber bigint, style text, anchor int, mode text, "
                    "openrange real, aggro real, bluff real, cbet real)")
        cur.execute("INSERT INTO train_randomize (ts_ms,port,handnumber,style,anchor,mode,openrange,aggro,bluff,cbet) "
                    "VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)",
                    (int(time.time() * 1000), port, hand, style, anchor, mode,
                     knobs["openrange"], knobs["aggro"], knobs["bluff"], knobs["cbet"]))
        cur.execute("DELETE FROM train_randomize WHERE id < (SELECT max(id)-20000 FROM train_randomize)")
        c.commit(); c.close()
    except Exception:
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ports", default="", help="comma list; default scans 27654..27664")
    ap.add_argument("--every", type=float, default=4.0, help="poll cadence seconds")
    ap.add_argument("--max-hold", type=float, default=45.0, help="force a re-roll after this many seconds even if the hand hasn't changed")
    ap.add_argument("--uniform-frac", type=float, default=0.5, help="fraction of rolls that use fully-uniform knobs vs style-centered")
    ap.add_argument("--jitter", type=float, default=0.22, help="+/- jitter added to style-centered knobs")
    ap.add_argument("--once", action="store_true", help="roll every live bot once and exit")
    ap.add_argument("--dry", action="store_true", help="log the rolls but do not push them")
    a = ap.parse_args()

    if a.ports.strip():
        candidates = [int(x) for x in a.ports.split(",") if x.strip()]
    else:
        candidates = list(range(27654, 27665))

    log("[train-randomize] START ports=%s every=%.1fs max-hold=%.0fs uniform=%.2f jitter=%.2f%s"
        % (candidates, a.every, a.max_hold, a.uniform_frac, a.jitter, " DRY" if a.dry else ""))

    last_hand = {}   # port -> last handnumber we rolled on
    last_roll = {}   # port -> wall time of last roll

    def roll(port, hand):
        style, anchor, knobs, mode = roll_profile(a.uniform_frac, a.jitter)
        apply_profile(port, style, knobs, a.dry)
        store(port, hand if hand is not None else -1, style, anchor, knobs, mode)
        last_hand[port] = hand
        last_roll[port] = time.time()
        log("[train-randomize] :%d hand=%s -> STYLE=%s(%d) %s knobs=%s"
            % (port, hand, style, anchor, mode, knobs))

    if a.once:
        for p in live_ports(candidates):
            roll(p, handnumber(p))
        log("[train-randomize] --once done")
        return

    while True:
        try:
            ports = live_ports(candidates)
            now = time.time()
            for p in ports:
                hand = handnumber(p)
                changed = (p not in last_hand) or (hand is not None and hand != last_hand.get(p))
                stale = (now - last_roll.get(p, 0)) >= a.max_hold
                if changed or stale:
                    roll(p, hand)
        except Exception as e:
            log("[train-randomize] loop error: %r" % e)
        time.sleep(a.every)


if __name__ == "__main__":
    main()
