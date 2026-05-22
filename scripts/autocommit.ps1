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
}
catch {
    Write-AutoCommitLog "ERROR: $($_.Exception.Message)"
    exit 1
}
