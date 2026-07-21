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

Trigger: a schtask every 7 min. On each firing we WAIT (up to ICM_WAIT_BUDGET) for the hero to fold
from UTG -- the one seat that buys a full orbit of clearance before hero must act again, which is what
the ~30s lobby navigation needs. Every LIVE table is watched in the same loop and scraped as it becomes
ready, so a second tournament is not starved by the first. Lock prevents overlap between firings.
"""
import subprocess, json, os, re, sys, time, difflib, urllib.request

CLAUDE = r"C:\Users\scarl\.local\bin\claude.exe"
PSQL   = r"C:\Program Files\PostgreSQL\12\bin\psql.exe"
PROJ   = r"C:\www\openholdembot_old"
NW     = 0x08000000
# Every Hiss instance serves its own table on its own port. ICM_PORTS overrides; otherwise we probe.
PORTS_ENV    = os.environ.get("ICM_PORTS", "")
PROBE_PORTS  = [27654, 27655, 27656, 27657]
PRIMARY_PORT = int(os.environ.get("ICM_PRIMARY_PORT", "27654"))
LOCK   = r"C:\tmp\icm_lobby.lock"
LOG    = r"C:\tmp\icm_lobby_driver.log"

# How long one firing will wait for a UTG fold before giving up. Hero is UTG once an orbit, and at the
# ~50s/hand this table runs that is ~5 min -- so a short budget misses far more often than it hits. The
# schtask fires every 7 min and is killed at 10, so 5.5 min of waiting still leaves room for the
# scrapes; a firing that overruns simply loses the lock race and skips, which is harmless.
WAIT_BUDGET_S = int(os.environ.get("ICM_WAIT_BUDGET", "330"))
# How many early seats count as "early enough". 1 = UTG only (a full orbit of clearance); 2 also
# accepts UTG+1, which roughly doubles the chance of catching a fold inside one firing's budget and
# still leaves ~4 seats before hero must act again. Set to 2 because at ~50s/hand hero reaches UTG
# only every ~5 min, and a UTG-only gate missed entire firings.
EP_SEATS      = max(1, int(os.environ.get("ICM_EP_SEATS", "2")))
POLL_S        = 1.0                    # hero folds fast; the 'holding cards' window is only seconds
# Fall back to the old "hero is simply out of the hand" gate when the budget expires without a UTG fold?
# Off by default: a button fold leaves ~2 seats of clearance and the navigation runs into the next hand.
ALLOW_FALLBACK = os.environ.get("ICM_ALLOW_FALLBACK", "0") == "1"
# How much of the lobby's tournament name must be found in the live table string before we believe the
# scrape belongs to this table. Both sides are OCR-noisy, so this is a fuzzy ratio, not containment.
MATCH_MIN = float(os.environ.get("ICM_NAME_MATCH_MIN", "0.65"))


def _find_bash():
    """GIT bash, by absolute path.

    Bare "bash" is not safe here: on this box PATH resolves it to C:\\WINDOWS\\system32\\bash.exe (WSL),
    which reads /c/www/... as a Linux path and exits 127 immediately. Git bash is the only one that can
    run lobby_fetch.sh, so name it explicitly rather than trusting PATH order."""
    for p in (os.environ.get("HISS_BASH", ""),
              r"C:\Program Files\Git\bin\bash.exe",
              r"C:\Program Files\Git\usr\bin\bash.exe",
              r"C:\Program Files (x86)\Git\bin\bash.exe"):
        if p and os.path.exists(p):
            return p
    return "bash"


BASH = _find_bash()


def paths(port):
    """Per-port capture files -- two tables scraped in one firing must not overwrite each other."""
    return (r"C:\tmp\lobby_%d_main.png" % port,
            r"C:\tmp\lobby_%d_moreinfo.png" % port,
            r"C:\tmp\lobby_%d_payouts.png" % port)


def log(m):
    line = time.strftime("%Y-%m-%d %H:%M:%S  ") + m
    print(line, flush=True)
    try:
        open(LOG, "a").write(line + "\n")
    except OSError:
        pass


def urlget(port, path, timeout=6):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def table_state(port, timeout=3):
    try:
        return json.loads(urlget(port, "/api/table-state", timeout=timeout))
    except Exception:
        return None


def live_ports():
    """Every Hiss instance that answers right now. One firing serves them all."""
    if PORTS_ENV.strip():
        want = [int(p) for p in PORTS_ENV.replace(";", ",").split(",") if p.strip()]
    else:
        want = PROBE_PORTS
    return [p for p in want if table_state(p) is not None]


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


TOURNEY_KEYS = ("freeroll", "gtd", "tourney", "tournament", "sit", "sng")


def is_tournament(st):
    name = (st.get("table") or "").lower()
    return any(k in name for k in TOURNEY_KEYS) or bool(st.get("tourney_id"))


def _hero(st):
    return next((p for p in (st.get("players") or []) if p.get("chair") == st.get("userchair")), {})


def _has_cards(pl):
    return bool([c for c in (pl.get("cards") or []) if c])


def early_chairs(st):
    """The EP_SEATS seats that act first preflop, starting with UTG (the one after the big blind).

    The dealer button is not published in /api/table-state (every chair reads dealer:false), so we
    recover position from the posted blinds instead: exactly one chair at the small blind and one at
    the big blind identifies the blinds, and UTG is the next SEATED chair after the BB. Returns None
    whenever that reading is not unambiguous -- a guess here would scrape at the wrong moment."""
    lim = st.get("limits") or {}
    try:
        sb = float(lim.get("sblind") or 0); bb = float(lim.get("bblind") or 0)
    except (TypeError, ValueError):
        return None
    if bb <= 0 or sb <= 0 or sb >= bb:
        return None
    if [c for c in (st.get("commonCards") or []) if c]:
        return None                                    # postflop: a 'bb'-sized bet is not a blind
    order = [p.get("chair") for p in (st.get("players") or []) if p.get("seated")]
    if len(order) < 3:                                 # heads-up inverts the blinds; don't guess
        return None

    def bet_of(ch):
        p = next((x for x in (st.get("players") or []) if x.get("chair") == ch), {})
        try:
            return float(p.get("bet") or 0)
        except (TypeError, ValueError):
            return 0.0

    eps = bb * 0.02
    at_sb = [c for c in order if abs(bet_of(c) - sb) <= eps]
    at_bb = [c for c in order if abs(bet_of(c) - bb) <= eps]
    if len(at_sb) != 1 or len(at_bb) != 1:
        return None
    i = order.index(at_bb[0])
    # The early seats, nearest-first. Never wrap past the button into the blinds themselves.
    n = min(EP_SEATS, max(1, len(order) - 3))
    return [order[(i + k) % len(order)] for k in range(1, n + 1)]


class UtgWatch(object):
    """Per-table state machine for 'hero was UTG this hand, and has now folded'.

    utg_chair() only reads cleanly in the narrow window between the blinds being posted and the first
    voluntary action -- one limp puts a second chair at the big blind and the seat becomes ambiguous.
    So the answer is CACHED per hand number: one good sample early in the hand fixes the position for
    the whole hand. Arming then needs to see hero holding cards in that seat; firing needs those cards
    to go away while the hand number is unchanged."""

    MAX_ROTATIONS = 3          # how many unread hands in a row we will carry the button forward

    def __init__(self):
        self.hand = None
        self.early = None
        self.armed = False
        self.rotations = 0

    def update(self, st):
        hand = st.get("handnumber")
        if hand != self.hand:
            # New hand. If we never got a clean blind reading last hand we would simply be blind to
            # hero's position -- but the button advances exactly one seated seat per hand, so carry the
            # previous answer forward instead. A later clean reading overwrites it, so drift from a
            # bust-out or a sit-out self-corrects rather than compounding.
            prev = self.early
            self.hand, self.early, self.armed = hand, None, False
            if prev and self.rotations < self.MAX_ROTATIONS:
                order = [p.get("chair") for p in (st.get("players") or []) if p.get("seated")]
                if prev[0] in order:
                    i = order.index(prev[0])
                    n = min(EP_SEATS, max(1, len(order) - 3))
                    self.early = [order[(i + 1 + k) % len(order)] for k in range(n)]
                    self.rotations += 1
        clean = early_chairs(st)                       # first clean reading wins, for this hand
        if clean:
            self.early, self.rotations = clean, 0
        hero = _hero(st)
        if not self.armed:
            if self.early and st.get("userchair") in self.early and _has_cards(hero):
                self.armed = True                      # hero is in an early seat with live cards
            return False
        if st.get("ismyturn") or st.get("toact") == st.get("userchair"):
            return False                               # still hero's decision -- never navigate now
        if not _has_cards(hero):
            self.armed = False
            return True                                # cards gone while UTG -> hero folded
        return False


def out_of_hand(st):
    """The old, weaker gate: hero simply is not in the current hand. Only used as a fallback."""
    if st.get("ismyturn") or st.get("toact") == st.get("userchair"):
        return False
    hero = _hero(st)
    return not (hero.get("active") and _has_cards(hero))


PROMPT_PAYS = (
    "Read the image file at %s . It is an ACR tournament PRIZE POOL / STRUCTURE payout screen (or a table). "
    "Output ONLY compact single-line JSON, no prose, keys: is_payouts (bool), places_paid (int or null), "
    "first_place (dollars float or null), payouts (array of dollar amounts top-down, or null). null if not "
    "a payout screen."
)


def gather(port):
    MAIN, MORE, PAYS = paths(port)
    for f in (MAIN, MORE, PAYS):
        try: os.remove(f)                  # never parse a previous firing's screenshot
        except OSError: pass
    log("[%d] lobby_fetch (navigate + capture) ..." % port)
    try:
        r = subprocess.run([BASH, "/c/www/openholdembot_old/mcp/lobby_fetch.sh", str(port), "3.5", "6",
                            "lobby_%d" % port],
                           capture_output=True, text=True, timeout=120, creationflags=NW)
        # ALWAYS log the outcome. This shelled out with stdout and stderr discarded, so when `bash`
        # resolved to WSL and every run died in under a second with exit 127, nothing said so -- and
        # vision() went on parsing week-old screenshots as if they were this tournament.
        if r.returncode != 0:
            log("[%d] lobby_fetch FAILED rc=%s stderr=%r" % (port, r.returncode, (r.stderr or "")[:300]))
        else:
            log("[%d] lobby_fetch: %s" % (port, " | ".join(
                l.strip() for l in (r.stdout or "").splitlines() if l.strip())[:400]))
    except Exception as e:
        log("[%d] lobby_fetch error: %s" % (port, e))
    m = vision(MAIN, PROMPT_MAIN)
    mo = vision(MORE, PROMPT_MORE)
    # exact payouts -- only present once the PRIZE POOL/STRUCTURE region+click are added to lobby_fetch.sh
    # (the joint next step). Until then this file won't exist and pay stays {} -> daemon models the ladder.
    pay = vision(PAYS, PROMPT_PAYS) if os.path.exists(PAYS) else {}
    return m, mo, pay


def _slug(s):
    return re.sub(r"[^a-z0-9]", "", (s or "").lower())


def name_score(lobby_name, table_name):
    """How much of the lobby's tournament name is present in the live table string, 0..1.

    Straight containment (what the daemon does) breaks on a single OCR slip -- the table here reads
    '50GYDFreeroll' for '$50 GTD Freeroll', and one wrong character fails an exact test. Matching
    blocks tolerate that while still scoring an unrelated tournament far below threshold."""
    a, b = _slug(lobby_name), _slug(table_name)
    if not a or not b:
        return 0.0
    blocks = difflib.SequenceMatcher(None, a, b).get_matching_blocks()
    return sum(bl.size for bl in blocks) / float(len(a))


def bb_chips_from_table(port):
    """Current big blind IN CHIPS from the live table, to convert an avg-stack-in-chips to bb."""
    st = table_state(port) or {}
    try:
        bb = float(((st.get("limits") or {}).get("bblind")) or st.get("bblind") or 0)
    except (TypeError, ValueError):
        return 0.0
    return bb if bb > 1.5 else 0.0         # >1.5 => real chip value, not the BB-frame '1'


def build_lobby_info(port, key, m, mo, pay=None):
    # Start from the existing row so we never lose fields the scrape didn't see -- but ONLY when that
    # row is the same tournament. Carrying fields across tournaments is how a foreign id/buyin/payout
    # set survives into a fresh scrape and quietly poisons every $ figure downstream.
    try:
        cur = json.loads(psql("SELECT value FROM settings WHERE key='%s';" % key) or "{}")
    except Exception:
        cur = {}
    if cur and m.get("tournament") and _slug(cur.get("tournament")) != _slug(m.get("tournament")):
        log("[%d] new tournament (%r -> %r) -- discarding carried-over fields"
            % (port, cur.get("tournament"), m.get("tournament")))
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
        sc = float(out.get("starting_chips") or 0); bbc = bb_chips_from_table(port)
        if ent and rem and sc and bbc:
            out["avg_stack_bb"] = round((ent * sc / rem) / bbc, 2)
            out["bb_chips"] = bbc
            log("[%d] derived avg_stack_bb=%.2f (avg %.0f chips / bb %.0f)"
                % (port, out["avg_stack_bb"], ent*sc/rem, bbc))
    if pay and pay.get("is_payouts"):          # EXACT payouts (once the PRIZE POOL region is placed)
        for k in ("places_paid", "first_place", "payouts"):
            if pay.get(k) is not None:
                out[k] = pay[k]
    if m.get("_scrape_table"):
        out["scrape_table"] = m["_scrape_table"]
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


def scrape_one(port, tbl_name):
    """Navigate, parse, and store this table's structure. Returns True if a row was written."""
    m, mo, pay = gather(port)
    if tbl_name:
        m["_scrape_table"] = tbl_name            # record the table we were at (icm_serve tourney-match)
    log("[%d] main=%s more=%s pay=%s"
        % (port, json.dumps(m)[:200], json.dumps(mo)[:100], json.dumps(pay)[:100]))

    # THE MATCH GATE. The lobby opens on whatever tournament is selected, which is not necessarily the
    # one hero is seated at -- that is how a foreign row ('DabPoker420 Champions') came to sit in
    # settings.lobby_info while hero played a PLO freeroll. The daemon then refused to speak an ICM
    # number, correctly but silently. Refuse the WRITE instead, so the mismatch is visible here and a
    # good row from an earlier firing survives.
    score = name_score(m.get("tournament"), tbl_name)
    if m.get("tournament") and tbl_name and score < MATCH_MIN:
        log("[%d] NOT writing: lobby read %r but table is %r (match %.2f < %.2f) -- lobby opened on the "
            "wrong tournament" % (port, m.get("tournament"), tbl_name, score, MATCH_MIN))
        return False

    key = "lobby_info_%d" % port
    info = build_lobby_info(port, key, m, mo, pay)
    if not (info.get("remaining") and info.get("tournament")):
        log("[%d] NOT writing: no remaining/tournament read (lobby nav blocked until button regions "
            "placed?)" % port)
        return False
    info["port"] = port
    info["name_match"] = round(score, 3)
    blob = json.dumps(info).replace("'", "''")
    # updated_at MUST be set explicitly: it defaults to now() only on INSERT, so an ON CONFLICT update
    # left it frozen at the row's original creation. The row was rewritten every 7 min but still looked
    # 35 days old, and the daemon's freshness check (LOBBY_MAX_AGE_S) threw it away every time.
    keys = [key] + (["lobby_info"] if port == PRIMARY_PORT else [])
    for k in keys:
        psql("INSERT INTO settings(key,value,updated_at) VALUES('%s','%s',now()) "
             "ON CONFLICT (key) DO UPDATE SET value=EXCLUDED.value, updated_at=now()" % (k, blob))
    log("[%d] wrote settings.%s: tourney=%r remaining=%s avg_stack_bb=%s match=%.2f"
        % (port, "+".join(keys), info.get("tournament"), info.get("remaining"),
           info.get("avg_stack_bb"), score))
    return True


def main():
    if not acquire():
        log("another run holds the lock -- skip"); return
    try:
        ports = live_ports()
        if not ports:
            log("no Hiss instance is answering -- skip"); return
        tables = {}
        for p in list(ports):
            st = table_state(p) or {}
            if not is_tournament(st):
                log("[%d] skip: not a tournament (table=%r)" % (p, st.get("table") or ""))
                ports.remove(p); continue
            tables[p] = st.get("table") or ""
        if not ports:
            return
        log("watching %s for a UTG fold (budget %ds): %s"
            % (", ".join(str(p) for p in ports), WAIT_BUDGET_S,
               "; ".join("%d=%r" % (p, tables[p]) for p in ports)))

        # One loop over every table. Each fires the moment ITS hero folds from UTG, so a second
        # tournament is not starved waiting on the first.
        watch = dict((p, UtgWatch()) for p in ports)
        pending, deadline = list(ports), time.time() + WAIT_BUDGET_S
        while pending and time.time() < deadline:
            for p in list(pending):
                st = table_state(p)
                if st is None:
                    continue
                tables[p] = st.get("table") or tables[p]
                if watch[p].update(st):
                    log("[%d] gate ok: hero folded from an early seat (%s, UTG first) -- ~an orbit of "
                        "clearance" % (p, watch[p].early))
                    pending.remove(p)
                    scrape_one(p, tables[p])
            if pending:
                time.sleep(POLL_S)

        for p in pending:
            if not ALLOW_FALLBACK:
                log("[%d] no UTG fold within %ds -- skipping (set ICM_ALLOW_FALLBACK=1 to scrape from "
                    "any out-of-hand moment)" % (p, WAIT_BUDGET_S))
                continue
            st = table_state(p) or {}
            if out_of_hand(st):
                log("[%d] FALLBACK: no UTG fold in %ds; scraping from a plain out-of-hand moment "
                    "(less clearance)" % (p, WAIT_BUDGET_S))
                scrape_one(p, st.get("table") or tables[p])
            else:
                log("[%d] no UTG fold and hero is in a hand -- skipping this firing" % p)
    finally:
        try: os.remove(LOCK)
        except OSError: pass


if __name__ == "__main__":
    main()
