/*
 * ============================================================================
 * Project: Ananth Engine V1.0
 * Title:   Dynamic Resource Optimization & Self-Healing Kernel Memory Engine
 * Author:  Mane Ram Naren (Ananth E Tech)
 * Patent:  Indian Patent Application No. 202641127187 (Patent Pending)
 *
 * LICENSE: Ananth E-Tech Fair-Use License
 * Non-commercial academic and student research use only.
 * ============================================================================
 */

#include "ananth.h"

AnanthContext g_engine = {
    nullptr, nullptr, 0,
    REG_SHIFT, (REG_SHIFT + REG_SHIFT), 0,
    0,
    0
};

thread_local FastCache t_cache = {};
static std::once_flag init_once_flag;
static std::atomic<size_t> g_thread_id{0};

static inline void init_thread_local_state() {
    size_t tid = g_thread_id.fetch_add(1, std::memory_order_relaxed);
    size_t stride = g_engine.metadata_map_entries >> 4;
    if (stride == 0) stride = 1;
    t_cache.local_cursor = (tid * stride) & (g_engine.metadata_map_entries - 1);
    t_cache.initialized = true;
}

void Ananth_init() {
    std::call_once(init_once_flag, []() {
        long sys_cache_line = -1;
        #ifdef _SC_LEVEL1_DCACHE_LINESIZE
        sys_cache_line = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
        #endif
        size_t actual_cache_line = (sys_cache_line > 0)
            ? static_cast<size_t>(sys_cache_line)
            : static_cast<size_t>(HW_CACHE_LINE);

        g_engine.cache_shift = REG_CTZ(actual_cache_line);
        actual_cache_line = static_cast<size_t>(1) << g_engine.cache_shift;

        g_engine.chunk_shift = g_engine.cache_shift + REG_SHIFT;
        size_t chunk_bytes = static_cast<size_t>(1) << g_engine.chunk_shift;

        long sys_page_size = -1;
        #ifdef _SC_PAGESIZE
        sys_page_size = sysconf(_SC_PAGESIZE);
        #endif
        size_t actual_page_size = (sys_page_size > 0)
            ? static_cast<size_t>(sys_page_size)
            : chunk_bytes;

        long phys_pages = -1;
        #ifdef _SC_PHYS_PAGES
        phys_pages = sysconf(_SC_PHYS_PAGES);
        #endif
        uint64_t total_system_ram = (phys_pages > 0)
            ? (static_cast<uint64_t>(phys_pages) * actual_page_size)
            : (static_cast<uint64_t>(chunk_bytes) << (REG_SHIFT + (REG_SHIFT >> 1)));

        uint64_t target_pool = total_system_ram >> 5;
        uint64_t min_pool = static_cast<uint64_t>(chunk_bytes) << (REG_SHIFT + (REG_SHIFT >> 1));
        uint64_t max_pool = min_pool << (REG_SHIFT >> 1);
        if (target_pool < min_pool) target_pool = min_pool;
        if (target_pool > max_pool) target_pool = max_pool;

        size_t raw_chunks = static_cast<size_t>(target_pool >> g_engine.chunk_shift);
        if (raw_chunks < REG_BITS) raw_chunks = REG_BITS;
        size_t pow2_chunks = static_cast<size_t>(1) << ((REG_BITS - 1) - REG_CLZ(raw_chunks));

        void* pool_mem = MAP_FAILED;
        while (pow2_chunks >= REG_BITS) {
            g_engine.metadata_map_entries = pow2_chunks;
            g_engine.pool_size = pow2_chunks << g_engine.chunk_shift;
            pool_mem = mmap(nullptr, g_engine.pool_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (pool_mem != MAP_FAILED) break;
            pow2_chunks >>= 1;
        }
        if (pool_mem == MAP_FAILED) {
            g_engine.buckets_pool = nullptr;
            return;
        }
        g_engine.buckets_pool = static_cast<char*>(pool_mem);

        size_t metadata_bytes = g_engine.metadata_map_entries * sizeof(reg_t);
        void* map_mem = mmap(nullptr, metadata_bytes, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (map_mem == MAP_FAILED) {
            munmap(g_engine.buckets_pool, g_engine.pool_size);
            g_engine.buckets_pool = nullptr;
            g_engine.metadata_map = nullptr;
            return;
        }
        g_engine.metadata_map = static_cast<reg_t*>(map_mem);

        // Dynamic ASLR Hardware Entropy for XOR Canary Seed (Zero Fixed Numbers!)
        uintptr_t pool_entropy = reinterpret_cast<uintptr_t>(g_engine.buckets_pool);
        uintptr_t map_entropy  = reinterpret_cast<uintptr_t>(g_engine.metadata_map);
        uintptr_t ctx_entropy  = reinterpret_cast<uintptr_t>(&g_engine);
        uintptr_t mixed_seed   = pool_entropy ^ (map_entropy << g_engine.cache_shift) ^ (ctx_entropy >> g_engine.cache_shift);
        g_engine.canary_seed   = mixed_seed ^ (mixed_seed >> (REG_BITS >> 1)) ^ (~static_cast<uintptr_t>(0) << CANARY_SIZE);

        reg_t all_free = ~static_cast<reg_t>(0);
        for (size_t i = 0; i < g_engine.metadata_map_entries; ++i) {
            g_engine.metadata_map[i] = all_free;
        }
        g_engine.cursor_pool = 0;
    });
}

static inline reg_t find_contiguous_bits(reg_t word, size_t blocks) {
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

void* Ananth_allocate_slow_refill(size_t req_bytes) {
    if (__builtin_expect(!g_engine.buckets_pool, 0)) {
        Ananth_init();
        if (__builtin_expect(!g_engine.buckets_pool, 0)) return nullptr;
    }
    if (__builtin_expect(!t_cache.initialized, 0)) init_thread_local_state();

    size_t shift     = g_engine.cache_shift;
    size_t line_size = static_cast<size_t>(1) << shift;
    size_t map_mask  = g_engine.metadata_map_entries - 1;
    size_t cursor    = t_cache.local_cursor;

    if (__builtin_expect(req_bytes <= line_size, 1)) {
        for (size_t attempts = 0; attempts < g_engine.metadata_map_entries; ++attempts) {
            reg_t word = __atomic_load_n(&g_engine.metadata_map[cursor], __ATOMIC_RELAXED);
            while (word != 0) {
                reg_t temp = word;
                reg_t claimed_mask = 0;
                size_t count = 0;

                while (temp != 0 && count < FAST_BATCH_SIZE) {
                    reg_t lowest_bit = temp & (-temp);
                    claimed_mask |= lowest_bit;
                    temp &= ~lowest_bit;
                    count++;
                }

                if (__atomic_compare_exchange_n(&g_engine.metadata_map[cursor], &word,
                                                word & ~claimed_mask, true,
                                                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                    char* base = g_engine.buckets_pool + (cursor << g_engine.chunk_shift);
                    while (claimed_mask != 0) {
                        reg_t b = REG_CTZ(claimed_mask);
                        claimed_mask &= (claimed_mask - 1);
                        void* ptr = base + (b << shift);
                        t_cache.free_list[t_cache.count++] =
                            reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ptr));
                    }
                    t_cache.local_cursor = cursor;
                    void* ret_ptr = t_cache.free_list[--t_cache.count];
                    return reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ret_ptr));
                }
            }
            cursor = (cursor + 1) & map_mask;
        }
        return nullptr;
    }

    size_t blocks = (req_bytes + (line_size - 1)) >> shift;

    if (__builtin_expect(blocks <= REG_BITS, 1)) {
        reg_t base_mask = (blocks == REG_BITS) ? ~static_cast<reg_t>(0) : ((static_cast<reg_t>(1) << blocks) - 1);

        for (size_t attempts = 0; attempts < g_engine.metadata_map_entries; ++attempts) {
            reg_t word = __atomic_load_n(&g_engine.metadata_map[cursor], __ATOMIC_RELAXED);
            while (word != 0) {
                reg_t w = find_contiguous_bits(word, blocks);
                if (w == 0) break;

                reg_t temp_word = word;
                reg_t total_claimed_mask = 0;
                size_t bit_positions[FAST_BIN_BATCH];
                size_t found = 0;

                while (found < FAST_BIN_BATCH) {
                    reg_t run_w = (found == 0) ? w : find_contiguous_bits(temp_word, blocks);
                    if (run_w == 0) break;
                    reg_t start_bit = REG_CTZ(run_w);
                    reg_t alloc_mask = base_mask << start_bit;
                    temp_word &= ~alloc_mask;
                    total_claimed_mask |= alloc_mask;
                    bit_positions[found++] = start_bit;
                }

                if (__atomic_compare_exchange_n(&g_engine.metadata_map[cursor], &word,
                                                word & ~total_claimed_mask, true,
                                                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                    char* base = g_engine.buckets_pool + (cursor << g_engine.chunk_shift);
                    for (size_t i = 0; i < found; ++i) {
                        void* ptr = base + (bit_positions[i] << shift);
                        t_cache.bin_list[blocks][t_cache.bin_count[blocks]++] =
                            reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ptr));
                    }
                    t_cache.local_cursor = cursor;
                    void* ret_ptr = t_cache.bin_list[blocks][--t_cache.bin_count[blocks]];
                    return reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ret_ptr));
                }
            }
            cursor = (cursor + 1) & map_mask;
        }
        return nullptr;
    }

    size_t chunks_needed = (blocks + REG_MASK) >> REG_SHIFT;
    if (__builtin_expect(chunks_needed > g_engine.metadata_map_entries, 0)) return nullptr;

    reg_t full_chunk = ~static_cast<reg_t>(0);

    for (size_t attempts = 0; attempts < g_engine.metadata_map_entries; ++attempts) {
        if ((cursor + chunks_needed) <= g_engine.metadata_map_entries) {
            bool all_free = true;
            for (size_t c = 0; c < chunks_needed; ++c) {
                if (__atomic_load_n(&g_engine.metadata_map[cursor + c], __ATOMIC_RELAXED) != full_chunk) {
                    all_free = false;
                    cursor = (cursor + c + 1) & map_mask;
                    break;
                }
            }
            if (all_free) {
                size_t claimed = 0;
                for (; claimed < chunks_needed; ++claimed) {
                    reg_t expected = full_chunk;
                    if (!__atomic_compare_exchange_n(&g_engine.metadata_map[cursor + claimed],
                                                     &expected, 0, false,
                                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                        break;
                    }
                }
                if (claimed == chunks_needed) {
                    t_cache.local_cursor = (cursor + chunks_needed) & map_mask;
                    return g_engine.buckets_pool + (cursor << g_engine.chunk_shift);
                }
                for (size_t r = 0; r < claimed; ++r) {
                    __atomic_store_n(&g_engine.metadata_map[cursor + r], full_chunk, __ATOMIC_RELEASE);
                }
                cursor = (cursor + claimed + 1) & map_mask;
            }
        } else {
            cursor = 0;
        }
    }
    return nullptr;
}

void Ananth_free_slow_flush(void* ptr, size_t req_bytes) {
    if (__builtin_expect(!g_engine.buckets_pool, 0)) return;

    size_t shift       = g_engine.cache_shift;
    size_t chunk_shift = g_engine.chunk_shift;
    size_t line_size   = static_cast<size_t>(1) << shift;
    uintptr_t pool_start = reinterpret_cast<uintptr_t>(g_engine.buckets_pool);

    if (__builtin_expect(req_bytes <= line_size, 1)) {
        size_t flush_target = FAST_BATCH_SIZE;
        size_t current_chunk = ~static_cast<size_t>(0);
        reg_t  coalesced_mask = 0;

        while (flush_target > 0 && t_cache.count > 0) {
            void* masked_p = t_cache.free_list[--t_cache.count];
            uintptr_t raw_p = ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(masked_p);
            uintptr_t offset = raw_p - pool_start;
            if (__builtin_expect(offset >= g_engine.pool_size, 0)) continue;

            size_t c_idx = offset >> chunk_shift;
            size_t b_idx = (offset >> shift) & REG_MASK;
            reg_t  bit   = static_cast<reg_t>(1) << b_idx;

            if (c_idx == current_chunk) {
                coalesced_mask |= bit;
            } else {
                if (coalesced_mask != 0) {
                    __atomic_fetch_or(&g_engine.metadata_map[current_chunk], coalesced_mask, __ATOMIC_RELEASE);
                    t_cache.local_cursor = current_chunk;
                }
                current_chunk  = c_idx;
                coalesced_mask = bit;
            }
            flush_target--;
        }
        if (coalesced_mask != 0) {
            __atomic_fetch_or(&g_engine.metadata_map[current_chunk], coalesced_mask, __ATOMIC_RELEASE);
            t_cache.local_cursor = current_chunk;
        }
        t_cache.free_list[t_cache.count++] =
            reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ptr));
        return;
    }

    size_t blocks = (req_bytes + (line_size - 1)) >> shift;

    if (__builtin_expect(blocks <= REG_BITS, 1)) {
        reg_t base_mask = (blocks == REG_BITS) ? ~static_cast<reg_t>(0) : ((static_cast<reg_t>(1) << blocks) - 1);
        size_t flush_target = FAST_BIN_BATCH;
        size_t current_chunk = ~static_cast<size_t>(0);
        reg_t  coalesced_mask = 0;

        while (flush_target > 0 && t_cache.bin_count[blocks] > 0) {
            void* masked_p = t_cache.bin_list[blocks][--t_cache.bin_count[blocks]];
            uintptr_t raw_p = ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(masked_p);
            uintptr_t offset = raw_p - pool_start;
            if (__builtin_expect(offset >= g_engine.pool_size, 0)) continue;

            size_t c_idx = offset >> chunk_shift;
            size_t b_idx = (offset >> shift) & REG_MASK;
            reg_t  mask  = base_mask << b_idx;

            if (c_idx == current_chunk) {
                coalesced_mask |= mask;
            } else {
                if (coalesced_mask != 0) {
                    __atomic_fetch_or(&g_engine.metadata_map[current_chunk], coalesced_mask, __ATOMIC_RELEASE);
                    t_cache.local_cursor = current_chunk;
                }
                current_chunk  = c_idx;
                coalesced_mask = mask;
            }
            flush_target--;
        }
        if (coalesced_mask != 0) {
            __atomic_fetch_or(&g_engine.metadata_map[current_chunk], coalesced_mask, __ATOMIC_RELEASE);
            t_cache.local_cursor = current_chunk;
        }
        t_cache.bin_list[blocks][t_cache.bin_count[blocks]++] =
            reinterpret_cast<void*>(ANANTH_CANARY_SEED ^ reinterpret_cast<uintptr_t>(ptr));
        return;
    }

    size_t chunks_needed = (blocks + REG_MASK) >> REG_SHIFT;
    uintptr_t offset = reinterpret_cast<uintptr_t>(ptr) - pool_start;
    if (__builtin_expect(offset >= g_engine.pool_size, 0)) return;

    size_t start_chunk = offset >> chunk_shift;
    reg_t full_chunk = ~static_cast<reg_t>(0);

    for (size_t c = 0; c < chunks_needed; ++c) {
        __atomic_store_n(&g_engine.metadata_map[start_chunk + c], full_chunk, __ATOMIC_RELEASE);
    }
    t_cache.local_cursor = start_chunk;
}