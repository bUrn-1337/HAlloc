CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -O2 -g

# Test builds use sanitizers to catch memory bugs.
TEST_FLAGS  = $(CFLAGS) -fsanitize=address,undefined

# Benchmark builds must NOT use sanitizers — we want real timings.
BENCH_FLAGS = $(CFLAGS)

SRC_DIR   = src
TEST_DIR  = tests
BENCH_DIR = bench

SRC_HEADERS = $(SRC_DIR)/freelist.h $(SRC_DIR)/halloc.h $(SRC_DIR)/slab.h

# Sanitized object files shared by all test binaries.
TEST_SRC_OBJS = $(TEST_DIR)/freelist.test.o \
                $(TEST_DIR)/slab.test.o     \
                $(TEST_DIR)/halloc.test.o

# Unsanitized object files for benchmark.
BENCH_SRC_OBJS = $(BENCH_DIR)/freelist.bench.o \
                 $(BENCH_DIR)/slab.bench.o     \
                 $(BENCH_DIR)/halloc.bench.o

# -------------------------------------------------------------------------
# Primary targets
# -------------------------------------------------------------------------

.PHONY: all test bench clean

all: test bench

# -------------------------------------------------------------------------
# Sanitized source objects (used by every test binary)
# -------------------------------------------------------------------------

$(TEST_DIR)/freelist.test.o: $(SRC_DIR)/freelist.c $(SRC_HEADERS)
	$(CC) $(TEST_FLAGS) -c $< -o $@

$(TEST_DIR)/slab.test.o: $(SRC_DIR)/slab.c $(SRC_HEADERS)
	$(CC) $(TEST_FLAGS) -c $< -o $@

$(TEST_DIR)/halloc.test.o: $(SRC_DIR)/halloc.c $(SRC_HEADERS)
	$(CC) $(TEST_FLAGS) -c $< -o $@

# -------------------------------------------------------------------------
# test_freelist binary
# -------------------------------------------------------------------------

$(TEST_DIR)/test_freelist.o: $(TEST_DIR)/test_freelist.c $(SRC_DIR)/freelist.h
	$(CC) $(TEST_FLAGS) -c $< -o $@

$(TEST_DIR)/test_freelist: $(TEST_DIR)/test_freelist.o $(TEST_SRC_OBJS)
	$(CC) $(TEST_FLAGS) $^ -o $@

# -------------------------------------------------------------------------
# test_slab binary
# -------------------------------------------------------------------------

$(TEST_DIR)/test_slab.o: $(TEST_DIR)/test_slab.c $(SRC_HEADERS)
	$(CC) $(TEST_FLAGS) -c $< -o $@

$(TEST_DIR)/test_slab: $(TEST_DIR)/test_slab.o $(TEST_SRC_OBJS)
	$(CC) $(TEST_FLAGS) $^ -o $@

# -------------------------------------------------------------------------
# test target: run both test suites
# -------------------------------------------------------------------------

test: $(TEST_DIR)/test_freelist $(TEST_DIR)/test_slab
	@echo ""
	@echo "--- Phase 1: free-list ---"
	./$(TEST_DIR)/test_freelist
	@echo ""
	@echo "--- Phase 2: slab ---"
	./$(TEST_DIR)/test_slab

# -------------------------------------------------------------------------
# Unsanitized source objects (benchmark)
# -------------------------------------------------------------------------

$(BENCH_DIR)/freelist.bench.o: $(SRC_DIR)/freelist.c $(SRC_HEADERS)
	$(CC) $(BENCH_FLAGS) -c $< -o $@

$(BENCH_DIR)/slab.bench.o: $(SRC_DIR)/slab.c $(SRC_HEADERS)
	$(CC) $(BENCH_FLAGS) -c $< -o $@

$(BENCH_DIR)/halloc.bench.o: $(SRC_DIR)/halloc.c $(SRC_HEADERS)
	$(CC) $(BENCH_FLAGS) -c $< -o $@

$(BENCH_DIR)/bench.o: $(BENCH_DIR)/bench.c $(SRC_DIR)/freelist.h $(BENCH_DIR)/bench.h
	$(CC) $(BENCH_FLAGS) -c $< -o $@

bench: $(BENCH_DIR)/bench
	./$(BENCH_DIR)/bench

$(BENCH_DIR)/bench: $(BENCH_DIR)/bench.o $(BENCH_SRC_OBJS)
	$(CC) $(BENCH_FLAGS) $^ -o $@ -lm

# -------------------------------------------------------------------------
# Clean
# -------------------------------------------------------------------------

clean:
	rm -f $(TEST_DIR)/*.o  $(TEST_DIR)/test_freelist $(TEST_DIR)/test_slab \
	      $(BENCH_DIR)/*.o $(BENCH_DIR)/bench
