SHELL = cmd.exe

# Optional machine-local overrides (toolchain paths, SQLite3 prefix). Never
# committed - see "Gates" in README.md. Included first so it wins over the
# defaults below.
-include ../Makefile.local

# Zephium Windows shell (Win32 + WebView2), build with: mingw32-make -C windows
#
#   mingw32-make            build zephium_win.exe + WebView2Loader.dll
#   mingw32-make run        build and open duckduckgo
#   mingw32-make adblock-test
#                           build and run adblock_selftest.exe (console test for
#                           the native ad blocker; also needs the Rust staticlib)
#   mingw32-make clean      remove build outputs (the Rust target/ dir is kept)
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

# Native ad blocking: Brave's adblock-rust behind blocker/adblock_ffi.h, built by
# cargo into a static archive and linked into the shell. rustup installs cargo in
# %USERPROFILE%\.cargo\bin but that directory is often not on the PATH of the
# shell that runs make, so test PATH first and fall back to the rustup location.
CARGO ?= cargo
ifeq ($(strip $(shell cargo --version 2>NUL)),)
CARGO := $(USERPROFILE)\.cargo\bin\cargo.exe
endif
RUST_MANIFEST = ../blocker/rust/Cargo.toml
RUST_LIB = ../blocker/rust/target/release/libzephium_adblock.a
RUST_SRCS = $(wildcard ../blocker/rust/src/*.rs) $(wildcard ../blocker/rust/src/**/*.rs) \
            $(wildcard ../blocker/rust/Cargo.lock) $(RUST_MANIFEST)

# Do not embed absolute paths (in particular the checkout location) in the binaries.
PREFIX_MAP = -ffile-prefix-map=$(CURDIR)=. -ffile-prefix-map=$(CURDIR)/..=.

CFLAGS = -std=c17 -Wall -Wextra -O2 -I.. $(SQLITE_CFLAGS) $(PREFIX_MAP)
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -DUNICODE -D_UNICODE -I.. -I$(SDK_INC) $(PREFIX_MAP)
# -lwinhttp/-lshlwapi: filter list downloads + SHCreateMemStream.
# -lsynchronization: Rust std's futex (WaitOnAddress/WakeByAddress*) is not in
# this MinGW's kernel32 import library. -lws2_32 -luserenv -lntdll are the rest of
# the Rust std Windows dependencies.
LIBS = $(SQLITE_LIBS) -lole32 -luser32 -lgdi32 -lshell32 -lbcrypt -ladvapi32 \
       -lwinhttp -lshlwapi -lsynchronization -lws2_32 -luserenv -lntdll

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

# Rust static library with the ad-block engine. Cargo is the only build step that
# can be slow (~1-2 min on a cold target/); the .a is then reused until the crate
# or its lockfile changes.
$(RUST_LIB): $(RUST_SRCS)
	$(CARGO) build --release --manifest-path $(RUST_MANIFEST)

zephium_win.o: zephium_win.cpp adblock_webview2.h ../blocker/adblock_ffi.h | obj
	$(GXX) $(CXXFLAGS) -c zephium_win.cpp -o zephium_win.o

obj/adblock_webview2.o: adblock_webview2.cpp adblock_webview2.h ../blocker/adblock_ffi.h | obj
	$(GXX) $(CXXFLAGS) -c adblock_webview2.cpp -o obj/adblock_webview2.o

zephium_win.exe: zephium_win.o obj/adblock_webview2.o $(CORE_OBJS) $(RUST_LIB)
	@if not exist $(SDK_INC)\WebView2.h echo [error] WebView2 SDK missing: run powershell -ExecutionPolicy Bypass -File fetch_sdk.ps1
	@if not exist $(RUST_LIB) echo [error] Rust staticlib missing: run powershell -ExecutionPolicy Bypass -File fetch_rust.ps1
	$(GXX) -municode -mwindows zephium_win.o obj/adblock_webview2.o $(CORE_OBJS) $(RUST_LIB) $(LIBS) -o zephium_win.exe

adblock_selftest.o: adblock_selftest.cpp adblock_webview2.h ../blocker/adblock_ffi.h | obj
	$(GXX) $(CXXFLAGS) -pthread -c adblock_selftest.cpp -o adblock_selftest.o

adblock_selftest.exe: adblock_selftest.o obj/adblock_webview2.o $(RUST_LIB)
	@if not exist $(RUST_LIB) echo [error] Rust staticlib missing: run powershell -ExecutionPolicy Bypass -File fetch_rust.ps1
	$(GXX) -pthread adblock_selftest.o obj/adblock_webview2.o $(RUST_LIB) $(LIBS) -o adblock_selftest.exe

# Console self test: classification helpers, FFI decisions, cache, thread
# affinity, panic containment and the 140k-rule benchmark. Test data goes to
# windows\obj\adblock-selftest (override with ZEPHIUM_ADBLOCK_DIR).
adblock-test: adblock_selftest.exe
	@.\adblock_selftest.exe

loader:
	@if exist "$(SDK_X64)\WebView2Loader.dll" copy /Y "$(SDK_X64)\WebView2Loader.dll" "WebView2Loader.dll" >NUL
	@if not exist "WebView2Loader.dll" echo [warn] WebView2Loader.dll not copied - run fetch_sdk.ps1

run: all
	.\zephium_win.exe https://duckduckgo.com

# The Rust target/ dir (several hundred MB of build artifacts) is deliberately
# left alone; run "cargo clean --manifest-path ../blocker/rust/Cargo.toml" for it.
clean:
	@if exist obj rmdir /S /Q obj
	@if exist zephium_win.o del /Q zephium_win.o
	@if exist adblock_selftest.o del /Q adblock_selftest.o
	@if exist zephium_win.exe del /Q zephium_win.exe
	@if exist adblock_selftest.exe del /Q adblock_selftest.exe
	@if exist WebView2Loader.dll del /Q WebView2Loader.dll

.PHONY: all obj loader run adblock-test clean
