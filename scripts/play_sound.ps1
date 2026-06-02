# Plays a PCM .wav through the sound card (used by the Claude Code Stop /
# Notification hooks). PlaySync blocks until the clip finishes so the sound is not
# cut off when this short-lived hook process exits. Never fails the turn: a missing
# file or audio error is swallowed.
param([string]$File)
$ErrorActionPreference = 'SilentlyContinue'
try {
    if ($File -and (Test-Path -LiteralPath $File)) {
        (New-Object System.Media.SoundPlayer $File).PlaySync()
    }
} catch {}
exit 0
