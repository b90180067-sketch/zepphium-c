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
CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wformat=2 -Werror -g -O2 $(PREFIX_MAP)
SAN_FLAGS = -std=c17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -fsanitize=undefined -g -O1 $(PREFIX_MAP)

all: build

build: build-gcc build-clang

build-gcc: port.c port.h bridge/zoom_bridge.c bridge/zoom_bridge.h
	$(CC_GCC) $(CFLAGS) -c port.c -o port_gcc.o
	$(CC_GCC) $(CFLAGS) -c bridge/zoom_bridge.c -o bridge_gcc.o
	$(CC_GCC) $(CFLAGS) harness/diff_runner.c port_gcc.o -o diff_runner_gcc.exe
	$(CC_GCC) $(CFLAGS) harness/bench_runner.c port_gcc.o -o bench_runner_gcc.exe

build-clang: port.c port.h bridge/zoom_bridge.c bridge/zoom_bridge.h
	$(CC_CLANG) $(CFLAGS) -c port.c -o port_clang.o
	$(CC_CLANG) $(CFLAGS) -c bridge/zoom_bridge.c -o bridge_clang.o
	$(CC_CLANG) $(CFLAGS) harness/diff_runner.c port_clang.o -o diff_runner_clang.exe

diff: build
	diff_runner_gcc.exe cases.jsonl
	diff_runner_clang.exe cases.jsonl

ubsan:
	$(CC_CLANG) $(SAN_FLAGS) port.c harness/diff_runner.c -o diff_runner_ubsan.exe
	diff_runner_ubsan.exe cases.jsonl

bench: build
	bench_runner_gcc.exe

gate: build diff ubsan bench
	python -c "print('=== GATE PASSED FOR ZEPHIUM ZOOM ===')"

clean:
	python -c "import glob, os; [os.remove(f) for f in glob.glob('*.o') + glob.glob('*.exe')]"

.PHONY: all build build-gcc build-clang diff ubsan bench gate clean
