# disk-sim - HDD I/O simulator
#
#   make          build libdisksim.a, the disksim CLI and gen_trace
#   make lib      build only build/libdisksim.a (+ include/disksim.h is the API)
#   make test     build + run unit tests
#   make asan     rebuild everything with AddressSanitizer + UBSan, run tests
#                 and a few full simulations under the sanitizers
#   make bench    run every scheduler x workload (+/- defects), write
#                 results/results.csv, results/results.md and PNG charts
#   make clean
#
# Sanitizer builds go to a separate directory (build-asan/) so switching
# between `make test` and `make asan` never mixes instrumented and
# uninstrumented object files.

CC      = gcc
BUILD  ?= build
SAN    ?= 0

WARN    = -Wall -Wextra -Werror -pedantic -Wshadow -Wstrict-prototypes \
          -Wmissing-prototypes -Wpointer-arith -Wformat=2
ifeq ($(SAN),1)
  OPT   = -O1 -g -fno-omit-frame-pointer
  SANF  = -fsanitize=address,undefined -fno-sanitize-recover=all
else
  OPT   = -O2 -g
  SANF  =
endif

CFLAGS  = -std=c11 $(WARN) $(OPT) $(SANF) -Iinclude -Isrc -MMD -MP
LDFLAGS = $(SANF)
LDLIBS  = -lm

LIB_SRCS  = src/geometry.c src/defects.c src/timing.c src/disk.c src/rng.c \
            src/workload.c src/scheduler.c src/sim.c src/metrics.c
TEST_SRCS = $(wildcard tests/*.c)

LIB_OBJS  = $(LIB_SRCS:%.c=$(BUILD)/%.o)
TEST_OBJS = $(TEST_SRCS:%.c=$(BUILD)/%.o)
LIB       = $(BUILD)/libdisksim.a
CLI       = $(BUILD)/disksim
GEN       = $(BUILD)/gen_trace
TESTS     = $(BUILD)/run_tests

PYTHON   ?= python3

.PHONY: all lib test asan bench clean

all: $(LIB) $(CLI) $(GEN)

lib: $(LIB)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJS)
	ar rcs $@ $^

$(CLI): $(BUILD)/src/main.o $(LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(GEN): $(BUILD)/tools/gen_trace.o $(LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(TESTS): $(TEST_OBJS) $(LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

test: $(TESTS)
	./$(TESTS)

# Re-invoke make with the sanitizer configuration, then exercise the CLI on
# every workload and scheduler family so the sanitizers see the real paths
# (trace parsing, defect remapping, SCAN sweeps), not only the unit tests.
asan:
	$(MAKE) BUILD=build-asan SAN=1 all build-asan/run_tests
	./build-asan/run_tests
	./build-asan/disksim --quiet --workload seq --requests 300 --rate 0 --size 128 --defects 2000 --plist 500 --sched sptf
	./build-asan/disksim --quiet --workload random --requests 500 --sched scan
	./build-asan/disksim --quiet --workload hotcold --requests 500 --sched cscan --qdepth 32
	./build-asan/disksim --quiet --trace traces/sample_trace.csv --sched sptf-aged --defects 100
	./build-asan/disksim --quiet --workload random --requests 300 --no-zoned --sector-size 4096 --sched clook
	@echo "asan: all tests and simulations clean"

bench: all
	sh bench/run_bench.sh $(BUILD)/disksim results
	$(PYTHON) bench/plot.py results

clean:
	rm -rf build build-asan

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(BUILD)/src/main.d $(BUILD)/tools/gen_trace.d
