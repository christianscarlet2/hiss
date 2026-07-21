# Claude Code "Stop" hook sound. Asks the local sb-soundpad sound server
# (C:\www\sb-soundpad\sb_soundpad.py, http://127.0.0.1:7895) to play a RANDOM completion
# sound; the server does the playback, so this is a quick fire-and-forget GET. The
# folder argument from the previous local-wav version is accepted but ignored. Never
# fails the turn: if the server isn't running the error is swallowed.
$ErrorActionPreference = 'SilentlyContinue'
# Stay silent for Hiss's own headless `claude -p` calls. Hiss shells out to Claude constantly
# while it plays (vision_driver reading tablemap features off a screencap, hud_calibrate_driver,
# deep_thought, decision_advisor, astrology, growth), and EVERY one of those ends a Claude
# session, which fires this Stop hook -- so the box chimes over and over with nobody to hear it.
# HISS_NO_CHIME is set once in launch_hiss.py and inherited by the whole Hiss process tree
# (python -> claude.exe -> this hook), so an INTERACTIVE session, which never has it set, still
# chimes normally. Suppress the sound, not the hook: the hook must still exit 0 quickly.
if ($env:HISS_NO_CHIME) { exit 0 }
try {
    Invoke-WebRequest -Uri 'http://127.0.0.1:7895/complete' -UseBasicParsing -TimeoutSec 4 | Out-Null
} catch {}
exit 0
