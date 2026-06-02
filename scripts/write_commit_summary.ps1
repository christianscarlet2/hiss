# Claude Code "Stop" hook: runs after every request/response. It does two things:
#   1. Builds a VERBOSE commit message in .git\CLAUDE_COMMIT_MSG containing both
#        * the original request (captured by capture_request.ps1 on UserPromptSubmit)
#        * the result           (the full text of the last assistant message)
#      formatted as: <subject>\n\nOriginal request:\n...\n\nResult:\n...
#   2. Immediately commits + pushes by invoking scripts\autocommit.ps1, so every
#      turn is captured right away instead of waiting for the periodic autocommit
#      watcher. autocommit.ps1 commits CLAUDE_COMMIT_MSG verbatim (via `git commit
#      -F`, nothing truncated), consumes it, and pushes to origin (main only).
#
# The hook receives a JSON payload on stdin that includes "transcript_path" (the
# conversation .jsonl). Never fail the turn: any error leaves the fallback message
# in place, and the commit step is always attempted regardless.

$ErrorActionPreference = 'SilentlyContinue'

$repo = "C:\www\openholdembot_old"
$gitDir = Join-Path $repo ".git"

# ---- 1. Best-effort: build a verbose commit message from the transcript. ----
try {
    $raw = [Console]::In.ReadToEnd()
    if ($raw -and (Test-Path -LiteralPath $gitDir)) {
        $payload = $raw | ConvertFrom-Json
        $transcript = $payload.transcript_path

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

        if ($request -or $result) {
            # Subject = first meaningful line (of the result, else the request),
            # stripped of markdown noise. Kept short (git convention); the full
            # detail goes in the body.
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

            # Prefer a concise, Claude-written subject (a short line describing just the
            # change) from <gitdir>\CLAUDE_COMMIT_SUBJECT, so the commit's first line
            # reads cleanly instead of echoing the whole reply. Consume it after use.
            $subject = $null
            $subjFile = Join-Path $gitDir "CLAUDE_COMMIT_SUBJECT"
            if (Test-Path -LiteralPath $subjFile) {
                $s = Get-Content -LiteralPath $subjFile -Raw
                if ($s) { $subject = (($s -split "`n")[0]).Trim() }
                Remove-Item -LiteralPath $subjFile -Force -ErrorAction SilentlyContinue
            }
            # Fallback when no concise subject was written: first meaningful line of the
            # result (else the request), stripped of markdown noise.
            if (-not $subject) { $subject = Get-FirstLine $result }
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
            # UTF-8 WITHOUT a BOM: autocommit.ps1 does `git commit -F` on this file
            # directly, and a BOM would land as a stray glyph at the subject start.
            [System.IO.File]::WriteAllText($outFile, ($sb.ToString().TrimEnd() + "`n"), (New-Object System.Text.UTF8Encoding($false)))

            # Consume the request: it is now embedded in the commit message. The next
            # UserPromptSubmit writes a fresh one.
            Remove-Item -LiteralPath $reqFile -Force -ErrorAction SilentlyContinue
        }
    }
}
catch {
    # Swallow everything: the commit just falls back to autocommit.ps1's default message.
}

# ---- 2. Always commit + push this turn's changes right now. ----
# autocommit.ps1 stages everything, commits CLAUDE_COMMIT_MSG (or its own verbose
# fallback when none was written above), and pushes to origin on main. It logs and
# swallows its own errors (offline/auth/non-fast-forward never fail the turn), so a
# failed push is only a warning -- the periodic watcher remains the safety net.
try {
    $autocommit = Join-Path $repo "scripts\autocommit.ps1"
    if ((Test-Path -LiteralPath $gitDir) -and (Test-Path -LiteralPath $autocommit)) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $autocommit | Out-Null
    }
}
catch {
    # Never fail the turn on a commit/push problem.
}

exit 0
