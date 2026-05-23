# ambil — Makefile
#
# Targets:
#   make            optimized release build (-O3 -pthread -DNDEBUG)
#   make debug      -O0 -g3 with -fsanitize=address,undefined
#   make test       run shell-based smoke tests against test/sample.log
#   make bench      generate a multi-GB log and benchmark vs grep/jq
#   make clean      remove build artifacts

CC      = gcc
CSTD    ?= -std=c11
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
NATIVE  ?= 1
ifeq ($(NATIVE),1)
ARCH    := -march=native -funroll-loops
else
ARCH    :=
endif
OPT     ?= -O3 -DNDEBUG $(ARCH)
INCS    := -Isrc -Isrc/core -Isrc/search -Isrc/io -Isrc/walk -Isrc/platform -Isrc/exec -Isrc/cmd
CFLAGS  ?= $(CSTD) $(WARN) $(OPT) -pthread $(INCS) -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
LDFLAGS ?= -pthread

ifeq ($(OS),Windows_NT)
EXEEXT  = .exe
else
EXEEXT  =
endif

# Detect whether we are being driven by a POSIX-style shell (MSYS2, Cygwin, Git
# Bash, WSL) vs cmd.exe. Under MSYS2 $(OS) is still "Windows_NT" but $$MSYSTEM
# is set and /bin/sh is bash, so cmd-syntax recipes (`if exist ...`) fail with
# "syntax error: unexpected end of file".
ifeq ($(OS),Windows_NT)
ifeq ($(MSYSTEM),)
WIN_CMD_SHELL := 1
else
WIN_CMD_SHELL :=
endif
else
WIN_CMD_SHELL :=
endif

ifdef WIN_CMD_SHELL
MKDIR_P   = if not exist "$(BIN_DIR)" mkdir "$(BIN_DIR)"
RM_RF     = if exist "$(BIN_DIR)" rmdir /S /Q "$(BIN_DIR)"
RUN_TEST   = set BIN=$(BIN)&& bash test/run_tests.sh && bash test/test_simd_parity.sh && bash test/test_chunked.sh && bash test/test_regex.sh && bash test/test_filetype.sh
RUN_BENCH  = set BIN=$(BIN)&& bash test/bench.sh
else
MKDIR_P    = mkdir -p "$(BIN_DIR)"
RM_RF      = rm -rf "$(BIN_DIR)"
RUN_TEST   = BIN="$(BIN)" bash test/run_tests.sh && BIN="$(BIN)" bash test/test_simd_parity.sh && BIN="$(BIN)" bash test/test_chunked.sh && BIN="$(BIN)" bash test/test_regex.sh && BIN="$(BIN)" bash test/test_filetype.sh
RUN_BENCH  = BIN="$(BIN)" bash test/bench.sh
endif

SRC_DIR := src
BIN_DIR := build
BIN     := $(BIN_DIR)/ambil$(EXEEXT)

SRCS := \
	$(SRC_DIR)/cmd/main.c \
	$(SRC_DIR)/search/search.c \
	$(SRC_DIR)/search/simd_search.c \
	$(SRC_DIR)/search/re_tiny.c \
	$(SRC_DIR)/exec/thread_pool.c \
	$(SRC_DIR)/core/util.c \
	$(SRC_DIR)/io/binary.c \
	$(SRC_DIR)/walk/ignore.c \
	$(SRC_DIR)/walk/walker.c \
	$(SRC_DIR)/io/json_emit.c \
	$(SRC_DIR)/io/file_reader.c \
	$(SRC_DIR)/io/filetype.c \
	$(SRC_DIR)/platform/platform_stat.c \
	$(SRC_DIR)/platform/platform.c \
	$(SRC_DIR)/core/help.c \
	$(SRC_DIR)/core/env.c \
	$(SRC_DIR)/cmd/cat.c \
	$(SRC_DIR)/cmd/wc.c \
	$(SRC_DIR)/cmd/ls.c \
	$(SRC_DIR)/cmd/find.c \
	$(SRC_DIR)/cmd/grep.c

OBJS := $(patsubst $(SRC_DIR)/%.c,$(BIN_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

ifdef WIN_CMD_SHELL
MKDIR_OBJ_DIR = if not exist "$(subst /,\,$(@D))" mkdir "$(subst /,\,$(@D))"
else
MKDIR_OBJ_DIR = mkdir -p "$(@D)"
endif

.PHONY: all debug clean test bench install release release-archive checksums fuzz universal cmake-init

all: $(BIN)

$(BIN): $(OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

$(BIN_DIR)/%.o: $(SRC_DIR)/%.c
	@$(MKDIR_OBJ_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BIN_DIR):
	@$(MKDIR_P)

debug:
	$(MAKE) OPT="-O0 -g3 -fsanitize=address,undefined" \
	        LDFLAGS="-pthread -fsanitize=address,undefined" all

clean:
	$(RM_RF)

test: $(BIN)
	@$(RUN_TEST)

bench: $(BIN)
	@$(RUN_BENCH)

install: $(BIN)
	install -m 0755 $(BIN) $(DESTDIR)/usr/local/bin/ambil

# ---------------------------------------------------------------------------
# Fuzzing (clang + libFuzzer; gcc/mingw cannot link -fsanitize=fuzzer).
#
#   make fuzz CC=clang
#   ./$(BIN_DIR)/ambil-fuzz -max_total_time=60 corpus/
#
# Skips the main.c entry; libFuzzer provides its own main.
# ---------------------------------------------------------------------------
FUZZ_BIN     := $(BIN_DIR)/ambil-fuzz$(EXEEXT)
FUZZ_SRCS    := $(filter-out $(SRC_DIR)/cmd/main.c,$(SRCS)) \
                $(SRC_DIR)/search/fuzz_search.c
FUZZ_OBJS    := $(patsubst $(SRC_DIR)/%.c,$(BIN_DIR)/fuzz/%.o,$(FUZZ_SRCS))
FUZZ_CFLAGS  := $(CSTD) $(WARN) -O1 -g $(INCS) -pthread \
                -fsanitize=fuzzer,address,undefined \
                -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
FUZZ_LDFLAGS := -pthread -fsanitize=fuzzer,address,undefined

$(BIN_DIR)/fuzz/%.o: $(SRC_DIR)/%.c
	@$(MKDIR_OBJ_DIR)
	$(CC) $(FUZZ_CFLAGS) -c $< -o $@

$(FUZZ_BIN): $(FUZZ_OBJS)
	$(CC) $(FUZZ_LDFLAGS) -o $@ $(FUZZ_OBJS)

fuzz: $(FUZZ_BIN)
	@echo "fuzz harness built: $(FUZZ_BIN)"
	@echo "run: $(FUZZ_BIN) -max_total_time=60 corpus/  (clang required)"

# ---------------------------------------------------------------------------
# macOS universal binary (lipo). Builds x86_64 and arm64 then fuses them.
# Only meaningful on macOS with both targets supported by the local clang.
# ---------------------------------------------------------------------------
universal:
	$(MAKE) clean
	$(MAKE) BIN_DIR=$(BIN_DIR)/x86_64 NATIVE=0 ARCH=-arch\ x86_64
	$(MAKE) clean
	$(MAKE) BIN_DIR=$(BIN_DIR)/arm64  NATIVE=0 ARCH=-arch\ arm64
	@mkdir -p $(BIN_DIR)
	lipo -create -output $(BIN_DIR)/ambil $(BIN_DIR)/x86_64/ambil $(BIN_DIR)/arm64/ambil
	@echo "fat binary: $(BIN_DIR)/ambil"
	@file $(BIN_DIR)/ambil 2>/dev/null || true

# ---------------------------------------------------------------------------
# CMake init: bootstrap an out-of-tree CMake build directory.
# CMakeLists.txt lives at repo root; this target just creates build-cmake/.
# ---------------------------------------------------------------------------
cmake-init:
	@cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
	@echo "configured: cd build-cmake && cmake --build ."

# ---------------------------------------------------------------------------
# release packaging
#
#   make release VERSION=v0.2.0
#
# Produces, under build/release/:
#   ambil-<VERSION>-<ARCH>-<OS>.tar.gz   (POSIX)
#   ambil-<VERSION>-<ARCH>-windows.zip   (Windows)
#   SHA256SUMS                           (appended/created)
#
# ARCH/OS are auto-detected from `uname -m -s` (POSIX) or PROCESSOR_ARCHITECTURE
# (Windows). Override by passing ARCH= and TARGET_OS= explicitly.
# ---------------------------------------------------------------------------

VERSION       ?= v0.2.0
RELEASE_DIR   := $(BIN_DIR)/release

ifeq ($(OS),Windows_NT)
TARGET_OS_DETECT := windows
ifeq ($(PROCESSOR_ARCHITECTURE),AMD64)
ARCH_DETECT := x86_64
else ifeq ($(PROCESSOR_ARCHITECTURE),ARM64)
ARCH_DETECT := aarch64
else ifeq ($(PROCESSOR_ARCHITECTURE),x86)
ARCH_DETECT := i686
else
ARCH_DETECT := $(PROCESSOR_ARCHITECTURE)
endif
else
TARGET_OS_DETECT := $(shell uname -s | tr '[:upper:]' '[:lower:]')
ARCH_DETECT      := $(shell uname -m)
ifeq ($(ARCH_DETECT),amd64)
ARCH_DETECT := x86_64
endif
ifeq ($(ARCH_DETECT),arm64)
ARCH_DETECT := aarch64
endif
endif

TARGET_OS ?= $(TARGET_OS_DETECT)
ARCH      ?= $(ARCH_DETECT)

RELEASE_NAME := ambil-$(VERSION)-$(ARCH)-$(TARGET_OS)

ifeq ($(TARGET_OS),windows)
RELEASE_ARCHIVE := $(RELEASE_DIR)/$(RELEASE_NAME).zip
else
RELEASE_ARCHIVE := $(RELEASE_DIR)/$(RELEASE_NAME).tar.gz
endif

release: release-archive checksums
	@echo "release artifacts in $(RELEASE_DIR):"
ifdef WIN_CMD_SHELL
	@dir /B "$(subst /,\,$(RELEASE_DIR))"
else
	@ls -1 "$(RELEASE_DIR)"
endif

ifdef WIN_CMD_SHELL
# ----- Windows native make (cmd.exe shell) ---------------------------------
RELEASE_DIR_W := $(subst /,\,$(RELEASE_DIR))
STAGE_DIR_W   := $(RELEASE_DIR_W)\$(RELEASE_NAME)
BIN_W         := $(subst /,\,$(BIN))
ARCHIVE_W     := $(subst /,\,$(RELEASE_ARCHIVE))

release-archive: $(BIN)
	@if not exist "$(RELEASE_DIR_W)" mkdir "$(RELEASE_DIR_W)"
	@if exist "$(STAGE_DIR_W)" rmdir /S /Q "$(STAGE_DIR_W)"
	@mkdir "$(STAGE_DIR_W)"
	@copy /Y "$(BIN_W)" "$(STAGE_DIR_W)\" >NUL
	@if exist README.md copy /Y README.md "$(STAGE_DIR_W)\" >NUL
	@if exist "$(ARCHIVE_W)" del /Q "$(ARCHIVE_W)"
	@powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '$(STAGE_DIR_W)' -DestinationPath '$(ARCHIVE_W)' -Force"
	@rmdir /S /Q "$(STAGE_DIR_W)"
	@echo packaged: $(RELEASE_ARCHIVE)

checksums: release-archive
	@scripts\sha256-windows.cmd "$(ARCHIVE_W)" "$(notdir $(RELEASE_ARCHIVE))" "$(RELEASE_DIR_W)\SHA256SUMS"
	@echo updated $(RELEASE_DIR)/SHA256SUMS

else
# ----- POSIX (Linux / macOS) -----------------------------------------------
release-archive: $(BIN)
	@mkdir -p "$(RELEASE_DIR)/$(RELEASE_NAME)"
	@cp "$(BIN)" "$(RELEASE_DIR)/$(RELEASE_NAME)/"
	@cp README.md "$(RELEASE_DIR)/$(RELEASE_NAME)/" 2>/dev/null || true
ifeq ($(TARGET_OS),windows)
	@cd "$(RELEASE_DIR)" && \
	  ( command -v zip >/dev/null 2>&1 && zip -qr "$(RELEASE_NAME).zip" "$(RELEASE_NAME)" ) || \
	  ( command -v 7z  >/dev/null 2>&1 && 7z a -bd -bso0 "$(RELEASE_NAME).zip" "$(RELEASE_NAME)" >/dev/null )
else
	@tar -C "$(RELEASE_DIR)" -czf "$(RELEASE_ARCHIVE)" "$(RELEASE_NAME)"
endif
	@rm -rf "$(RELEASE_DIR)/$(RELEASE_NAME)"
	@echo "packaged: $(RELEASE_ARCHIVE)"

checksums: release-archive
	@cd "$(RELEASE_DIR)" && \
	  archive="$$(basename '$(RELEASE_ARCHIVE)')"; \
	  if [ -f SHA256SUMS ]; then grep -v " $$archive$$" SHA256SUMS > SHA256SUMS.tmp || true; mv SHA256SUMS.tmp SHA256SUMS; fi; \
	  if   command -v sha256sum  >/dev/null 2>&1; then sha256sum  "$$archive" >> SHA256SUMS; \
	  elif command -v shasum     >/dev/null 2>&1; then shasum -a 256 "$$archive" >> SHA256SUMS; \
	  elif command -v gsha256sum >/dev/null 2>&1; then gsha256sum "$$archive" >> SHA256SUMS; \
	  else echo "no sha256 tool available" >&2; exit 1; fi
	@echo "updated $(RELEASE_DIR)/SHA256SUMS"
endif

-include $(DEPS)
