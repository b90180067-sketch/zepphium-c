SHELL = cmd.exe

CC_GCC = C:/mingw64/bin/gcc.exe
CC_CLANG = C:/msys64/clang64/bin/clang.exe

CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wformat=2 -Werror -g -O2
SAN_FLAGS = -std=c17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -fsanitize=undefined -g -O1

all: build

build: build-gcc build-clang

build-gcc: port.c port.h bridge/navigation_bridge.c bridge/navigation_bridge.h
	$(CC_GCC) $(CFLAGS) -c port.c -o port_gcc.o
	$(CC_GCC) $(CFLAGS) -c bridge/navigation_bridge.c -o bridge_gcc.o
	$(CC_GCC) $(CFLAGS) harness/diff_runner.c port_gcc.o -o diff_runner_gcc.exe
	$(CC_GCC) $(CFLAGS) harness/bench_runner.c port_gcc.o -o bench_runner_gcc.exe

build-clang: port.c port.h bridge/navigation_bridge.c bridge/navigation_bridge.h
	$(CC_CLANG) $(CFLAGS) -c port.c -o port_clang.o
	$(CC_CLANG) $(CFLAGS) -c bridge/navigation_bridge.c -o bridge_clang.o
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
	python -c "print('=== GATE PASSED FOR ZEPHIUM NAVIGATION ===')"

clean:
	python -c "import glob, os; [os.remove(f) for f in glob.glob('*.o') + glob.glob('*.exe')]"

.PHONY: all build build-gcc build-clang diff ubsan bench gate clean
