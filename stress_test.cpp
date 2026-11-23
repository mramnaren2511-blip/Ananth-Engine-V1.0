// ============================================================================
// ANANTH ENGINE V1.1 - CORRECTNESS, ZERO-LEAK & CONCURRENCY STRESS SUITE
// ============================================================================
#include "ananth.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static void test_auto_init_and_sizes() {
    // Deliberately call Ananth_allocate BEFORE Ananth_init() to verify safe auto-init
    const size_t sizes[] = {16, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 65536};
    for (size_t sz : sizes) {
        uint8_t* p = static_cast<uint8_t*>(Ananth_allocate(sz));
        assert(p != nullptr);
        uint8_t pat = static_cast<uint8_t>((sz * 19 + 7) & 0xFF);
        std::memset(p, pat, sz);
        for (size_t i = 0; i < sz; ++i) assert(p[i] == pat);
        Ananth_free(p, sz);
    }
    std::printf("[PASS] test_auto_init_and_sizes (16B .. 64KB without prior Ananth_init)\n");
}

static void test_foreign_pointer_guard() {
    int stack_var = 42;
    Ananth_free(&stack_var, 64); // Must be safely ignored
    Ananth_free(nullptr, 64);    // Must be safely ignored
    std::printf("[PASS] test_foreign_pointer_guard\n");
}

static void test_thread_exit_zero_leak() {
    // Spawn 64 threads that allocate & free into their thread_local FastCache and exit.
    // FastCache::~FastCache() must flush every block back to g_engine.metadata_map.
    for (int round = 0; round < 64; ++round) {
        std::thread t([]() {
            std::vector<void*> ptrs;
            for (int i = 0; i < 128; ++i) {
                void* p = Ananth_allocate(64);
                assert(p != nullptr);
                ptrs.push_back(p);
            }
            for (void* p : ptrs) Ananth_free(p, 64);
            // Exiting with all 128 blocks sitting in t_cache.free_list!
        });
        t.join();
    }

    // Verify all metadata_map bits are fully restored after flushing main thread cache
    Ananth_flush_thread_cache();
    reg_t all_free = ~static_cast<reg_t>(0);
    for (size_t i = 0; i < g_engine.metadata_map_entries; ++i) {
        assert(g_engine.metadata_map[i].load(std::memory_order_acquire) == all_free);
    }
    std::printf("[PASS] test_thread_exit_zero_leak (64 thread teardowns, 100%% bitmap reclaimed)\n");
}

static void test_multithreaded_stress() {
    constexpr int NTHREADS = 8;
    constexpr int ITERS = 10000;
    std::vector<std::thread> threads;

    for (int tid = 0; tid < NTHREADS; ++tid) {
        threads.emplace_back([tid]() {
            std::vector<std::pair<void*, size_t>> live;
            live.reserve(32);
            const size_t mix[] = {64, 128, 256, 512, 1024, 2048, 8192};
            for (int i = 0; i < ITERS; ++i) {
                size_t sz = mix[(i + tid) % 7];
                void* p = Ananth_allocate(sz);
                assert(p != nullptr);
                static_cast<uint8_t*>(p)[0] = static_cast<uint8_t>(tid);
                static_cast<uint8_t*>(p)[sz - 1] = static_cast<uint8_t>(tid ^ 0x55);
                live.emplace_back(p, sz);

                if (live.size() >= 16) {
                    for (auto& kv : live) {
                        uint8_t* b = static_cast<uint8_t*>(kv.first);
                        assert(b[0] == static_cast<uint8_t>(tid));
                        assert(b[kv.second - 1] == static_cast<uint8_t>(tid ^ 0x55));
                        Ananth_free(kv.first, kv.second);
                    }
                    live.clear();
                }
            }
            for (auto& kv : live) Ananth_free(kv.first, kv.second);
        });
    }
    for (auto& t : threads) t.join();

    Ananth_flush_thread_cache();
    reg_t all_free = ~static_cast<reg_t>(0);
    for (size_t i = 0; i < g_engine.metadata_map_entries; ++i) {
        assert(g_engine.metadata_map[i].load(std::memory_order_acquire) == all_free);
    }
    std::printf("[PASS] test_multithreaded_stress (8 threads x 10K mixed allocs, 100%% reclaimed)\n");
}

static void test_destroy_and_reinit() {
    Ananth_destroy();
    assert(g_engine.buckets_pool == nullptr);
    assert(g_engine.metadata_map == nullptr);
    void* p = Ananth_allocate(128);
    assert(p != nullptr);
    Ananth_free(p, 128);
    Ananth_destroy();
    std::printf("[PASS] test_destroy_and_reinit\n");
}

int main() {
    test_auto_init_and_sizes();
    test_foreign_pointer_guard();
    test_thread_exit_zero_leak();
    test_multithreaded_stress();
    test_destroy_and_reinit();
    std::printf("ALL ANANTH ENGINE V1.1 TESTS PASSED!\n");
    return 0;
}
