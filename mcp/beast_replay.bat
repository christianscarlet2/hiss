@echo off
REM BEAST advanced replay server (ALL frames). Serves the local all-frames store on :8090.
REM UI: http://192.168.1.137:8090/   API: /api/hands /api/frames/<h> /api/img/<h>/<ts> /api/stream /api/voice
set PGPASSWORD=dbpass
cd /d C:\www\openholdembot_old
C:\Users\scarl\AppData\Local\Programs\Python\Python310\python.exe mcp\beast_replay_server.py --port 80
