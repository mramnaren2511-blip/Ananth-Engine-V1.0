// ============================================================================
// ANANTH ENGINE V1.1 vs SYSTEM GLIBC BENCHMARK SUITE
// ============================================================================
#include "ananth.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static inline void escape(void* p) {
    asm volatile("" : : "g"(p) : "memory");
}

static double bench_st(bool use_ananth, size_t sz, size_t iters, size_t batch) {
    std::vector<void*> ptrs(batch, nullptr);
    auto t0 = Clock::now();
    for (size_t r = 0; r < iters / batch; ++r) {
        for (size_t i = 0; i < batch; ++i) {
            void* p = use_ananth ? Ananth_allocate(sz) : std::malloc(sz);
            static_cast<volatile char*>(p)[0] = 1;
            escape(p);
            ptrs[i] = p;
        }
        for (size_t i = 0; i < batch; ++i) {
            if (use_ananth) Ananth_free(ptrs[i], sz);
            else std::free(ptrs[i]);
        }
    }
    auto t1 = Clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    return (static_cast<double>(iters) / sec) / 1e6;
}

static double bench_mt(bool use_ananth, int nthreads, size_t sz, size_t iters_per_thread) {
    constexpr size_t BATCH = 32;
    auto t0 = Clock::now();
    std::vector<std::thread> ts;
    for (int t = 0; t < nthreads; ++t) {
        ts.emplace_back([use_ananth, sz, iters_per_thread]() {
            std::vector<void*> ptrs(BATCH, nullptr);
            for (size_t r = 0; r < iters_per_thread / BATCH; ++r) {
                for (size_t i = 0; i < BATCH; ++i) {
                    void* p = use_ananth ? Ananth_allocate(sz) : std::malloc(sz);
                    static_cast<volatile char*>(p)[0] = 1;
                    escape(p);
                    ptrs[i] = p;
                }
                for (size_t i = 0; i < BATCH; ++i) {
                    if (use_ananth) Ananth_free(ptrs[i], sz);
                    else std::free(ptrs[i]);
                }
            }
        });
    }
    for (auto& t : ts) t.join();
    auto t1 = Clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    return (static_cast<double>(iters_per_thread * nthreads) / sec) / 1e6;
}

int main() {
    Ananth_init();

    double g_64b  = bench_st(false, 64, 2000000, 64);
    double a_64b  = bench_st(true,  64, 2000000, 64);

    double g_256b = bench_st(false, 256, 1000000, 32);
    double a_256b = bench_st(true,  256, 1000000, 32);

    double g_2kb  = bench_st(false, 2048, 500000, 16);
    double a_2kb  = bench_st(true,  2048, 500000, 16);

    double g_8kb  = bench_st(false, 8192, 100000, 8);
    double a_8kb  = bench_st(true,  8192, 100000, 8);

    double g_4t64 = bench_mt(false, 4, 64, 1000000);
    double a_4t64 = bench_mt(true,  4, 64, 1000000);

    std::printf("Workload             |   glibc (M/s) | Ananth V1.1 (M/s) | Speedup\n");
    std::printf("---------------------+---------------+-------------------+--------\n");
    std::printf("64 B  (Single-Thread)| %13.2f | %17.2f | %6.2fx\n", g_64b,  a_64b,  a_64b / g_64b);
    std::printf("256 B (Single-Thread)| %13.2f | %17.2f | %6.2fx\n", g_256b, a_256b, a_256b / g_256b);
    std::printf("2 KB  (Single-Thread)| %13.2f | %17.2f | %6.2fx\n", g_2kb,  a_2kb,  a_2kb / g_2kb);
    std::printf("8 KB  (Single-Thread)| %13.2f | %17.2f | %6.2fx\n", g_8kb,  a_8kb,  a_8kb / g_8kb);
    std::printf("64 B  (4 Threads)    | %13.2f | %17.2f | %6.2fx\n", g_4t64, a_4t64, a_4t64 / g_4t64);

    Ananth_destroy();
    return 0;
}
