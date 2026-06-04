# Claude Code "Stop" hook sound. Asks the local sb-soundpad sound server
# (C:\www\sb-soundpad\sb_soundpad.py, http://127.0.0.1:7895) to play a RANDOM completion
# sound; the server does the playback, so this is a quick fire-and-forget GET. The
# folder argument from the previous local-wav version is accepted but ignored. Never
# fails the turn: if the server isn't running the error is swallowed.
$ErrorActionPreference = 'SilentlyContinue'
try {
    Invoke-WebRequest -Uri 'http://127.0.0.1:7895/complete' -UseBasicParsing -TimeoutSec 4 | Out-Null
} catch {}
exit 0
