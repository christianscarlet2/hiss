#!/usr/bin/env python3
# Claude Code PostToolUse hook: after any *.ohf edit, run the hardened OHF
# validator (build_and_lint.py) and, on failure, feed the errors straight back
# to Claude (exit code 2 -> stderr is shown to the model) so it self-corrects
# BEFORE the bad strategy ever reaches the running bot.
#
# Catches the class the text-concat lint used to miss, e.g. the OpenPPL parser
# rejects '<>' / '!=' (there is no not-equal operator; use NOT (a = b)).
import sys, os, json, subprocess

REPO = os.environ.get("HISS_REPO", r"C:\www\openholdembot_old")

def main():
    try:
        data = json.load(sys.stdin)
    except Exception:
        sys.exit(0)  # nothing to do
    ti = data.get("tool_input", {}) or {}
    fp = str(ti.get("file_path") or ti.get("path") or "")
    if not fp.lower().endswith(".ohf"):
        sys.exit(0)  # not an OHF edit; ignore

    lintdir = os.path.join(REPO, ".strategy_build")
    if not os.path.isfile(os.path.join(lintdir, "build_and_lint.py")):
        sys.exit(0)
    try:
        proc = subprocess.run([sys.executable, "build_and_lint.py"], cwd=lintdir,
                              capture_output=True, text=True, timeout=120)
    except Exception as e:
        sys.stderr.write("OHF validator could not run: %s\n" % e)
        sys.exit(0)  # don't block on tooling failure

    if proc.returncode != 0:
        sys.stderr.write("\n[OHF VALIDATION FAILED] after editing %s\n" % fp)
        sys.stderr.write((proc.stdout or "") + (proc.stderr or ""))
        sys.stderr.write("\nFix every error above before deploying or restarting Hiss. "
                         "Reminder: OpenPPL has NO not-equal operator -- rewrite '<>' / '!=' "
                         "as NOT (a = b). Re-run validate_ohf to confirm green.\n")
        sys.exit(2)  # PostToolUse: surfaces stderr to Claude for self-correction
    sys.exit(0)

if __name__ == "__main__":
    main()
