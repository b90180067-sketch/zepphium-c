SHELL = cmd.exe

CC_GCC = C:/mingw64/bin/gcc.exe
CC_CLANG = C:/msys64/clang64/bin/clang.exe

INCLUDES = -IC:/mingw64/opt/include
SQLITE_LIB = C:/mingw64/opt/lib/libsqlite3.a

CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wformat=2 -Werror -g -O2 $(INCLUDES)
SAN_FLAGS = -std=c17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -fsanitize=undefined -g -O1 $(INCLUDES)

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
