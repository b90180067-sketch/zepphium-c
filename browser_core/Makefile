SHELL = cmd.exe

CC_GCC = C:/mingw64/bin/gcc.exe
CC_CLANG = C:/msys64/clang64/bin/clang.exe

INCLUDES = -IC:/mingw64/opt/include -I..
SQLITE_LIB = C:/mingw64/opt/lib/libsqlite3.a -lbcrypt

CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wformat=2 -Werror -g -O2 $(INCLUDES)
SAN_FLAGS = -std=c17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -fsanitize=undefined -g -O1 $(INCLUDES)

DEPS_SRC = port.c ../zoom/port.c ../geometry/port.c ../ids/port.c ../navigation/port.c ../blocker/port.c ../store/port.c

all: build

build: build-gcc build-clang

build-gcc: $(DEPS_SRC)
	$(CC_GCC) $(CFLAGS) -c port.c -o port_gcc.o
	$(CC_GCC) $(CFLAGS) harness/diff_runner.c $(DEPS_SRC) $(SQLITE_LIB) -o diff_runner_gcc.exe
	$(CC_GCC) $(CFLAGS) harness/bench_runner.c $(DEPS_SRC) $(SQLITE_LIB) -o bench_runner_gcc.exe
	$(CC_GCC) $(CFLAGS) harness/fuzz_target.c $(DEPS_SRC) $(SQLITE_LIB) -o fuzz_target_gcc.exe

build-clang: $(DEPS_SRC)
	$(CC_CLANG) $(CFLAGS) -c port.c -o port_clang.o
	$(CC_CLANG) $(CFLAGS) harness/diff_runner.c $(DEPS_SRC) $(SQLITE_LIB) -o diff_runner_clang.exe
	$(CC_CLANG) $(CFLAGS) harness/fuzz_target.c $(DEPS_SRC) $(SQLITE_LIB) -o fuzz_target_clang.exe

diff: build
	diff_runner_gcc.exe cases.jsonl
	diff_runner_clang.exe cases.jsonl

fuzz: build
	fuzz_target_gcc.exe
	fuzz_target_clang.exe

ubsan:
	$(CC_CLANG) $(SAN_FLAGS) harness/diff_runner.c $(DEPS_SRC) $(SQLITE_LIB) -o diff_runner_ubsan.exe
	diff_runner_ubsan.exe cases.jsonl

bench: build
	bench_runner_gcc.exe

gate: build diff fuzz ubsan bench
	python -c "print('=== GATE PASSED FOR ZEPHIUM BROWSER CORE ===')"

clean:
	python -c "import glob, os; [os.remove(f) for f in glob.glob('*.o') + glob.glob('*.exe')]"

.PHONY: all build build-gcc build-clang diff fuzz ubsan bench gate clean
