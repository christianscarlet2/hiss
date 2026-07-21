#!/usr/bin/env python3
r"""automation_daemon.py -- step 1 of the join-a-game automation.

Watches every live Hiss instance. When an instance has AUTOMATION ON but is not sat at (or railing)
a table, the poker client on that instance's phone is closed and reopened over adb -- the recovery
that gets a wandered/crashed/logged-out client back to a state where it can join a game.

WHY THE CHECK IS PARANOID
    Restarting the client is destructive: if the hero really is at a table it drops them mid-hand.
    So a restart needs ALL of:
      * /api/automation-enabled  -> on, for that port
      * /api/seat-status         -> state == "not_at_table"
      * that state STABLE for NOT_AT_TABLE_S (Hiss debounces it; we additionally require our own
        consecutive-reading count, so a wedged Hiss that freezes its verdict cannot trigger us)
      * the device is not in cooldown, and is under the hourly restart cap
    Hiss's own verdict is built from several independent signals (identity, blinds, populated seats,
    hero seat/name/stack/cards) precisely so the lobby -- which still emits a table-ish string like
    "antCliedog|EE.1." -- can never read as a table. See CHeartbeatThread::UpdateSeatStatus.

DEVICE MAPPING
    settings.automation_devices in postgres maps a terminal port to the phone and how to relaunch:
      {"27654": {"serial": "R5GL205FT7Y",
                 "launch_package": "org.chromium.webapk.af4bc3cbf4bae1771_v2",
                 "kill_package": "com.android.chrome"}}
    ACR is a Chrome PWA, not a native app: on a phone where it was installed as a WebAPK the shell
    package launches it; the hosting Chrome process is what must be killed.

Run:  python mcp/automation_daemon.py            (AUTOMATION_DRY_RUN=1 to log without acting)
"""
import json, os, re, subprocess, sys, time, urllib.request

PSQL     = os.environ.get("HISS_PSQL", r"C:\Program Files\PostgreSQL\12\bin\psql.exe")
PGUSER   = os.environ.get("PGUSER", "postgres")
PGDB     = os.environ.get("PGDATABASE", "hiss")
PGPASS   = os.environ.get("PGPASSWORD", "dbpass")
ADB      = os.environ.get("HISS_ADB", "adb")
PROBE_PORTS = [int(p) for p in os.environ.get(
    "AUTOMATION_PORTS", "27654,27655,27656,27657").replace(";", ",").split(",") if p.strip()]

TICK_S          = 10
NOT_AT_TABLE_S  = int(os.environ.get("AUTOMATION_NOT_AT_TABLE_S", "120"))  # how long "no table" must hold
COOLDOWN_S      = int(os.environ.get("AUTOMATION_COOLDOWN_S", "300"))      # min gap between restarts
MAX_PER_HOUR    = int(os.environ.get("AUTOMATION_MAX_PER_HOUR", "4"))      # loop guard
SETTLE_S        = int(os.environ.get("AUTOMATION_SETTLE_S", "25"))         # wait for the client to load
DRY_RUN         = os.environ.get("AUTOMATION_DRY_RUN", "0") == "1"

NOWIN = 0x08000000 if os.name == "nt" else 0
LOG   = r"C:\tmp\automation_daemon.log"

# AIL-style kill switch: the same file the other daemons honour, so one switch stops everything.
AIL_STATE = os.path.join(os.environ.get("HISS_RELEASE", r"C:\www\openholdembot_old\Release"),
                         "logs", "ail_state.json")


def log(*a):
    line = time.strftime("%Y-%m-%d %H:%M:%S  ") + " ".join(str(x) for x in a)
    print(line, flush=True)
    try:
        open(LOG, "a").write(line + "\n")
    except OSError:
        pass


def psql(sql):
    env = dict(os.environ); env["PGPASSWORD"] = PGPASS
    try:
        p = subprocess.run([PSQL, "-U", PGUSER, "-d", PGDB, "-t", "-A", "-c", sql],
                           capture_output=True, text=True, env=env, timeout=20, creationflags=NOWIN)
        return p.stdout.strip() if p.returncode == 0 else ""
    except Exception:
        return ""


def get(port, path, timeout=4):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception:
        return None


def adb(serial, *args, **kw):
    cmd = [ADB]
    if serial:
        cmd += ["-s", serial]
    cmd += list(args)
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=kw.get("timeout", 25), creationflags=NOWIN)
        return p.returncode, (p.stdout or "").strip(), (p.stderr or "").strip()
    except Exception as e:
        return -1, "", str(e)


def device_map():
    """port -> {serial, launch_package, kill_package}. Empty map => nothing is ever restarted."""
    raw = psql("SELECT value FROM settings WHERE key='automation_devices';")
    try:
        m = json.loads(raw) if raw else {}
    except ValueError:
        log("automation_devices is not valid JSON -- refusing to act"); return {}
    out = {}
    for k, v in (m or {}).items():
        try:
            out[int(k)] = v
        except (TypeError, ValueError):
            continue
    return out


def ail_enabled():
    try:
        with open(AIL_STATE) as f:
            return bool((json.load(f).get("enabled") or {}).get("automation", True))
    except (OSError, ValueError):
        return True


# icm_lobby_driver.py drives a client OFF the felt and through the lobby to read the tournament
# structure. During that the client is legitimately "not at a table" -- indistinguishable, from
# seat-status alone, from a client that has wandered off and needs restarting. Restarting it
# mid-scrape would kill the navigation and strand the app, so the driver raises a per-port flag and
# we stand down while it is fresh. Freshness (not mere existence) is the test: a driver killed
# mid-navigation leaves the flag behind, and a stale file must not disable automation forever.
NAV_FLAG = r"C:\tmp\icm_navigating_%d.flag"
NAV_FLAG_MAX_AGE_S = int(os.environ.get("AUTOMATION_NAV_MAX_AGE", "300"))


def lobby_navigating(port):
    try:
        return (time.time() - os.path.getmtime(NAV_FLAG % port)) < NAV_FLAG_MAX_AGE_S
    except OSError:
        return False


def foreground(serial):
    rc, out, _ = adb(serial, "shell",
                     "dumpsys window 2>/dev/null | grep -m1 -oE 'mCurrentFocus=Window\\{[^}]*\\}'")
    return out


def restart_client(port, dev):
    """Close the poker client and reopen it. Returns True if it came back to the foreground."""
    serial = dev.get("serial")
    launch = dev.get("launch_package")
    kill = dev.get("kill_package") or launch
    if not (serial and launch):
        log("[%d] device entry lacks serial/launch_package -- skipping" % port); return False
    if DRY_RUN:
        log("[%d] DRY RUN: would force-stop %s then launch %s on %s" % (port, kill, launch, serial))
        return False

    log("[%d] restarting poker client on %s: force-stop %s" % (port, serial, kill))
    rc, out, err = adb(serial, "shell", "am", "force-stop", kill)
    if rc != 0:
        log("[%d] force-stop failed rc=%s %s" % (port, rc, err[:160]))
    time.sleep(3)
    rc, out, err = adb(serial, "shell", "monkey", "-p", launch,
                       "-c", "android.intent.category.LAUNCHER", "1")
    if rc != 0:
        log("[%d] launch FAILED rc=%s %s" % (port, rc, (err or out)[:200])); return False

    time.sleep(SETTLE_S)
    fg = foreground(serial)
    ok = ("chrome" in fg.lower() or launch.split(".")[-1][:12] in fg)
    log("[%d] after restart foreground=%s -> %s" % (port, fg or "(unknown)", "OK" if ok else "NOT BACK"))
    return ok


def main():
    log("automation daemon up. not_at_table>=%ds, cooldown %ds, max %d/h%s"
        % (NOT_AT_TABLE_S, COOLDOWN_S, MAX_PER_HOUR, "  [DRY RUN]" if DRY_RUN else ""))
    last_restart = {}     # port -> epoch
    history = {}          # port -> [epoch, ...] within the last hour
    streak = {}           # port -> consecutive not_at_table readings WE observed
    last_ail = 0.0

    while True:
        if time.time() - last_ail > 20:
            last_ail = time.time()
            if not ail_enabled():
                log("AIL 'automation' switched off -- exiting"); return

        devices = device_map()
        for port in PROBE_PORTS:
            seat = get(port, "/api/seat-status")
            if seat is None or not seat.get("ok"):
                streak.pop(port, None)
                continue
            auto = get(port, "/api/automation-enabled") or {}
            if not auto.get("enabled"):
                streak.pop(port, None)
                continue

            # An ICM lobby scrape is driving this client right now: it is off the felt on purpose.
            # Clear the streak too, so the readings taken during navigation cannot be counted
            # towards a restart the moment the flag clears.
            if lobby_navigating(port):
                if streak.pop(port, None):
                    log("[%d] lobby scrape in progress -- stand down" % port)
                continue

            state = seat.get("state")
            if state != "not_at_table":
                if streak.pop(port, None):
                    log("[%d] back to '%s' -- stand down" % (port, state))
                continue

            # Hiss's debounce AND our own consecutive count must both agree. Hiss's stable_ms alone
            # would keep climbing if its heartbeat wedged; our streak only advances on fresh reads.
            streak[port] = streak.get(port, 0) + 1
            need = max(2, NOT_AT_TABLE_S // TICK_S)
            stable_ms = seat.get("stable_ms") or 0
            if streak[port] < need or stable_ms < NOT_AT_TABLE_S * 1000:
                continue

            dev = devices.get(port)
            if not dev:
                log("[%d] not at a table, but no automation_devices entry -- not acting" % port)
                streak[port] = 0
                continue

            now = time.time()
            if now - last_restart.get(port, 0) < COOLDOWN_S:
                continue
            recent = [t for t in history.get(port, []) if now - t < 3600]
            history[port] = recent
            if len(recent) >= MAX_PER_HOUR:
                log("[%d] restart cap reached (%d in the last hour) -- not restarting again"
                    % (port, len(recent)))
                streak[port] = 0
                continue

            log("[%d] NOT AT A TABLE for %ds (evidence %s) -- restarting the client"
                % (port, stable_ms // 1000, json.dumps(seat.get("evidence") or {})))
            last_restart[port] = now
            history[port] = recent + [now]
            restart_client(port, dev)
            streak[port] = 0

        time.sleep(TICK_S)


if __name__ == "__main__":
    main()
