@echo off
REM ============================================================================
REM  THE NEURAL-NET KILLSWITCH for the LIVE bot. No rebuild -- this is the
REM  "special setting": run this and the trained NN plays; close it (or the
REM  window) and the OHF strategy plays again. Nothing here touches hiss.exe.
REM
REM  Use:  nn_driver.bat          -> NN PLAYS the live table (autoplayer off + driver)
REM        nn_driver.bat dry      -> NN decides + prints, but does NOT click (safe watch)
REM  Stop: close this window / Ctrl-C  (then re-engage the OHF autoplayer if you want it)
REM ============================================================================
setlocal
set BOT=http://127.0.0.1:27654

if /I "%1"=="dry" (
  echo [nn_driver.bat] DRY-RUN: NN will decide and print only, no clicks.
  python "%~dp0nn_driver.py" --dry-run
  goto :eof
)

echo [nn_driver.bat] Disengaging the OHF autoplayer so the NN drives...
curl -s "%BOT%/api/autoplayer?on=0" >nul
echo [nn_driver.bat] NN is now driving the live table. Close this window to hand control back to the OHF.
python "%~dp0nn_driver.py"
endlocal
