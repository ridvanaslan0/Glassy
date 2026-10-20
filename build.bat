@echo off
REM Build Glassy with MinGW-w64 (gcc must be on PATH)
gcc -O2 -s -mwindows -municode Glassy.c -o Glassy.exe ^
    -lgdi32 -luser32 -lshell32 -ladvapi32 -lcomctl32 -ldwmapi -lwinmm -lm
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)
echo Built Glassy.exe
