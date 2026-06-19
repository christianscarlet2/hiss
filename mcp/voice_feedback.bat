@echo off
REM Spoken feedback loop -- records the mic, transcribes, pins each note to the live hand, stores it
REM as a replay-aligned indicator in postgres voice_feedback (the AIL then improves the bot from it).
REM   voice_feedback.bat            run with the default mic
REM   voice_feedback.bat --list     list mic devices
REM   voice_feedback.bat --device 1 pick a mic by index or name
cd /d C:\www\openholdembot_old\mcp
"C:\Users\scarl\AppData\Local\Programs\Python\Python310\python.exe" voice_feedback.py --bot-url http://127.0.0.1:27654 %*
