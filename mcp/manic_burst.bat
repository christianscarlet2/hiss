@echo off
REM Manic Burst -- when a table is very passive, fire a short maniac burst to wake people up,
REM then revert to small ball (length of a song). Auto-discovers the live Hiss across 27654..27659.
REM Pass --dry to log decisions without touching the knobs.
cd /d C:\www\openholdembot_old\mcp
REM pythonw.exe = no console window at all (daemon stays windowless).
"C:\Users\scarl\AppData\Local\Programs\Python\Python310\pythonw.exe" manic_burst.py %*
