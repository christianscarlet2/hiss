@echo off
REM engage_brain.bat -- bring the whole Hiss BRAIN online in one command (run on BEAST, where Hiss runs).
REM Launches every brain daemon in the BACKGROUND with NO console window (pythonw = GUI subsystem, so no
REM terminal ever pops up -- not even when decision_advisor/deep_thought/growth fire Claude). Each daemon's
REM output is redirected to Release\logs\brain_*.out.log. Telemetry records the rest. [Emrald: background only]
REM Preferred path is the React table-view Brain button (AIL), which launches these the same windowless way.
setlocal
set HISS_PG_DSN=host=127.0.0.1 port=5432 dbname=hiss user=postgres password=dbpass
set PORT=27655
if exist "C:\www\openholdembot_old\Release\logs\terminal_port.txt" set /p PORT=<C:\www\openholdembot_old\Release\logs\terminal_port.txt
set BOT=http://127.0.0.1:%PORT%
set LOGS=C:\www\openholdembot_old\Release\logs
cd /d C:\www\openholdembot_old\mcp

echo ============================================================
echo   ENGAGING THE BRAIN on %BOT%   (background, no windows)
echo ============================================================

REM --- the reads (gametype-separated HUD + introspection profiles) ---
start "" pythonw hud_aggregator.py --watch        >"%LOGS%\brain_hud_aggregator.out.log" 2>&1
start "" pythonw introspect_aggregator.py --watch >"%LOGS%\brain_introspect_aggregator.out.log" 2>&1

REM --- the brain (harmonizes -> brain_state + brain_log + brain_load telemetry) ---
start "" pythonw synapse_map.py --bot-url %BOT% --watch >"%LOGS%\brain_synapse_map.out.log" 2>&1

REM --- the ismyturn advisor (fires Claude windowless, steers the live bot + NN via the knobs) ---
start "" pythonw decision_advisor.py --bot-url %BOT% >"%LOGS%\brain_decision_advisor.out.log" 2>&1

REM --- the nervous system: parallel pathway-EV + async deep-thought + self-growth (all fire Claude windowless) ---
start "" pythonw brain_service.py            >"%LOGS%\brain_brain_service.out.log" 2>&1
start "" pythonw deep_thought.py --serve     >"%LOGS%\brain_deep_thought.out.log" 2>&1
start "" pythonw growth.py --watch           >"%LOGS%\brain_growth.out.log" 2>&1

echo.
echo   Brain engaged in the BACKGROUND. No console windows; logs in %LOGS%\brain_*.out.log
echo   To run the brain on swiftsnake instead (32 cores), the hiss-brain.service is already live there.
echo ============================================================
endlocal
