@echo off
REM AIL control server -- on/off switches + merged feedback for the Autonomous-Improvement-Loop daemons.
REM Backs the browser Terminal's "AIL" tab and the Hiss MCP ail_* tools. Listens on 0.0.0.0:7900.
cd /d C:\www\openholdembot_old\mcp
"C:\Users\scarl\AppData\Local\Programs\Python\Python310\python.exe" ail_server.py %*
