# Claude Code "Stop" hook: build a VERBOSE commit message in .git\CLAUDE_COMMIT_MSG
# that the autocommit watcher (scripts\autocommit.ps1) commits verbatim (via
# `git commit -F`, so nothing is truncated). The message contains both:
#   * the original request  (captured by capture_request.ps1 on UserPromptSubmit)
#   * the result            (the full text of the last assistant message)
# formatted as: <subject>\n\nOriginal request:\n...\n\nResult:\n...
#
# The hook receives a JSON payload on stdin that includes "transcript_path"
# (the conversation .jsonl). Never fail the turn: any error leaves the fallback
# message in place.

$ErrorActionPreference = 'SilentlyContinue'

try {
    $raw = [Console]::In.ReadToEnd()
    if (-not $raw) { exit 0 }

    $payload = $raw | ConvertFrom-Json
    $transcript = $payload.transcript_path

    $repo = "C:\www\openholdembot_old"
    $gitDir = Join-Path $repo ".git"
    if (-not (Test-Path -LiteralPath $gitDir)) { exit 0 }

    # The original request captured on UserPromptSubmit.
    $reqFile = Join-Path $gitDir "CLAUDE_ORIGINAL_REQUEST"
    $request = $null
    if (Test-Path -LiteralPath $reqFile) {
        $request = Get-Content -LiteralPath $reqFile -Raw
        if ($request) { $request = $request.Trim() }
    }

    # Full text of the last assistant message = the "result" (NOT truncated).
    $result = $null
    if ($transcript -and (Test-Path -LiteralPath $transcript)) {
        $lines = Get-Content -LiteralPath $transcript
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
            if ($text.Trim() -ne '') { $result = $text.Trim() }
            break   # only inspect the last assistant message
        }
    }

    if (-not $request -and -not $result) { exit 0 }

    # Subject = first meaningful line (of the result, else the request), stripped of
    # markdown noise. Kept short (git convention); the full detail goes in the body.
    function Get-FirstLine([string]$s) {
        if (-not $s) { return '' }
        foreach ($cand in ($s -split "`n")) {
            $c = $cand.Trim()
            if ($c -eq '') { continue }
            $c = $c -replace '^[#>\-\*\s]+', ''
            $c = $c -replace '\*\*|__|`', ''
            $c = $c.Trim()
            if ($c.Length -ge 4) { return $c }
        }
        return ''
    }

    $subject = Get-FirstLine $result
    if (-not $subject) { $subject = Get-FirstLine $request }
    if (-not $subject) { $subject = 'autocommit' }
    if ($subject.Length -gt 72) { $subject = $subject.Substring(0, 72).TrimEnd() }

    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine($subject)
    [void]$sb.AppendLine('')
    if ($request) {
        [void]$sb.AppendLine('Original request:')
        [void]$sb.AppendLine($request)
        [void]$sb.AppendLine('')
    }
    if ($result) {
        [void]$sb.AppendLine('Result:')
        [void]$sb.AppendLine($result)
    }

    $outFile = Join-Path $gitDir "CLAUDE_COMMIT_MSG"
    # UTF-8 WITHOUT a BOM: autocommit.ps1 does `git commit -F` on this file directly,
    # and a BOM would land as a stray glyph at the start of the subject.
    [System.IO.File]::WriteAllText($outFile, ($sb.ToString().TrimEnd() + "`n"), (New-Object System.Text.UTF8Encoding($false)))

    # Consume the request: it is now embedded in the commit message. The next
    # UserPromptSubmit writes a fresh one.
    Remove-Item -LiteralPath $reqFile -Force -ErrorAction SilentlyContinue
}
catch {
    # Swallow everything: the commit just falls back to the default message.
}
exit 0
