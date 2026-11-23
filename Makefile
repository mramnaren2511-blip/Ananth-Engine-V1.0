CXX      := g++
CXXFLAGS := -O3 -std=c++17 -Wall -Wextra -pthread

TEST_BIN  := tests/stress_test
BENCH_BIN := benchmarks/bench_suite

.PHONY: all test bench clean

all: $(TEST_BIN) $(BENCH_BIN)

$(TEST_BIN): tests/stress_test.cpp ananth.cpp ananth.h
	$(CXX) $(CXXFLAGS) -I. tests/stress_test.cpp ananth.cpp -o $(TEST_BIN)

$(BENCH_BIN): benchmarks/bench_suite.cpp ananth.cpp ananth.h
	$(CXX) $(CXXFLAGS) -I. benchmarks/bench_suite.cpp ananth.cpp -o $(BENCH_BIN)

test: $(TEST_BIN)
	@echo "=== Running Ananth Engine V1.1 Correctness, Zero-Leak & Concurrency Suite ==="
	./$(TEST_BIN)

bench: $(BENCH_BIN)
	@echo "=== Running Ananth Engine V1.1 vs System glibc Benchmark ==="
	./$(BENCH_BIN)

clean:
	rm -f $(TEST_BIN) $(BENCH_BIN)
