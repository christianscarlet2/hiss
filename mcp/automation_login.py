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
    # errors="replace": dumpsys emits bytes that the console codepage cannot decode, and
    # a UnicodeDecodeError here would take down a login half-way through.
    r = subprocess.run([ADB, "-s", serial] + list(args), capture_output=True, text=True,
                       encoding="utf-8", errors="replace",
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


def keyboard_showing(serial):
    """Is the IME actually up? BACK is only safe to send when it is."""
    try:
        out = adb(serial, "shell", "dumpsys", "input_method", timeout=30) or ""
    except (RuntimeError, subprocess.TimeoutExpired):
        return False                              # unknown -> assume down, never send BACK
    m = re.search(r"mInputShown=(\w+)", out)
    return bool(m) and m.group(1).lower() == "true"


def hide_keyboard(serial, dry):
    """Put the soft keyboard away -- and ONLY that.

    BACK closes the keyboard when it is showing. When it is NOT showing, BACK is a
    navigation: in this WebAPK it walks out of the app to the launcher, which is how a
    run ended up typing into the home screen (and, earlier, into the Play Store). So
    check first and send nothing if there is no keyboard to dismiss.
    """
    if dry or not keyboard_showing(serial):
        return
    adb(serial, "shell", "input", "keyevent", "4")
    time.sleep(0.4)


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


TESSERACT = os.environ.get("HISS_TESSERACT",
                           r"C:\www\openholdembot_old\tesseract-demo\tesseract\tesseract.exe")


def grab_region(title, rect, pad=2):
    """Fresh capture of the mirror, cropped to one region."""
    shot = os.path.join(os.environ.get("TEMP", "/tmp"), "_verify_probe.png")
    AM.capture(title, shot)
    from PIL import Image
    l, t, r, b = (int(round(v)) for v in rect)
    with Image.open(shot) as im:
        return im.convert("RGB").crop((l + pad, t + pad, r - pad, b - pad))


def ocr_region(title, rect):
    """What does this field actually say? Used to CHECK what was typed, not to guess."""
    crop = grab_region(title, rect)
    png = os.path.join(os.environ.get("TEMP", "/tmp"), "_verify_crop.png")
    crop.save(png)
    r = subprocess.run([TESSERACT, png, "stdout", "--psm", "7", "-l", "eng"],
                       capture_output=True, text=True, timeout=60, creationflags=NO_WINDOW)
    return " ".join((r.stdout or "").split()).strip()


def field_ink(title, rect):
    """How many glyph clusters are in the box? Counts a masked password's dots, which OCR
    cannot read, and answers 'is this field empty' for either field."""
    crop = grab_region(title, rect).convert("L")
    import numpy as np
    a = np.asarray(crop).astype(int)
    dark = (a < 128).sum(axis=0)                 # ink per column, on the white field
    clusters, run = 0, False
    for v in dark:
        if v > 0 and not run:
            clusters += 1; run = True
        elif v == 0:
            run = False
    return clusters


def field_ink_columns(title, rect):
    """Width of the ink in a box -- answers "is anything in here" for a masked field."""
    crop = grab_region(title, rect).convert("L")
    import numpy as np
    a = np.asarray(crop).astype(int)
    return int(((a < 128).sum(axis=0) > 0).sum())


def pullout_showing(title, guard):
    """Is Chrome's "Use saved password?" sheet up?

    Read the use_saved_password region and look for the words. This is the same rule
    hiss.exe applies with autoocr0 on that region -- note the space-stripped compare,
    because OCR of that line comes back with the spacing anywhere or nowhere.

    (Suppressing the Android autofill_service does NOT stop this: it is Chrome's own
    password manager, inside the WebAPK.)
    """
    if "use_saved_password" not in guard:
        return False
    txt = ocr_region(title, guard["use_saved_password"]["rect"])
    hit = "usesavedpassword" in txt.replace(" ", "").lower()
    if hit:
        print("      pullout detected: %r" % txt)
    return hit


def dismiss_pullout(serial, m, guard, dry, settle):
    """Tap the blank strip ABOVE the sheet -- tapping the sheet itself would accept it."""
    if "pullout_dismiss" not in guard:
        return False
    print("      dismissing the saved-password pullout")
    tap(serial, m.centre(guard["pullout_dismiss"]["rect"]), dry)
    time.sleep(settle * 1.5)
    return True


def fill_field(serial, title, m, region, text, dry, settle, secret, label,
               guard=None, email_region=None, email_text=None, tries=3):
    """Focus, clear, type -- then READ THE FIELD BACK and only accept it if it took.

    Every step is checked against a fresh capture rather than assumed, because two faults
    are invisible otherwise: the page autofills a previously SAVED (and, from an early
    failed attempt, scrambled) credential, and Chrome's saved-password sheet can steal the
    focus mid-sequence.
    """
    guard = guard or {}
    for attempt in range(1, tries + 1):
        print("   %s: attempt %d" % (label, attempt))
        hide_keyboard(serial, dry)
        time.sleep(settle * 0.5)
        tap(serial, m.centre(region["rect"]), dry)
        time.sleep(settle)

        # Touching a credential field is what raises the sheet; clear it and re-focus.
        if not dry and pullout_showing(title, guard):
            dismiss_pullout(serial, m, guard, dry, settle)
            tap(serial, m.centre(region["rect"]), dry)
            time.sleep(settle)

        clear_field(serial, dry)
        time.sleep(settle * 0.5)
        type_text(serial, text, dry, secret=secret)
        time.sleep(settle)
        if dry:
            return True

        hide_keyboard(serial, dry)
        time.sleep(settle * 0.6)

        # A sheet that appears AFTER typing has swallowed the keystrokes: what is left in
        # the box is Chrome's autofilled value, not what was sent. Dismissing alone would
        # leave that stale value behind and it verifies as "something is there" -- which is
        # how a wrong password reached the submit. Dismiss AND type it again.
        if not dry and pullout_showing(title, guard):
            dismiss_pullout(serial, m, guard, dry, settle)
            print("      the sheet swallowed the keystrokes -- retyping")
            continue

        if secret:
            # The mask glyphs run together into one blob, so counting them is useless.
            # What matters is that SOMETHING landed here and that it did not land in the
            # email box instead -- the exact failure that produced the swapped fields.
            ink = field_ink_columns(title, region["rect"])
            still = ocr_region(title, email_region["rect"]) if email_region else ""
            same = (not email_text) or still.replace(" ", "").lower() == email_text.replace(" ", "").lower()
            ok = ink > 20 and same
            print("      %d ink columns, email still %r -> %s"
                  % (ink, still, "ok" if ok else "WRONG"))
        else:
            got = ocr_region(title, region["rect"])
            ok = got.replace(" ", "").lower() == text.replace(" ", "").lower()
            print("      reads %r -> %s" % (got, "ok" if ok else "WRONG"))
        if ok:
            return True
    return False


def enter_credentials(serial, m, rows, email, password, dry, settle, title=None, guard=None):
    if not fill_field(serial, title, m, rows["acr_email"], email, dry, settle, False, "email",
                      guard=guard):
        return False
    if not fill_field(serial, title, m, rows["acr_password"], password, dry, settle, True, "password",
                      guard=guard, email_region=rows["acr_email"], email_text=email):
        return False

    hide_keyboard(serial, dry)
    time.sleep(0.6)
    print("login button:")
    tap(serial, m.centre(rows["acr_login_button"]["rect"]), dry)
    return True


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


TESSERACT = os.environ.get("HISS_TESSERACT",
                           r"C:\www\openholdembot_old\tesseract-demo\tesseract\tesseract.exe")


def grab_region(title, rect, pad=2):
    """Fresh capture of the mirror, cropped to one region."""
    shot = os.path.join(os.environ.get("TEMP", "/tmp"), "_verify_probe.png")
    AM.capture(title, shot)
    from PIL import Image
    l, t, r, b = (int(round(v)) for v in rect)
    with Image.open(shot) as im:
        return im.convert("RGB").crop((l + pad, t + pad, r - pad, b - pad))


def ocr_region(title, rect):
    """What does this field actually say? Used to CHECK what was typed, not to guess."""
    crop = grab_region(title, rect)
    png = os.path.join(os.environ.get("TEMP", "/tmp"), "_verify_crop.png")
    crop.save(png)
    r = subprocess.run([TESSERACT, png, "stdout", "--psm", "7", "-l", "eng"],
                       capture_output=True, text=True, timeout=60, creationflags=NO_WINDOW)
    return " ".join((r.stdout or "").split()).strip()


def field_ink(title, rect):
    """How many glyph clusters are in the box? Counts a masked password's dots, which OCR
    cannot read, and answers 'is this field empty' for either field."""
    crop = grab_region(title, rect).convert("L")
    import numpy as np
    a = np.asarray(crop).astype(int)
    dark = (a < 128).sum(axis=0)                 # ink per column, on the white field
    clusters, run = 0, False
    for v in dark:
        if v > 0 and not run:
            clusters += 1; run = True
        elif v == 0:
            run = False
    return clusters


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
        if not enter_credentials(serial, m, rows, email, password, dry, settle, title, guard):
            print("could not get the fields to hold what was typed")
            return False
        time.sleep(settle * 2)

        if dry or "use_saved_password" not in guard or "pullout_dismiss" not in guard:
            return True
        if not pullout_showing(title, guard):
            print("no saved-password pullout -- login submitted")
            return True

        # It ate the entry. Tap the blank strip ABOVE the sheet to dismiss, then
        # start the whole entry again: the fields it intercepted are not reliable.
        print("saved-password pullout is up -- dismissing above it and retrying")
        dismiss_pullout(serial, m, guard, dry, settle)

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
