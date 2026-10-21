@echo off
rem Build Glassy.exe on Windows with MinGW-w64 (e.g. from MSYS2: pacman -S mingw-w64-x86_64-gcc).
rem Run this from a shell where gcc and windres are on PATH.
setlocal
cd /d "%~dp0"
if not exist build mkdir build

pushd resources
windres glassy.rc -O coff -o ..\build\glassy_res.o || goto :fail
popd

gcc -O2 -Wall -municode -mwindows -static -s src\glassy.c build\glassy_res.o -o build\Glassy.exe ^
  -lcomctl32 -lgdi32 -luser32 -lshell32 -ladvapi32 -lwinmm -ldwmapi -lm || goto :fail

echo Built build\Glassy.exe
exit /b 0

:fail
echo Build failed.
exit /b 1
