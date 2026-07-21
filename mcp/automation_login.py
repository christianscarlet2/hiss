#!/usr/bin/env python3
r"""automation_login.py -- drive the ACR client's login screen on a phone mirror.

hiss.exe's automation heartbeat decides WHEN to log in (it OCRs the login button and
acts only while the automation button is on); this module is the HOW. Kept out of the
bot for the same reason automation_map.py is: it is adb plumbing, and it is far easier
to test and fix here than inside an MFC heartbeat.

WHAT IT DOES
    tap the email field -> clear it -> type the email
    tap the password field -> clear it -> type the password
    tap LOGIN

CLEARING A FIELD
    Android has no reliable "select all" over adb -- KEYCODE_CTRL_A does nothing in
    most WebViews and long-press-select is a UI dance. So: move the caret to the end
    (KEYCODE_MOVE_END) and send a burst of backspaces, enough to swallow any plausible
    value. They go in ONE `input keyevent` invocation -- adb accepts a list of keycodes
    and the round-trip, not the keypress, is what costs time (60 separate calls take
    ~30s; one call takes under a second).

COORDINATES
    Regions are stored in scrcpy-WINDOW pixels (that is what the bot scrapes), while adb
    taps want DEVICE pixels. scrcpy letterboxes: the mirror is aspect-fit and centred, so
    the transform is a scale plus an x/y offset. Derived from the two sizes at run time
    and verified against a real screencap (predicted button centre 538,1689 vs actual
    539,1690).

Usage:
    python automation_login.py --port 27654 --dry-run
    python automation_login.py --port 27654
"""
import argparse, json, os, re, subprocess, sys, time, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import automation_map as AM  # noqa: E402

ADB = os.environ.get("HISS_ADB", "adb")
NO_WINDOW = 0x08000000 if os.name == "nt" else 0
KEY_MOVE_END, KEY_DEL, KEY_TAB = 123, 67, 61
CLEAR_PRESSES = int(os.environ.get("AUTOMATION_CLEAR_PRESSES", "60"))


def adb(serial, *args, timeout=30):
    r = subprocess.run([ADB, "-s", serial] + list(args), capture_output=True, text=True,
                       timeout=timeout, creationflags=NO_WINDOW)
    if r.returncode:
        raise RuntimeError("adb %s: %s" % (" ".join(args), (r.stderr or r.stdout).strip()))
    return r.stdout


def device_for_port(port):
    """settings.automation_devices maps terminal port -> phone. One source of truth."""
    raw = AM.psql("SELECT value FROM settings WHERE key='automation_devices'")
    if not raw.strip():
        raise SystemExit("settings.automation_devices is empty")
    devices = json.loads(raw)
    entry = devices.get(str(port))
    if not entry:
        raise SystemExit("no device mapped to port %s" % port)
    return entry["serial"], entry


def device_size(serial):
    out = adb(serial, "shell", "wm", "size")
    m = re.search(r"(\d+)x(\d+)", out)
    if not m:
        raise SystemExit("could not read device size: " + out.strip())
    return int(m.group(1)), int(m.group(2))


def window_size(title):
    """The mirror's client area -- the coordinate space the regions were mapped in."""
    shot = os.path.join(os.environ.get("TEMP", "/tmp"), "_login_probe.png")
    AM.capture(title, shot)
    from PIL import Image
    with Image.open(shot) as im:
        return im.size


class Mapper:
    """scrcpy aspect-fits the device into the window and centres it."""

    def __init__(self, win, dev):
        (ww, wh), (dw, dh) = win, dev
        self.scale = min(ww / dw, wh / dh)
        self.ox = (ww - dw * self.scale) / 2.0
        self.oy = (wh - dh * self.scale) / 2.0

    def to_device(self, x, y):
        return int(round((x - self.ox) / self.scale)), int(round((y - self.oy) / self.scale))

    def centre(self, rect):
        l, t, r, b = rect
        return self.to_device((l + r) / 2.0, (t + b) / 2.0)


def shell_quote(s):
    """`input text` runs through the device shell: single-quote it so *, &, (, ) are literal."""
    if "'" in s:
        # close, escaped quote, reopen -- the only case single quotes cannot cover directly
        s = s.replace("'", "'\\''")
    return "'" + s.replace(" ", "%s") + "'"


# Settings that make the phone interrupt an automated login. All of them are plain
# secure-settings values, saved before they are changed and restorable -- deliberately
# NOT `ime disable` / `pm disable`, which is how this phone got soft-rebooted once:
# disabling the ACTIVE IME took system_server down with it.
SUPPRESS = {
    # Google's / Samsung Pass's "Use saved password?" sheet steals the field focus and
    # eats whatever is typed next.
    "autofill_service": "null",
    # Assistant / voice interaction ("google talk") can surface over the client.
    "assistant": "null",
    "voice_interaction_service": "null",
}
SUPPRESS_BACKUP = r"C:\tmp\automation_suppress_%s.json"


def suppress_prompts(serial, dry=False):
    """Silence autofill + assistant on this phone, remembering what was there."""
    saved = {}
    for key, want in SUPPRESS.items():
        was = adb(serial, "shell", "settings", "get", "secure", key).strip()
        saved[key] = was
        print("   %s: %s -> %s" % (key, was, want))
        if not dry:
            adb(serial, "shell", "settings", "put", "secure", key, want)
    path = SUPPRESS_BACKUP % serial
    if not dry:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(saved, f, indent=2)
        print("   previous values saved to %s" % path)
    return saved


def restore_prompts(serial):
    path = SUPPRESS_BACKUP % serial
    if not os.path.exists(path):
        raise SystemExit("no saved values at %s" % path)
    with open(path, encoding="utf-8") as f:
        saved = json.load(f)
    for key, was in saved.items():
        if was and was.lower() != "null":
            adb(serial, "shell", "settings", "put", "secure", key, was)
        else:
            adb(serial, "shell", "settings", "delete", "secure", key)
        print("   %s restored to %s" % (key, was))


def hide_keyboard(serial, dry):
    """Put the soft keyboard away without touching the IME.

    Tapping a field raises the keyboard, which slides the page and can cover the login
    button. BACK closes the keyboard when it is up and does not navigate -- the gentle
    version of what disabling the IME was meant to achieve.
    """
    if not dry:
        adb(serial, "shell", "input", "keyevent", "111")   # ESCAPE: closes the IME
        adb(serial, "shell", "input", "keyevent", "4")     # BACK: same, for IMEs that ignore ESC


def tap(serial, xy, dry):
    print("   tap %s" % (xy,))
    if not dry:
        adb(serial, "shell", "input", "tap", str(xy[0]), str(xy[1]))


def clear_field(serial, dry, presses=CLEAR_PRESSES):
    """Caret to the end, then a burst of backspaces -- one adb round-trip for the lot."""
    print("   clear: MOVE_END + %d x DEL" % presses)
    if not dry:
        adb(serial, "shell", "input", "keyevent", str(KEY_MOVE_END))
        keys = " ".join([str(KEY_DEL)] * presses)
        adb(serial, "shell", "input keyevent " + keys, timeout=120)


def type_text(serial, text, dry, secret=False):
    print("   type %s" % ("*" * len(text) if secret else text))
    if not dry:
        adb(serial, "shell", "input text " + shell_quote(text), timeout=60)


def fetch_credentials(port):
    """The bot's own ACR login, from the site, using THIS instance's token."""
    raw = AM.psql("SELECT value::jsonb->>'api_token_%d' FROM settings WHERE key='automation_api'" % port)
    token = raw.strip()
    base = AM.psql("SELECT value::jsonb->>'api_base' FROM settings WHERE key='automation_api'").strip()
    if not token:
        raise SystemExit("no api_token_%d in settings.automation_api" % port)
    req = urllib.request.Request(base.rstrip("/") + "/api/v1/automation/credentials",
                                 headers={"Authorization": "Bearer " + token,
                                          "Accept": "application/json",
                                          # Cloudflare 403s the default python-urllib agent;
                                          # match what hiss.exe's WinHTTP client sends.
                                          "User-Agent": "Hiss-Automation/1.0"})
    with urllib.request.urlopen(req, timeout=15) as r:
        body = json.loads(r.read().decode("utf-8"))
    if not body.get("ok"):
        raise SystemExit("site returned no credentials: %s" % body.get("error"))
    return body["acr"]["email"], body["acr"]["password"]


def open_app(port, dry=False, settle=20):
    """(Re)start the ACR client on this phone.

    NOT `monkey -p <pkg> -c LAUNCHER`: the WebAPK shell has no launcher activity of the
    kind monkey wants, so that silently opened the Play Store instead. And the shell's
    SplashActivity is not exported, so it cannot be started by component either. What
    works is a VIEW deep link at the app's own URL, restricted to its package -- the
    system then hands it to Chrome's SameTaskWebApkActivity, which IS how the app runs.
    """
    serial, entry = device_for_port(port)
    url = entry.get("launch_url") or "https://app.acrpoker.eu/"
    pkg = entry.get("launch_package")
    print("opening %s on %s" % (url, serial))
    if dry:
        return
    for target in (entry.get("kill_package"), pkg):
        if target:
            try:
                adb(serial, "shell", "am", "force-stop", target)
            except RuntimeError:
                pass
    time.sleep(2)
    args = ["shell", "am", "start", "-a", "android.intent.action.VIEW", "-d", url]
    if pkg:
        args += ["-p", pkg]
    adb(serial, *args, timeout=60)
    time.sleep(settle)


def pullout_showing(title, rect):
    """Is Google's saved-password sheet up?

    hiss.exe answers this with autoocr0 over use_saved_password. Here -- outside the bot,
    with no OCR engine -- the same region answers it structurally: the sheet is a light
    slab, so its bright-pixel fraction jumps from ~nothing to a large share of the box.
    """
    shot = os.path.join(os.environ.get("TEMP", "/tmp"), "_pullout_probe.png")
    AM.capture(title, shot)
    from PIL import Image
    with Image.open(shot) as im:
        crop = im.convert("L").crop((rect[0], rect[1], rect[2] + 1, rect[3] + 1))
    px = list(crop.getdata())
    bright = sum(1 for v in px if v > 165) / float(len(px) or 1)
    print("   pullout probe: %.1f%% bright in use_saved_password" % (bright * 100))
    return bright > 0.05


def enter_credentials(serial, m, rows, email, password, dry, settle):
    print("email field:")
    hide_keyboard(serial, dry)                 # start from the un-scrolled layout
    time.sleep(settle * 0.5)
    tap(serial, m.centre(rows["acr_email"]["rect"]), dry); time.sleep(settle)
    clear_field(serial, dry)
    type_text(serial, email, dry)
    time.sleep(settle)

    # Put the keyboard away BEFORE aiming at the next field. With it up the page is
    # scrolled, so a tap computed from the un-scrolled reference capture lands on the
    # wrong input -- which is exactly how the email and password ended up in each
    # other's boxes. TAB was tried instead and does not move focus in this WebView.
    hide_keyboard(serial, dry)
    time.sleep(settle)

    print("password field:")
    tap(serial, m.centre(rows["acr_password"]["rect"]), dry)
    time.sleep(settle)
    clear_field(serial, dry)
    type_text(serial, password, dry, secret=True)
    time.sleep(settle)

    hide_keyboard(serial, dry)
    time.sleep(0.6)

    print("login button:")
    tap(serial, m.centre(rows["acr_login_button"]["rect"]), dry)


def login(port, map_name=None, process="login", step=1, dry=False, settle=1.2, attempts=3):
    serial, entry = device_for_port(port)
    title = entry.get("window") or ("A17" if "A17" in entry.get("note", "") else None)
    if not title:
        raise SystemExit("no scrcpy window title known for port %s (add \"window\" to "
                         "settings.automation_devices)" % port)
    map_name = map_name or ("automation_%s" % title.lower())

    tm = AM.map_id(map_name)
    rows = {r["name"]: r for r in AM.regions(tm, process, step)}
    # Step 2 of the login process is the interception: Google's "Use saved password?"
    # sheet, and the blank strip above it we tap to make it go away.
    guard = {r["name"]: r for r in AM.regions(tm, process, 2)}
    for need in ("acr_email", "acr_password", "acr_login_button"):
        if need not in rows:
            raise SystemExit("map %s has no region %r for %s step %d" % (map_name, need, process, step))

    win, dev = window_size(title), device_size(serial)

    # Regions are stored in the pixels of the REFERENCE capture they were drawn on. The
    # mirror window can legitimately be a different size now (someone resized it, scrcpy
    # picked another default), so rescale rather than demand an exact match -- an unnoticed
    # size change silently aims every tap at the wrong place.
    ref = AM.screenshot_size(tm, process, step)
    if ref and ref != win:
        fx, fy = win[0] / float(ref[0]), win[1] / float(ref[1])
        print("window is %s but the map was drawn at %s -- scaling regions x%.4f, y%.4f"
              % (win, ref, fx, fy))
        for d in (rows, guard):
            for r in d.values():
                l, t, rr, b = r["rect"]
                r["rect"] = (l * fx, t * fy, rr * fx, b * fy)

    m = Mapper(win, dev)
    print("device %s  window %s -> device %s  (scale %.4f, offset %.1f,%.1f)"
          % (serial, win, dev, m.scale, m.ox, m.oy))

    email, password = fetch_credentials(port)
    print("credentials for port %d: %s / %d chars" % (port, email, len(password)))

    suppress_prompts(serial, dry)
    for attempt in range(1, attempts + 1):
        print("-- attempt %d --" % attempt)
        enter_credentials(serial, m, rows, email, password, dry, settle)
        time.sleep(settle * 2)

        if dry or "use_saved_password" not in guard or "pullout_dismiss" not in guard:
            return True
        if not pullout_showing(title, guard["use_saved_password"]["rect"]):
            print("no saved-password pullout -- login submitted")
            return True

        # It ate the entry. Tap the blank strip ABOVE the sheet to dismiss, then
        # start the whole entry again: the fields it intercepted are not reliable.
        print("saved-password pullout is up -- dismissing above it and retrying")
        tap(serial, m.centre(guard["pullout_dismiss"]["rect"]), dry)
        time.sleep(settle * 2)

    print("gave up after %d attempts" % attempts)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True, help="terminal port of the instance")
    ap.add_argument("--map", default=None)
    ap.add_argument("--process", default="login")
    ap.add_argument("--step", type=int, default=1)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--attempts", type=int, default=3)
    ap.add_argument("--open-app", action="store_true", help="restart the ACR client first")
    ap.add_argument("--suppress-only", action="store_true", help="just silence autofill/assistant")
    ap.add_argument("--restore-prompts", action="store_true", help="put those settings back")
    a = ap.parse_args()
    if a.open_app:
        open_app(a.port, a.dry_run)
    if a.suppress_only or a.restore_prompts:
        serial, _ = device_for_port(a.port)
        restore_prompts(serial) if a.restore_prompts else suppress_prompts(serial, a.dry_run)
        return
    login(a.port, a.map, a.process, a.step, dry=a.dry_run, attempts=a.attempts)


if __name__ == "__main__":
    main()
