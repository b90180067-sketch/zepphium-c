# Zephium build wrapper.
#
# CMake is the single source of truth for the build. This Makefile only
# translates the historical convenience targets into CMake invocations so that
# `make`, `make test` and `make run` keep working on Linux, WSL and
# Windows/MinGW without hard-coded toolchain paths.
#
# Environment overrides:
#   CC                C compiler for CMake to use (honoured on first configure)
#   CMAKE_GENERATOR   CMake generator, e.g. "Ninja" or "Unix Makefiles"
#   CMAKE_BUILD_TYPE  Release (default), Debug, RelWithDebInfo, MinSizeRel
#   BUILD_DIR         build directory (default: build)
#   CMAKE             cmake executable (default: cmake)
#   CTEST             ctest executable (default: ctest)
#   CMAKE_FLAGS       extra -D... flags forwarded to CMake

CMAKE ?= cmake
CTEST ?= ctest
BUILD_DIR ?= build
BUILD_TYPE ?= $(if $(CMAKE_BUILD_TYPE),$(CMAKE_BUILD_TYPE),Release)
CMAKE_FLAGS ?=

# Pick a reproducible default generator for the host. CMake projects built with
# MinGW on Windows must use the MinGW Makefiles generator; POSIX hosts use the
# standard Unix Makefiles generator. Override with CMAKE_GENERATOR (for
# example CMAKE_GENERATOR=Ninja).
ifeq ($(OS),Windows_NT)
  HOST_OS := windows
  EXEEXT := .exe
  DEFAULT_GENERATOR := MinGW Makefiles
else
  HOST_OS := $(shell uname -s 2>/dev/null | tr '[:upper:]' '[:lower:]')
  EXEEXT :=
  DEFAULT_GENERATOR := Unix Makefiles
endif

GENERATOR ?= $(if $(strip $(CMAKE_GENERATOR)),$(CMAKE_GENERATOR),$(DEFAULT_GENERATOR))
GENERATOR_ARG := $(if $(strip $(GENERATOR)),-G "$(GENERATOR)")

all: build

# Always let CMake re-run so edits to CMakeLists.txt are picked up. A configure
# of this project takes well under a second on both supported host OSes.
configure:
	$(CMAKE) -S . -B "$(BUILD_DIR)" $(GENERATOR_ARG) -DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" $(CMAKE_FLAGS)

build: configure
	$(CMAKE) --build "$(BUILD_DIR)" --config "$(BUILD_TYPE)"

test: build
	$(CTEST) --test-dir "$(BUILD_DIR)" --output-on-failure -C "$(BUILD_TYPE)" $(CTEST_ARGS)

run: build
	"$(BUILD_DIR)/zephium$(EXEEXT)"

# Legacy per-unit verification gates (diff/bench/fuzz/UBSan). Their Makefiles
# still contain MinGW-specific paths, so keep them on the Windows host where
# they were historically used. On Linux/WSL `gate` is the CMake test suite.
gate: test
ifeq ($(HOST_OS),windows)
	$(MAKE) -C zoom gate
	$(MAKE) -C geometry gate
	$(MAKE) -C ids gate
	$(MAKE) -C navigation gate
	$(MAKE) -C blocker gate
	$(MAKE) -C store gate
	$(MAKE) -C browser_core gate
endif

clean:
	$(CMAKE) -E rm -rf "$(BUILD_DIR)"

help:
	@echo "Zephium build targets:"
	@echo "  make              configure + build (BUILD_DIR=$(BUILD_DIR), $(BUILD_TYPE))"
	@echo "  make test         build and run the CTest suite"
	@echo "  make run          build and launch $(BUILD_DIR)/zephium$(EXEEXT)"
	@echo "  make clean        remove $(BUILD_DIR)"
	@echo "  make gate         test (Windows also runs the legacy per-unit gates)"
	@echo ""
	@echo "Overrides: CC, CMAKE_GENERATOR, CMAKE_BUILD_TYPE, BUILD_DIR, CMAKE_FLAGS"

.PHONY: all configure build test run gate clean help
