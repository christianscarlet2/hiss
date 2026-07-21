#!/usr/bin/env python3
r"""icm_lobby_driver.py  (runs on windows-beast) -- sibling of vision_driver.py

Scrapes the ACR tournament LOBBY/INFO screens via claude.exe -p vision and writes settings.lobby_info,
the structure that icm_chip_daemon.py consumes to compute the hero's ICM $-equity / bubble factor.

Flow (mirrors the icm-chip-value skill):
  1. lobby_fetch.sh -> navigate to the info page + MORE INFO, capture C:/tmp/lobby_main.png +
     lobby_moreinfo.png (binary-safe via PIL). *** Clicks are click-region calls that are NO-OPS until
     the lobby button REGIONS are placed in the Vision tablemap -- until then this captures the table
     screen, not the lobby (players_remaining stays unrefreshed). ***
  2. claude.exe -p (vision) reads each PNG -> compact JSON.
  3. merge, compute avg_stack_bb, write settings.lobby_info (postgres, the daemon's source of truth).

Trigger: run on a schtask every ~5 min, only during a real tournament, and (poller wrapper) only after
the hero has folded in early position so the bot is idle while we read. Lock prevents overlap.
"""
import subprocess, json, os, re, sys, time, urllib.request

CLAUDE = r"C:\Users\scarl\.local\bin\claude.exe"
PSQL   = r"C:\Program Files\PostgreSQL\12\bin\psql.exe"
PROJ   = r"C:\www\openholdembot_old"
NW     = 0x08000000
PORT   = int(os.environ.get("ICM_PORT", "27654"))
LOCK   = r"C:\tmp\icm_lobby.lock"
LOG    = r"C:\tmp\icm_lobby_driver.log"
MAIN   = r"C:\tmp\lobby_main.png"
MORE   = r"C:\tmp\lobby_moreinfo.png"


def log(m):
    line = time.strftime("%Y-%m-%d %H:%M:%S  ") + m
    print(line, flush=True)
    try:
        open(LOG, "a").write(line + "\n")
    except OSError:
        pass


def urlget(path, timeout=6):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (PORT, path), timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def psql(sql):
    env = dict(os.environ); env["PGPASSWORD"] = os.environ.get("PGPASSWORD", "dbpass")
    p = subprocess.run([PSQL, "-U", "postgres", "-d", "hiss", "-t", "-A", "-c", sql],
                       capture_output=True, text=True, timeout=20, env=env, creationflags=NW)
    return p.stdout.strip()


def vision(path, prompt):
    if not (os.path.exists(path) and os.path.getsize(path) > 1000):
        return {}
    env = dict(os.environ); env.pop("ANTHROPIC_API_KEY", None); env["HISS_NO_CHIME"] = "1"
    r = subprocess.run([CLAUDE, "-p", prompt % path, "--allowedTools", "Read"],
                       capture_output=True, text=True, timeout=240, env=env,
                       creationflags=NW, stdin=subprocess.DEVNULL, cwd=PROJ)
    m = re.search(r"\{.*\}", r.stdout, re.S)
    return json.loads(m.group(0)) if m else {}


PROMPT_MAIN = (
    "Read the image file at %s . It is an ACR Poker tournament INFO page (or a poker table). Output ONLY "
    "a compact single-line JSON object, no prose, with keys: "
    "is_info_page (true only if this is the tournament info/lobby screen, false if it is a table), "
    "tournament (name string or null), remaining (players left, int or null), entrants (int or null), "
    "prize_pool (dollars float or null), avg_stack_bb (average stack in BIG BLINDS if shown 'Avg Stack "
    "68.48 BB' -> 68.48, else null), level (int or null), sb_dollars (float or null), bb_dollars (float "
    "or null), next_level_minutes (float or null). Use null for anything not visible."
)
PROMPT_MORE = (
    "Read the image file at %s . ACR tournament MORE-INFO popup (or a table). Output ONLY compact "
    "single-line JSON, no prose, keys: is_more_info (bool), starting_chips (int or null), "
    "blind_minutes (int or null), max_seats (int or null), places_paid (int or null), "
    "first_place (dollars float or null). null for anything not visible."
)


def gather():
    log("lobby_fetch (navigate + capture) ...")
    try:
        subprocess.run(["bash", "/c/www/openholdembot_old/mcp/lobby_fetch.sh", str(PORT), "3.5", "6"],
                       timeout=120, creationflags=NW)
    except Exception as e:
        log("lobby_fetch error: %s" % e)
    m = vision(MAIN, PROMPT_MAIN)
    mo = vision(MORE, PROMPT_MORE)
    return m, mo


def bb_chips_from_table():
    """Current big blind IN CHIPS from the live table, to convert an avg-stack-in-chips to bb."""
    try:
        st = json.loads(urlget("/api/table-state", timeout=3))
        bb = float(((st.get("limits") or {}).get("bblind")) or st.get("bblind") or 0)
        return bb if bb > 1.5 else 0.0     # >1.5 => real chip value, not the BB-frame '1'
    except Exception:
        return 0.0


def build_lobby_info(m, mo):
    # start from the existing row so we never lose fields the scrape didn't see
    try:
        cur = json.loads(psql("SELECT value FROM settings WHERE key='lobby_info';") or "{}")
    except Exception:
        cur = {}
    out = dict(cur)
    if m.get("is_info_page"):
        for k in ("tournament", "remaining", "entrants", "prize_pool", "avg_stack_bb", "level"):
            if m.get(k) is not None:
                out[k] = m[k]
    if mo.get("is_more_info"):
        for k, kk in (("starting_chips", "starting_chips"), ("blind_minutes", "blind_minutes"),
                      ("max_seats", "max_seats"), ("places_paid", "places_paid"), ("first_place", "first_place")):
            if mo.get(k) is not None:
                out[kk] = mo[k]
    # derive avg_stack_bb from chip figures if the lobby didn't print it in BB
    if not out.get("avg_stack_bb"):
        ent = float(out.get("entrants") or 0); rem = float(out.get("remaining") or 0)
        sc = float(out.get("starting_chips") or 0); bbc = bb_chips_from_table()
        if ent and rem and sc and bbc:
            out["avg_stack_bb"] = round((ent * sc / rem) / bbc, 2)
            out["bb_chips"] = bbc
            log("derived avg_stack_bb=%.2f (avg %.0f chips / bb %.0f)" % (out["avg_stack_bb"], ent*sc/rem, bbc))
    out["parsed_ms"] = int(time.time() * 1000)
    return out


def acquire():
    try:
        fd = os.open(LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY); os.write(fd, str(os.getpid()).encode()); os.close(fd)
        return True
    except FileExistsError:
        try:
            if time.time() - os.path.getmtime(LOCK) > 600:
                os.remove(LOCK); return acquire()
        except OSError:
            pass
        return False


def main():
    if not acquire():
        log("another run holds the lock -- skip"); return
    try:
        m, mo = gather()
        log("main=%s more=%s" % (json.dumps(m)[:200], json.dumps(mo)[:120]))
        info = build_lobby_info(m, mo)
        if info.get("remaining") and info.get("tournament"):
            psql("INSERT INTO settings(key,value) VALUES('lobby_info','%s') ON CONFLICT (key) DO UPDATE SET value=EXCLUDED.value"
                 % json.dumps(info).replace("'", "''"))
            log("wrote settings.lobby_info: tourney=%r remaining=%s avg_stack_bb=%s"
                % (info.get("tournament"), info.get("remaining"), info.get("avg_stack_bb")))
        else:
            log("NOT writing: no remaining/tournament read (lobby nav blocked until button regions placed?)")
    finally:
        try: os.remove(LOCK)
        except OSError: pass


if __name__ == "__main__":
    main()
