#!/usr/bin/env python3
"""PostToolUse hook — closely couple Windows Hiss <-> hiss-linux. [Emrald 2026-06-22]

Whenever an ENGINE source file under Hiss\\ is edited (Edit/Write/MultiEdit) AND a same-named file exists in
the Linux port (snapshot in mcp/hiss_linux_manifest.txt), this reminds Claude to apply the EQUIVALENT change
to /var/www/hiss-linux/engine/oh/<file> on swiftsnake and rebuild — so a feature, modification, or bug fix to
Hiss is mirrored to hiss-linux and both engines stay in lockstep. It also appends a durable checkbox to
Release/logs/hiss_linux_sync_pending.md. Windows-only files (MFC/WinHTTP/GDI/UI) are flagged "likely skip".

Reads the PostToolUse payload on stdin; emits hookSpecificOutput.additionalContext (non-blocking, exit 0)."""
import sys, json, os, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(REPO, "mcp", "hiss_linux_manifest.txt")
PENDING = os.path.join(REPO, "Release", "logs", "hiss_linux_sync_pending.md")
# substrings that mark a Windows-only TU (no faithful Linux counterpart): MFC UI / WinHTTP / GDI overlay / docs
WIN_ONLY = ("hudoverlaywindow", "chatterminalserver", "mainfrm", "openholdemdoc", "openholdemview",
            "dialog", "dlg", "menu", "cscarletbeast", "messagebox", "toolbar")


def emit(ctx):
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext": ctx}}))


def main():
    try:
        data = json.load(sys.stdin)
    except Exception:
        return
    fp = (data.get("tool_input") or {}).get("file_path") or ""
    if not fp:
        return
    low = fp.replace("\\", "/").lower()
    if "/hiss/" not in low or not low.endswith((".cpp", ".h")):
        return                                  # only Windows Hiss engine source
    base = os.path.basename(fp.replace("\\", "/"))
    try:
        manifest = set(l.strip() for l in open(MANIFEST, encoding="utf-8") if l.strip())
    except Exception:
        manifest = set()
    if base not in manifest:
        return                                  # no hiss-linux counterpart -> nothing to couple
    win_only = any(w in base.lower() for w in WIN_ONLY)
    note = ("  NOTE: this looks Windows-only (MFC/WinHTTP/GDI/UI) — usually SKIP, but verify it isn't shared engine logic."
            if win_only else "")
    emit("HISS<->HISS-LINUX COUPLING: you edited Hiss/%s, which has a Linux counterpart at "
         "/var/www/hiss-linux/engine/oh/%s (builds against the compat shim). If this is ENGINE LOGIC "
         "(feature / modification / bug fix, not Windows-only), apply the EQUIVALENT change there "
         "(ssh asterisk@192.168.1.39) and rebuild with ./build.sh so both engines stay in lockstep.%s "
         "Tracked in Release/logs/hiss_linux_sync_pending.md." % (base, base, note))
    try:
        os.makedirs(os.path.dirname(PENDING), exist_ok=True)
        with open(PENDING, "a", encoding="utf-8") as f:
            f.write("- [ ] %s  Hiss/%s edited -> mirror to hiss-linux engine/oh/%s%s\n"
                    % (time.strftime("%Y-%m-%d %H:%M"), base, base, "  (likely win-only)" if win_only else ""))
    except Exception:
        pass


if __name__ == "__main__":
    main()
