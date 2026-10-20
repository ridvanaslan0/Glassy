# Cross-compile from Linux/macOS with MinGW-w64, or run natively under MSYS2.
# Usage: make            (builds Glassy.exe)
#        make clean
CC      ?= x86_64-w64-mingw32-gcc
CFLAGS  ?= -O2 -s -mwindows -municode
LIBS     = -lgdi32 -luser32 -lshell32 -ladvapi32 -lcomctl32 -ldwmapi -lwinmm -lm

Glassy.exe: Glassy.c
	$(CC) $(CFLAGS) $< -o $@ $(LIBS)

clean:
	rm -f Glassy.exe

.PHONY: clean
