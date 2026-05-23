#!/usr/bin/env pwsh
# OCR Name Mapping Migration Script
# This script creates the ocr_name_mappings table in your PokerTracker 4 database
# and populates it with verified player names from the player table

param(
    [string]$Host = "localhost",
    [string]$Port = "5432",
    [string]$Username = "postgres",
    [string]$Password = "",
    [string]$Database = "pokertracker",
    [string]$MigrationFile = "PT4_OCRNameMapping_Migration.sql"
)

Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "OCR Name Mapping - PT4 Database Migration" -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan
Write-Host ""

# Validate migration file exists
if (-not (Test-Path $MigrationFile)) {
    Write-Host "ERROR: Migration file not found: $MigrationFile" -ForegroundColor Red
    Write-Host "Expected location: $(Get-Location)\$MigrationFile" -ForegroundColor Red
    exit 1
}

Write-Host "Connection Details:" -ForegroundColor Yellow
Write-Host "  Host: $Host" 
Write-Host "  Port: $Port"
Write-Host "  Username: $Username"
Write-Host "  Database: $Database"
Write-Host ""

# Check if psql is available
$psqlPath = Get-Command psql -ErrorAction SilentlyContinue
if (-not $psqlPath) {
    Write-Host "ERROR: psql (PostgreSQL client) not found in PATH" -ForegroundColor Red
    Write-Host "Please install PostgreSQL client tools" -ForegroundColor Yellow
    exit 1
}

Write-Host "PostgreSQL Client: $($psqlPath.Source)" -ForegroundColor Green
Write-Host ""

# Set environment variable for password if provided
if ($Password) {
    $env:PGPASSWORD = $Password
}

Write-Host "Running migration..." -ForegroundColor Yellow
Write-Host ""

# Run the migration
try {
    $output = & psql -h $Host -p $Port -U $Username -d $Database -f $MigrationFile 2>&1
    $exitCode = $LASTEXITCODE
    
    if ($exitCode -eq 0) {
        Write-Host $output -ForegroundColor Green
        Write-Host ""
        Write-Host "==================================================" -ForegroundColor Green
        Write-Host "Migration completed successfully!" -ForegroundColor Green
        Write-Host "==================================================" -ForegroundColor Green
        Write-Host ""
        Write-Host "Next steps:" -ForegroundColor Cyan
        Write-Host "  1. Rebuild OpenHoldem solution in Visual Studio"
        Write-Host "  2. Start OpenHoldem and configure PT4 connection if needed"
        Write-Host "  3. Begin scraping - verified player names will show in BRIGHT GREEN"
        Write-Host ""
    } else {
        Write-Host $output -ForegroundColor Red
        Write-Host ""
        Write-Host "ERROR: Migration failed with exit code $exitCode" -ForegroundColor Red
        Write-Host ""
        Write-Host "Troubleshooting:" -ForegroundColor Yellow
        Write-Host "  - Verify PostgreSQL credentials are correct"
        Write-Host "  - Ensure PokerTracker 4 database is running"
        Write-Host "  - Check that you have CREATE TABLE permissions"
        Write-Host "  - Try connecting manually: psql -h $Host -U $Username -d $Database"
        exit 1
    }
} catch {
    Write-Host "ERROR: Failed to run migration: $_" -ForegroundColor Red
    exit 1
} finally {
    # Clear password from environment
    if ($env:PGPASSWORD) {
        Remove-Item env:PGPASSWORD -ErrorAction SilentlyContinue
    }
}
