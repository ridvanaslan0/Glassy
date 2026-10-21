#!/usr/bin/env bash
# Cross-compile Glassy.exe on Linux/macOS (needs mingw-w64) or from an MSYS2 shell.
#   Ubuntu/Debian: sudo apt install mingw-w64
set -euo pipefail
cd "$(dirname "$0")"

CC="${CC:-x86_64-w64-mingw32-gcc}"
WINDRES="${WINDRES:-x86_64-w64-mingw32-windres}"

mkdir -p build
(cd resources && "$WINDRES" glassy.rc -O coff -o ../build/glassy_res.o)
"$CC" -O2 -Wall -municode -mwindows -static -s \
  src/glassy.c build/glassy_res.o -o build/Glassy.exe \
  -lcomctl32 -lgdi32 -luser32 -lshell32 -ladvapi32 -lwinmm -ldwmapi -lm

echo "Built build/Glassy.exe"
