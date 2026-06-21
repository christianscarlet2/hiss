@echo off
REM engage_brain.bat -- bring the whole Hiss BRAIN online in one command (run on BEAST, where Hiss runs).
REM Reads the live Hiss terminal port and launches every brain daemon in its own window. The advisor +
REM brain-steered NN drive the seated bot via the knobs (NO rebuild required); telemetry records it all.
setlocal
set HISS_PG_DSN=host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass
set PORT=27655
if exist "C:\www\openholdembot_old\Release\logs\terminal_port.txt" set /p PORT=<C:\www\openholdembot_old\Release\logs\terminal_port.txt
set BOT=http://127.0.0.1:%PORT%
cd /d C:\www\openholdembot_old\mcp

echo ============================================================
echo   ENGAGING THE BRAIN on %BOT%
echo ============================================================

REM --- the reads (gametype-separated HUD + introspection profiles) ---
start "hud_aggregator"        cmd /k python hud_aggregator.py --watch
start "introspect_aggregator" cmd /k python introspect_aggregator.py --watch

REM --- the brain (harmonizes -> brain_state + brain_log + brain_load telemetry) ---
start "synapse_brain"         cmd /k python synapse_map.py --bot-url %BOT% --watch

REM --- the ismyturn advisor (fires Claude, steers the live bot + NN via the knobs) ---
start "decision_advisor"      cmd /k python decision_advisor.py --bot-url %BOT%

REM --- the nervous system: parallel pathway-EV + async deep-thought + self-growth ---
start "brain_service"         cmd /k python brain_service.py
start "deep_thought"          cmd /k python deep_thought.py --serve
start "growth"                cmd /k python growth.py --watch

echo.
echo   Brain engaged. Seat the bot and it plays exploit-adjusted.
echo   To run the brain on swiftsnake instead, set HISS_PG_DSN to the primary
echo   and launch synapse_map/brain_service/deep_thought THERE (32 cores).
echo ============================================================
endlocal
