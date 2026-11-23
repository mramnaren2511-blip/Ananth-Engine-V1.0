# Ananth Engine V1.1

**Dynamic Resource Optimization & Self-Healing Kernel Memory Engine**  
**Author:** Mane Ram Naren ([Ananth E Tech](https://github.com/mramnaren2511-blip/Ananth-Engine-V1.0))  
**Patent Pending:** Indian Patent Application No. `202641127187` (Filed `26-10-2026`)  
**License:** Ananth E-Tech Fair-Use License (Free for individuals, researchers & startups under $2M revenue/funding; commercial license for enterprises)

---

## Architecture & V1.1 Production Hardening

Ananth Engine is a bare-metal, lock-free fixed-pool slab/bitmap memory engine designed for embedded, kernel, and ultra-low-latency C++17 workloads on **x86_64, ARM64, and 32-bit** platforms across **Linux, Windows, macOS, and Android**.

### Core Design
- **O(1) Inlined Fast-Path (`Ananth_allocate` / `Ananth_free`):** Serves `<= 4 KB` allocations from compact `thread_local FastCache` pockets with zero locks and zero atomics on the hot path.
- **SWAR Contiguous Bit Discovery:** Finds contiguous free blocks (`2..64` cache lines) in $O(\log_2 \text{REG\_BITS})$ bitwise operations.
- **Coalesced Atomic Flushing:** Merges multiple freed blocks belonging to the same chunk into a single atomic `fetch_or(..., memory_order_release)` commit.
- **Full-Width ASLR + SplitMix64 XOR Canary Masking:** Protects cached freelist pointers with full-entropy hardware-seeded XOR masking.

### V1.1 Fixes & Upgrades
- **True Cross-Platform Windows & POSIX Support:** Added `VirtualAlloc`/`VirtualFree` + `GlobalMemoryStatusEx` on Windows alongside POSIX `mmap`/`munmap`, plus portable C++17 `std::atomic<reg_t>` and MSVC/GCC/Clang bit intrinsics.
- **Zero-Leak Thread Teardown:** Added `FastCache::~FastCache()` destructor to flush all cached blocks back to `g_engine.metadata_map` on worker thread exit (100% bitmap reclamation verified across 64 thread teardowns).
- **14.6x Smaller Per-Thread TLS Footprint:** Reduced `FastCache` static TLS usage from ~264 KB/thread to ~18 KB/thread.
- **Safe Lifecycle & Bounds Guards:** Added idempotent thread-safe auto-initialization, `Ananth_destroy()` cleanup API, out-of-pool pointer rejection in `Ananth_free`, and spin-backoff on multi-chunk contention.
- **Aligned Licensing:** Unified headers and documentation under the Ananth E-Tech Fair-Use License (<$2M free).

---

## Quick Start (Build, Test & Benchmark)

```bash
# Run correctness, zero-leak thread teardown & 8-thread concurrency stress tests
make test

# Run side-by-side benchmark vs system glibc malloc
make bench
```

---

## Verified Benchmark Results (Linux x86_64, GCC `-O3`)

Measured via `make bench` (`benchmarks/bench_suite.cpp`, throughput in million alloc+free pairs per second):

| Workload | System `glibc` | Ananth Engine V1.1 | Speedup |
| :--- | :---: | :---: | :---: |
| **64 B (4 Threads)** | 76.80 M/s | **194.47 M/s** | **2.53x** |
| **2 KB (Single-Thread)** | 39.62 M/s | **95.51 M/s** | **2.41x** |
| **256 B (Single-Thread)** | 43.61 M/s | **83.63 M/s** | **1.92x** |
| **64 B (Single-Thread)** | 80.26 M/s | **80.28 M/s** | **1.00x** |
| **8 KB (Single-Thread)\*** | 47.15 M/s | 1.73 M/s | 0.04x |

*\*Note: Ananth Engine is specialized for `<= 4 KB` slab/kernel blocks cached in `FastCache`. Requests `> 4 KB` bypass TLS bins and perform direct multi-chunk atomic bitmap CAS; for large-buffer (`64 KB – 4 MB`) workloads, use **Ananth E-Flash V2.0**.*
