SHELL = cmd.exe

CC_GCC = C:/mingw64/bin/gcc.exe
CC_CLANG = C:/msys64/clang64/bin/clang.exe

INCLUDES = -IC:/mingw64/opt/include -I.
SQLITE_LIB = C:/mingw64/opt/lib/libsqlite3.a -lbcrypt

CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wformat=2 -Werror -g -O2 $(INCLUDES)

CORE_SRCS = \
	zoom/port.c \
	geometry/port.c \
	ids/port.c \
	navigation/port.c \
	blocker/port.c \
	store/port.c \
	browser_core/port.c

UNITS = zoom geometry ids navigation blocker store browser_core

all: build

build: build-gcc build-clang

build-gcc: $(CORE_SRCS) main.c
	$(CC_GCC) $(CFLAGS) main.c $(CORE_SRCS) $(SQLITE_LIB) -o zephium_gcc.exe

build-clang: $(CORE_SRCS) main.c
	$(CC_CLANG) $(CFLAGS) main.c $(CORE_SRCS) $(SQLITE_LIB) -o zephium_clang.exe

test: build
	zephium_gcc.exe --batch
	zephium_clang.exe --batch

gate: test
	cd zoom && $(MAKE) gate
	cd geometry && $(MAKE) gate
	cd ids && $(MAKE) gate
	cd navigation && $(MAKE) gate
	cd blocker && $(MAKE) gate
	cd store && $(MAKE) gate
	cd browser_core && $(MAKE) gate
	python -c "print('====================================================')"
	python -c "print('=== ALL ZEPHIUM UNITS PASSED 100%% VERIFICATION ===')"
	python -c "print('====================================================')"

clean:
	python -c "import glob, os; [os.remove(f) for f in glob.glob('*.o') + glob.glob('*.exe')]"
	cd zoom && $(MAKE) clean
	cd geometry && $(MAKE) clean
	cd ids && $(MAKE) clean
	cd navigation && $(MAKE) clean
	cd blocker && $(MAKE) clean
	cd store && $(MAKE) clean
	cd browser_core && $(MAKE) clean

.PHONY: all build build-gcc build-clang test gate clean $(UNITS)
