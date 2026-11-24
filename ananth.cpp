/*
 * =========================================================================
 * Project: Ananth Engine V1.1 (Hardened Cross-Platform Edition)
 * Title:   Dynamic Resource Optimization & Self-Healing Kernel Memory Engine
 * Author:  Mane Ram Naren (Ananth E Tech)
 * Patent:  Indian Patent Application No. 202641127187 (Patent Pending)
 *
 * LICENSE: ANANTH E-TECH FAIR-USE LICENSE
 * - FREE for individuals, researchers & startups (<$1M revenue / <$1M funding).
 * - Commercial license required for Enterprises (HFTs, Cloud Giants, etc.).
 * See LICENSE file for full terms and conditions.
 * =========================================================================
 */

#include "ananth.h"
#include <chrono>

AnanthContext g_engine = {
    nullptr, nullptr, 0,
    REG_SHIFT, (REG_SHIFT + REG_SHIFT), 0,
    0,
    {0},
    0
};

thread_local FastCache t_cache = {};
static std::atomic<size_t> g_thread_id{0};

static inline void ananth_cpu_relax() noexcept {
#if defined(ANANTH_ARCH_X86_64_AMD_INTEL)
    #if defined(_MSC_VER)
        _mm_pause();
    #else
        __builtin_ia32_pause();
    #endif
#elif defined(ANANTH_ARCH_ARM64)
    #if defined(_MSC_VER)
        __yield();
    #else
        asm volatile("yield" ::: "memory");
    #endif
#endif
}

// Portable OS Virtual Memory Bridge (Windows VirtualAlloc + POSIX mmap)
static void* ananth_os_alloc(size_t bytes) noexcept {
#ifdef ANANTH_OS_WINDOWS
    return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
#endif
}

static void ananth_os_free(void* ptr, size_t bytes) noexcept {
    if (!ptr || bytes == 0) return;
#ifdef ANANTH_OS_WINDOWS
    (void)bytes;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, bytes);
#endif
}

// SplitMix64 avalanche mixer for full-width XOR canary entropy
static inline uintptr_t splitmix_avalanche(uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return static_cast<uintptr_t>(x | 1ULL);
}

static inline void init_thread_local_state() noexcept {
    size_t tid = g_thread_id.fetch_add(1, std::memory_order_relaxed);
    size_t stride = g_engine.metadata_map_entries >> 4;
    if (stride == 0) stride = 1;
    t_cache.local_cursor = (tid * stride) & (g_engine.metadata_map_entries - 1);
    t_cache.initialized = true;
}

static void flush_list_coalesced(void** list, size_t& count, size_t flush_target, reg_t base_mask) noexcept {
    if (!g_engine.buckets_pool || !g_engine.metadata_map || count == 0) return;

    size_t shift         = g_engine.cache_shift;
    size_t chunk_shift   = g_engine.chunk_shift;
    uintptr_t pool_start = reinterpret_cast<uintptr_t>(g_engine.buckets_pool);
    size_t current_chunk = ~static_cast<size_t>(0);
    reg_t coalesced_mask = 0;

    while (flush_target > 0 && count > 0) {
        void* masked_p   = list[--count];
        uintptr_t raw_p  = ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(masked_p);
        uintptr_t offset = raw_p - pool_start;
        if (ANANTH_UNLIKELY(offset >= g_engine.pool_size)) continue;

        size_t c_idx = offset >> chunk_shift;
        size_t b_idx = (offset >> shift) & REG_MASK;
        reg_t  mask  = base_mask << b_idx;

        if (c_idx == current_chunk) {
            coalesced_mask |= mask;
        } else {
            if (coalesced_mask != 0) {
                g_engine.metadata_map[current_chunk].fetch_or(coalesced_mask, std::memory_order_release);
                t_cache.local_cursor = current_chunk;
            }
            current_chunk = c_idx;
            coalesced_mask = mask;
        }
        flush_target--;
    }

    if (coalesced_mask != 0) {
        g_engine.metadata_map[current_chunk].fetch_or(coalesced_mask, std::memory_order_release);
        t_cache.local_cursor = current_chunk;
    }
}

void Ananth_flush_thread_cache() noexcept {
    if (g_engine.init_state.load(std::memory_order_acquire) != 2) return;
    if (!g_engine.buckets_pool || !g_engine.metadata_map) return;

    if (t_cache.count > 0) {
        flush_list_coalesced(t_cache.free_list, t_cache.count, t_cache.count, static_cast<reg_t>(1));
    }
    for (size_t blocks = 2; blocks <= REG_BITS; ++blocks) {
        if (t_cache.bin_count[blocks] > 0) {
            reg_t base_mask = (blocks == REG_BITS)
                ? ~static_cast<reg_t>(0)
                : ((static_cast<reg_t>(1) << blocks) - 1);
            flush_list_coalesced(t_cache.bin_list[blocks], t_cache.bin_count[blocks],
                                 t_cache.bin_count[blocks], base_mask);
        }
    }
}

// Guaranteed Zero-Leak Thread Teardown Destructor
FastCache::~FastCache() noexcept {
    Ananth_flush_thread_cache();
    initialized = false;
}

void Ananth_init() noexcept {
    uint8_t expected = 0;
    if (!g_engine.init_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        while (g_engine.init_state.load(std::memory_order_acquire) == 1) ananth_cpu_relax();
        return;
    }

    size_t actual_cache_line = HW_CACHE_LINE;
#if !defined(ANANTH_OS_WINDOWS) && defined(_SC_LEVEL1_DCACHE_LINESIZE)
    long sys_cache_line = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (sys_cache_line > 0) actual_cache_line = static_cast<size_t>(sys_cache_line);
#endif

    g_engine.cache_shift = ananth_ctz(static_cast<reg_t>(actual_cache_line));
    actual_cache_line    = static_cast<size_t>(1) << g_engine.cache_shift;

    g_engine.chunk_shift = g_engine.cache_shift + REG_SHIFT;
    size_t chunk_bytes   = static_cast<size_t>(1) << g_engine.chunk_shift;

    uint64_t total_system_ram = static_cast<uint64_t>(chunk_bytes) << (REG_SHIFT + (REG_SHIFT >> 1));
#ifdef ANANTH_OS_WINDOWS
    MEMORYSTATUSEX mem_status{};
    mem_status.dwLength = sizeof(mem_status);
    if (GlobalMemoryStatusEx(&mem_status) && mem_status.ullTotalPhys > 0) {
        total_system_ram = static_cast<uint64_t>(mem_status.ullTotalPhys);
    }
#else
    long sys_page_size = -1;
    #ifdef _SC_PAGESIZE
    sys_page_size = sysconf(_SC_PAGESIZE);
    #endif
    size_t actual_page_size = (sys_page_size > 0) ? static_cast<size_t>(sys_page_size) : chunk_bytes;

    long phys_pages = -1;
    #ifdef _SC_PHYS_PAGES
    phys_pages = sysconf(_SC_PHYS_PAGES);
    #endif
    if (phys_pages > 0) {
        total_system_ram = static_cast<uint64_t>(phys_pages) * actual_page_size;
    }
#endif

    // Pool sizing: 1/32 of RAM clamped between 2 MB and 64 MB on 64-bit
    uint64_t target_pool = total_system_ram >> 5;
    uint64_t min_pool    = static_cast<uint64_t>(chunk_bytes) << (REG_SHIFT + (REG_SHIFT >> 1));
    uint64_t max_pool    = min_pool << (REG_SHIFT - 1); // Up to 64 MB on 64-bit (vs 16 MB cap in V1.0)
    if (target_pool < min_pool) target_pool = min_pool;
    if (target_pool > max_pool) target_pool = max_pool;

    size_t raw_chunks = static_cast<size_t>(target_pool >> g_engine.chunk_shift);
    if (raw_chunks < REG_BITS) raw_chunks = REG_BITS;
    size_t pow2_chunks = static_cast<size_t>(1) << ((REG_BITS - 1) - ananth_clz(static_cast<reg_t>(raw_chunks)));

    void* pool_mem = nullptr;
    while (pow2_chunks >= REG_BITS) {
        g_engine.metadata_map_entries = pow2_chunks;
        g_engine.pool_size = pow2_chunks << g_engine.chunk_shift;
        pool_mem = ananth_os_alloc(g_engine.pool_size);
        if (pool_mem != nullptr) break;
        pow2_chunks >>= 1;
    }
    if (!pool_mem) {
        g_engine.buckets_pool = nullptr;
        g_engine.init_state.store(0, std::memory_order_release);
        return;
    }
    g_engine.buckets_pool = static_cast<char*>(pool_mem);

    size_t metadata_bytes = g_engine.metadata_map_entries * sizeof(std::atomic<reg_t>);
    void* map_mem = ananth_os_alloc(metadata_bytes);
    if (!map_mem) {
        ananth_os_free(g_engine.buckets_pool, g_engine.pool_size);
        g_engine.buckets_pool = nullptr;
        g_engine.metadata_map = nullptr;
        g_engine.init_state.store(0, std::memory_order_release);
        return;
    }
    g_engine.metadata_map = static_cast<std::atomic<reg_t>*>(map_mem);

    // Full 64-bit SplitMix64 ASLR + Clock Entropy XOR Canary Seed
    uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    uint64_t raw_entropy = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_engine.buckets_pool)) ^
                           (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_engine.metadata_map)) << 13) ^
                           static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&g_engine)) ^
                           now_ns;
    g_engine.canary_seed = splitmix_avalanche(raw_entropy);

    reg_t all_free = ~static_cast<reg_t>(0);
    for (size_t i = 0; i < g_engine.metadata_map_entries; ++i) {
        g_engine.metadata_map[i].store(all_free, std::memory_order_relaxed);
    }
    g_engine.cursor_pool = 0;
    g_engine.init_state.store(2, std::memory_order_release);
}

void Ananth_destroy() noexcept {
    if (g_engine.init_state.load(std::memory_order_acquire) != 2) return;
    Ananth_flush_thread_cache();

    uint8_t expected = 2;
    if (!g_engine.init_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        return;
    }

    size_t metadata_bytes = g_engine.metadata_map_entries * sizeof(std::atomic<reg_t>);
    ananth_os_free(g_engine.metadata_map, metadata_bytes);
    ananth_os_free(g_engine.buckets_pool, g_engine.pool_size);

    g_engine.metadata_map         = nullptr;
    g_engine.buckets_pool         = nullptr;
    g_engine.metadata_map_entries = 0;
    g_engine.pool_size            = 0;
    g_engine.canary_seed          = 0;
    g_engine.cursor_pool          = 0;
    t_cache.initialized           = false;

    g_engine.init_state.store(0, std::memory_order_release);
}

// SWAR O(log2(REG_BITS)) Contiguous Bit Discovery
static inline reg_t find_contiguous_bits(reg_t word, size_t blocks) noexcept {
    reg_t w = word;
    size_t run_len = 1;
    size_t s = 1;
    while (s <= (blocks - run_len)) {
        w &= (w >> s);
        run_len += s;
        s <<= 1;
    }
    if (run_len < blocks) {
        w &= (w >> (blocks - run_len));
    }
    return w;
}

void* Ananth_allocate_slow_refill(size_t req_bytes) noexcept {
    if (ANANTH_UNLIKELY(g_engine.init_state.load(std::memory_order_acquire) != 2)) {
        Ananth_init();
        if (ANANTH_UNLIKELY(g_engine.init_state.load(std::memory_order_acquire) != 2)) return nullptr;
    }
    if (ANANTH_UNLIKELY(!t_cache.initialized)) init_thread_local_state();

    size_t shift     = g_engine.cache_shift;
    size_t line_size = static_cast<size_t>(1) << shift;
    size_t map_mask  = g_engine.metadata_map_entries - 1;
    size_t cursor    = t_cache.local_cursor & map_mask;

    if (ANANTH_LIKELY(req_bytes <= line_size)) {
        for (size_t attempts = 0; attempts < g_engine.metadata_map_entries; ++attempts) {
            reg_t word = g_engine.metadata_map[cursor].load(std::memory_order_relaxed);
            while (word != 0) {
                reg_t temp = word;
                reg_t claimed_mask = 0;
                size_t count = 0;
                size_t room = FAST_CACHE_CAPACITY - t_cache.count;
                size_t batch_limit = (room < FAST_BATCH_SIZE) ? room : FAST_BATCH_SIZE;
                if (batch_limit == 0) batch_limit = 1;

                while (temp != 0 && count < batch_limit) {
                    reg_t lowest_bit = temp & (~temp + static_cast<reg_t>(1));
                    claimed_mask |= lowest_bit;
                    temp &= ~lowest_bit;
                    count++;
                }

                if (g_engine.metadata_map[cursor].compare_exchange_weak(
                        word, word & ~claimed_mask,
                        std::memory_order_acquire, std::memory_order_relaxed)) {
                    char* base = g_engine.buckets_pool + (cursor << g_engine.chunk_shift);
                    void* ret_ptr = nullptr;
                    while (claimed_mask != 0) {
                        unsigned b = ananth_ctz(claimed_mask);
                        claimed_mask &= (claimed_mask - 1);
                        void* ptr = base + (static_cast<size_t>(b) << shift);
                        if (!ret_ptr) {
                            ret_ptr = ptr;
                        } else if (t_cache.count < FAST_CACHE_CAPACITY) {
                            t_cache.free_list[t_cache.count++] =
                                reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ptr));
                        }
                    }
                    t_cache.local_cursor = cursor;
                    return ret_ptr;
                }
            }
            cursor = (cursor + 1) & map_mask;
        }
        return nullptr;
    }

    size_t blocks = (req_bytes + (line_size - 1)) >> shift;

    if (ANANTH_LIKELY(blocks <= REG_BITS)) {
        reg_t base_mask = (blocks == REG_BITS)
            ? ~static_cast<reg_t>(0)
            : ((static_cast<reg_t>(1) << blocks) - 1);

        for (size_t attempts = 0; attempts < g_engine.metadata_map_entries; ++attempts) {
            reg_t word = g_engine.metadata_map[cursor].load(std::memory_order_relaxed);
            while (word != 0) {
                reg_t w = find_contiguous_bits(word, blocks);
                if (w == 0) break;

                reg_t temp_word = word;
                reg_t total_claimed_mask = 0;
                size_t bit_positions[FAST_BIN_BATCH];
                size_t found = 0;
                size_t room = FAST_BIN_CAPACITY - t_cache.bin_count[blocks];
                size_t limit = (room + 1 < FAST_BIN_BATCH) ? (room + 1) : FAST_BIN_BATCH;
                if (limit == 0) limit = 1;

                while (found < limit) {
                    reg_t run_w = (found == 0) ? w : find_contiguous_bits(temp_word, blocks);
                    if (run_w == 0) break;
                    unsigned start_bit = ananth_ctz(run_w);
                    reg_t alloc_mask = base_mask << start_bit;
                    temp_word &= ~alloc_mask;
                    total_claimed_mask |= alloc_mask;
                    bit_positions[found++] = start_bit;
                }

                if (g_engine.metadata_map[cursor].compare_exchange_weak(
                        word, word & ~total_claimed_mask,
                        std::memory_order_acquire, std::memory_order_relaxed)) {
                    char* base = g_engine.buckets_pool + (cursor << g_engine.chunk_shift);
                    void* ret_ptr = base + (bit_positions[0] << shift);
                    for (size_t i = 1; i < found; ++i) {
                        void* ptr = base + (bit_positions[i] << shift);
                        if (t_cache.bin_count[blocks] < FAST_BIN_CAPACITY) {
                            t_cache.bin_list[blocks][t_cache.bin_count[blocks]++] =
                                reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ptr));
                        }
                    }
                    t_cache.local_cursor = cursor;
                    return ret_ptr;
                }
            }
            cursor = (cursor + 1) & map_mask;
        }
        return nullptr;
    }

    // Multi-Chunk Allocation (> 1 Chunk, e.g. > 4KB on 64-bit) with Contention Backoff
    size_t chunks_needed = (blocks + REG_MASK) >> REG_SHIFT;
    if (ANANTH_UNLIKELY(chunks_needed > g_engine.metadata_map_entries)) return nullptr;

    reg_t full_chunk = ~static_cast<reg_t>(0);
    size_t max_scans = g_engine.metadata_map_entries + 2;

    for (size_t attempts = 0; attempts < max_scans; ++attempts) {
        if ((cursor + chunks_needed) > g_engine.metadata_map_entries) {
            cursor = 0;
        }

        bool all_free = true;
        for (size_t c = 0; c < chunks_needed; ++c) {
            if (g_engine.metadata_map[cursor + c].load(std::memory_order_relaxed) != full_chunk) {
                all_free = false;
                cursor = (cursor + c + 1) & map_mask;
                break;
            }
        }
        if (all_free) {
            size_t claimed = 0;
            for (; claimed < chunks_needed; ++claimed) {
                reg_t expected = full_chunk;
                if (!g_engine.metadata_map[cursor + claimed].compare_exchange_strong(
                        expected, 0, std::memory_order_acquire, std::memory_order_relaxed)) {
                    break;
                }
            }
            if (claimed == chunks_needed) {
                t_cache.local_cursor = (cursor + chunks_needed) & map_mask;
                return g_engine.buckets_pool + (cursor << g_engine.chunk_shift);
            }
            // Rollback partial claim and apply brief spin backoff to prevent livelock
            for (size_t r = 0; r < claimed; ++r) {
                g_engine.metadata_map[cursor + r].store(full_chunk, std::memory_order_release);
            }
            ananth_cpu_relax();
            cursor = (cursor + claimed + 1) & map_mask;
        }
    }
    return nullptr;
}

void Ananth_free_slow_flush(void* ptr, size_t req_bytes) noexcept {
    if (ANANTH_UNLIKELY(!g_engine.buckets_pool || !g_engine.metadata_map)) return;

    size_t shift       = g_engine.cache_shift;
    size_t chunk_shift = g_engine.chunk_shift;
    size_t line_size   = static_cast<size_t>(1) << shift;

    if (ANANTH_LIKELY(req_bytes <= line_size)) {
        flush_list_coalesced(t_cache.free_list, t_cache.count, FAST_BATCH_SIZE, static_cast<reg_t>(1));
        t_cache.free_list[t_cache.count++] =
            reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ptr));
        return;
    }

    size_t blocks = (req_bytes + (line_size - 1)) >> shift;

    if (ANANTH_LIKELY(blocks <= REG_BITS)) {
        reg_t base_mask = (blocks == REG_BITS)
            ? ~static_cast<reg_t>(0)
            : ((static_cast<reg_t>(1) << blocks) - 1);
        flush_list_coalesced(t_cache.bin_list[blocks], t_cache.bin_count[blocks], FAST_BIN_BATCH, base_mask);
        t_cache.bin_list[blocks][t_cache.bin_count[blocks]++] =
            reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ptr));
        return;
    }

    size_t chunks_needed = (blocks + REG_MASK) >> REG_SHIFT;
    uintptr_t pool_start = reinterpret_cast<uintptr_t>(g_engine.buckets_pool);
    uintptr_t offset     = reinterpret_cast<uintptr_t>(ptr) - pool_start;
    if (ANANTH_UNLIKELY(offset >= g_engine.pool_size)) return;

    size_t start_chunk = offset >> chunk_shift;
    if (ANANTH_UNLIKELY(start_chunk + chunks_needed > g_engine.metadata_map_entries)) return;

    reg_t full_chunk = ~static_cast<reg_t>(0);
    for (size_t c = 0; c < chunks_needed; ++c) {
        g_engine.metadata_map[start_chunk + c].store(full_chunk, std::memory_order_release);
    }
    t_cache.local_cursor = start_chunk;
}
