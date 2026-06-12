@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat" x86 1>/dev/null 2>/dev/null
cd /d C:\www\openholdembot_old\Hiss
echo === cl available? ===
where cl
echo === compiling CScarletBeast.cpp ===
cl /c /EHsc /nologo /std:c++17 CScarletBeast.cpp
echo === exit code: %errorlevel% ===
