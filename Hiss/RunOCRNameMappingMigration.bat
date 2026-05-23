@echo off
REM OCR Name Mapping Migration Script (Batch Version)
REM This script creates the ocr_name_mappings table in your PokerTracker 4 database

setlocal enabledelayedexpansion

echo.
echo ==================================================
echo OCR Name Mapping - PT4 Database Migration
echo ==================================================
echo.

REM Default values - adjust these for your system
set "PT4_HOST=localhost"
set "PT4_PORT=5432"
set "PT4_USER=postgres"
set "PT4_DATABASE=pokertracker"
set "MIGRATION_FILE=PT4_OCRNameMapping_Migration.sql"

echo Connection Details:
echo   Host: !PT4_HOST!
echo   Port: !PT4_PORT!
echo   User: !PT4_USER!
echo   Database: !PT4_DATABASE!
echo.

REM Check if migration file exists
if not exist !MIGRATION_FILE! (
    echo ERROR: Migration file not found: !MIGRATION_FILE!
    echo Please ensure this script is in the same directory as !MIGRATION_FILE!
    pause
    exit /b 1
)

REM Check if psql is available
where psql >nul 2>nul
if errorlevel 1 (
    echo ERROR: psql ^(PostgreSQL client^) not found in PATH
    echo Please install PostgreSQL client tools or add PostgreSQL to your PATH
    echo.
    echo You can download PostgreSQL from: https://www.postgresql.org/download/windows/
    pause
    exit /b 1
)

echo PostgreSQL Client found in PATH
echo.
echo Running migration...
echo.

REM Run the migration
REM Note: You will be prompted for the password if one is required
psql -h !PT4_HOST! -p !PT4_PORT! -U !PT4_USER! -d !PT4_DATABASE! -f !MIGRATION_FILE!

if errorlevel 1 (
    echo.
    echo ERROR: Migration failed
    echo.
    echo Troubleshooting:
    echo   - Verify PostgreSQL credentials are correct
    echo   - Ensure PokerTracker 4 database is running
    echo   - Check that you have CREATE TABLE permissions
    echo   - Try connecting manually: psql -h !PT4_HOST! -U !PT4_USER! -d !PT4_DATABASE!
    echo.
    pause
    exit /b 1
)

echo.
echo ==================================================
echo Migration completed successfully!
echo ==================================================
echo.
echo Next steps:
echo   1. Rebuild OpenHoldem solution in Visual Studio
echo   2. Start OpenHoldem and configure PT4 connection if needed
echo   3. Begin scraping - verified player names will show in BRIGHT GREEN
echo.
pause
