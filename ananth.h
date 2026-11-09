/*
 * ============================================================================
 * Project: Ananth Engine V1.0
 * Title:   Dynamic Resource Optimization & Self-Healing Kernel Memory Engine
 * Author:  Mane Ram Naren (Ananth E Tech)
 * Patent:  Indian Patent Application No. 202641127187 (Patent Pending)
 *
 * LICENSE: Ananth E-Tech Fair-Use License
 * Permission is hereby granted, free of charge, to any student, academic
 * researcher, or non-commercial entity to use, study, modify, and distribute
 * this software for educational and non-commercial research purposes only.
 * Any commercial, corporate, or production enterprise deployment strictly
 * requires a separate Commercial License from Ananth E Tech.
 * ============================================================================
 */

#ifndef ANANTH_H
#define ANANTH_H

#include <iostream>
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>

#ifdef _WIN32
    #define ANANTH_OS_WINDOWS
    #include <windows.h>
#elif defined(__linux__) && !defined(__ANDROID__)
    #define ANANTH_OS_LINUX
    #include <unistd.h>
    #include <sys/mman.h>
    #include <sched.h>
#elif defined(__ANDROID__)
    #define ANANTH_OS_ANDROID
    #include <unistd.h>
    #include <sys/mman.h>
    #include <sched.h>
#elif defined(__APPLE__)
    #define ANANTH_OS_APPLE
    #include <unistd.h>
    #include <sys/mman.h>
    #include <sched.h>
#else
    #error "Ananth Engine failed to recognise the OS platform"
#endif

#if defined(__amd64__) || defined(__x86_64__) || defined(_M_X64)
    #define ANANTH_ARCH_X86_64_AMD_INTEL
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define ANANTH_ARCH_ARM64
#elif defined(__arm__) || defined(_M_ARM) || defined(__i386__)
    #define ANANTH_ARCH_32BIT
#endif

#if UINTPTR_MAX == 0xffffffffffffffffULL
    #define ANANTH_64_BIT
    typedef uint64_t reg_t;
    typedef int64_t  sreg_t;
    #define REG_BITS  (sizeof(reg_t) << 3)
    #define REG_SHIFT 6
    #define REG_MASK  (REG_BITS - 1)
    #define REG_CTZ   __builtin_ctzll
    #define REG_CLZ   __builtin_clzll
#elif UINTPTR_MAX == 0xffffffffU
    #define ANANTH_32_BIT
    typedef uint32_t reg_t;
    typedef int32_t  sreg_t;
    #define REG_BITS  (sizeof(reg_t) << 3)
    #define REG_SHIFT 5
    #define REG_MASK  (REG_BITS - 1)
    #define REG_CTZ   __builtin_ctz
    #define REG_CLZ   __builtin_clz
#else
    #error "Ananth Engine: Unknown CPU Bit Architecture!!"
#endif

#define CANARY_SIZE sizeof(reg_t)

#ifdef __GCC_DESTRUCTIVE_SIZE
    #define HW_CACHE_LINE __GCC_DESTRUCTIVE_SIZE
#else
    #define HW_CACHE_LINE REG_BITS
#endif

// Elastic Hardware Pockets: Scales dynamically with CPU Architecture
constexpr size_t FAST_CACHE_CAPACITY = (REG_BITS << 3);
constexpr size_t FAST_BATCH_SIZE     = (FAST_CACHE_CAPACITY >> 2);
constexpr size_t FAST_BIN_CAPACITY   = (REG_BITS << 3);
constexpr size_t FAST_BIN_BATCH      = (FAST_BIN_CAPACITY >> 2);

struct alignas(HW_CACHE_LINE) AnanthContext {
    // Cache Line 1: Read-Only Hardware Constants + XOR Canary Seed (Zero Extra Cache Misses!)
    char*     buckets_pool;
    reg_t*    metadata_map;
    size_t    metadata_map_entries;
    size_t    cache_shift;
    size_t    chunk_shift;
    size_t    pool_size;
    uintptr_t canary_seed;

    // Cache Line 2: Separate Alignment for Global Cursor
    alignas(HW_CACHE_LINE) size_t cursor_pool;
};

struct alignas(HW_CACHE_LINE) FastCache {
    void*  free_list[FAST_CACHE_CAPACITY];
    size_t count;
    void*  bin_list[REG_BITS + 1][FAST_BIN_CAPACITY];
    size_t bin_count[REG_BITS + 1];
    size_t local_cursor;
    bool   initialized;
};

extern AnanthContext g_engine;
extern thread_local FastCache t_cache;

// Dynamic Hardware XOR Canary Seed Macro
#define ANANTH_CANARY_SEED (g_engine.canary_seed)

void  Ananth_init();
void* Ananth_allocate_slow_refill(size_t req_bytes);
void  Ananth_free_slow_flush(void* ptr, size_t req_bytes);

// 1-Cycle Inlined Fast-Path with O(1) XOR Canary Unmasking
inline __attribute__((always_inline)) void* Ananth_allocate(size_t req_bytes) {
    size_t shift     = g_engine.cache_shift;
    size_t line_size = static_cast<size_t>(1) << shift;

    if (__builtin_expect(req_bytes <= line_size, 1)) {
        if (__builtin_expect(t_cache.count > 0, 1)) {
            void* ptr = t_cache.free_list[--t_cache.count];
            return reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ptr));
        }
        return Ananth_allocate_slow_refill(req_bytes);
    }

    size_t blocks = (req_bytes + (line_size - 1)) >> shift;
    if (__builtin_expect(blocks <= REG_BITS && t_cache.bin_count[blocks] > 0, 1)) {
        void* ptr = t_cache.bin_list[blocks][--t_cache.bin_count[blocks]];
        return reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ptr));
    }
    return Ananth_allocate_slow_refill(req_bytes);
}

// 1-Cycle Inlined Fast-Path with O(1) XOR Canary Masking
inline __attribute__((always_inline)) void Ananth_free(void* ptr, size_t req_bytes) {
    if (__builtin_expect(ptr == nullptr, 0)) return;

    size_t shift     = g_engine.cache_shift;
    size_t line_size = static_cast<size_t>(1) << shift;

    if (__builtin_expect(req_bytes <= line_size, 1)) {
        if (__builtin_expect(t_cache.count < FAST_CACHE_CAPACITY, 1)) {
            t_cache.free_list[t_cache.count++] =
                reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ptr));
            return;
        }
        Ananth_free_slow_flush(ptr, req_bytes);
        return;
    }

    size_t blocks = (req_bytes + (line_size - 1)) >> shift;
    if (__builtin_expect(blocks <= REG_BITS && t_cache.bin_count[blocks] < FAST_BIN_CAPACITY, 1)) {
        t_cache.bin_list[blocks][t_cache.bin_count[blocks]++] =
            reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ptr));
        return;
    }
    Ananth_free_slow_flush(ptr, req_bytes);
}

#endif