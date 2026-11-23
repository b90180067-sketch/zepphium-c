SHELL = cmd.exe

# Optional machine-local overrides (toolchain paths, SQLite3 prefix). Never
# committed - see "Gates" in README.md. Included first so it wins over the
# defaults below.
-include ../Makefile.local

# Toolchain: the defaults are the names MinGW-w64/MSYS2 put on PATH. Override with
# e.g. mingw32-make CC_GCC=gcc CC_CLANG=clang gate
CC_GCC ?= gcc
CC_CLANG ?= clang

# Do not embed absolute paths (in particular the checkout location) in the debug
# info of the gate binaries.
PREFIX_MAP = -ffile-prefix-map=$(CURDIR)=. -ffile-prefix-map=$(CURDIR)/..=.

# SQLite3 development files: pkg-config covers MSYS2/CI installs, otherwise the
# compiler's own search path is used. For a custom prefix set SQLITE_CFLAGS and
# SQLITE_LIBS on the command line or in ../Makefile.local, e.g.
#   mingw32-make SQLITE_CFLAGS=-IC:/deps/sqlite/include \
#                SQLITE_LIBS=C:/deps/sqlite/lib/libsqlite3.a gate
SQLITE_CFLAGS ?= $(shell pkg-config --cflags sqlite3 2>NUL)
SQLITE_LIBS ?= $(shell pkg-config --libs sqlite3 2>NUL)
ifeq ($(strip $(SQLITE_LIBS)),)
SQLITE_LIBS := -lsqlite3
endif

INCLUDES = $(SQLITE_CFLAGS)
SQLITE_LIB = $(SQLITE_LIBS)

CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wformat=2 -Werror -g -O2 $(PREFIX_MAP) $(INCLUDES)
SAN_FLAGS = -std=c17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -fsanitize=undefined -g -O1 $(PREFIX_MAP) $(INCLUDES)

all: build

build: build-gcc build-clang

build-gcc: port.c port.h bridge/store_bridge.c bridge/store_bridge.h
	$(CC_GCC) $(CFLAGS) -c port.c -o port_gcc.o
	$(CC_GCC) $(CFLAGS) -c bridge/store_bridge.c -o bridge_gcc.o
	$(CC_GCC) $(CFLAGS) harness/diff_runner.c port_gcc.o $(SQLITE_LIB) -o diff_runner_gcc.exe
	$(CC_GCC) $(CFLAGS) harness/bench_runner.c port_gcc.o $(SQLITE_LIB) -o bench_runner_gcc.exe

build-clang: port.c port.h bridge/store_bridge.c bridge/store_bridge.h
	$(CC_CLANG) $(CFLAGS) -c port.c -o port_clang.o
	$(CC_CLANG) $(CFLAGS) -c bridge/store_bridge.c -o bridge_clang.o
	$(CC_CLANG) $(CFLAGS) harness/diff_runner.c port_clang.o $(SQLITE_LIB) -o diff_runner_clang.exe

diff: build
	diff_runner_gcc.exe cases.jsonl
	diff_runner_clang.exe cases.jsonl

ubsan:
	$(CC_CLANG) $(SAN_FLAGS) port.c harness/diff_runner.c $(SQLITE_LIB) -o diff_runner_ubsan.exe
	diff_runner_ubsan.exe cases.jsonl

bench: build
	bench_runner_gcc.exe

gate: build diff ubsan bench
	python -c "print('=== GATE PASSED FOR ZEPHIUM STORE ===')"

clean:
	python -c "import glob, os; [os.remove(f) for f in glob.glob('*.o') + glob.glob('*.exe')]"

.PHONY: all build build-gcc build-clang diff ubsan bench gate clean
