# SPDX-License-Identifier: GPL-3.0
#
# latbench build system.
#
# Feature-test macros are intentionally NOT defined here: strict C99 sources in
# src/core/ and src/bench/ must not see them. Every platform source defines its
# own macro at the top of the file (#define _POSIX_C_SOURCE 200809L or
# #define _DARWIN_C_SOURCE 1).

SHELL := /bin/sh

CC ?= cc
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy
CPPCHECK ?= cppcheck

# SPDX identifier enforced by tools/style-check.sh for every C source/header.
SPDX_LICENSE ?= GPL-3.0

UNAME_S := $(shell uname -s)

VERSION ?= 0.1.0
CPPFLAGS ?=
override CPPFLAGS += -DLB_VERSION=\"$(VERSION)\"
LDFLAGS ?=
LDLIBS ?=

COMMON_CFLAGS := -std=c99 -Wall -Wextra -Wshadow -Wstrict-prototypes \
	-Wmissing-prototypes -Wvla -pedantic -fno-common
RELEASE_CFLAGS := $(COMMON_CFLAGS) -O2
DEV_CFLAGS := $(COMMON_CFLAGS) -O1 -g -Werror -Wconversion -Wsign-conversion \
	-Wcast-qual -Wwrite-strings -Wformat=2 -Wundef -Wpointer-arith \
	-Wmissing-declarations
LEAKS_CFLAGS := $(COMMON_CFLAGS) -O1 -g -fsanitize=address,undefined \
	-fno-omit-frame-pointer
LEAKS_LDFLAGS := -fsanitize=address,undefined

ifeq ($(UNAME_S),Darwin)
	PLATFORM_SRCS := src/platform/pal_posix.c src/platform/pal_macos.c
	CPPCHECK_DEFINES := -D__APPLE__
else ifeq ($(UNAME_S),Linux)
	PLATFORM_SRCS := src/platform/pal_posix.c src/platform/pal_linux.c
	CPPCHECK_DEFINES := -D__linux__
else
	PLATFORM_SRCS := src/platform/pal_posix.c
	CPPCHECK_DEFINES :=
endif

# Portable core sources are listed explicitly so link order stays deterministic.
# Add new src/core/*.c files here as their modules are implemented.
CORE_SRCS := src/core/main.c src/core/stats.c src/core/tty.c src/core/util.c \
	src/core/auto.c src/core/passport.c

# Benchmark suites are listed explicitly so link order stays deterministic.
# Add new src/bench/*.c files here as their modules are implemented.
BENCH_SRCS := src/bench/bench_common.c src/bench/bench_sys.c src/bench/bench_mem.c \
	src/bench/bench_tlb.c src/bench/bench_core.c src/bench/bench_branch.c \
	src/bench/bench_dram.c src/bench/bench_topo.c src/bench/bench_os.c

SRCS := $(CORE_SRCS) $(BENCH_SRCS)
SRCS += $(PLATFORM_SRCS)

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin
TEST_OBJDIR := $(OBJ_DIR)/test
TEST_BINDIR := $(BIN_DIR)/tests

RELEASE_OBJDIR := $(OBJ_DIR)/rel
DEV_OBJDIR := $(OBJ_DIR)/dev
LEAKS_OBJDIR := $(OBJ_DIR)/asan
RELEASE_OBJS := $(SRCS:%.c=$(RELEASE_OBJDIR)/%.o)
DEV_OBJS := $(SRCS:%.c=$(DEV_OBJDIR)/%.o)
LEAKS_OBJS := $(SRCS:%.c=$(LEAKS_OBJDIR)/%.o)

BIN := $(BIN_DIR)/latbench
DEV_BIN := $(BIN_DIR)/latbench-dev
LEAKS_BIN := $(BIN_DIR)/latbench-asan

PLATFORM_RELEASE_OBJS := $(PLATFORM_SRCS:%.c=$(RELEASE_OBJDIR)/%.o)
PLATFORM_LEAKS_OBJS := $(PLATFORM_SRCS:%.c=$(LEAKS_OBJDIR)/%.o)

TEST_SRCS := tests/core/test_stats.c tests/core/test_tty.c tests/core/test_util.c \
	tests/core/test_auto.c $(wildcard tests/platform/test_*.c)
TEST_RELEASE_OBJS := $(patsubst tests/%.c,$(TEST_OBJDIR)/rel/tests/%.o,$(TEST_SRCS))
TEST_LEAKS_OBJS := $(patsubst tests/%.c,$(LEAKS_OBJDIR)/tests/%.o,$(TEST_SRCS))
TEST_BINS := $(patsubst tests/%.c,$(TEST_BINDIR)/rel/%,$(TEST_SRCS))
LEAKS_TEST_BINS := $(patsubst tests/%.c,$(TEST_BINDIR)/asan/%,$(TEST_SRCS))

# Test binaries need the portable core objects but must not link main().
TEST_LINK_RELEASE_OBJS := $(filter-out $(RELEASE_OBJDIR)/src/core/main.o,$(RELEASE_OBJS))
TEST_LINK_LEAKS_OBJS := $(filter-out $(LEAKS_OBJDIR)/src/core/main.o,$(LEAKS_OBJS))

FORMAT_FILES := $(shell find include src tests -type f \
	\( -name '*.c' -o -name '*.h' \) 2>/dev/null | LC_ALL=C sort)

ifeq ($(STATIC),1)
	ifeq ($(UNAME_S),Linux)
		RELEASE_LDFLAGS += -static
	else
		$(warning STATIC=1 is supported only on Linux; ignoring)
	endif
endif

.PHONY: all dev leaks test lint format format-check style-check clean

all: $(BIN)

dev: $(DEV_BIN)

$(BIN): $(RELEASE_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(RELEASE_LDFLAGS) $^ $(LDLIBS) -o $@

$(DEV_BIN): $(DEV_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(LEAKS_BIN): $(LEAKS_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(LEAKS_LDFLAGS) $^ $(LDLIBS) -o $@

$(RELEASE_OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(RELEASE_CFLAGS) $(CFLAGS) $(CPPFLAGS) -Iinclude -MMD -MP -c $< -o $@

$(DEV_OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(DEV_CFLAGS) $(CFLAGS) $(CPPFLAGS) -Iinclude -MMD -MP -c $< -o $@

$(LEAKS_OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(LEAKS_CFLAGS) $(CFLAGS) $(CPPFLAGS) -Iinclude -MMD -MP -c $< -o $@

$(TEST_OBJDIR)/rel/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(RELEASE_CFLAGS) $(CFLAGS) $(CPPFLAGS) -Iinclude -MMD -MP -c $< -o $@

$(LEAKS_OBJDIR)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LEAKS_CFLAGS) $(CFLAGS) $(CPPFLAGS) -Iinclude -MMD -MP -c $< -o $@

$(TEST_BINDIR)/rel/%: $(TEST_OBJDIR)/rel/tests/%.o $(TEST_LINK_RELEASE_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(TEST_BINDIR)/asan/%: $(LEAKS_OBJDIR)/tests/%.o $(TEST_LINK_LEAKS_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(LEAKS_LDFLAGS) $^ $(LDLIBS) -o $@

# `$(BIN)` is the RELEASE_CFLAGS (-O2) build on purpose: the selftest verifies
# that optimizer transformations did not fold the benchmark loops away.
test: $(BIN) $(TEST_BINS)
	@set -e; \
	for test_bin in $(TEST_BINS); do \
		echo "RUN $$test_bin"; \
		./$$test_bin; \
	done
	@echo "RUN ./$(BIN) selftest"
	@./$(BIN) selftest
	@echo "RUN ./$(BIN) selftest --no-timing"
	@./$(BIN) selftest --no-timing

leaks: $(LEAKS_BIN) $(LEAKS_TEST_BINS)
	@set -e; \
	for test_bin in $(LEAKS_TEST_BINS); do \
		echo "RUN $$test_bin"; \
		ASAN_OPTIONS=detect_leaks=1 \
		UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
		./$$test_bin; \
	done

lint: style-check format-check
	@if command -v $(CLANG_TIDY) >/dev/null 2>&1; then \
		$(CLANG_TIDY) $(SRCS) -- $(CPPFLAGS) $(RELEASE_CFLAGS) -Iinclude; \
	elif [ "$${CI:-}" = "true" ]; then \
		echo "error: $(CLANG_TIDY) is required in CI" >&2; \
		exit 1; \
	else \
		echo "lint: $(CLANG_TIDY) not found, skipping"; \
	fi
	@if command -v $(CPPCHECK) >/dev/null 2>&1; then \
		$(CPPCHECK) --std=c99 --enable=warning,style,performance,portability \
			--error-exitcode=1 --quiet --inline-suppr -Iinclude \
			$(CPPCHECK_DEFINES) $(SRCS); \
	elif [ "$${CI:-}" = "true" ]; then \
		echo "error: $(CPPCHECK) is required in CI" >&2; \
		exit 1; \
	else \
		echo "lint: $(CPPCHECK) not found, skipping"; \
	fi

format:
	@if [ -z "$(FORMAT_FILES)" ]; then \
		echo "format: no C sources or headers found" >&2; \
		exit 1; \
	fi
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	@if [ -z "$(FORMAT_FILES)" ]; then \
		echo "format-check: no C sources or headers found" >&2; \
		exit 1; \
	fi
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

style-check:
	@SPDX_LICENSE='$(SPDX_LICENSE)' sh tools/style-check.sh

clean:
	rm -rf $(BUILD_DIR)
	rm -f latbench latbench-dev latbench-asan

-include $(RELEASE_OBJS:.o=.d) $(DEV_OBJS:.o=.d) $(LEAKS_OBJS:.o=.d) \
	$(TEST_RELEASE_OBJS:.o=.d) $(TEST_LEAKS_OBJS:.o=.d)
