@echo off
REM Launch TWO NN drivers, one per Hiss instance (sequential ports 27654 + 27655).
REM Each drives whatever phone/table its instance is attached to (A17 / S10) — independently.
REM Pre-reqs at tournament time: BOTH Hiss instances up + split + attached to their freeroll
REM tables, and the OHF autoplayer DISENGAGED on both (the NN driver does the clicking).
REM Pass --dry-run as an arg to test without clicking:  nn_driver_dual.bat --dry-run
cd /d C:\www\openholdembot_old\mcp
start "NN driver - bot 27654" cmd /k "set NN_BOT_URL=http://127.0.0.1:27654&& python nn_driver.py %*"
start "NN driver - bot 27655" cmd /k "set NN_BOT_URL=http://127.0.0.1:27655&& python nn_driver.py %*"
echo Launched two NN drivers (27654 + 27655). Watch each window for decisions.
