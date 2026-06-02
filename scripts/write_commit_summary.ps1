# Claude Code "Stop" hook: write a one-line summary of the work just completed
# to .git\CLAUDE_COMMIT_MSG so the autocommit watcher (scripts\autocommit.ps1)
# uses it as the commit message instead of the "latest change in <file>" fallback.
#
# The hook receives a JSON payload on stdin that includes "transcript_path"
# (the conversation .jsonl). We read the last assistant text message and use its
# first meaningful line as the commit subject.
#
# Never fail the turn: any error just leaves the fallback message in place.

$ErrorActionPreference = 'SilentlyContinue'

try {
    $raw = [Console]::In.ReadToEnd()
    if (-not $raw) { exit 0 }

    $payload = $raw | ConvertFrom-Json
    $transcript = $payload.transcript_path
    if (-not $transcript -or -not (Test-Path -LiteralPath $transcript)) { exit 0 }

    $repo = "C:\www\openholdembot_old"
    $gitDir = Join-Path $repo ".git"
    if (-not (Test-Path -LiteralPath $gitDir)) { exit 0 }

    # Walk the transcript from the end and find the last assistant text content.
    $lines = Get-Content -LiteralPath $transcript
    $summary = $null
    for ($i = $lines.Count - 1; $i -ge 0; $i--) {
        $line = $lines[$i]
        if (-not $line) { continue }
        $obj = $null
        try { $obj = $line | ConvertFrom-Json } catch { continue }
        $msg = $obj.message
        if (-not $msg) { continue }
        if ($msg.role -ne 'assistant') { continue }

        $text = ''
        foreach ($block in $msg.content) {
            if ($block.type -eq 'text' -and $block.text) { $text += $block.text + "`n" }
        }
        if ($text.Trim() -eq '') { continue }

        # First non-empty line, stripped of common markdown noise.
        foreach ($candidate in ($text -split "`n")) {
            $c = $candidate.Trim()
            if ($c -eq '') { continue }
            $c = $c -replace '^[#>\-\*\s]+', ''          # leading markdown bullets/headers
            $c = $c -replace '\*\*|__|`', ''               # bold/code markers
            $c = $c.Trim()
            if ($c.Length -ge 8) { $summary = $c; break }
        }
        break   # only inspect the last assistant message
    }

    if (-not $summary) { exit 0 }
    if ($summary.Length -gt 100) { $summary = $summary.Substring(0, 100).TrimEnd() }

    $outFile = Join-Path $gitDir "CLAUDE_COMMIT_MSG"
    # Write UTF-8 WITHOUT a BOM: autocommit.ps1 does `git commit -F` on this file
    # directly, and a BOM would land as a stray glyph at the start of the subject.
    [System.IO.File]::WriteAllText($outFile, $summary, (New-Object System.Text.UTF8Encoding($false)))
}
catch {
    # Swallow everything: the commit just falls back to the default message.
}
exit 0
