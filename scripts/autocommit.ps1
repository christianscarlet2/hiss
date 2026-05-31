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

    $message = "autocommit"
    if ($null -ne $newestChange) {
        $relativePath = Resolve-Path -LiteralPath $newestChange.FullName -Relative
        $relativePath = $relativePath.TrimStart(".", "\", "/")
        $message = "autocommit: latest change in $relativePath"
    }

    $message = "$message ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"

    & git add -A
    $commitOutput = & git commit -m $message 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw ($commitOutput -join "`n")
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
}
catch {
    Write-AutoCommitLog "ERROR: $($_.Exception.Message)"
    exit 1
}
