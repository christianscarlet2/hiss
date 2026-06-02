param(
    [string]$RepoPath = "C:\www\openholdembot_old"
)

$ErrorActionPreference = "Stop"

function Write-AutoCommitLog {
    param([string]$Message)

    $logPath = Join-Path $RepoPath "scripts\autocommit.log"
    $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    Add-Content -LiteralPath $logPath -Value "[$timestamp] $Message"
}

try {
    if (-not (Test-Path -LiteralPath $RepoPath)) {
        throw "Repository path does not exist: $RepoPath"
    }

    Set-Location -LiteralPath $RepoPath

    $repoRoot = (& git rev-parse --show-toplevel 2>$null).Trim()
    if (-not $repoRoot) {
        throw "Not a git repository: $RepoPath"
    }

    $statusLines = @(& git status --porcelain)
    if ($statusLines.Count -eq 0) {
        Write-AutoCommitLog "No changes to commit."
        exit 0
    }

    $changedFiles = @(
        $statusLines |
            ForEach-Object {
                $path = $_.Substring(3)
                if ($path -match " -> ") {
                    $path = ($path -split " -> ")[-1]
                }
                $path.Trim('"')
            } |
            Where-Object { $_ -ne "" }
    )

    $newestChange = $null
    foreach ($file in $changedFiles) {
        $fullPath = Join-Path $repoRoot $file
        if (Test-Path -LiteralPath $fullPath) {
            $item = Get-Item -LiteralPath $fullPath
            if ($null -eq $newestChange -or $item.LastWriteTime -gt $newestChange.LastWriteTime) {
                $newestChange = $item
            }
        }
    }

    # Prefer a Claude-authored summary of the work (written to
    # .git\CLAUDE_COMMIT_MSG) so commits describe WHAT was done, not just which
    # file changed last. The file lives under .git, so it is never staged itself,
    # and it is consumed (deleted) after a successful commit. When it is absent we
    # fall back to the "autocommit: latest change in <file>" message.
    $gitDir = (& git rev-parse --git-dir 2>$null).Trim()
    $summaryFile = if ($gitDir) { Join-Path $gitDir "CLAUDE_COMMIT_MSG" } else { $null }

    $message = $null
    $usedSummary = $false
    if ($summaryFile -and (Test-Path -LiteralPath $summaryFile)) {
        $summary = Get-Content -LiteralPath $summaryFile -Raw
        if ($summary -and $summary.Trim() -ne "") {
            $message = $summary.Trim()
            $usedSummary = $true
        }
    }

    if (-not $usedSummary) {
        $message = "autocommit"
        if ($null -ne $newestChange) {
            $relativePath = Resolve-Path -LiteralPath $newestChange.FullName -Relative
            $relativePath = $relativePath.TrimStart(".", "\", "/")
            $message = "autocommit: latest change in $relativePath"
        }
        $message = "$message ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
    }

    & git add -A
    if ($usedSummary) {
        # Commit straight from the file so newlines and special characters (e.g.
        # quotes) in the summary survive -- passing them via -m through PowerShell
        # 5.1's native-arg handling corrupts the message.
        $commitOutput = & git commit -F $summaryFile 2>&1
    } else {
        $commitOutput = & git commit -m $message 2>&1
    }
    if ($LASTEXITCODE -ne 0) {
        throw ($commitOutput -join "`n")
    }

    # Consume the summary so it is used for exactly one commit.
    if ($usedSummary) {
        Remove-Item -LiteralPath $summaryFile -Force -ErrorAction SilentlyContinue
    }

    Write-AutoCommitLog "Committed changes with message: $message"

    # Push the committed changes to origin, but ONLY when on the main branch.
    # A push failure (e.g. offline, auth, non-fast-forward) must NOT fail the
    # script: the commit already succeeded locally and will be pushed later.
    $currentBranch = (& git rev-parse --abbrev-ref HEAD).Trim()
    if ($currentBranch -eq "main") {
        # git push writes its normal output to stderr. Under
        # $ErrorActionPreference = "Stop", '2>&1' would turn that first stderr
        # line into a terminating NativeCommandError before we can inspect the
        # exit code, so relax the preference just for the push and rely on
        # $LASTEXITCODE to determine success.
        $previousErrorAction = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $pushOutput = & git push origin main 2>&1
        $pushExitCode = $LASTEXITCODE
        $ErrorActionPreference = $previousErrorAction

        if ($pushExitCode -eq 0) {
            Write-AutoCommitLog "Pushed branch 'main' to origin."
        }
        else {
            Write-AutoCommitLog "WARNING: push of branch 'main' to origin failed: $(($pushOutput | Out-String).Trim() -replace '\s+', ' ')"
        }
    }
    else {
        Write-AutoCommitLog "On branch '$currentBranch' (not main); skipping push."
    }

    # A failed push is only a warning above; make sure its native exit code does
    # not become the script's process exit code after a successful commit.
    exit 0
}
catch {
    Write-AutoCommitLog "ERROR: $($_.Exception.Message)"
    exit 1
}
