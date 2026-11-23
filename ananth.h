/*
 * =========================================================================
 * Project: Ananth Engine V1.1 (Hardened Cross-Platform Edition)
 * Title:   Dynamic Resource Optimization & Self-Healing Kernel Memory Engine
 * Author:  Mane Ram Naren (Ananth E Tech)
 * Patent:  Indian Patent Application No. 202641127187 (Patent Pending)
 *
 * LICENSE: ANANTH E-TECH FAIR-USE LICENSE
 * - FREE for individuals, researchers & startups (<$2M revenue / <$2M funding).
 * - Commercial license required for Enterprises (HFTs, Cloud Giants, etc.).
 * See LICENSE file for full terms and conditions.
 * =========================================================================
 */

#ifndef ANANTH_H
#define ANANTH_H

#include <cstdint>
#include <cstddef>
#include <atomic>

#ifdef _WIN32
    #define ANANTH_OS_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #ifdef _MSC_VER
        #include <intrin.h>
    #endif
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
#elif defined(__arm__) || defined(_M_ARM) || defined(__i386__) || defined(_M_IX86)
    #define ANANTH_ARCH_32BIT
#endif

#if defined(_MSC_VER)
    #define ANANTH_ALWAYS_INLINE __forceinline
    #define ANANTH_LIKELY(x)     (x)
    #define ANANTH_UNLIKELY(x)   (x)
#else
    #define ANANTH_ALWAYS_INLINE inline __attribute__((always_inline))
    #define ANANTH_LIKELY(x)     __builtin_expect(!!(x), 1)
    #define ANANTH_UNLIKELY(x)   __builtin_expect(!!(x), 0)
#endif

#if UINTPTR_MAX == 0xffffffffffffffffULL
    #define ANANTH_64_BIT
    typedef uint64_t reg_t;
    typedef int64_t  sreg_t;
    constexpr size_t REG_BITS  = 64;
    constexpr size_t REG_SHIFT = 6;
    constexpr size_t REG_MASK  = 63;

    ANANTH_ALWAYS_INLINE unsigned ananth_ctz(reg_t v) noexcept {
    #if defined(_MSC_VER)
        unsigned long idx = 0;
        _BitScanForward64(&idx, v);
        return static_cast<unsigned>(idx);
    #else
        return static_cast<unsigned>(__builtin_ctzll(v));
    #endif
    }

    ANANTH_ALWAYS_INLINE unsigned ananth_clz(reg_t v) noexcept {
    #if defined(_MSC_VER)
        unsigned long idx = 0;
        _BitScanReverse64(&idx, v);
        return static_cast<unsigned>(63 - idx);
    #else
        return static_cast<unsigned>(__builtin_clzll(v));
    #endif
    }
#elif UINTPTR_MAX == 0xffffffffU
    #define ANANTH_32_BIT
    typedef uint32_t reg_t;
    typedef int32_t  sreg_t;
    constexpr size_t REG_BITS  = 32;
    constexpr size_t REG_SHIFT = 5;
    constexpr size_t REG_MASK  = 31;

    ANANTH_ALWAYS_INLINE unsigned ananth_ctz(reg_t v) noexcept {
    #if defined(_MSC_VER)
        unsigned long idx = 0;
        _BitScanForward(&idx, v);
        return static_cast<unsigned>(idx);
    #else
        return static_cast<unsigned>(__builtin_ctz(v));
    #endif
    }

    ANANTH_ALWAYS_INLINE unsigned ananth_clz(reg_t v) noexcept {
    #if defined(_MSC_VER)
        unsigned long idx = 0;
        _BitScanReverse(&idx, v);
        return static_cast<unsigned>(31 - idx);
    #else
        return static_cast<unsigned>(__builtin_clz(v));
    #endif
    }
#else
    #error "Ananth Engine: Unknown CPU Bit Architecture!!"
#endif

#ifdef __GCC_DESTRUCTIVE_SIZE
    constexpr size_t HW_CACHE_LINE = __GCC_DESTRUCTIVE_SIZE;
#else
    constexpr size_t HW_CACHE_LINE = 64;
#endif

// Compact, Cache-Friendly Per-Thread Pockets (~18 KB TLS footprint vs ~264 KB in V1.0)
constexpr size_t FAST_CACHE_CAPACITY = (REG_BITS << 2); // 256 blocks on 64-bit
constexpr size_t FAST_BATCH_SIZE     = (FAST_CACHE_CAPACITY >> 2); // 64
constexpr size_t FAST_BIN_CAPACITY   = 32;              // 32 entries per multi-block bin
constexpr size_t FAST_BIN_BATCH      = 8;

struct alignas(HW_CACHE_LINE) AnanthContext {
    // Cache Line 1: Read-Only Hardware Constants + Full 64-Bit XOR Canary Seed
    char*                buckets_pool;
    std::atomic<reg_t>*  metadata_map;
    size_t               metadata_map_entries;
    size_t               cache_shift;
    size_t               chunk_shift;
    size_t               pool_size;
    uintptr_t            canary_seed;

    // Cache Line 2: Separate Alignment for State & Global Cursor
    alignas(HW_CACHE_LINE) std::atomic<uint8_t> init_state; // 0 = Uninit, 1 = Initing, 2 = Ready
    size_t cursor_pool;
};

struct alignas(HW_CACHE_LINE) FastCache {
    void*  free_list[FAST_CACHE_CAPACITY];
    size_t count;
    void*  bin_list[REG_BITS + 1][FAST_BIN_CAPACITY];
    size_t bin_count[REG_BITS + 1];
    size_t local_cursor;
    bool   initialized;

    ~FastCache() noexcept; // Flushes all cached blocks back to metadata_map on thread exit
};

extern AnanthContext g_engine;
extern thread_local FastCache t_cache;

#define ANANTH_CANARY_SEED (g_engine.canary_seed)

void  Ananth_init() noexcept;
void  Ananth_destroy() noexcept;
void  Ananth_flush_thread_cache() noexcept;
void* Ananth_allocate_slow_refill(size_t req_bytes) noexcept;
void  Ananth_free_slow_flush(void* ptr, size_t req_bytes) noexcept;

// 1-Cycle Inlined Fast-Path with O(1) XOR Canary Unmasking
ANANTH_ALWAYS_INLINE void* Ananth_allocate(size_t req_bytes) noexcept {
    if (ANANTH_UNLIKELY(g_engine.init_state.load(std::memory_order_acquire) != 2)) {
        return Ananth_allocate_slow_refill(req_bytes);
    }

    size_t shift     = g_engine.cache_shift;
    size_t line_size = static_cast<size_t>(1) << shift;

    if (ANANTH_LIKELY(req_bytes <= line_size)) {
        if (ANANTH_LIKELY(t_cache.count > 0)) {
            void* ptr = t_cache.free_list[--t_cache.count];
            return reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ptr));
        }
        return Ananth_allocate_slow_refill(req_bytes);
    }

    size_t blocks = (req_bytes + (line_size - 1)) >> shift;
    if (ANANTH_LIKELY(blocks <= REG_BITS && t_cache.bin_count[blocks] > 0)) {
        void* ptr = t_cache.bin_list[blocks][--t_cache.bin_count[blocks]];
        return reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ptr));
    }
    return Ananth_allocate_slow_refill(req_bytes);
}

// 1-Cycle Inlined Fast-Path with Bounds Verification & O(1) XOR Canary Masking
ANANTH_ALWAYS_INLINE void Ananth_free(void* ptr, size_t req_bytes) noexcept {
    if (ANANTH_UNLIKELY(ptr == nullptr)) return;
    if (ANANTH_UNLIKELY(g_engine.init_state.load(std::memory_order_acquire) != 2)) return;

    // Guard against foreign / out-of-pool pointers before caching in t_cache
    uintptr_t offset = reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(g_engine.buckets_pool);
    if (ANANTH_UNLIKELY(offset >= g_engine.pool_size)) return;

    size_t shift     = g_engine.cache_shift;
    size_t line_size = static_cast<size_t>(1) << shift;

    if (ANANTH_LIKELY(req_bytes <= line_size)) {
        if (ANANTH_LIKELY(t_cache.count < FAST_CACHE_CAPACITY)) {
            t_cache.free_list[t_cache.count++] =
                reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ptr));
            return;
        }
        Ananth_free_slow_flush(ptr, req_bytes);
        return;
    }

    size_t blocks = (req_bytes + (line_size - 1)) >> shift;
    if (ANANTH_LIKELY(blocks <= REG_BITS && t_cache.bin_count[blocks] < FAST_BIN_CAPACITY)) {
        t_cache.bin_list[blocks][t_cache.bin_count[blocks]++] =
            reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ptr));
        return;
    }
    Ananth_free_slow_flush(ptr, req_bytes);
}

#endif // ANANTH_H
