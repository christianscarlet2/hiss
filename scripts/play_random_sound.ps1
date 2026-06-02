# Plays a RANDOM PCM .wav from a folder (used by the Claude Code Stop hook so the
# completion sound varies each turn). PlaySync blocks until the clip finishes so it
# is not cut off when this short-lived hook process exits. Never fails the turn: a
# missing folder, no files, or an audio error is swallowed.
param([string]$Folder)
$ErrorActionPreference = 'SilentlyContinue'
try {
    if ($Folder -and (Test-Path -LiteralPath $Folder)) {
        $files = @(Get-ChildItem -LiteralPath $Folder -Filter *.wav -File)
        if ($files.Count -gt 0) {
            $pick = $files | Get-Random
            (New-Object System.Media.SoundPlayer $pick.FullName).PlaySync()
        }
    }
} catch {}
exit 0
