@echo off
cd /d C:\www\openholdembot_old
"C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" Hiss.sln /t:Hiss /p:Configuration=Release /p:Platform=Win32 /m /v:minimal /nologo
echo === MSBUILD EXIT: %errorlevel% ===
