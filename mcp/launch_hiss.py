r"""launch_hiss.py  (run via pythonw.exe -> NO console window)

Windowless launcher that reliably brings up ALL Hiss instances in ONE run: it launches one Hiss.exe
per connected phone / scrcpy table, WAITS for each to bind its terminal port before moving on, and
RETRIES any launch that fails to bind (the classic "2nd table didn't attach" case). Hiss self-limits
-- an extra instance with no free scrcpy window just exits -- so over-launching is harmless. Then it
makes sure the HUD auto-recalibrate poller + vision driver are running. Every child is windowless.

Run once via the HissLaunch scheduled task (`schtasks /run /tn HissLaunch`). Idempotent: it only
launches the instances that are still missing, so re-running when some are already up is safe.
"""
import subprocess, os, time, urllib.request

REL = r"C:\www\openholdembot_old\Release"
MCP = r"C:\www\openholdembot_old\mcp"
PYW = r"C:\Users\scarl\AppData\Local\Programs\Python\Python310\pythonw.exe"
DETACHED  = 0x00000008 | 0x00000200      # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
NO_WINDOW = 0x08000000                   # CREATE_NO_WINDOW
PORTS     = range(27654, 27665)
BIND_WAIT = 35                           # seconds to wait for a launched Hiss to bind a NEW port
RETRIES   = 2                            # extra attempts per instance if it fails to bind

# Silence the Claude Code "Stop"/"Notification" hook chimes for everything Hiss runs.
# Hiss shells out to headless `claude -p` constantly while it plays (vision_driver reading
# tablemap features off a screencap, hud_calibrate_driver, deep_thought, decision_advisor,
# astrology, growth). Each of those ENDS a Claude session, firing the Stop hook, so the
# speakers chime every few seconds for calls no human is waiting on.
# Set here, at the ROOT of the process tree: every child inherits it (Hiss.exe and the python
# helpers -> claude.exe -> the powershell hook), so one line covers every current and future
# caller. scripts\play_sound.ps1 / play_random_sound.ps1 check it and exit 0 without playing.
# An INTERACTIVE Claude session is not a child of this launcher, never sees the variable, and
# keeps chiming as before -- which is the whole point.
os.environ["HISS_NO_CHIME"] = "1"


def bound_ports():
    b = set()
    for p in PORTS:
        try:
            urllib.request.urlopen("http://127.0.0.1:%d/api/table-state" % p, timeout=1).read()
            b.add(p)
        except Exception:
            pass
    return b


def phone_count():
    """One Hiss per connected phone (each drives one scrcpy table). Falls back to 2."""
    try:
        r = subprocess.run(["adb", "devices"], capture_output=True, text=True, timeout=15,
                           creationflags=NO_WINDOW)
        n = sum(1 for ln in r.stdout.splitlines()[1:] if "\tdevice" in ln)
        return n if n > 0 else 2
    except Exception:
        return 2


def launch_one():
    """Launch one Hiss and wait for a NEW port to bind; retry on failure. Returns the port or None."""
    for _ in range(RETRIES + 1):
        before = bound_ports()
        subprocess.Popen([os.path.join(REL, "Hiss.exe")], cwd=REL, close_fds=True,
                         creationflags=DETACHED)
        for _ in range(BIND_WAIT):
            time.sleep(1)
            new = bound_ports() - before
            if new:
                return sorted(new)[0]
    return None


def start_helpers():
    # HUD auto-recalibrate poller (its own lock keeps it single-instance) ...
    subprocess.Popen([PYW, os.path.join(MCP, "hud_calibrate_driver.py")],
                     cwd=MCP, close_fds=True, creationflags=DETACHED | NO_WINDOW)
    # ... one vision cycle once ports are up ...
    subprocess.Popen([PYW, os.path.join(MCP, "vision_driver.py"), "--once"],
                     cwd=MCP, close_fds=True, creationflags=DETACHED | NO_WINDOW)
    # ... and the teardown watchdog: runs terminate_daemons.ps1 whenever Hiss is closed by ANY method
    # (a force-kill bypasses Hiss's own ExitInstance teardown). It lives in C:\tmp so the teardown's
    # 'openholdembot' regex never kills it; its own lock keeps it single-instance.
    subprocess.Popen([PYW, r"C:\tmp\hiss_watchdog.py"], close_fds=True, creationflags=DETACHED | NO_WINDOW)


def main():
    target = phone_count()
    need = max(0, target - len(bound_ports()))
    for _ in range(need):
        launch_one()
        time.sleep(2)
    start_helpers()


if __name__ == "__main__":
    main()
