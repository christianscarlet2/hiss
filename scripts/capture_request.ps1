# Claude Code "UserPromptSubmit" hook: capture the user's request (prompt) to
# .git\CLAUDE_ORIGINAL_REQUEST so the Stop hook (write_commit_summary.ps1) can fold
# it into the autocommit message alongside the result. The file lives under .git,
# so it is never staged, and it is consumed when the commit message is built.
#
# Never fail the turn: any error is swallowed (the commit just omits the request).

$ErrorActionPreference = 'SilentlyContinue'

try {
    $raw = [Console]::In.ReadToEnd()
    if (-not $raw) { exit 0 }

    $payload = $raw | ConvertFrom-Json
    $prompt = $payload.prompt
    if (-not $prompt -or $prompt.Trim() -eq '') { exit 0 }

    $repo = "C:\www\openholdembot_old"
    $gitDir = Join-Path $repo ".git"
    if (-not (Test-Path -LiteralPath $gitDir)) { exit 0 }

    $outFile = Join-Path $gitDir "CLAUDE_ORIGINAL_REQUEST"
    # UTF-8 without BOM (the message is later committed via `git commit -F`).
    [System.IO.File]::WriteAllText($outFile, $prompt.Trim(), (New-Object System.Text.UTF8Encoding($false)))
}
catch {
    # Swallow everything: the commit just omits the original request.
}
exit 0
