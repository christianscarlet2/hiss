#!/usr/bin/env python3
"""parse_guard.py -- AUTO-REPAIR for OHF parse errors (and any startup hang).

A Parse-Error modal BLOCKS Hiss at load: the process is up but its terminal HTTP port never binds. This
guard watches exactly that signal -- Hiss.exe running but the port dead for too long -- and AUTO-REPAIRS:
it saves the offending master aside, REVERTS the live master to the last KNOWN-GOOD snapshot, kills +
relaunches Hiss, and records the incident (passed to the MCP via the parse_incidents table + the AIL
feed + a spoken lilith warning). When Hiss is healthy it keeps the .lastgood snapshot current, so the
revert is always to the last OHF that actually parsed. [Emrald: parse errors auto-passed to MCP + repaired]

  python parse_guard.py --watch
"""
import os, sys, time, shutil, subprocess, urllib.request

ROOT = r"C:\www\openholdembot_old"
RELEASE = os.path.join(ROOT, "Release")
MASTER = os.path.join(RELEASE, "ScarletBeast_PowerHoldem.ohf")
LASTGOOD = MASTER + ".lastgood"
PORTFILE = os.path.join(RELEASE, "logs", "terminal_port.txt")
DSN = os.environ.get("HISS_PG_DSN", "host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass")
LILITH = os.environ.get("HISS_LILITH", os.path.join(RELEASE, "lilith.exe"))
CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0

STUCK_SECONDS = 45        # Hiss up but port dead this long => stuck on a modal / hang
HEALTHY_SECONDS = 60      # port healthy this long => snapshot the master as .lastgood
COOLDOWN_SECONDS = 120    # don't re-repair within this window (let a restart settle)

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def log(m):
    print("[parse_guard] " + m, flush=True)


def read_port():
    try:
        return open(PORTFILE).read().strip()
    except Exception:
        return ""


def port_responds():
    """Is Hiss ANSWERING -- on whatever port it actually bound?

    This used to trust terminal_port.txt alone. But Hiss binds the next free port when the previous
    one is still in TIME_WAIT after a restart (27654 -> 27655), and there is a window where the file
    still names the OLD port while the new instance is happily serving on a new one. This function
    then reported "dead", the guard declared STUCK, and it REVERTED THE LIVE OHF MASTER to .lastgood
    and restarted a bot that was never broken. A guard that rolls back your strategy because it looked
    at the wrong socket is worse than no guard.
    """
    p = read_port()
    if p:
        try:
            urllib.request.urlopen("http://127.0.0.1:%s/api/table-state" % p, timeout=2).read()
            return True
        except Exception:
            pass
    # The file's port is dead. Before condemning Hiss, look for it: it may simply have MOVED.
    for cand in range(27654, 27665):
        if str(cand) == str(p):
            continue
        try:
            urllib.request.urlopen("http://127.0.0.1:%d/api/table-state" % cand, timeout=0.5).read()
            log("Hiss moved to port %d (the port file said %r) -- alive, NOT stuck. No repair."
                % (cand, p or "none"))
            try:                       # heal the stale file so everything else finds it too
                with open(PORTFILE, "w") as f:
                    f.write(str(cand))
            except Exception:
                pass
            return True
        except Exception:
            continue
    return False


def hiss_running():
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Hiss.exe", "/NH"],
                             capture_output=True, text=True, timeout=10,
                             creationflags=CREATE_NO_WINDOW).stdout
        return "Hiss.exe" in out
    except Exception:
        return False


def speak(text):
    try:
        subprocess.Popen([LILITH, text], creationflags=CREATE_NO_WINDOW)
    except Exception:
        pass


def record(symbol_hint, action):
    try:
        import psycopg2
        c = psycopg2.connect(DSN); cur = c.cursor()
        cur.execute("CREATE TABLE IF NOT EXISTS parse_incidents (id bigserial primary key, ts_ms bigint, "
                    "detail text, action text)")
        cur.execute("INSERT INTO parse_incidents (ts_ms, detail, action) VALUES (%s,%s,%s)",
                    (int(time.time() * 1000), symbol_hint, action))
        c.commit(); c.close()
    except Exception as e:
        log("record error: " + str(e))


def kill_and_relaunch():
    """Try to restart Hiss. taskkill may be Access-Denied if Hiss is elevated; then the reverted OHF is
    staged and we flag for an elevated restart (the MCP / Emrald). Returns True if we relaunched."""
    killed = False
    try:
        r = subprocess.run(["taskkill", "/F", "/IM", "Hiss.exe"], capture_output=True, text=True,
                           timeout=15, creationflags=CREATE_NO_WINDOW)
        killed = (r.returncode == 0)
        if not killed:
            log("taskkill failed (elevated?): " + (r.stderr or r.stdout or "").strip()[:120])
    except Exception as e:
        log("taskkill error: " + str(e))
    if not killed:
        return False
    time.sleep(2)
    try:
        subprocess.Popen([os.path.join(RELEASE, "Hiss.exe")], cwd=RELEASE,
                         creationflags=(0x00000008 | 0x00000200 | CREATE_NO_WINDOW))  # DETACHED|NEW_GROUP|NO_WINDOW
        return True
    except Exception as e:
        log("relaunch error: " + str(e))
        return False


def repair():
    log("STUCK detected: Hiss up but the terminal port is dead -> auto-repairing.")
    ts = int(time.time())
    # save the offending master aside for diagnosis
    try:
        if os.path.exists(MASTER):
            shutil.copyfile(MASTER, MASTER + ".bad_%d" % ts)
    except Exception:
        pass
    reverted = False
    if os.path.exists(LASTGOOD):
        try:
            shutil.copyfile(LASTGOOD, MASTER)
            reverted = True
            log("reverted live master to .lastgood")
        except Exception as e:
            log("revert error: " + str(e))
    relaunched = kill_and_relaunch()
    action = ("reverted_to_lastgood+restarted" if (reverted and relaunched)
              else "reverted_to_lastgood (needs elevated restart)" if reverted
              else "no lastgood snapshot -- manual fix needed")
    record("OHF parse error / startup hang", action)
    speak("Parse error caught. " + ("Reverted and restarting." if relaunched
          else "Reverted the strategy; needs an elevated restart."))
    log("repair action: " + action)
    return relaunched


def main():
    log("online -- guarding the OHF (.lastgood=%s)" % ("present" if os.path.exists(LASTGOOD) else "none yet"))
    healthy_since = 0.0
    down_since = 0.0
    last_repair = 0.0
    while True:
        try:
            up = port_responds()
            now = time.time()
            if up:
                down_since = 0.0
                if healthy_since == 0.0:
                    healthy_since = now
                elif now - healthy_since > HEALTHY_SECONDS:
                    # snapshot the currently-loaded, known-good master
                    try:
                        if os.path.exists(MASTER):
                            shutil.copyfile(MASTER, LASTGOOD)
                            log("snapshot .lastgood (Hiss healthy %ds)" % HEALTHY_SECONDS)
                    except Exception:
                        pass
                    healthy_since = now + 1e9  # snapshot once per healthy streak; reset on next outage
            else:
                healthy_since = 0.0
                if hiss_running():
                    if down_since == 0.0:
                        down_since = now
                    elif (now - down_since > STUCK_SECONDS) and (now - last_repair > COOLDOWN_SECONDS):
                        repair()
                        last_repair = now
                        down_since = 0.0
                else:
                    down_since = 0.0   # Hiss not running (clean shutdown) -> not a parse-error hang
            time.sleep(5)
        except KeyboardInterrupt:
            break
        except Exception as e:
            log("loop error: " + str(e))
            time.sleep(5)


if __name__ == "__main__":
    main()
