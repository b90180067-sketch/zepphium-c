SHELL = cmd.exe

# Optional machine-local overrides (toolchain paths, SQLite3 prefix). Never
# committed - see "Gates" in README.md. Included first so it wins over the
# defaults below.
-include ../Makefile.local

# Zephium Windows shell (Win32 + WebView2), build with: mingw32-make -C windows
GCC ?= gcc
GXX ?= g++
SDK_INC = sdk/build/native/include
SDK_X64 = sdk\build\native\x64

# SQLite3 development files: pkg-config covers MSYS2/CI installs, otherwise the
# compiler's own search path is used. For a custom prefix set SQLITE_CFLAGS and
# SQLITE_LIBS on the command line or in ../Makefile.local.
SQLITE_CFLAGS ?= $(shell pkg-config --cflags sqlite3 2>NUL)
SQLITE_LIBS ?= $(shell pkg-config --libs sqlite3 2>NUL)
ifeq ($(strip $(SQLITE_LIBS)),)
SQLITE_LIBS := -lsqlite3
endif

# Do not embed absolute paths (in particular the checkout location) in the binaries.
PREFIX_MAP = -ffile-prefix-map=$(CURDIR)=. -ffile-prefix-map=$(CURDIR)/..=.

CFLAGS = -std=c17 -Wall -Wextra -O2 -I.. $(SQLITE_CFLAGS) $(PREFIX_MAP)
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -DUNICODE -D_UNICODE -I.. -I$(SDK_INC) $(PREFIX_MAP)
LIBS = $(SQLITE_LIBS) -lole32 -luser32 -lgdi32 -lshell32 -lbcrypt -ladvapi32

CORE_OBJS = obj/zoom_port.o obj/geometry_port.o obj/ids_port.o obj/navigation_port.o obj/blocker_port.o obj/store_port.o obj/browser_core_port.o

all: zephium_win.exe loader

obj:
	@if not exist obj mkdir obj

obj/zoom_port.o: ../zoom/port.c | obj
	$(GCC) $(CFLAGS) -c ../zoom/port.c -o obj/zoom_port.o

obj/geometry_port.o: ../geometry/port.c | obj
	$(GCC) $(CFLAGS) -c ../geometry/port.c -o obj/geometry_port.o

obj/ids_port.o: ../ids/port.c | obj
	$(GCC) $(CFLAGS) -c ../ids/port.c -o obj/ids_port.o

obj/navigation_port.o: ../navigation/port.c | obj
	$(GCC) $(CFLAGS) -c ../navigation/port.c -o obj/navigation_port.o

obj/blocker_port.o: ../blocker/port.c | obj
	$(GCC) $(CFLAGS) -c ../blocker/port.c -o obj/blocker_port.o

obj/store_port.o: ../store/port.c | obj
	$(GCC) $(CFLAGS) -c ../store/port.c -o obj/store_port.o

obj/browser_core_port.o: ../browser_core/port.c | obj
	$(GCC) $(CFLAGS) -c ../browser_core/port.c -o obj/browser_core_port.o

zephium_win.o: zephium_win.cpp | obj
	$(GXX) $(CXXFLAGS) -c zephium_win.cpp -o zephium_win.o

zephium_win.exe: zephium_win.o $(CORE_OBJS)
	@if not exist $(SDK_INC)\WebView2.h echo [error] WebView2 SDK missing: run powershell -ExecutionPolicy Bypass -File fetch_sdk.ps1
	$(GXX) -municode -mwindows zephium_win.o $(CORE_OBJS) $(LIBS) -o zephium_win.exe

loader:
	@if exist "$(SDK_X64)\WebView2Loader.dll" copy /Y "$(SDK_X64)\WebView2Loader.dll" "WebView2Loader.dll" >NUL
	@if not exist "WebView2Loader.dll" echo [warn] WebView2Loader.dll not copied - run fetch_sdk.ps1

run: all
	.\zephium_win.exe https://duckduckgo.com

clean:
	@if exist obj rmdir /S /Q obj
	@if exist zephium_win.o del /Q zephium_win.o
	@if exist zephium_win.exe del /Q zephium_win.exe
	@if exist WebView2Loader.dll del /Q WebView2Loader.dll

.PHONY: all obj loader run clean
