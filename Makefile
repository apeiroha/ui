# ============================================================
# ui — 并发/协程库 (うい) build system
# ============================================================
#
# Toolchain selection (TOOLCHAIN=):
#   zig      — zig cc + musl
#   gcc      — gcc + glibc
#   gcc-musl — gcc + musl-cross
#   clang    — clang + musl (default)
#
# Build mode (BUILD=):
#   debug   — -O0, full debug info (default)
#   release — optimized build, strip symbols
#
# Standard (STD=):
#   c23     — ISO C23 (default)
#   gnu23   — C23 + GNU extensions
#
# Optimization notes:
#   - release: clang -O3, gcc/gcc-musl -O2, zig -O0 (LLVM PIE bug).

TOOLCHAIN ?= clang
BUILD    ?= debug
STD      ?= c23

# 禁用 make 内置隐含规则（避免 .iroha.c ↔ .iroha 循环依赖）
MAKEFLAGS += -r

ifeq ($(TOOLCHAIN), zig)
  CC       = zig cc
  _TARGET  = -target x86_64-linux-musl
  _STATIC  = -static
  _PIE     =
  _LIB     = -lpthread
  _OPT_UI  = -O0
endif

ifeq ($(TOOLCHAIN), gcc)
  CC       = x86_64-linux-gnu-gcc
  _TARGET  =
  _STATIC  = -static
  _PIE     = -fno-pie -no-pie
  _LIB     = -lpthread
  _OPT_UI  = $(if $(filter release,$(BUILD)),-O2,-O0)
endif

ifeq ($(TOOLCHAIN), gcc-musl)
  CC       = x86_64-linux-musl-gcc
  _TARGET  =
  _STATIC  = -static
  _PIE     = -fno-pie -no-pie
  _LIB     = -lpthread
  _OPT_UI  = $(if $(filter release,$(BUILD)),-O2,-O0)
endif

ifeq ($(TOOLCHAIN), clang)
  CC       = clang
  _TARGET  = -target x86_64-linux-musl
  _STATIC  = -static
  _PIE     =
  _LIB     = -lpthread
  _OPT_UI  = $(if $(filter release,$(BUILD)),-O3,-O0)
endif

_CFLAGS_COMMON = -std=$(STD) -g -Wall -Wextra -msse4.2
UI_CFLAGS = $(_TARGET) $(_STATIC) $(_OPT_UI) $(_CFLAGS_COMMON) $(_PIE) -D_GNU_SOURCE
LDFLAGS   = $(_TARGET) $(_STATIC) $(_PIE)
STRIP ?= $(if $(filter release,$(BUILD)),yes,no)

BUILD_DIR = build

# ── UI sources ──
UI_SRC     = ui.c ui_core.c ui_stack.c ui_chan.c ui_sync.c ui_io.c ui_waitq.c
UI_START   = start.c
UI_ASM     = ui_switch.S

# ── Library ──
libui: $(BUILD_DIR)/libui.a

$(BUILD_DIR)/libui.a: $(UI_SRC) $(UI_START) $(UI_ASM) ui.h ui_internal.h | $(BUILD_DIR)
	$(CC) $(UI_CFLAGS) -I. -fno-sanitize=all -c -o $(BUILD_DIR)/ui_core.o ui_core.c
	$(CC) $(UI_CFLAGS) -I. -fno-sanitize=all -c -o $(BUILD_DIR)/ui_stack.o ui_stack.c
	$(CC) $(UI_CFLAGS) -I. -fno-sanitize=all -c -o $(BUILD_DIR)/ui_chan.o ui_chan.c
	$(CC) $(UI_CFLAGS) -I. -fno-sanitize=all -c -o $(BUILD_DIR)/ui_sync.o ui_sync.c
	$(CC) $(UI_CFLAGS) -I. -fno-sanitize=all -c -o $(BUILD_DIR)/ui_io.o ui_io.c
	$(CC) $(UI_CFLAGS) -I. -fno-sanitize=all -c -o $(BUILD_DIR)/ui_waitq.o ui_waitq.c
	$(CC) $(UI_CFLAGS) -I. -fno-sanitize=all -c -o $(BUILD_DIR)/start.o start.c
	$(CC) $(_TARGET) $(_STATIC) -c -o $(BUILD_DIR)/ui_switch.o ui_switch.S
	$(AR) rcs $@ $(BUILD_DIR)/ui_core.o $(BUILD_DIR)/ui_stack.o $(BUILD_DIR)/ui_chan.o \
	          $(BUILD_DIR)/ui_sync.o $(BUILD_DIR)/ui_io.o $(BUILD_DIR)/ui_waitq.o \
	          $(BUILD_DIR)/start.o $(BUILD_DIR)/ui_switch.o

$(BUILD_DIR)/start.o: start.c ui.h ui_internal.h | $(BUILD_DIR)
	$(CC) $(UI_CFLAGS) -I. -fno-sanitize=all -c -o $@ start.c

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# ── Tests ──
TEST_UI_SRC = tests/test_ui.c
TEST_C8_SRC = tests/test_c8_race.c
TEST_P0_SRC = tests/test_p0.c
TEST_UDP_SRC = tests/test_udp_echo.c
TEST_IOPARK_SRC = tests/test_sched_iopark.c
TEST_OVERFLOW_SRC = tests/test_stack_overflow.c

$(BUILD_DIR)/test_ui: $(TEST_UI_SRC) $(UI_SRC) $(UI_ASM) $(BUILD_DIR)/start.o | $(BUILD_DIR)
	$(CC) $(UI_CFLAGS) -I. -o $@ $^ $(LDFLAGS) $(_LIB)
ifneq ($(STRIP),no)
	strip $@
endif

test-ui: $(BUILD_DIR)/test_ui
	./$(BUILD_DIR)/test_ui

$(BUILD_DIR)/test_c8_race: $(TEST_C8_SRC) $(UI_SRC) $(UI_ASM) $(BUILD_DIR)/start.o | $(BUILD_DIR)
	$(CC) $(UI_CFLAGS) -I. -o $@ $^ $(LDFLAGS) $(_LIB)

test-c8-race: $(BUILD_DIR)/test_c8_race
	./$(BUILD_DIR)/test_c8_race

$(BUILD_DIR)/test_p0: $(TEST_P0_SRC) $(UI_SRC) $(UI_ASM) $(BUILD_DIR)/start.o | $(BUILD_DIR)
	$(CC) $(UI_CFLAGS) -I. -o $@ $^ $(LDFLAGS) $(_LIB)

test-p0: $(BUILD_DIR)/test_p0
	./$(BUILD_DIR)/test_p0

$(BUILD_DIR)/test_p0_asan: $(TEST_P0_SRC) $(UI_SRC) $(UI_ASM) $(BUILD_DIR)/start.o | $(BUILD_DIR)
	$(CC) -O1 -g -fsanitize=address -fno-omit-frame-pointer -D_GNU_SOURCE \
	  $(_CFLAGS_COMMON) -I. -o $@ $^ $(_PIE) $(_LIB) -fsanitize=address

test-p0-asan: $(BUILD_DIR)/test_p0_asan
	ASAN_OPTIONS=abort_on_error=1:halt_on_error=1 ./$(BUILD_DIR)/test_p0_asan

$(BUILD_DIR)/test_udp_echo: $(TEST_UDP_SRC) $(UI_SRC) $(UI_ASM) $(BUILD_DIR)/start.o | $(BUILD_DIR)
	$(CC) $(UI_CFLAGS) -I. -o $@ $^ $(LDFLAGS) $(_LIB)

test-udp-echo: $(BUILD_DIR)/test_udp_echo
	./$(BUILD_DIR)/test_udp_echo

$(BUILD_DIR)/test_sched_iopark: $(TEST_IOPARK_SRC) $(UI_SRC) $(UI_ASM) $(BUILD_DIR)/start.o | $(BUILD_DIR)
	$(CC) $(UI_CFLAGS) -I. -o $@ $^ $(LDFLAGS) $(_LIB)

test-sched-iopark: $(BUILD_DIR)/test_sched_iopark
	./$(BUILD_DIR)/test_sched_iopark

TEST_SPAWN_STEAL_SRC = tests/test_spawn_steal.c

$(BUILD_DIR)/test_spawn_steal: $(TEST_SPAWN_STEAL_SRC) $(UI_SRC) $(UI_ASM) $(BUILD_DIR)/start.o | $(BUILD_DIR)
	$(CC) $(UI_CFLAGS) -I. -o $@ $^ $(LDFLAGS) $(_LIB)

test-spawn-steal: $(BUILD_DIR)/test_spawn_steal
	./$(BUILD_DIR)/test_spawn_steal

TEST_LIFO_SLOT_SRC = tests/test_lifo_slot.c

$(BUILD_DIR)/test_lifo_slot: $(TEST_LIFO_SLOT_SRC) $(UI_SRC) $(UI_ASM) $(BUILD_DIR)/start.o | $(BUILD_DIR)
	$(CC) $(UI_CFLAGS) -I. -o $@ $^ $(LDFLAGS) $(_LIB)

test-lifo-slot: $(BUILD_DIR)/test_lifo_slot
	./$(BUILD_DIR)/test_lifo_slot

$(BUILD_DIR)/test_stack_overflow: $(TEST_OVERFLOW_SRC) $(UI_SRC) $(UI_ASM) $(BUILD_DIR)/start.o | $(BUILD_DIR)
	$(CC) $(UI_CFLAGS) -I. -o $@ $^ $(LDFLAGS) $(_LIB)

test-stack-overflow: $(BUILD_DIR)/test_stack_overflow
	@out=$$(./$(BUILD_DIR)/test_stack_overflow 2>&1); rc=$$?; \
	  echo "$$out" | grep -q "ui stack overflow: exceeded 8MB limit" && \
	  [ "$$rc" -eq 1 ] && \
	  echo "  TEST: 8MB overflow clean report (guard page) ... PASS" || \
	  { echo "$$out"; echo "  TEST: 8MB overflow clean report (guard page) ... FAIL (rc=$$rc)"; exit 1; }

# ── Benchmarks ──
$(BUILD_DIR)/bench_ui: tests/bench_ui.c $(UI_SRC) $(UI_ASM) $(BUILD_DIR)/start.o | $(BUILD_DIR)
	$(CC) $(UI_CFLAGS) -I. -o $@ $^ $(LDFLAGS) $(_LIB)
ifneq ($(STRIP),no)
	strip $@
endif

bench-ui: $(BUILD_DIR)/bench_ui
	./$(BUILD_DIR)/bench_ui

$(BUILD_DIR)/bench_ui_io: tests/bench_ui_io.c $(UI_SRC) $(UI_ASM) $(BUILD_DIR)/start.o | $(BUILD_DIR)
	$(CC) $(UI_CFLAGS) -I. -o $@ $^ $(LDFLAGS) $(_LIB)
ifneq ($(STRIP),no)
	strip $@
endif

bench-ui-io: $(BUILD_DIR)/bench_ui_io
	./$(BUILD_DIR)/bench_ui_io

# ── Perf profiling ──
.PHONY: bench-ui-perf
bench-ui-perf:
	@echo "Usage: scripts/perf-ui.sh <command>"
	@echo ""

# ── Aggregate ──
.PHONY: all test test-ui test-c8-race test-p0 test-p0-asan test-udp-echo \
        test-sched-iopark test-spawn-steal test-lifo-slot test-stack-overflow \
        bench-ui bench-ui-io libui clean

test: test-ui test-c8-race test-p0 test-udp-echo test-sched-iopark \
       test-spawn-steal test-stack-overflow

all: libui

clean:
	rm -rf $(BUILD_DIR)