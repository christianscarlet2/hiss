@echo off
cd /d C:\www\openholdembot_old\mcp
start "NN driver 27654 (patched)" cmd /k "C:\Users\scarl\AppData\Local\Programs\Python\Python310\python.exe nn_driver.py --bot-url http://127.0.0.1:27654"
