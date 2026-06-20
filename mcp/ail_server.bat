@echo off
REM AIL control server -- on/off switches + merged feedback for the Autonomous-Improvement-Loop daemons.
REM Backs the browser Terminal's "AIL" tab and the Hiss MCP ail_* tools. Listens on 0.0.0.0:7900.
cd /d C:\www\openholdembot_old\mcp
REM pythonw.exe = no console window at all (the server + the daemons it spawns stay windowless).
"C:\Users\scarl\AppData\Local\Programs\Python\Python310\pythonw.exe" ail_server.py %*
